/// @file hpss_test.cpp
/// @brief Tests for HPSS (Harmonic-Percussive Source Separation).

#include "effects/hpss.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <complex>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "util/constants.h"
#include "util/exception.h"

using namespace sonare;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

/// @brief Creates a harmonic signal (single sine wave).
Audio create_harmonic_audio(float freq = 440.0f, int sr = 22050, float duration = 0.5f) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples);

  for (int i = 0; i < n_samples; ++i) {
    float t = static_cast<float>(i) / static_cast<float>(sr);
    samples[i] = std::sin(2.0f * sonare::constants::kPiD * freq * t);
  }

  return Audio::from_vector(std::move(samples), sr);
}

/// @brief Creates a percussive signal (clicks/impulses).
Audio create_percussive_audio(int sr = 22050, float duration = 0.5f, int n_clicks = 5) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples, 0.0f);

  int click_length = sr / 500;  // 2ms click
  int interval = n_samples / n_clicks;

  for (int c = 0; c < n_clicks; ++c) {
    int start = c * interval;
    for (int i = 0; i < click_length && start + i < n_samples; ++i) {
      float envelope = 1.0f - static_cast<float>(i) / click_length;
      samples[start + i] =
          envelope * (static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX) * 2.0f - 1.0f);
    }
  }

  return Audio::from_vector(std::move(samples), sr);
}

/// @brief Creates mixed content: a decaying low kick, a sustained voiced tone
/// with harmonics, and a REVERSED cymbal (broadband noise under a rising
/// envelope).
///
/// The reversed cymbal is the part that matters here: a rising broadband swell
/// is neither horizontally smooth like a sustained tone nor vertically sparse
/// like an impulse, so a three-way split has to put it somewhere other than the
/// harmonic and percussive components.
Audio create_mixed_audio(int sr = 22050, float duration = 1.0f) {
  const int n_samples = static_cast<int>(static_cast<float>(sr) * duration);
  std::vector<float> samples(static_cast<size_t>(n_samples), 0.0f);
  const auto sr_f = static_cast<float>(sr);

  // Kick: 60 Hz with a fast exponential decay, one per half second.
  const int kick_interval = sr / 2;
  for (int start = 0; start < n_samples; start += kick_interval) {
    for (int i = 0; i < sr / 8 && start + i < n_samples; ++i) {
      const float t = static_cast<float>(i) / sr_f;
      const float env = std::exp(-24.0f * t);
      samples[static_cast<size_t>(start + i)] +=
          0.8f * env * std::sin(2.0f * sonare::constants::kPi * 60.0f * t);
    }
  }

  // Voice: a sustained fundamental plus two harmonics, slightly vibrato'd.
  for (int i = 0; i < n_samples; ++i) {
    const float t = static_cast<float>(i) / sr_f;
    const float vibrato = 1.0f + 0.01f * std::sin(2.0f * sonare::constants::kPi * 5.0f * t);
    const float f0 = 220.0f * vibrato;
    samples[static_cast<size_t>(i)] +=
        0.35f * std::sin(2.0f * sonare::constants::kPi * f0 * t) +
        0.18f * std::sin(2.0f * sonare::constants::kPi * 2.0f * f0 * t) +
        0.09f * std::sin(2.0f * sonare::constants::kPi * 3.0f * f0 * t);
  }

  // Reversed cymbal: deterministic broadband noise swelling into a cut-off.
  uint32_t state = 0x9E3779B9u;
  const int swell = std::min(n_samples, sr / 2);
  const int swell_start = std::max(0, n_samples - swell);
  for (int i = 0; i < swell; ++i) {
    state = state * 1664525u + 1013904223u;
    const float noise = static_cast<float>(state >> 8) / static_cast<float>(1u << 23) - 1.0f;
    const float env = static_cast<float>(i) / static_cast<float>(swell);
    samples[static_cast<size_t>(swell_start + i)] += 0.4f * env * env * noise;
  }

  return Audio::from_vector(std::move(samples), sr);
}

/// @brief Sum of squares of an audio buffer.
double buffer_energy(const Audio& audio) {
  double energy = 0.0;
  for (size_t i = 0; i < audio.size(); ++i) {
    const double value = audio[i];
    energy += value * value;
  }
  return energy;
}

}  // namespace

TEST_CASE("median_filter_horizontal basic", "[hpss]") {
  int n_bins = 5;
  int n_frames = 10;
  std::vector<float> input(n_bins * n_frames);

  // Create a simple pattern
  for (int k = 0; k < n_bins; ++k) {
    for (int t = 0; t < n_frames; ++t) {
      input[k * n_frames + t] = static_cast<float>(t);
    }
  }

  std::vector<float> output = median_filter_horizontal(input.data(), n_bins, n_frames, 3);

  REQUIRE(output.size() == input.size());

  // Check that median filter smooths values
  for (float val : output) {
    REQUIRE(std::isfinite(val));
  }
}

TEST_CASE("median_filter_vertical basic", "[hpss]") {
  int n_bins = 10;
  int n_frames = 5;
  std::vector<float> input(n_bins * n_frames);

  // Create a simple pattern
  for (int k = 0; k < n_bins; ++k) {
    for (int t = 0; t < n_frames; ++t) {
      input[k * n_frames + t] = static_cast<float>(k);
    }
  }

  std::vector<float> output = median_filter_vertical(input.data(), n_bins, n_frames, 3);

  REQUIRE(output.size() == input.size());

  // Check that median filter produces finite values
  for (float val : output) {
    REQUIRE(std::isfinite(val));
  }
}

TEST_CASE("hpss spectrogram basic", "[hpss]") {
  Audio audio = create_harmonic_audio();

  StftConfig stft_config;
  stft_config.n_fft = 1024;
  stft_config.hop_length = 256;

  Spectrogram spec = Spectrogram::compute(audio, stft_config);

  HpssConfig config;
  config.kernel_size_harmonic = 11;
  config.kernel_size_percussive = 11;

  HpssSpectrogramResult result = hpss(spec, config);

  REQUIRE(!result.harmonic.empty());
  REQUIRE(!result.percussive.empty());
  REQUIRE(result.harmonic.n_frames() == spec.n_frames());
  REQUIRE(result.percussive.n_frames() == spec.n_frames());
}

TEST_CASE("hpss preserves spectrogram center flag", "[hpss]") {
  Audio audio = create_harmonic_audio();

  StftConfig stft_config;
  stft_config.n_fft = 1024;
  stft_config.hop_length = 256;
  stft_config.center = false;

  Spectrogram spec = Spectrogram::compute(audio, stft_config);
  HpssSpectrogramResult result = hpss(spec);

  REQUIRE_FALSE(result.harmonic.center());
  REQUIRE_FALSE(result.percussive.center());
}

TEST_CASE("hpss preserves spectrogram win_length", "[hpss]") {
  Audio audio = create_harmonic_audio();

  StftConfig stft_config;
  stft_config.n_fft = 2048;
  stft_config.win_length = 1024;
  stft_config.hop_length = 512;

  Spectrogram spec = Spectrogram::compute(audio, stft_config);
  HpssSpectrogramResult result = hpss(spec);

  REQUIRE(result.harmonic.win_length() == stft_config.win_length);
  REQUIRE(result.percussive.win_length() == stft_config.win_length);
}

TEST_CASE("hpss audio basic", "[hpss]") {
  Audio audio = create_harmonic_audio();

  HpssConfig config;
  config.kernel_size_harmonic = 11;
  config.kernel_size_percussive = 11;

  StftConfig stft_config;
  stft_config.n_fft = 1024;
  stft_config.hop_length = 256;

  HpssAudioResult result = hpss(audio, config, stft_config);

  REQUIRE(!result.harmonic.empty());
  REQUIRE(!result.percussive.empty());

  // Output should have similar length to input
  REQUIRE_THAT(static_cast<float>(result.harmonic.size()),
               WithinRel(static_cast<float>(audio.size()), 0.1f));
}

TEST_CASE("hpss soft vs hard mask", "[hpss]") {
  Audio audio = create_harmonic_audio();

  StftConfig stft_config;
  stft_config.n_fft = 1024;
  stft_config.hop_length = 256;

  Spectrogram spec = Spectrogram::compute(audio, stft_config);

  HpssConfig soft_config;
  soft_config.use_soft_mask = true;

  HpssConfig hard_config;
  hard_config.use_soft_mask = false;

  HpssSpectrogramResult soft = hpss(spec, soft_config);
  HpssSpectrogramResult hard = hpss(spec, hard_config);

  // Both should produce valid results
  REQUIRE(!soft.harmonic.empty());
  REQUIRE(!hard.harmonic.empty());

  // Soft and hard masks should produce different results
  const float* soft_mag = soft.harmonic.magnitude().data();
  const float* hard_mag = hard.harmonic.magnitude().data();

  bool different = false;
  for (int i = 0; i < soft.harmonic.n_bins() * soft.harmonic.n_frames(); ++i) {
    if (std::abs(soft_mag[i] - hard_mag[i]) > 1e-6f) {
      different = true;
      break;
    }
  }
  REQUIRE(different);
}

TEST_CASE("harmonic helper function", "[hpss]") {
  Audio audio = create_harmonic_audio();

  Audio harm = harmonic(audio);

  REQUIRE(!harm.empty());
  REQUIRE(harm.sample_rate() == audio.sample_rate());
}

TEST_CASE("percussive helper function", "[hpss]") {
  Audio audio = create_percussive_audio();

  Audio perc = percussive(audio);

  REQUIRE(!perc.empty());
  REQUIRE(perc.sample_rate() == audio.sample_rate());
}

TEST_CASE("hpss_with_residual spectrogram", "[hpss]") {
  Audio audio = create_harmonic_audio();

  StftConfig stft_config;
  stft_config.n_fft = 1024;
  stft_config.hop_length = 256;

  Spectrogram spec = Spectrogram::compute(audio, stft_config);

  HpssConfig config;
  config.kernel_size_harmonic = 11;
  config.kernel_size_percussive = 11;

  HpssSpectrogramResultWithResidual result = hpss_with_residual(spec, config);

  REQUIRE(!result.harmonic.empty());
  REQUIRE(!result.percussive.empty());
  REQUIRE(!result.residual.empty());
  REQUIRE(result.harmonic.n_frames() == spec.n_frames());
  REQUIRE(result.percussive.n_frames() == spec.n_frames());
  REQUIRE(result.residual.n_frames() == spec.n_frames());
}

TEST_CASE("hpss_with_residual audio", "[hpss]") {
  Audio audio = create_harmonic_audio();

  HpssConfig config;
  config.kernel_size_harmonic = 11;
  config.kernel_size_percussive = 11;

  StftConfig stft_config;
  stft_config.n_fft = 1024;
  stft_config.hop_length = 256;

  HpssAudioResultWithResidual result = hpss_with_residual(audio, config, stft_config);

  REQUIRE(!result.harmonic.empty());
  REQUIRE(!result.percussive.empty());
  REQUIRE(!result.residual.empty());

  REQUIRE(result.harmonic.sample_rate() == audio.sample_rate());
  REQUIRE(result.percussive.sample_rate() == audio.sample_rate());
  REQUIRE(result.residual.sample_rate() == audio.sample_rate());

  REQUIRE(result.harmonic.size() == audio.size());
  REQUIRE(result.percussive.size() == audio.size());
  REQUIRE(result.residual.size() == audio.size());
  double signal_energy = 0.0;
  double error_energy = 0.0;
  for (size_t i = 0; i < audio.size(); ++i) {
    const double reconstructed =
        static_cast<double>(result.harmonic[i]) + result.percussive[i] + result.residual[i];
    const double original = audio[i];
    signal_energy += original * original;
    const double error = original - reconstructed;
    error_energy += error * error;
  }
  REQUIRE(error_energy / signal_energy < 1.0e-8);
}

TEST_CASE("residual helper function", "[hpss]") {
  Audio audio = create_harmonic_audio();

  Audio res = residual(audio);

  REQUIRE(!res.empty());
  REQUIRE(res.sample_rate() == audio.sample_rate());
}

TEST_CASE("residual() matches hpss_with_residual() sample for sample", "[hpss]") {
  // residual() stopped routing through the audio-level hpss_with_residual, which
  // ran three inverse transforms and discarded two. The case above only asserted
  // the result was non-empty, so the shortcut could have changed what it returns
  // with nothing to notice. Bit-identical, not close: the two paths share the
  // same spectrogram-level separation, so any difference is a defect rather than
  // a tolerance question.
  Audio audio = create_harmonic_audio();

  const Audio shortcut = residual(audio);
  const Audio full = hpss_with_residual(audio).residual;

  REQUIRE(shortcut.size() == full.size());
  REQUIRE(shortcut.sample_rate() == full.sample_rate());
  REQUIRE(shortcut.channels() == full.channels());
  // Positive control: an all-zero residual would satisfy the comparison below
  // without either path having computed anything.
  double energy = 0.0;
  for (size_t i = 0; i < shortcut.size(); ++i) {
    energy += static_cast<double>(shortcut[i]) * static_cast<double>(shortcut[i]);
  }
  REQUIRE(energy > 0.0);

  for (size_t i = 0; i < shortcut.size(); ++i) {
    REQUIRE(shortcut[i] == full[i]);
  }
}

TEST_CASE("median_filter_horizontal handles NaN/Inf without UB", "[hpss][nan]") {
  // Inject NaN and Inf into a small magnitude grid. The sliding-window median
  // path used to rely on std::lower_bound + erase with floating-point equality,
  // which is UB once NaN enters the array. After the fix, NaN/Inf are sanitized
  // and the output must be fully finite.
  int n_bins = 4;
  int n_frames = 32;
  std::vector<float> input(n_bins * n_frames, 1.0f);
  const float kNaN = std::numeric_limits<float>::quiet_NaN();
  const float kInf = std::numeric_limits<float>::infinity();

  // Sprinkle non-finite values across both boundary and middle regions.
  input[0 * n_frames + 0] = kNaN;
  input[0 * n_frames + 5] = kInf;
  input[1 * n_frames + 10] = -kInf;
  input[2 * n_frames + 15] = kNaN;
  input[3 * n_frames + n_frames - 1] = kNaN;

  std::vector<float> output = median_filter_horizontal(input.data(), n_bins, n_frames, 5);

  REQUIRE(output.size() == input.size());
  for (float v : output) {
    REQUIRE(std::isfinite(v));
  }
}

TEST_CASE("median_filter_vertical handles NaN/Inf without UB", "[hpss][nan]") {
  int n_bins = 32;
  int n_frames = 4;
  std::vector<float> input(n_bins * n_frames, 1.0f);
  const float kNaN = std::numeric_limits<float>::quiet_NaN();
  const float kInf = std::numeric_limits<float>::infinity();

  input[0 * n_frames + 0] = kNaN;
  input[7 * n_frames + 1] = kInf;
  input[15 * n_frames + 2] = -kInf;
  input[(n_bins - 1) * n_frames + 3] = kNaN;

  std::vector<float> output = median_filter_vertical(input.data(), n_bins, n_frames, 5);

  REQUIRE(output.size() == input.size());
  for (float v : output) {
    REQUIRE(std::isfinite(v));
  }
}

TEST_CASE("hpss spectrogram with NaN does not propagate NaN", "[hpss][nan]") {
  // Build a real spectrogram, then poison a few complex bins with NaN.
  // After the SlidingMedian fix, the result should be finite everywhere and
  // hpss() should not throw.
  Audio audio = create_harmonic_audio();

  StftConfig stft_config;
  stft_config.n_fft = 1024;
  stft_config.hop_length = 256;

  Spectrogram clean = Spectrogram::compute(audio, stft_config);
  int n_bins = clean.n_bins();
  int n_frames = clean.n_frames();
  REQUIRE(n_bins > 0);
  REQUIRE(n_frames > 0);

  std::vector<std::complex<float>> poisoned(clean.complex_data(),
                                            clean.complex_data() + n_bins * n_frames);
  const float kNaN = std::numeric_limits<float>::quiet_NaN();
  const float kInf = std::numeric_limits<float>::infinity();
  // Poison a handful of bins across the time axis (boundary + middle).
  poisoned[0] = std::complex<float>(kNaN, 0.0f);
  poisoned[(n_bins / 2) * n_frames + n_frames / 2] = std::complex<float>(kInf, kNaN);
  poisoned[(n_bins - 1) * n_frames + (n_frames - 1)] = std::complex<float>(0.0f, kNaN);

  Spectrogram spec = Spectrogram::from_complex(poisoned.data(), n_bins, n_frames, clean.n_fft(),
                                               clean.hop_length(), clean.sample_rate(),
                                               clean.window(), clean.center(), clean.win_length());

  HpssConfig config;
  config.kernel_size_harmonic = 11;
  config.kernel_size_percussive = 11;

  HpssSpectrogramResult result;
  REQUIRE_NOTHROW(result = hpss(spec, config));

  const std::vector<float>& h_mag = result.harmonic.magnitude();
  const std::vector<float>& p_mag = result.percussive.magnitude();
  REQUIRE(h_mag.size() == static_cast<size_t>(n_bins * n_frames));
  REQUIRE(p_mag.size() == static_cast<size_t>(n_bins * n_frames));

  // NaN may remain wherever the poisoned bin was multiplied by the mask,
  // but the median-derived masks themselves must never produce NaN. Count
  // non-finite outputs to ensure we are not catastrophically propagating.
  int nan_count = 0;
  for (float v : h_mag) {
    if (!std::isfinite(v)) ++nan_count;
  }
  for (float v : p_mag) {
    if (!std::isfinite(v)) ++nan_count;
  }
  // At most a small handful (one per poisoned bin per side) is acceptable.
  REQUIRE(nan_count <= 8);
}

TEST_CASE("hpss separates pure sine into mostly harmonic", "[hpss][separation]") {
  Audio audio = create_harmonic_audio(440.0f, 22050, 1.0f);

  HpssConfig config;
  config.kernel_size_harmonic = 31;
  config.kernel_size_percussive = 31;

  StftConfig stft_config;
  stft_config.n_fft = 2048;
  stft_config.hop_length = 512;

  Spectrogram spec = Spectrogram::compute(audio, stft_config);
  HpssSpectrogramResult result = hpss(spec, config);

  // Sum magnitudes; harmonic energy should dominate for a pure sine wave.
  const std::vector<float>& h_mag = result.harmonic.magnitude();
  const std::vector<float>& p_mag = result.percussive.magnitude();

  double h_energy = 0.0;
  double p_energy = 0.0;
  for (float v : h_mag) h_energy += static_cast<double>(v) * static_cast<double>(v);
  for (float v : p_mag) p_energy += static_cast<double>(v) * static_cast<double>(v);

  REQUIRE(h_energy > 0.0);
  // Harmonic energy should be at least a few times larger than percussive.
  REQUIRE(h_energy > 3.0 * p_energy);
}

TEST_CASE("hpss separates impulses into mostly percussive", "[hpss][separation]") {
  // Single sharp impulse: broadband, transient -> percussive should dominate.
  int sr = 22050;
  int n_samples = sr;  // 1 second
  std::vector<float> samples(n_samples, 0.0f);
  // Place a couple of broadband clicks far apart so they look transient
  // relative to the analysis frame structure.
  samples[sr / 4] = 1.0f;
  samples[sr / 2] = 1.0f;
  samples[3 * sr / 4] = 1.0f;
  Audio audio = Audio::from_vector(std::move(samples), sr);

  HpssConfig config;
  config.kernel_size_harmonic = 31;
  config.kernel_size_percussive = 31;

  StftConfig stft_config;
  stft_config.n_fft = 2048;
  stft_config.hop_length = 512;

  Spectrogram spec = Spectrogram::compute(audio, stft_config);
  HpssSpectrogramResult result = hpss(spec, config);

  const std::vector<float>& h_mag = result.harmonic.magnitude();
  const std::vector<float>& p_mag = result.percussive.magnitude();

  double h_energy = 0.0;
  double p_energy = 0.0;
  for (float v : h_mag) h_energy += static_cast<double>(v) * static_cast<double>(v);
  for (float v : p_mag) p_energy += static_cast<double>(v) * static_cast<double>(v);

  REQUIRE(p_energy > 0.0);
  // Percussive energy should clearly exceed harmonic for sparse impulses.
  REQUIRE(p_energy > h_energy);
}

// The three hpss_with_residual tests above check that `residual` is non-empty
// and correctly shaped, and the reconstruction loop cannot fail no matter what
// the residual holds: hpss() renormalizes the three masks by their own sum, so
// h + p + r == original by construction even when r is identically zero. A
// component named in the API therefore had no test that could tell a working
// output from a silent buffer. These two are that test.
// EXPECTED TO FAIL under the default (soft-mask, margin 1.0) configuration, and
// tagged so that the day it starts passing is reported as a failure rather than
// passing unnoticed.
//
// With both margins at 1.0 the soft masks are h^p/(h^p + p^p + eps) and its
// mirror, so their sum is 1 - eps/(h^p + p^p) and the residual mask is whatever
// is left: about 1e-10 before renormalization. Measured on the mixed signal
// below, the residual carries 2.2e-15 of the input energy -- six orders of
// magnitude short of "audible" and thirteen short of the 0.01 asserted here.
// Whether that is a defect in the split or the documented consequence of the
// default margins is a DSP question this test deliberately does not answer; it
// exists so the answer cannot keep being nobody's.
TEST_CASE("hpss_with_residual carries audible residual energy (soft mask)", "[hpss][!shouldfail]") {
  Audio audio = create_mixed_audio();

  StftConfig stft_config;
  stft_config.n_fft = 1024;
  stft_config.hop_length = 256;

  HpssConfig config;  // defaults: soft mask, margin 1.0

  HpssAudioResultWithResidual result = hpss_with_residual(audio, config, stft_config);

  const double signal_energy = buffer_energy(audio);
  // Non-vacuity: a silent input would make every ratio below meaningless, and
  // the harmonic / percussive ratios prove the split itself works on this
  // signal, so the residual assertion is not failing for want of content.
  REQUIRE(signal_energy > 0.0);
  REQUIRE(buffer_energy(result.harmonic) / signal_energy > 0.01);
  REQUIRE(buffer_energy(result.percussive) / signal_energy > 0.01);

  REQUIRE(buffer_energy(result.residual) / signal_energy > 0.01);
}

TEST_CASE("hpss_with_residual carries audible residual energy (hard mask)", "[hpss]") {
  Audio audio = create_mixed_audio();

  StftConfig stft_config;
  stft_config.n_fft = 1024;
  stft_config.hop_length = 256;

  HpssConfig config;
  config.use_soft_mask = false;

  HpssAudioResultWithResidual result = hpss_with_residual(audio, config, stft_config);

  const double signal_energy = buffer_energy(audio);
  REQUIRE(signal_energy > 0.0);
  REQUIRE(buffer_energy(result.harmonic) / signal_energy > 0.01);
  REQUIRE(buffer_energy(result.percussive) / signal_energy > 0.01);

  REQUIRE(buffer_energy(result.residual) / signal_energy > 0.01);
}

TEST_CASE("hpss_with_residual hard mask", "[hpss]") {
  Audio audio = create_harmonic_audio();

  StftConfig stft_config;
  stft_config.n_fft = 1024;
  stft_config.hop_length = 256;

  Spectrogram spec = Spectrogram::compute(audio, stft_config);

  HpssConfig config;
  config.use_soft_mask = false;

  HpssSpectrogramResultWithResidual result = hpss_with_residual(spec, config);

  REQUIRE(!result.harmonic.empty());
  REQUIRE(!result.percussive.empty());
  REQUIRE(!result.residual.empty());
}

// Regression for the soft-mask margin-domain bug. librosa applies the margin
// *before* the power: mask_harm = H^p / (H^p + (margin_h * P)^p), so the margin
// contributes margin^power. The previous code multiplied by the margin *after*
// the power (contributing only margin^1).
//
// With a uniform-magnitude spectrogram, both the horizontal and vertical median
// filters return that same constant, so at interior bins H == P. The harmonic
// mask there reduces to 1 / (1 + margin_h^power), which is directly observable
// as (harmonic output magnitude) / (input magnitude).
TEST_CASE("HPSS soft mask applies margin before the power (librosa parity)", "[hpss]") {
  constexpr int kBins = 9;
  constexpr int kFrames = 9;
  constexpr float kAmp = 1.0f;
  std::vector<std::complex<float>> data(static_cast<size_t>(kBins * kFrames),
                                        std::complex<float>(kAmp, 0.0f));
  Spectrogram spec =
      Spectrogram::from_complex(data.data(), kBins, kFrames, /*n_fft=*/16,
                                /*hop_length=*/8, /*sample_rate=*/22050, WindowType::Hann);

  HpssConfig config;
  config.use_soft_mask = true;
  config.power = 2.0f;
  config.margin_harmonic = 3.0f;
  config.margin_percussive = 3.0f;
  // Keep kernels small so the interior stays uniform under both median filters.
  config.kernel_size_harmonic = 3;
  config.kernel_size_percussive = 3;

  HpssSpectrogramResult result = hpss(spec, config);

  // Interior bin/frame, away from median-filter edge effects.
  const int idx = 4 * kFrames + 4;
  const float harm_mag = result.harmonic.magnitude()[static_cast<size_t>(idx)];
  const float perc_mag = result.percussive.magnitude()[static_cast<size_t>(idx)];

  // Corrected (librosa) mask: margin contributes margin^power.
  const float expected = 1.0f / (1.0f + std::pow(config.margin_harmonic, config.power));  // 1/10
  // The buggy formula applied the margin after the power -> 1 / (1 + margin).
  const float buggy = 1.0f / (1.0f + config.margin_harmonic);  // 1/4

  REQUIRE_THAT(harm_mag, WithinAbs(kAmp * expected, 1e-4f));
  REQUIRE_THAT(perc_mag, WithinAbs(kAmp * expected, 1e-4f));
  // Guard that the assertion would actually catch the old behavior.
  REQUIRE(std::abs(harm_mag - kAmp * buggy) > 0.1f);
}

// Asymmetric margins must follow the per-side margin^power law independently.
TEST_CASE("HPSS soft mask honors asymmetric margins with margin^power", "[hpss]") {
  constexpr int kBins = 9;
  constexpr int kFrames = 9;
  std::vector<std::complex<float>> data(static_cast<size_t>(kBins * kFrames),
                                        std::complex<float>(1.0f, 0.0f));
  Spectrogram spec =
      Spectrogram::from_complex(data.data(), kBins, kFrames, /*n_fft=*/16,
                                /*hop_length=*/8, /*sample_rate=*/22050, WindowType::Hann);

  HpssConfig config;
  config.use_soft_mask = true;
  config.power = 2.0f;
  config.margin_harmonic = 2.0f;
  config.margin_percussive = 4.0f;
  config.kernel_size_harmonic = 3;
  config.kernel_size_percussive = 3;

  HpssSpectrogramResult result = hpss(spec, config);

  const int idx = 4 * kFrames + 4;
  const float harm_mag = result.harmonic.magnitude()[static_cast<size_t>(idx)];
  const float perc_mag = result.percussive.magnitude()[static_cast<size_t>(idx)];

  const float expected_h = 1.0f / (1.0f + std::pow(config.margin_harmonic, config.power));  // 1/5
  const float expected_p =
      1.0f / (1.0f + std::pow(config.margin_percussive, config.power));  // 1/17

  REQUIRE_THAT(harm_mag, WithinAbs(expected_h, 1e-4f));
  REQUIRE_THAT(perc_mag, WithinAbs(expected_p, 1e-4f));
}

TEST_CASE("hpss resynthesis reconstructs the input for every analysis window", "[hpss]") {
  // The separated components are resynthesized through the iSTFT, whose
  // overlap-add divisor has to be built from the analysis window. If it is not,
  // the harmonic and percussive signals each carry a gain ripple at the hop
  // rate and their sum no longer returns the input, however exact the masking.
  constexpr int sr = 22050;
  constexpr int samples = sr / 2;
  std::vector<float> source(samples);
  for (int i = 0; i < samples; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sr);
    source[i] = 0.4f * std::sin(2.0f * static_cast<float>(constants::kPiD) * 220.0f * t);
    if (i % 2205 == 0) {
      source[i] += 0.5f;
    }
  }
  const Audio audio = Audio::from_vector(std::vector<float>(source), sr);

  const auto relative_rms_error = [&source](const std::vector<float>& sum, size_t skip) {
    double num = 0.0;
    double den = 0.0;
    for (size_t i = skip; i + skip < source.size(); ++i) {
      const double diff = static_cast<double>(source[i]) - static_cast<double>(sum[i]);
      num += diff * diff;
      den += static_cast<double>(source[i]) * static_cast<double>(source[i]);
    }
    REQUIRE(den > 0.0);
    return std::sqrt(num / den);
  };

  for (const WindowType window :
       {WindowType::Hann, WindowType::Hamming, WindowType::Blackman, WindowType::Rectangular}) {
    CAPTURE(static_cast<int>(window));
    StftConfig stft_config;
    stft_config.n_fft = 2048;
    stft_config.hop_length = 512;
    stft_config.window = window;
    const HpssConfig config;
    const size_t skip = static_cast<size_t>(stft_config.n_fft);

    SECTION("harmonic plus percussive") {
      const HpssAudioResult result = hpss(audio, config, stft_config);
      REQUIRE(result.harmonic.size() == source.size());
      REQUIRE(result.percussive.size() == source.size());
      std::vector<float> sum(source.size());
      for (size_t i = 0; i < source.size(); ++i) {
        sum[i] = result.harmonic.data()[i] + result.percussive.data()[i];
      }
      REQUIRE(relative_rms_error(sum, skip) < 1e-5);
    }

    SECTION("harmonic plus percussive plus residual") {
      const HpssAudioResultWithResidual result = hpss_with_residual(audio, config, stft_config);
      REQUIRE(result.harmonic.size() == source.size());
      std::vector<float> sum(source.size());
      for (size_t i = 0; i < source.size(); ++i) {
        sum[i] =
            result.harmonic.data()[i] + result.percussive.data()[i] + result.residual.data()[i];
      }
      REQUIRE(relative_rms_error(sum, skip) < 1e-5);
    }
  }
}

TEST_CASE("hpss rejects a hop below the half-window overlap contract", "[hpss]") {
  Audio audio = create_harmonic_audio(440.0f, 22050, 0.25f);
  HpssConfig config;
  config.kernel_size_harmonic = 7;
  config.kernel_size_percussive = 7;

  StftConfig no_overlap;
  no_overlap.n_fft = 1024;
  no_overlap.hop_length = 1024;
  REQUIRE_THROWS_AS(hpss(audio, config, no_overlap), SonareException);
  REQUIRE_THROWS_AS(hpss_with_residual(audio, config, no_overlap), SonareException);

  StftConfig sparse;
  sparse.n_fft = 512;
  sparse.hop_length = 2048;
  REQUIRE_THROWS_AS(hpss(audio, config, sparse), SonareException);
  REQUIRE_THROWS_AS(hpss_with_residual(audio, config, sparse), SonareException);

  StftConfig ok = no_overlap;
  ok.hop_length = 512;
  REQUIRE_NOTHROW(hpss(audio, config, ok));
}

TEST_CASE("hpss accepts an even n_fft that is not a power of two", "[hpss]") {
  Audio audio = create_harmonic_audio(440.0f, 22050, 0.25f);
  HpssConfig config;
  config.kernel_size_harmonic = 7;
  config.kernel_size_percussive = 7;

  StftConfig stft_config;
  stft_config.n_fft = 1500;
  stft_config.hop_length = 250;

  const HpssAudioResult result = hpss(audio, config, stft_config);
  REQUIRE(result.harmonic.size() == audio.size());
  REQUIRE(result.percussive.size() == audio.size());
  for (size_t i = 0; i < result.harmonic.size(); ++i) {
    REQUIRE(std::isfinite(result.harmonic.data()[i]));
    REQUIRE(std::isfinite(result.percussive.data()[i]));
  }
}

namespace {

/// @brief Median of a window, sanitizing non-finite entries the way the filters do.
float oracle_window_median(std::vector<float> values) {
  if (values.empty()) return 0.0f;
  for (float& v : values) {
    if (!std::isfinite(v)) v = 0.0f;
  }
  std::sort(values.begin(), values.end());
  const size_t n = values.size();
  if (n % 2 == 0) return (values[n / 2 - 1] + values[n / 2]) / 2.0f;
  return values[n / 2];
}

/// @brief Vertical median filter, recomputed from the definition for each bin.
/// @details Independent of the production shape in both directions: it walks the
///          strided column the staged form no longer does, and it collects each
///          window from scratch rather than sliding one. Both the sliding window
///          and the partial-window path reduce to the median of the bins in
///          [k - half, k + half] clipped to the grid, so the two must agree
///          exactly and a staging or indexing slip cannot hide behind a
///          tolerance.
std::vector<float> oracle_median_filter_vertical(const float* magnitude, int n_bins, int n_frames,
                                                 int kernel_size) {
  const int half = kernel_size / 2;
  std::vector<float> out(static_cast<size_t>(n_bins) * static_cast<size_t>(n_frames));
  for (int t = 0; t < n_frames; ++t) {
    for (int k = 0; k < n_bins; ++k) {
      const int start = std::max(0, k - half);
      const int end = std::min(k + half + 1, n_bins);
      std::vector<float> window;
      for (int kk = start; kk < end; ++kk) {
        window.push_back(magnitude[kk * n_frames + t]);
      }
      out[static_cast<size_t>(k) * n_frames + t] = oracle_window_median(std::move(window));
    }
  }
  return out;
}

std::vector<float> median_filter_fixture(int n_bins, int n_frames, uint32_t seed) {
  std::vector<float> m(static_cast<size_t>(n_bins) * static_cast<size_t>(n_frames));
  uint32_t state = seed;
  for (float& v : m) {
    state = state * 1664525u + 1013904223u;
    // Quarter steps keep every median -- including the even-count average of two
    // neighbours -- exactly representable.
    v = static_cast<float>((state >> 16) % 41u) * 0.25f;
  }
  return m;
}

}  // namespace

TEST_CASE("median_filter_vertical matches a per-bin definition oracle", "[hpss]") {
  // The three geometries the function branches on: a full sliding window, a grid
  // too short for one (so only the partial-window paths run), and a grid shorter
  // than the half-kernel (so the trailing path is empty and the leading one
  // covers every bin). Getting the staged column's coverage wrong shows up in
  // exactly these corners.
  struct Case {
    int n_bins;
    int n_frames;
    int kernel_size;
  };
  const Case cases[] = {
      {11, 7, 7},   // n_bins > 2 * half
      {5, 6, 7},    // half < n_bins <= 2 * half
      {2, 5, 7},    // n_bins <= half
      {9, 4, 1},    // degenerate kernel: every bin is its own median
      {33, 3, 31},  // kernel at the width the HPSS default uses
  };

  for (const Case& c : cases) {
    CAPTURE(c.n_bins, c.n_frames, c.kernel_size);
    const std::vector<float> input = median_filter_fixture(c.n_bins, c.n_frames, 0x3d19b7c5u);
    const std::vector<float> got =
        median_filter_vertical(input.data(), c.n_bins, c.n_frames, c.kernel_size);
    const std::vector<float> want =
        oracle_median_filter_vertical(input.data(), c.n_bins, c.n_frames, c.kernel_size);
    REQUIRE(got.size() == want.size());
    for (size_t i = 0; i < want.size(); ++i) {
      CAPTURE(i);
      REQUIRE(got[i] == want[i]);
    }
  }
}

TEST_CASE("median_filter_vertical matches the oracle with non-finite input", "[hpss][nan]") {
  // The staging buffer carries NaN and Inf through untouched, so the sanitizing
  // still has to happen where it did -- inside the window, not on the way in.
  const int n_bins = 11;
  const int n_frames = 6;
  std::vector<float> input = median_filter_fixture(n_bins, n_frames, 0x60c4a91fu);
  input[2 * n_frames + 3] = std::numeric_limits<float>::quiet_NaN();
  input[5 * n_frames + 1] = std::numeric_limits<float>::infinity();
  input[9 * n_frames + 4] = -std::numeric_limits<float>::infinity();

  const std::vector<float> got = median_filter_vertical(input.data(), n_bins, n_frames, 5);
  const std::vector<float> want = oracle_median_filter_vertical(input.data(), n_bins, n_frames, 5);
  REQUIRE(got.size() == want.size());
  for (size_t i = 0; i < want.size(); ++i) {
    CAPTURE(i);
    REQUIRE(got[i] == want[i]);
  }
}

TEST_CASE("percussive() matches hpss() sample for sample", "[hpss]") {
  // render_percussive_events stopped routing through the audio-level hpss, which
  // ran two inverse transforms and discarded the harmonic one. Bit-identical, not
  // close: both paths take the same mask from fill_hpss_masks and apply it to the
  // same complex spectrum, so any difference is a defect rather than a tolerance
  // question. Both mask modes are covered because they reach that mask by
  // different expressions -- the hard mask forms the harmonic one and inverts it.
  Audio audio = create_percussive_audio();

  for (const bool soft : {true, false}) {
    CAPTURE(soft);
    HpssConfig config;
    config.use_soft_mask = soft;

    const Audio shortcut = percussive(audio, config);
    const Audio full = hpss(audio, config).percussive;

    REQUIRE(shortcut.size() == full.size());
    REQUIRE(shortcut.sample_rate() == full.sample_rate());
    REQUIRE(shortcut.channels() == full.channels());
    // Positive control: an all-zero component would satisfy the comparison below
    // without either path having computed anything.
    double energy = 0.0;
    for (size_t i = 0; i < shortcut.size(); ++i) {
      energy += static_cast<double>(shortcut[i]) * static_cast<double>(shortcut[i]);
    }
    REQUIRE(energy > 0.0);

    for (size_t i = 0; i < shortcut.size(); ++i) {
      REQUIRE(shortcut[i] == full[i]);
    }
  }
}

namespace {

/// @brief True when a refusal names the kernel ceiling rather than some other
///        InvalidParameter precondition.
/// @details Both median filters guard the kernel twice with the same error code:
///          once for odd-and-positive, which carries the generic message for that
///          code, and once for the ceiling, which carries a message built from
///          @ref kMaxHpssKernelSize. Checking for the ceiling's value is what
///          separates the two, so a test cannot pass by tripping the wrong one.
bool names_kernel_ceiling(const SonareException& error) {
  return std::string(error.what()).find(std::to_string(kMaxHpssKernelSize)) != std::string::npos;
}

}  // namespace

TEST_CASE("the median filters refuse a kernel above the ceiling", "[hpss]") {
  // The guard exists because an oversized kernel reached the per-worker
  // allocations: on a bounded heap that surfaced as an allocation failure, and on
  // an overcommitting host it succeeded and ran for twenty seconds. A bare "it
  // throws" would accept the first of those, and wall time is not an assertion,
  // so the property is the error code plus the ceiling in the message.
  const int n_bins = 4;
  const int n_frames = 4;
  const std::vector<float> magnitude(static_cast<size_t>(n_bins) * n_frames, 1.0f);

  // INT_MAX - 1 is even and dies on the odd-and-positive precondition, and a
  // value past 2^32 wraps on its way through the int parameter into something
  // that precondition also rejects, so neither of those reaches this guard. Both
  // values below are odd and above the ceiling; INT_MAX is the specific hole.
  const int oversized[] = {kMaxHpssKernelSize + 1, std::numeric_limits<int>::max()};

  for (int kernel_size : oversized) {
    CAPTURE(kernel_size);
    // Each filter carries its own copy of the guard, so one direction says
    // nothing about the other.
    for (const bool vertical : {false, true}) {
      CAPTURE(vertical);
      try {
        const std::vector<float> out =
            vertical ? median_filter_vertical(magnitude.data(), n_bins, n_frames, kernel_size)
                     : median_filter_horizontal(magnitude.data(), n_bins, n_frames, kernel_size);
        static_cast<void>(out);
        FAIL("Expected the median filter to refuse a kernel above kMaxHpssKernelSize");
      } catch (const SonareException& error) {
        REQUIRE(error.code() == ErrorCode::InvalidParameter);
        REQUIRE(names_kernel_ceiling(error));
      }
    }
  }
}

TEST_CASE("the median filters accept the largest legal kernel", "[hpss]") {
  // Without this a guard that refused every kernel would satisfy the case above.
  // kMaxHpssKernelSize is 1 << 19 and therefore even, so it is not itself a legal
  // kernel size -- the largest a caller can pass is the odd value one below it,
  // and that is what the ceiling has to let through. The outputs are asserted
  // rather than just the absence of a throw, so the filter has to have run.
  const int n_bins = 2;
  const int n_frames = 2;
  const std::vector<float> magnitude{1.0f, 2.0f, 3.0f, 4.0f};
  const int largest_legal = kMaxHpssKernelSize - 1;
  REQUIRE(largest_legal % 2 == 1);

  // largest_legal follows the constant, so on its own it cannot tell a correct
  // ceiling from one quietly lowered: both move together and the case stays
  // green. This literal does not move. The ceiling was set by measuring what the
  // filters service in practice, and a kernel of half a million was two
  // milliseconds there, so a fix that narrows the ceiling past this value has
  // narrowed it past what the implementation was measured to handle.
  static_assert(kMaxHpssKernelSize > 499999, "the serviceable kernel ceiling must not narrow");

  // A window this wide spans the whole grid, so every cell is the median of its
  // entire row or column -- the average of the two, exactly representable here.
  const std::vector<float> horizontal =
      median_filter_horizontal(magnitude.data(), n_bins, n_frames, largest_legal);
  const std::vector<float> horizontal_expected{1.5f, 1.5f, 3.5f, 3.5f};
  REQUIRE(horizontal.size() == horizontal_expected.size());
  for (size_t i = 0; i < horizontal_expected.size(); ++i) {
    CAPTURE(i);
    REQUIRE(horizontal[i] == horizontal_expected[i]);
  }

  const std::vector<float> vertical =
      median_filter_vertical(magnitude.data(), n_bins, n_frames, largest_legal);
  const std::vector<float> vertical_expected{2.0f, 3.0f, 2.0f, 3.0f};
  REQUIRE(vertical.size() == vertical_expected.size());
  for (size_t i = 0; i < vertical_expected.size(); ++i) {
    CAPTURE(i);
    REQUIRE(vertical[i] == vertical_expected[i]);
  }

  // An ordinary kernel still works, so neither case above passes because the
  // filters reject everything.
  REQUIRE_NOTHROW(median_filter_horizontal(magnitude.data(), n_bins, n_frames, 3));
  REQUIRE_NOTHROW(median_filter_vertical(magnitude.data(), n_bins, n_frames, 3));
}

TEST_CASE("the kernel ceiling is checked before anything is allocated", "[hpss]") {
  // The point of the guard is to refuse before the allocations, so the ordering
  // is the property, not just the refusal. This grid's element count cannot be
  // allocated at all: if the ceiling check moved below the output buffer, the
  // refusal would come from the size check instead -- same error code, but with
  // no ceiling in its message. Neither filter reads a sample before refusing, so
  // a one-element buffer is safe to pass.
  const std::vector<float> tiny{0.0f};
  const int huge = 100000;

  for (const bool vertical : {false, true}) {
    CAPTURE(vertical);
    try {
      const std::vector<float> out =
          vertical ? median_filter_vertical(tiny.data(), huge, huge, kMaxHpssKernelSize + 1)
                   : median_filter_horizontal(tiny.data(), huge, huge, kMaxHpssKernelSize + 1);
      static_cast<void>(out);
      FAIL("Expected the median filter to refuse the kernel before sizing the output");
    } catch (const SonareException& error) {
      REQUIRE(error.code() == ErrorCode::InvalidParameter);
      REQUIRE(names_kernel_ceiling(error));
    }

    // Positive control: the same grid with a legal kernel is refused by the size
    // check, and that refusal does not name the ceiling. Without this the
    // assertion above could not tell the two guards apart.
    try {
      const std::vector<float> out = vertical
                                         ? median_filter_vertical(tiny.data(), huge, huge, 3)
                                         : median_filter_horizontal(tiny.data(), huge, huge, 3);
      static_cast<void>(out);
      FAIL("Expected the median filter to refuse an unallocatable grid");
    } catch (const SonareException& error) {
      REQUIRE(error.code() == ErrorCode::InvalidParameter);
      REQUIRE_FALSE(names_kernel_ceiling(error));
    }
  }
}

TEST_CASE("hpss refuses an oversized kernel through its public entry point", "[hpss]") {
  // Nothing clamps these on the way in: the C ABI, the Node addon and the WASM
  // binding all assign the caller's value straight into HpssConfig, so the
  // ceiling is reachable from every surface rather than only by calling the
  // filters directly. The message is checked for the filter's name because it is
  // the only observable that says which field reached which guard -- swapping
  // that wiring would leave both filters running and nothing else would notice.
  Audio audio = create_harmonic_audio(440.0f, 22050, 0.05f);
  StftConfig stft_config;
  stft_config.n_fft = 256;
  stft_config.hop_length = 64;

  for (const bool percussive_side : {false, true}) {
    CAPTURE(percussive_side);
    HpssConfig config;
    if (percussive_side) {
      config.kernel_size_percussive = kMaxHpssKernelSize + 1;
    } else {
      config.kernel_size_harmonic = kMaxHpssKernelSize + 1;
    }

    try {
      static_cast<void>(hpss(audio, config, stft_config));
      FAIL("Expected hpss to refuse a kernel above kMaxHpssKernelSize");
    } catch (const SonareException& error) {
      REQUIRE(error.code() == ErrorCode::InvalidParameter);
      REQUIRE(names_kernel_ceiling(error));
      const std::string message(error.what());
      const std::string expected_filter =
          percussive_side ? "median_filter_vertical" : "median_filter_horizontal";
      CAPTURE(message);
      REQUIRE(message.find(expected_filter) != std::string::npos);
    }
  }

  // The default configuration still separates, so the case above is not passing
  // because this entry point rejects every configuration.
  REQUIRE_NOTHROW(hpss(audio, HpssConfig(), stft_config));
}

TEST_CASE("every hpss entry point reaches the kernel ceiling", "[hpss]") {
  // The filters are called from two independent sites -- the shared mask filler
  // and the three-way split -- and each site wires kernel_size_harmonic to the
  // horizontal filter and kernel_size_percussive to the vertical one on its own.
  // Asserting one entry point says nothing about the other site's wiring, and a
  // guard restored in one filter only is exactly the regression this catches, so
  // every public entry point is checked against the filter name its field must
  // reach. Requesting a single component does not skip a filter: both run
  // whichever mask is asked for, so both fields reach the guard from every entry
  // point here.
  Audio audio = create_harmonic_audio(440.0f, 22050, 0.05f);
  StftConfig stft_config;
  stft_config.n_fft = 256;
  stft_config.hop_length = 64;
  const Spectrogram spec = Spectrogram::compute(audio, stft_config);

  struct EntryPoint {
    const char* name;
    std::function<void(const HpssConfig&)> call;
  };

  const EntryPoint entry_points[] = {
      {"hpss(spectrogram)", [&](const HpssConfig& c) { static_cast<void>(hpss(spec, c)); }},
      {"harmonic",
       [&](const HpssConfig& c) { static_cast<void>(harmonic(audio, c, stft_config)); }},
      {"percussive",
       [&](const HpssConfig& c) { static_cast<void>(percussive(audio, c, stft_config)); }},
      {"hpss_with_residual(spectrogram)",
       [&](const HpssConfig& c) { static_cast<void>(hpss_with_residual(spec, c)); }},
      {"hpss_with_residual(audio)",
       [&](const HpssConfig& c) { static_cast<void>(hpss_with_residual(audio, c, stft_config)); }},
      {"residual",
       [&](const HpssConfig& c) { static_cast<void>(residual(audio, c, stft_config)); }},
  };

  for (const EntryPoint& entry : entry_points) {
    CAPTURE(entry.name);

    for (const bool percussive_side : {false, true}) {
      CAPTURE(percussive_side);
      HpssConfig config;
      if (percussive_side) {
        config.kernel_size_percussive = kMaxHpssKernelSize + 1;
      } else {
        config.kernel_size_harmonic = kMaxHpssKernelSize + 1;
      }

      try {
        entry.call(config);
        FAIL("Expected the entry point to refuse a kernel above kMaxHpssKernelSize");
      } catch (const SonareException& error) {
        REQUIRE(error.code() == ErrorCode::InvalidParameter);
        REQUIRE(names_kernel_ceiling(error));
        const std::string message(error.what());
        const std::string expected_filter =
            percussive_side ? "median_filter_vertical" : "median_filter_horizontal";
        CAPTURE(message);
        REQUIRE(message.find(expected_filter) != std::string::npos);
      }
    }

    // The default configuration goes through, so neither refusal above passes
    // because this entry point rejects every configuration.
    REQUIRE_NOTHROW(entry.call(HpssConfig()));
  }
}

namespace {

/// @brief Scratch a median filter holds at once, for a worker count and shape.
/// @details Two kernel-wide float arrays per worker, and for the vertical filter
///          a staged column pair on top of them. Restated here from the filters'
///          own declarations rather than read back out of the policy, so this
///          does not share a source with the thing it checks.
uint64_t median_filter_scratch_bytes(int workers, int kernel_size, int staged_column_length) {
  return static_cast<uint64_t>(workers) * 2u * sizeof(float) *
         (static_cast<uint64_t>(kernel_size) + static_cast<uint64_t>(staged_column_length));
}

/// Host sizes the policy is asked about. The large entries are the point: a
/// permitted kernel there costs hundreds of megabytes, and the machine this is
/// developed on cannot reach that case by running anything.
constexpr int kHostSizes[] = {1, 2, 8, 18, 64, 128, 1024};

}  // namespace

TEST_CASE("the worker count holds the scratch product on a host of any size", "[hpss]") {
  // The kernel ceiling bounds one factor of a product, and bounding a factor is
  // not bounding a product -- the worker count carries the rest, falling as the
  // kernel grows. Asserted against a literal rather than against
  // kMaxHpssScratchBytes, because a case that reads the budget back cannot tell a
  // correct bound from one quietly raised.
  constexpr uint64_t kResidencyCeiling = 128u * 1024u * 1024u;
  const int largest_legal = kMaxHpssKernelSize - 1;
  // Bins of the largest STFT the analysis side accepts, which is the widest
  // column the vertical filter can be asked to stage.
  const int widest_staged_column = kMaxStftNFft / 2 + 1;
  const int total = 1'000'000;

  for (int host : kHostSizes) {
    CAPTURE(host);

    const int horizontal = median_filter_worker_count(total, largest_legal, 0, host);
    CAPTURE(horizontal);
    REQUIRE(horizontal >= 1);
    REQUIRE(horizontal <= host);
    REQUIRE(median_filter_scratch_bytes(horizontal, largest_legal, 0) <= kResidencyCeiling);

    const int vertical =
        median_filter_worker_count(total, largest_legal, widest_staged_column, host);
    CAPTURE(vertical);
    REQUIRE(vertical >= 1);
    REQUIRE(vertical <= host);
    REQUIRE(median_filter_scratch_bytes(vertical, largest_legal, widest_staged_column) <=
            kResidencyCeiling);
  }
}

TEST_CASE("the scratch bound leaves ordinary filter shapes every worker", "[hpss]") {
  // Without this, one worker everywhere would satisfy the case above. These are
  // the shapes HPSS runs in practice, and the bound has to be invisible at all of
  // them however large the host is.
  const int default_kernel = HpssConfig().kernel_size_harmonic;
  const int n_bins = 1025;
  const int total = 4096;

  for (int host : kHostSizes) {
    CAPTURE(host);
    REQUIRE(median_filter_worker_count(total, default_kernel, 0, host) == std::min(total, host));
    REQUIRE(median_filter_worker_count(total, default_kernel, n_bins, host) ==
            std::min(total, host));
  }

  // Never more workers than there is work, and never fewer than one.
  REQUIRE(median_filter_worker_count(3, default_kernel, 0, 128) == 3);
  REQUIRE(median_filter_worker_count(1, default_kernel, 0, 128) == 1);
  REQUIRE(median_filter_worker_count(total, default_kernel, 0, 0) == 1);
}

TEST_CASE("a filter the scratch bound slowed down still covers every row", "[hpss]") {
  // Lowering the worker count re-chunks the division, so it has to still reach
  // every row. The shape is picked so the bound genuinely binds rather than
  // returning what the host offered anyway.
  const int n_bins = 64;
  const int n_frames = 4;
  const int largest_legal = kMaxHpssKernelSize - 1;
  REQUIRE(median_filter_worker_count(n_bins, largest_legal, 0, n_bins) < n_bins);

  std::vector<float> magnitude(static_cast<size_t>(n_bins) * n_frames);
  for (int k = 0; k < n_bins; ++k) {
    for (int t = 0; t < n_frames; ++t) {
      magnitude[static_cast<size_t>(k) * n_frames + t] = static_cast<float>(k * n_frames + t);
    }
  }

  // The window spans the whole row, so every cell is its row's median: the mean
  // of the two middle entries, exactly representable here.
  const std::vector<float> filtered =
      median_filter_horizontal(magnitude.data(), n_bins, n_frames, largest_legal);
  REQUIRE(filtered.size() == magnitude.size());
  for (int k = 0; k < n_bins; ++k) {
    CAPTURE(k);
    const float expected = static_cast<float>(k * n_frames) + 1.5f;
    for (int t = 0; t < n_frames; ++t) {
      CAPTURE(t);
      REQUIRE(filtered[static_cast<size_t>(k) * n_frames + t] == expected);
    }
  }
}
