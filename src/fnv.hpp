#pragma once

// FNV-1a hash helpers. Non-cryptographic, no external dependency.
// Threat model is non-adversarial; see memory/project_hash_choice.md.

#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

namespace csgify {

constexpr std::uint64_t kFnv1aOffset64 = 0xcbf29ce484222325ULL;
constexpr std::uint64_t kFnv1aPrime64  = 0x100000001b3ULL;

// FNV-1a-64. Returns a 64-bit hash of the bytes.
inline std::uint64_t fnv1a64(const void* data, std::size_t len,
                             std::uint64_t seed = kFnv1aOffset64) {
    std::uint64_t h = seed;
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= kFnv1aPrime64;
    }
    return h;
}

inline std::uint64_t fnv1a64(const std::string& s,
                             std::uint64_t seed = kFnv1aOffset64) {
    return fnv1a64(s.data(), s.size(), seed);
}

inline std::string to_hex64(std::uint64_t v) {
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << v;
    return out.str();
}

// FNV-1a-128 implemented as two FNV-1a-64 passes with distinct seeds.
// Sufficient for accidental-collision avoidance over any realistic STEP
// file; not cryptographic. Output is 32 hex chars, low half then high.
struct Fnv128 {
    std::uint64_t lo = 0;
    std::uint64_t hi = 0;
};

inline Fnv128 fnv1a128(const void* data, std::size_t len) {
    Fnv128 r;
    r.lo = fnv1a64(data, len, kFnv1aOffset64);
    // Second seed: byte-reversal of the canonical offset, just to give the
    // two halves divergent state.
    constexpr std::uint64_t kSeedHi = 0x2523224284e89cdfULL;
    r.hi = fnv1a64(data, len, kSeedHi);
    return r;
}

inline std::string fnv1a128_hex(const Fnv128& v) {
    return to_hex64(v.lo) + to_hex64(v.hi);
}

// Streaming digest of a file. Reads in chunks so large STEP files do not
// require a full in-memory copy. Returns empty string on read failure.
inline std::string fnv1a128_file_hex(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::uint64_t h_lo = kFnv1aOffset64;
    constexpr std::uint64_t kSeedHi = 0x2523224284e89cdfULL;
    std::uint64_t h_hi = kSeedHi;
    char buf[64 * 1024];
    while (in) {
        in.read(buf, sizeof(buf));
        const std::streamsize got = in.gcount();
        for (std::streamsize i = 0; i < got; ++i) {
            const auto byte = static_cast<unsigned char>(buf[i]);
            h_lo ^= byte; h_lo *= kFnv1aPrime64;
            h_hi ^= byte; h_hi *= kFnv1aPrime64;
        }
    }
    return to_hex64(h_lo) + to_hex64(h_hi);
}

} // namespace csgify
