#pragma once
#include "cache/vector_store.h"
#include <cstdint>
#include <string>
#include <vector>

namespace aegisgate {

// Phase 6.2 (D5=B): offline dump/restore tool that streams cache entries
// through a versioned binary snapshot file. See
// docs/specs/2026-05-13-phase6-completion-design.md §5.3.1 for the wire
// format. SR2 mandates a trailing SHA-256 over all preceding bytes so the
// reader can detect tampering before any vector is restored.
class CacheMigrator {
public:
    static constexpr const char kMagic[16] =
        {'A', 'G', 'S', 'C', 'A', 'C', 'H', 'E',
         0, 0, 0, 0, 0, 0, 0, 1};
    static constexpr uint32_t kFormatVersion = 1;

    struct DumpStats {
        size_t entries_written = 0;
        std::string sha256_hex;   // 64-char lowercase hex of the file tail.
        size_t bytes_written = 0; // including header + entries + tail digest.
    };

    struct RestoreStats {
        size_t entries_read = 0;
        size_t entries_restored = 0;
        size_t entries_skipped_tenant = 0;
        size_t entries_corrupted = 0;
        std::string failure_reason;
        bool checksum_ok = false;
        bool version_ok = false;
    };

    // Returns DumpStats with entries_written == 0 when the source backend
    // does not support enumeration (e.g., remote Milvus/Qdrant). Throws
    // std::runtime_error on I/O failure.
    DumpStats dump(const VectorStore& source, const std::string& output_path);

    // Verifies the SR2 trailing sha256 of `path` without consuming entries.
    // Returns true iff the on-disk digest matches a fresh re-hash of the
    // file's preceding bytes.
    bool verifyChecksum(const std::string& path) const;

    // Reads a snapshot file and replays entries into `target`. SR2 mandates
    // sha256 verification *before* any insert is attempted; tenant allowlist
    // filters by exact match against the partition key prefix
    // ("tenant:<id>|...") and drops non-matching entries. Magic / version /
    // dim mismatch cause an early failure (entries_restored = 0).
    RestoreStats restore(const std::string& input_path, VectorStore& target,
                         const std::vector<std::string>& tenant_allowlist = {});
};

} // namespace aegisgate
