#include "control_plane/ulid.h"

#include <array>
#include <chrono>

namespace aegisgate {

namespace {

// Crockford Base32 alphabet — excludes I, L, O, U to avoid visual ambiguity.
constexpr char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

// Encode `value` into exactly `count` characters (big-endian) into `out`.
void encodeFixed(char* out, std::uint64_t value, int count) {
    for (int i = count - 1; i >= 0; --i) {
        out[i] = kAlphabet[value & 0x1F];
        value >>= 5;
    }
}

// Inverse of kAlphabet. -1 for invalid chars.
std::int8_t decodeChar(char c) {
    for (std::int8_t i = 0; i < 32; ++i) {
        if (kAlphabet[i] == c) return i;
    }
    return -1;
}

} // namespace

Ulid::Ulid() {
    std::random_device rd;
    std::array<std::uint64_t, 2> seed = {
        (static_cast<std::uint64_t>(rd()) << 32) | rd(),
        (static_cast<std::uint64_t>(rd()) << 32) | rd(),
    };
    std::seed_seq ss(seed.begin(), seed.end());
    rng_.seed(ss);
}

Ulid::Ulid(std::uint64_t seed) : rng_(seed) {}

std::string Ulid::generate() {
    using namespace std::chrono;
    auto now = duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
    return generateAt(now);
}

std::string Ulid::generateAt(std::int64_t unix_millis) {
    std::string out(26, '0');

    // --- 48-bit timestamp into 10 chars (50 bits of capacity, 2 top bits zero) ---
    // Only the low 48 bits are meaningful; shift into the top of a 50-bit slot.
    std::uint64_t ts = static_cast<std::uint64_t>(unix_millis) & 0xFFFFFFFFFFFFULL;
    encodeFixed(&out[0], ts, 10);

    // --- 80-bit randomness into 16 chars ---
    //
    // Emulate a 128-bit register where the top 80 bits hold the randomness and
    // the bottom 48 are zero. We then repeatedly take the top 5 bits and shift
    // left by 5. This mirrors the standard big-endian encoding while avoiding
    // any endianness gotchas.
    //
    //   hi64 [ r1 (64 bits)                                     ]
    //   lo64 [ r2_low16 (16 bits) | 0 (48 bits)                 ]
    //   combined 128 = (hi64 << 64) | lo64   -> top 80 bits = randomness.
    std::uint64_t hi64 = rng_();
    std::uint64_t lo64 = (rng_() & 0xFFFFULL) << 48;
    for (int i = 0; i < 16; ++i) {
        std::uint8_t chunk = static_cast<std::uint8_t>((hi64 >> 59) & 0x1F);
        out[10 + i] = kAlphabet[chunk];
        hi64 = (hi64 << 5) | (lo64 >> 59);
        lo64 <<= 5;
    }

    return out;
}

std::optional<std::int64_t> Ulid::decodeTimestamp(const std::string& id) {
    if (id.size() != 26) return std::nullopt;
    std::uint64_t ts = 0;
    for (int i = 0; i < 10; ++i) {
        auto v = decodeChar(id[i]);
        if (v < 0) return std::nullopt;
        ts = (ts << 5) | static_cast<std::uint64_t>(v);
    }
    return static_cast<std::int64_t>(ts);
}

} // namespace aegisgate
