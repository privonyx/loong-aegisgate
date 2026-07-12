#pragma once

// Phase 9.3 Epic 3 Task 3.2 — Crockford Base32 ULID generator.
//
// 128-bit identifier: [48-bit ms unix timestamp | 80-bit randomness] encoded in
// 26 Crockford Base32 characters. Lexicographic order matches chronological
// order which listConfigVersions relies on as the default sort key.
//
// Thread safety: a single Ulid instance holds a mt19937_64 and must not be
// shared across threads without external locking. ConfigServiceCore constructs
// its own instance; call sites needing to mint IDs should do the same.

#include <cstdint>
#include <optional>
#include <random>
#include <string>

namespace aegisgate {

class Ulid {
public:
    // Default constructor seeds mt19937_64 with std::random_device.
    Ulid();

    // Deterministic constructor for tests that need reproducibility.
    explicit Ulid(std::uint64_t seed);

    // Generates a new 26-char ULID using the current wall-clock ms.
    std::string generate();

    // Generates a ULID anchored at the supplied millisecond timestamp. Used by
    // tests and by callers that want to pin the timestamp portion explicitly.
    std::string generateAt(std::int64_t unix_millis);

    // Parses the first 10 characters of a ULID back into unix ms. Returns
    // std::nullopt on malformed input (wrong length, characters outside
    // Crockford Base32 alphabet).
    static std::optional<std::int64_t> decodeTimestamp(const std::string& id);

private:
    std::mt19937_64 rng_;
};

} // namespace aegisgate
