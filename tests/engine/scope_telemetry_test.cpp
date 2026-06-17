/// @file scope_telemetry_test.cpp
/// @brief Scope telemetry FFT band calibration: absolute band levels must be
///        independent of the host block size (the band power is normalized by
///        the coherent gain of the window actually applied, not a fixed
///        2/n_fft factor that under-read short blocks by ~20*log10(m/n_fft) dB).

#include "engine/scope_telemetry.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <utility>
#include <vector>

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kNfft = 2048;
constexpr uint32_t kBands = 32;

// Drives a full-scale tone through a freshly prepared tap at the given block
// size and returns {peak band index, peak band level dB}.
std::pair<uint32_t, float> peak_band(int block, double freq) {
  sonare::engine::ScopeTelemetryTap tap;
  tap.prepare(kSampleRate, kNfft, 16, kNfft, kBands);

  std::vector<float> buf(static_cast<size_t>(block));
  for (int i = 0; i < block; ++i) {
    buf[static_cast<size_t>(i)] =
        static_cast<float>(std::sin(2.0 * M_PI * freq * static_cast<double>(i) / kSampleRate));
  }
  float* channels[2] = {buf.data(), buf.data()};
  tap.begin_block(block, block);  // interval == block -> capture this block
  tap.process(channels, 2, block, 0, 7);

  sonare::engine::ScopeTelemetryRecord rec{};
  REQUIRE(tap.pop(rec));
  uint32_t pk = 0;
  for (uint32_t b = 1; b < rec.band_count; ++b) {
    if (rec.bands[b] > rec.bands[pk]) pk = b;
  }
  return {pk, rec.bands[pk]};
}

}  // namespace

TEST_CASE("ScopeTelemetryTap band level is correctly calibrated for a full-scale tone",
          "[engine][scope_telemetry]") {
  // Block size == FFT size so the window covers the whole frame (no
  // zero-padding/leakage-width confound): the band level then reflects the true
  // calibration. A full-scale tone recovers ~0 dBFS at its peak bin; spread over
  // a ~32-bin band by the Hann main lobe the band-averaged level lands near
  // -13 dB. The old 2/n_fft normalization (instead of 2/sum(window)) read ~6 dB
  // low here (~-19 dB), so the [-16, -10] window confirms the fix and fails the
  // regression.
  constexpr double kFreq = 3375.0;            // centered in a band (bin ~144 of 2048).
  const auto full = peak_band(kNfft, kFreq);  // m == n_fft
  REQUIRE(full.second > -16.0f);
  REQUIRE(full.second < -10.0f);

  // Coherent-gain normalization recovers the peak amplitude regardless of block
  // size, so a short block no longer collapses to the old ~12 dB systematic
  // deficit (leakage only redistributes energy across bins within the band).
  const auto short_block = peak_band(512, kFreq);
  REQUIRE(short_block.first == full.first);
  REQUIRE(short_block.second > -18.0f);
}
