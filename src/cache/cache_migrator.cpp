#include "cache/cache_migrator.h"
#include "core/crypto.h"
#include <spdlog/spdlog.h>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <ios>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace aegisgate {

namespace {

void writeU32LE(std::ostream& out, uint32_t v) {
    char b[4] = {char(v & 0xFF), char((v >> 8) & 0xFF),
                  char((v >> 16) & 0xFF), char((v >> 24) & 0xFF)};
    out.write(b, 4);
}

void writeU64LE(std::ostream& out, uint64_t v) {
    char b[8];
    for (int i = 0; i < 8; ++i) b[i] = char((v >> (i * 8)) & 0xFF);
    out.write(b, 8);
}

// Escapes a JSON string value: backslash, double-quote, control chars.
std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':  out.append("\\\""); break;
            case '\\': out.append("\\\\"); break;
            case '\n': out.append("\\n"); break;
            case '\r': out.append("\\r"); break;
            case '\t': out.append("\\t"); break;
            case '\b': out.append("\\b"); break;
            case '\f': out.append("\\f"); break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned int>(c));
                    out.append(buf);
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

std::string buildPayloadJson(const std::string& partition,
                             const std::string& id) {
    std::string s;
    s.reserve(partition.size() + id.size() + 32);
    s.append("{\"partition\":\"").append(jsonEscape(partition))
     .append("\",\"id\":\"").append(jsonEscape(id)).append("\"}");
    return s;
}

// Hex helpers: encode 32 bytes -> 64 hex chars, decode and compare.
std::string hexEncode(const unsigned char* data, size_t len) {
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.resize(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out[i * 2]     = kHex[(data[i] >> 4) & 0x0F];
        out[i * 2 + 1] = kHex[data[i] & 0x0F];
    }
    return out;
}

}  // namespace

CacheMigrator::DumpStats
CacheMigrator::dump(const VectorStore& source, const std::string& output_path) {
    DumpStats stats;

    // Build the body in memory so we can hash it deterministically before
    // appending the SR2 trailer. Snapshots are small relative to RSS in
    // typical deployments (tens of MB); the cost is bounded by max_elements.
    std::ostringstream body(std::ios::binary);

    // ---- Header --------------------------------------------------------
    body.write(reinterpret_cast<const char*>(kMagic), sizeof(kMagic));
    writeU32LE(body, kFormatVersion);

    // Dim is unknown until the first entry; we'll back-patch.
    uint32_t dim_field = 0;
    auto dim_pos = body.tellp();
    writeU32LE(body, dim_field);

    auto count_pos = body.tellp();
    writeU64LE(body, 0);

    // ---- Entries -------------------------------------------------------
    uint64_t entry_count = 0;
    uint32_t dim = 0;
    if (!source.enumerate(
            [&](const std::string& partition, const std::string& id,
                const std::vector<float>& vec) -> bool {
                if (dim == 0) dim = static_cast<uint32_t>(vec.size());
                else if (static_cast<uint32_t>(vec.size()) != dim) {
                    // Skip entries with inconsistent dim (defensive; the
                    // backend should never produce mixed-dim partitions).
                    return true;
                }
                const uint32_t vec_bytes =
                    static_cast<uint32_t>(vec.size() * sizeof(float));
                writeU32LE(body, vec_bytes);
                body.write(reinterpret_cast<const char*>(vec.data()),
                           vec_bytes);

                const std::string payload = buildPayloadJson(partition, id);
                writeU32LE(body, static_cast<uint32_t>(payload.size()));
                body.write(payload.data(), payload.size());

                ++entry_count;
                return true;
            })) {
        spdlog::info(
            "CacheMigrator::dump: backend '{}' does not support enumerate;"
            " emitting empty snapshot",
            source.backendName());
    }

    // Back-patch dim and entry count in the header.
    std::string serialized = body.str();
    auto dim_off = static_cast<size_t>(dim_pos);
    auto count_off = static_cast<size_t>(count_pos);
    serialized[dim_off]     = char(dim & 0xFF);
    serialized[dim_off + 1] = char((dim >> 8) & 0xFF);
    serialized[dim_off + 2] = char((dim >> 16) & 0xFF);
    serialized[dim_off + 3] = char((dim >> 24) & 0xFF);
    for (int i = 0; i < 8; ++i) {
        serialized[count_off + i] =
            char((entry_count >> (i * 8)) & 0xFF);
    }

    // ---- SR2 trailing sha256 over all preceding bytes ------------------
    std::string digest_hex = crypto::sha256(serialized);
    if (digest_hex.size() != 64) {
        throw std::runtime_error(
            "CacheMigrator::dump: sha256 produced unexpected digest length");
    }
    unsigned char digest_bytes[32];
    for (size_t i = 0; i < 32; ++i) {
        auto hi = digest_hex[i * 2];
        auto lo = digest_hex[i * 2 + 1];
        auto h2d = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
            if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
            return 0;
        };
        digest_bytes[i] = static_cast<unsigned char>((h2d(hi) << 4) | h2d(lo));
    }

    std::ofstream out(output_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("CacheMigrator::dump: cannot open output: " +
                                 output_path);
    }
    out.write(serialized.data(), serialized.size());
    out.write(reinterpret_cast<const char*>(digest_bytes), 32);
    out.flush();
    if (!out) {
        throw std::runtime_error("CacheMigrator::dump: write failed: " +
                                 output_path);
    }

    stats.entries_written = static_cast<size_t>(entry_count);
    stats.sha256_hex = digest_hex;
    stats.bytes_written = serialized.size() + 32;
    return stats;
}

namespace {

uint32_t readU32LE(const unsigned char* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint64_t readU64LE(const unsigned char* p) {
    return uint64_t(p[0]) | (uint64_t(p[1]) << 8) |
           (uint64_t(p[2]) << 16) | (uint64_t(p[3]) << 24) |
           (uint64_t(p[4]) << 32) | (uint64_t(p[5]) << 40) |
           (uint64_t(p[6]) << 48) | (uint64_t(p[7]) << 56);
}

// Trivial extractor for the two known JSON fields produced by dump().
// We deliberately avoid pulling in a full JSON parser here; the payload is
// produced internally and the format is fixed, so a string scan is enough.
bool extractField(const std::string& payload, const std::string& key,
                  std::string& out) {
    const std::string needle = "\"" + key + "\":\"";
    size_t p = payload.find(needle);
    if (p == std::string::npos) return false;
    p += needle.size();
    std::string val;
    while (p < payload.size()) {
        char c = payload[p];
        if (c == '\\' && p + 1 < payload.size()) {
            char n = payload[p + 1];
            switch (n) {
                case '"':  val.push_back('"');  break;
                case '\\': val.push_back('\\'); break;
                case 'n':  val.push_back('\n'); break;
                case 'r':  val.push_back('\r'); break;
                case 't':  val.push_back('\t'); break;
                case 'b':  val.push_back('\b'); break;
                case 'f':  val.push_back('\f'); break;
                default:   val.push_back(n);    break;
            }
            p += 2;
            continue;
        }
        if (c == '"') { out = std::move(val); return true; }
        val.push_back(c);
        ++p;
    }
    return false;
}

// "tenant:abc|conv:1" -> "abc"
std::string tenantFromPartition(const std::string& partition) {
    constexpr const char kPrefix[] = "tenant:";
    if (partition.compare(0, sizeof(kPrefix) - 1, kPrefix) != 0) return "";
    size_t end = partition.find('|', sizeof(kPrefix) - 1);
    if (end == std::string::npos) end = partition.size();
    return partition.substr(sizeof(kPrefix) - 1,
                            end - (sizeof(kPrefix) - 1));
}

}  // namespace

CacheMigrator::RestoreStats
CacheMigrator::restore(const std::string& input_path, VectorStore& target,
                       const std::vector<std::string>& tenant_allowlist) {
    RestoreStats stats;

    // SR2: read whole file, verify trailing sha256 before touching `target`.
    std::ifstream in(input_path, std::ios::binary);
    if (!in) {
        stats.failure_reason = "cannot open input: " + input_path;
        return stats;
    }
    in.seekg(0, std::ios::end);
    std::streamoff end_pos = in.tellg();
    if (end_pos < static_cast<std::streamoff>(32 + 32)) {
        stats.failure_reason = "truncated file (< minimum size)";
        return stats;
    }
    size_t total = static_cast<size_t>(end_pos);
    in.seekg(0, std::ios::beg);
    std::string buffer(total, '\0');
    in.read(buffer.data(), total);
    if (static_cast<size_t>(in.gcount()) != total) {
        stats.failure_reason = "short read";
        return stats;
    }

    size_t body_len = total - 32;
    std::string body(buffer.data(), body_len);
    std::string actual_hex = crypto::sha256(body);
    std::string stored_hex =
        hexEncode(reinterpret_cast<const unsigned char*>(buffer.data() + body_len),
                  32);
    if (!crypto::constantTimeEquals(actual_hex, stored_hex)) {
        stats.failure_reason = "sha256 mismatch (SR2)";
        return stats;
    }
    stats.checksum_ok = true;

    // ---- Header parse --------------------------------------------------
    auto raw = reinterpret_cast<const unsigned char*>(buffer.data());
    if (std::memcmp(raw, kMagic, 16) != 0) {
        stats.failure_reason = "bad magic";
        return stats;
    }
    uint32_t ver = readU32LE(raw + 16);
    if (ver != kFormatVersion) {
        stats.failure_reason = "unsupported version " + std::to_string(ver);
        return stats;
    }
    stats.version_ok = true;
    uint32_t dim = readU32LE(raw + 20);
    uint64_t count = readU64LE(raw + 24);

    // ---- Entries -------------------------------------------------------
    size_t off = 32;
    for (uint64_t i = 0; i < count; ++i) {
        if (off + 4 > body_len) {
            stats.failure_reason = "truncated entry header";
            return stats;
        }
        uint32_t vec_bytes = readU32LE(raw + off);
        off += 4;
        if (off + vec_bytes > body_len) {
            stats.failure_reason = "truncated vector bytes";
            return stats;
        }
        std::vector<float> vec;
        if (vec_bytes % sizeof(float) == 0) {
            size_t n = vec_bytes / sizeof(float);
            vec.resize(n);
            std::memcpy(vec.data(), raw + off, vec_bytes);
        }
        off += vec_bytes;

        if (off + 4 > body_len) {
            stats.failure_reason = "truncated payload length";
            return stats;
        }
        uint32_t payload_len = readU32LE(raw + off);
        off += 4;
        if (off + payload_len > body_len) {
            stats.failure_reason = "truncated payload";
            return stats;
        }
        std::string payload(reinterpret_cast<const char*>(raw + off),
                            payload_len);
        off += payload_len;
        ++stats.entries_read;

        std::string partition, id;
        if (!extractField(payload, "partition", partition) ||
            !extractField(payload, "id", id)) {
            ++stats.entries_corrupted;
            continue;
        }

        // SR2 tenant allowlist (empty list = pass through).
        if (!tenant_allowlist.empty()) {
            std::string tenant = tenantFromPartition(partition);
            bool allowed = false;
            for (const auto& t : tenant_allowlist) {
                if (t == tenant) { allowed = true; break; }
            }
            if (!allowed) {
                ++stats.entries_skipped_tenant;
                continue;
            }
        }

        if (vec.size() != dim) {
            ++stats.entries_corrupted;
            continue;
        }

        try {
            target.insert(partition, id, vec);
            ++stats.entries_restored;
        } catch (const std::exception& e) {
            spdlog::warn("CacheMigrator::restore: insert failed for {}/{}: {}",
                         partition, id, e.what());
            ++stats.entries_corrupted;
        }
    }
    return stats;
}

bool CacheMigrator::verifyChecksum(const std::string& path) const {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    auto end_pos = in.tellg();
    if (end_pos < static_cast<std::streamoff>(32 + 32)) return false;
    size_t total = static_cast<size_t>(end_pos);
    size_t body_len = total - 32;

    std::string body(body_len, '\0');
    in.seekg(0, std::ios::beg);
    in.read(body.data(), body_len);
    if (static_cast<size_t>(in.gcount()) != body_len) return false;

    unsigned char stored[32];
    in.read(reinterpret_cast<char*>(stored), 32);
    if (in.gcount() != 32) return false;

    std::string actual_hex = crypto::sha256(body);
    std::string stored_hex = hexEncode(stored, 32);
    return crypto::constantTimeEquals(actual_hex, stored_hex);
}

} // namespace aegisgate
