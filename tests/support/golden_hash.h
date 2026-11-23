#pragma once

/// @file golden_hash.h
/// @brief Shared FNV-1a hash over quantized audio samples for golden tests.
///
/// The quantization (clamp to [-2, 2], scale by 1e6, round with std::lrint) and
/// the FNV-1a accumulation (byte order, offset basis, prime) are reproduced
/// verbatim from the per-test copies so the emitted hash is byte-identical and
/// committed golden `*.tsv` fixtures keep matching. Do NOT "clean up" the
/// arithmetic — any change reshuffles the hash and breaks every golden.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sonare::test {

/// Which build the committed digests belong to, and whether this one is it.
///
/// Every digest a `[golden]` case compares is recorded from the macOS arm64
/// Release build `make test-golden` configures. A Debug run compares two
/// optimization levels: at -O0 the piano and vocal engine renders differ while
/// the other 126 GM programs reproduce exactly, and that subset once read as an
/// unattributed regression. A Release run on another platform compares two libm
/// and code-generation choices: the repair hum and denoise pins differ on Linux
/// x86_64 Release while macOS reproduces them. Pass this to `INFO` at the head
/// of a golden case so a mismatch says which of these it is.
///
/// RelWithDebInfo defines NDEBUG as well and is not separated here; nothing in
/// this tree runs a golden from one.
inline constexpr const char* kGoldenDigestProvenance =
#if defined(NDEBUG) && defined(__APPLE__) && defined(__aarch64__)
    "golden digests are recorded from a macOS arm64 Release build, and this run is one";
#elif defined(NDEBUG)
    "golden digests are recorded from a macOS arm64 Release build and this run is on another "
    "platform: a mismatch here compares two platforms rather than two states of the code (use make "
    "test-golden on macOS)";
#else
    "golden digests are recorded from a macOS arm64 Release build and this run is NOT a Release "
    "one: a mismatch here compares two optimization levels rather than two states of the code (use "
    "make test-golden)";
#endif

/// FNV-1a 64-bit offset basis.
inline constexpr std::uint64_t kFnvOffsetBasis = 1469598103934665603ull;

/// FNV-1a 64-bit prime.
inline constexpr std::uint64_t kFnvPrime = 1099511628211ull;

/// Quantizes one sample the way the golden hashes do: clamp to [-2, 2], scale by
/// 1e6, round to the nearest integer with std::lrint.
inline std::int32_t golden_quantize(float sample) {
  return static_cast<std::int32_t>(std::lrint(std::clamp(sample, -2.0f, 2.0f) * 1000000.0f));
}

/// Folds one quantized sample's four little-endian bytes into an FNV-1a state.
inline void golden_fnv_accumulate(std::uint64_t& hash, std::int32_t q) {
  for (int byte = 0; byte < 4; ++byte) {
    hash ^= static_cast<std::uint8_t>((static_cast<std::uint32_t>(q) >> (byte * 8)) & 0xffu);
    hash *= kFnvPrime;
  }
}

/// FNV-1a over a contiguous (mono or interleaved) sample buffer.
inline std::uint64_t fnv1a_quantized(const float* samples, std::size_t count) {
  std::uint64_t hash = kFnvOffsetBasis;
  for (std::size_t i = 0; i < count; ++i) {
    golden_fnv_accumulate(hash, golden_quantize(samples[i]));
  }
  return hash;
}

/// FNV-1a over a mono sample vector.
inline std::uint64_t fnv1a_quantized(const std::vector<float>& samples) {
  return fnv1a_quantized(samples.data(), samples.size());
}

/// FNV-1a over a stereo pair, interleaving left[i] then right[i] per frame (the
/// two channels must be the same length).
inline std::uint64_t fnv1a_quantized_stereo(const std::vector<float>& left,
                                            const std::vector<float>& right) {
  std::uint64_t hash = kFnvOffsetBasis;
  for (std::size_t i = 0; i < left.size(); ++i) {
    golden_fnv_accumulate(hash, golden_quantize(left[i]));
    golden_fnv_accumulate(hash, golden_quantize(right[i]));
  }
  return hash;
}

}  // namespace sonare::test
