/// @file phase_vocoder_test.cpp
/// @brief Phase vocoder precision and regression tests (P1-1).
/// @details Targets the synthesis-phase accumulator, which was promoted from
///          float to double in src/effects/phase_vocoder.cpp to keep long
///          pitch-shift / time-stretch runs free of monotonic phase drift.

#include "effects/phase_vocoder.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <vector>

#include "core/audio.h"
#include "core/spectrum.h"
#include "util/constants.h"

using namespace sonare;

namespace {

/// @brief Generates a pure sine wave.
Audio make_sine(float freq_hz, int sample_rate, float duration_sec) {
  const int n_samples = static_cast<int>(sample_rate * duration_sec);
  std::vector<float> samples(n_samples);
  const double w = 2.0 * sonare::constants::kPiD * static_cast<double>(freq_hz) /
                   static_cast<double>(sample_rate);
  for (int i = 0; i < n_samples; ++i) {
    samples[i] = static_cast<float>(std::sin(w * static_cast<double>(i)));
  }
  return Audio::from_vector(std::move(samples), sample_rate);
}

}  // namespace

TEST_CASE("phase_vocoder stays finite and bounded on long 10s time-stretched signal",
          "[phase_vocoder][precision]") {
  /// Long single-tone signal time-stretched to ~2x. The synthesis accumulator
  /// runs for ~860 frames (≈55k radians of total phase advance). With a float
  /// accumulator, large-magnitude wrap operations lose precision and the
  /// per-frame increment quantization can produce non-finite tails in extreme
  /// cases (denormals/NaN from spurious magnitude spikes). With a double
  /// accumulator, the whole output stays finite and within a sane magnitude
  /// bound vs the input magnitude envelope.
  const int sr = 22050;
  const float duration_sec = 10.0f;
  const float freq = 440.0f;
  Audio audio = make_sine(freq, sr, duration_sec);

  StftConfig stft_config;
  stft_config.n_fft = 2048;
  stft_config.hop_length = 512;
  stft_config.center = true;

  Spectrogram spec = Spectrogram::compute(audio, stft_config);
  REQUIRE(spec.n_frames() >= 200);

  PhaseVocoderConfig pv_config;
  pv_config.hop_length = stft_config.hop_length;

  Spectrogram out = phase_vocoder(spec, 0.5f, pv_config);  // 2x time stretch
  REQUIRE(!out.empty());
  REQUIRE(out.n_bins() == spec.n_bins());
  REQUIRE(out.n_frames() >= spec.n_frames() + 200);

  const auto& in_mag = spec.magnitude();
  const auto& out_mag = out.magnitude();

  /// Output must be finite end-to-end (no NaN/Inf from accumulator overflow).
  for (float v : out_mag) REQUIRE(std::isfinite(v));

  /// Total magnitude energy ratio should be near unity (interpolation only).
  double in_sum = 0.0;
  double out_sum = 0.0;
  for (float v : in_mag) in_sum += static_cast<double>(v);
  for (float v : out_mag) out_sum += static_cast<double>(v);
  REQUIRE(in_sum > 0.0);
  REQUIRE(out_sum / in_sum > 1.5);  // ~2x because output is ~2x longer
  REQUIRE(out_sum / in_sum < 2.5);

  /// Peak bin should remain the same (440 Hz).
  const int n_fft = stft_config.n_fft;
  const int peak_bin = static_cast<int>(
      std::round(static_cast<double>(freq) * static_cast<double>(n_fft) / static_cast<double>(sr)));
  REQUIRE(peak_bin > 0);

  /// Sample peak bin magnitude in the last 25% of frames; it should be
  /// comparable to the input peak bin magnitude (within a factor of 2).
  const int n_out = out.n_frames();
  const int late_start = (n_out * 3) / 4;
  double out_late_peak = 0.0;
  for (int t = late_start; t < n_out; ++t) {
    out_late_peak = std::max(out_late_peak, static_cast<double>(out_mag[peak_bin * n_out + t]));
  }
  const int n_in = spec.n_frames();
  double in_peak = 0.0;
  for (int t = 0; t < n_in; ++t) {
    in_peak = std::max(in_peak, static_cast<double>(in_mag[peak_bin * n_in + t]));
  }
  REQUIRE(in_peak > 0.0);
  REQUIRE(out_late_peak / in_peak > 0.5);
  REQUIRE(out_late_peak / in_peak < 2.0);
}

TEST_CASE("phase_vocoder is deterministic across repeated runs (double precision)",
          "[phase_vocoder][precision]") {
  /// Determinism is a necessary condition for correctness of the double-precision
  /// rewrite — the accumulator must not depend on stale uninitialized memory.
  /// Two independent runs on the same input must produce bit-identical output.
  const int sr = 22050;
  Audio audio = make_sine(440.0f, sr, 5.0f);

  StftConfig stft_config;
  stft_config.n_fft = 2048;
  stft_config.hop_length = 512;
  Spectrogram spec = Spectrogram::compute(audio, stft_config);

  PhaseVocoderConfig pv_config;
  pv_config.hop_length = stft_config.hop_length;

  Spectrogram out_a = phase_vocoder(spec, 1.25f, pv_config);
  Spectrogram out_b = phase_vocoder(spec, 1.25f, pv_config);

  REQUIRE(out_a.n_frames() == out_b.n_frames());
  REQUIRE(out_a.n_bins() == out_b.n_bins());

  const std::complex<float>* a = out_a.complex_data();
  const std::complex<float>* b = out_b.complex_data();
  const size_t n = static_cast<size_t>(out_a.n_frames()) * static_cast<size_t>(out_a.n_bins());
  for (size_t i = 0; i < n; ++i) {
    REQUIRE(a[i].real() == b[i].real());
    REQUIRE(a[i].imag() == b[i].imag());
  }
}

TEST_CASE("phase_vocoder_phaselocked stays finite on long 10s time-stretched signal",
          "[phase_vocoder][precision]") {
  /// Same long-run finiteness check for the phase-locked variant.
  const int sr = 22050;
  const float duration_sec = 10.0f;
  const float freq = 440.0f;
  Audio audio = make_sine(freq, sr, duration_sec);

  StftConfig stft_config;
  stft_config.n_fft = 2048;
  stft_config.hop_length = 512;
  stft_config.center = true;

  Spectrogram spec = Spectrogram::compute(audio, stft_config);
  REQUIRE(spec.n_frames() >= 200);

  PhaseVocoderConfig pv_config;
  pv_config.hop_length = stft_config.hop_length;

  Spectrogram out = phase_vocoder_phaselocked(spec, 0.5f, pv_config);
  REQUIRE(!out.empty());

  const auto& mag = out.magnitude();
  for (float v : mag) REQUIRE(std::isfinite(v));

  /// Peak bin magnitude at the end of the run must remain comparable.
  const int n_fft = stft_config.n_fft;
  const int peak_bin = static_cast<int>(
      std::round(static_cast<double>(freq) * static_cast<double>(n_fft) / static_cast<double>(sr)));
  REQUIRE(peak_bin > 0);
  const int n_out = out.n_frames();
  const int late_start = (n_out * 3) / 4;
  double out_late_peak = 0.0;
  for (int t = late_start; t < n_out; ++t) {
    out_late_peak = std::max(out_late_peak, static_cast<double>(mag[peak_bin * n_out + t]));
  }
  REQUIRE(out_late_peak > 0.0);
  REQUIRE(std::isfinite(out_late_peak));
}

TEST_CASE("phase_vocoder short-signal regression preserves frame count and magnitude",
          "[phase_vocoder][regression]") {
  /// Short signal: identity rate (1.0f) must keep frame count within ±1 and
  /// preserve the magnitude envelope. Exercises the t_out==0 initialization
  /// branch that was changed to double-precision casting.
  const int sr = 22050;
  Audio audio = make_sine(440.0f, sr, 0.5f);  // 0.5 s

  StftConfig stft_config;
  stft_config.n_fft = 1024;
  stft_config.hop_length = 256;

  Spectrogram spec = Spectrogram::compute(audio, stft_config);
  REQUIRE(spec.n_frames() >= 2);

  PhaseVocoderConfig pv_config;
  pv_config.hop_length = stft_config.hop_length;
  Spectrogram out = phase_vocoder(spec, 1.0f, pv_config);

  REQUIRE(!out.empty());
  REQUIRE(out.n_bins() == spec.n_bins());
  REQUIRE(std::abs(out.n_frames() - spec.n_frames()) <= 1);

  /// Total magnitude energy should match the input within tight tolerance:
  /// the synthesis path only resamples in time and reuses analysis magnitudes,
  /// so the ratio is dominated by interpolation, not by phase precision.
  const auto& in_mag = spec.magnitude();
  const auto& out_mag = out.magnitude();
  double in_sum = 0.0;
  double out_sum = 0.0;
  for (float v : in_mag) in_sum += static_cast<double>(v);
  for (float v : out_mag) out_sum += static_cast<double>(v);
  REQUIRE(in_sum > 0.0);
  const double ratio = out_sum / in_sum;
  REQUIRE(ratio > 0.8);
  REQUIRE(ratio < 1.2);

  for (float v : out_mag) REQUIRE(std::isfinite(v));
}

TEST_CASE("phase_vocoder short-signal rate change still produces finite output",
          "[phase_vocoder][regression]") {
  /// Time-stretch and squash on a short signal. Verifies the rate-≠-1 path
  /// (which exercises non-zero phase_diff and the in-loop accumulator update)
  /// still produces finite output under the double-precision rewrite.
  const int sr = 22050;
  Audio audio = make_sine(440.0f, sr, 0.5f);

  StftConfig stft_config;
  stft_config.n_fft = 1024;
  stft_config.hop_length = 256;
  Spectrogram spec = Spectrogram::compute(audio, stft_config);

  PhaseVocoderConfig pv_config;
  pv_config.hop_length = stft_config.hop_length;

  for (float rate : {0.5f, 1.25f, 2.0f}) {
    Spectrogram out = phase_vocoder(spec, rate, pv_config);
    REQUIRE(!out.empty());
    const auto& mag = out.magnitude();
    for (float v : mag) REQUIRE(std::isfinite(v));
  }
}
