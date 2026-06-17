/// @file spectral_edit_test.cpp
/// @brief Tests for region-based spectral editing (gain/attenuate/mute/heal).

#include "effects/spectral_edit.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstring>
#include <vector>

#include "core/spectrum.h"
#include "util/constants.h"
#include "util/exception.h"

using namespace sonare;

namespace {
using sonare::constants::kTwoPi;

constexpr int kSampleRate = 22050;

std::vector<float> generate_sine(int samples, float freq, float amp = 1.0f) {
  std::vector<float> out(samples);
  for (int i = 0; i < samples; ++i) {
    out[i] = amp * std::sin(kTwoPi * freq * i / kSampleRate);
  }
  return out;
}

float compute_snr(const float* original, const float* reconstructed, size_t size) {
  float signal_power = 0.0f;
  float noise_power = 0.0f;
  for (size_t i = 0; i < size; ++i) {
    signal_power += original[i] * original[i];
    const float diff = original[i] - reconstructed[i];
    noise_power += diff * diff;
  }
  if (noise_power < 1e-10f) {
    return 100.0f;
  }
  return 10.0f * std::log10(signal_power / noise_power);
}

/// @brief Sum of magnitude^2 over an inclusive Hz band across all STFT frames.
float band_energy(const Audio& audio, float low_hz, float high_hz, int n_fft = 2048,
                  int hop_length = 512) {
  StftConfig cfg;
  cfg.n_fft = n_fft;
  cfg.hop_length = hop_length;
  Spectrogram spec = Spectrogram::compute(audio, cfg);
  const int n_bins = spec.n_bins();
  const int n_frames = spec.n_frames();
  const double scale = static_cast<double>(n_fft) / kSampleRate;
  int bin_lo = std::max(0, static_cast<int>(std::floor(low_hz * scale)));
  int bin_hi = std::min(n_bins - 1, static_cast<int>(std::ceil(high_hz * scale)));
  const std::vector<float>& mag = spec.magnitude();
  float energy = 0.0f;
  for (int bin = bin_lo; bin <= bin_hi; ++bin) {
    for (int f = 0; f < n_frames; ++f) {
      const float m = mag[bin * n_frames + f];
      energy += m * m;
    }
  }
  return energy;
}

SpectralEditConfig default_config() {
  SpectralEditConfig cfg;  // n_fft=2048, hop=512, Hann, heal_radius=2
  return cfg;
}

}  // namespace

TEST_CASE("spectral_edit identity round-trip (no ops) preserves the signal", "[spectral_edit]") {
  const int samples = kSampleRate / 2;  // 0.5 s
  std::vector<float> original = generate_sine(samples, 440.0f, 0.5f);
  Audio audio = Audio::from_vector(std::vector<float>(original), kSampleRate);

  Audio out = spectral_edit(audio, default_config(), nullptr, 0);

  REQUIRE(out.size() == audio.size());
  const size_t skip = 2048;
  REQUIRE(out.size() > 2 * skip);
  const float snr = compute_snr(original.data() + skip, out.data() + skip, out.size() - 2 * skip);
  REQUIRE(snr > 20.0f);
}

TEST_CASE("spectral_edit gain_db=0 over full band is an identity edit", "[spectral_edit]") {
  const int samples = kSampleRate / 2;
  std::vector<float> original = generate_sine(samples, 440.0f, 0.5f);
  Audio audio = Audio::from_vector(std::vector<float>(original), kSampleRate);

  SpectralRegionOp op;
  op.start_sample = 0;
  op.end_sample = samples;
  op.low_hz = 0.0f;
  op.high_hz = 0.0f;  // => nyquist
  op.gain_db = 0.0f;
  op.mode = SpectralEditMode::Gain;

  Audio out = spectral_edit(audio, default_config(), &op, 1);

  const size_t skip = 2048;
  const float snr = compute_snr(original.data() + skip, out.data() + skip, out.size() - 2 * skip);
  REQUIRE(snr > 20.0f);
}

TEST_CASE("spectral_edit attenuates a frequency band by the requested amount", "[spectral_edit]") {
  const int samples = kSampleRate / 2;
  std::vector<float> mix(samples);
  std::vector<float> tone_low = generate_sine(samples, 1000.0f, 0.4f);
  std::vector<float> tone_high = generate_sine(samples, 5000.0f, 0.4f);
  for (int i = 0; i < samples; ++i) {
    mix[i] = tone_low[i] + tone_high[i];
  }
  Audio audio = Audio::from_vector(std::move(mix), kSampleRate);

  const float low_before = band_energy(audio, 800.0f, 1200.0f);
  const float high_before = band_energy(audio, 4000.0f, 6000.0f);

  SpectralRegionOp op;
  op.start_sample = 0;
  op.end_sample = samples;
  op.low_hz = 4000.0f;
  op.high_hz = 6000.0f;
  op.gain_db = -24.0f;
  op.mode = SpectralEditMode::Attenuate;

  Audio out = spectral_edit(audio, default_config(), &op, 1);

  const float low_after = band_energy(out, 800.0f, 1200.0f);
  const float high_after = band_energy(out, 4000.0f, 6000.0f);

  // 5 kHz band energy drops ~24 dB (10*log10 of mag^2 == gain_db); allow slack
  // for spectral leakage and the STFT/iSTFT roundtrip.
  const float high_drop_db = 10.0f * std::log10(high_after / high_before);
  REQUIRE(high_drop_db < -15.0f);
  REQUIRE(high_drop_db > -33.0f);

  // 1 kHz band is outside the region and stays essentially unchanged.
  const float low_ratio_db = 10.0f * std::log10(low_after / low_before);
  REQUIRE(low_ratio_db > -3.0f);
  REQUIRE(low_ratio_db < 3.0f);
}

TEST_CASE("spectral_edit mutes a frequency band", "[spectral_edit]") {
  const int samples = kSampleRate / 2;
  std::vector<float> mix(samples);
  std::vector<float> tone_low = generate_sine(samples, 1000.0f, 0.4f);
  std::vector<float> tone_high = generate_sine(samples, 5000.0f, 0.4f);
  for (int i = 0; i < samples; ++i) {
    mix[i] = tone_low[i] + tone_high[i];
  }
  Audio audio = Audio::from_vector(std::move(mix), kSampleRate);

  const float low_before = band_energy(audio, 800.0f, 1200.0f);
  const float high_before = band_energy(audio, 4000.0f, 6000.0f);

  SpectralRegionOp op;
  op.start_sample = 0;
  op.end_sample = samples;
  op.low_hz = 4000.0f;
  op.high_hz = 6000.0f;
  op.mode = SpectralEditMode::Mute;

  Audio out = spectral_edit(audio, default_config(), &op, 1);

  const float high_after = band_energy(out, 4000.0f, 6000.0f);
  const float low_after = band_energy(out, 800.0f, 1200.0f);

  // Muted band drops to a tiny fraction of its original energy.
  REQUIRE(high_after < high_before * 0.02f);
  // 1 kHz tone is preserved.
  REQUIRE(low_after > low_before * 0.5f);
}

TEST_CASE("spectral_edit heal removes a transient click", "[spectral_edit]") {
  const int samples = kSampleRate / 2;
  std::vector<float> clean = generate_sine(samples, 1000.0f, 0.3f);
  std::vector<float> clicked = clean;
  const int click_center = samples / 2;
  for (int i = click_center - 2; i <= click_center + 2; ++i) {
    clicked[i] += 0.9f;  // broadband transient
  }

  Audio clean_audio = Audio::from_vector(std::vector<float>(clean), kSampleRate);
  Audio clicked_audio = Audio::from_vector(std::vector<float>(clicked), kSampleRate);

  SpectralEditConfig cfg = default_config();
  // clean reference passed through the same STFT/iSTFT roundtrip.
  Audio ref = spectral_edit(clean_audio, cfg, nullptr, 0);
  // click passed through unmodified.
  Audio passthrough = spectral_edit(clicked_audio, cfg, nullptr, 0);

  SpectralRegionOp op;
  op.start_sample = click_center - cfg.hop_length * 3;
  op.end_sample = click_center + cfg.hop_length * 3;
  op.low_hz = 0.0f;
  op.high_hz = 0.0f;  // full band
  op.mode = SpectralEditMode::Heal;
  Audio healed = spectral_edit(clicked_audio, cfg, &op, 1);

  // Residual energy vs the clean reference, measured around the click.
  const size_t lo = static_cast<size_t>(click_center) - 1024;
  const size_t hi = static_cast<size_t>(click_center) + 1024;
  auto residual = [&](const Audio& a) {
    float e = 0.0f;
    for (size_t i = lo; i < hi; ++i) {
      const float d = a.data()[i] - ref.data()[i];
      e += d * d;
    }
    return e;
  };
  const float click_residual = residual(passthrough);
  const float healed_residual = residual(healed);

  // Heal substantially reduces the click residual relative to passthrough.
  REQUIRE(healed_residual < click_residual * 0.5f);
}

TEST_CASE("spectral_edit is deterministic (bit-identical)", "[spectral_edit]") {
  const int samples = kSampleRate / 2;
  std::vector<float> mix(samples);
  for (int i = 0; i < samples; ++i) {
    mix[i] = 0.3f * std::sin(kTwoPi * 1000.0f * i / kSampleRate) +
             0.2f * std::sin(kTwoPi * 5000.0f * i / kSampleRate);
  }
  Audio audio = Audio::from_vector(std::move(mix), kSampleRate);

  std::vector<SpectralRegionOp> ops(2);
  ops[0] = {0, samples, 4000.0f, 6000.0f, -12.0f, SpectralEditMode::Attenuate};
  ops[1] = {samples / 4, 3 * samples / 4, 0.0f, 0.0f, 0.0f, SpectralEditMode::Heal};

  Audio a = spectral_edit(audio, default_config(), ops.data(), ops.size());
  Audio b = spectral_edit(audio, default_config(), ops.data(), ops.size());

  REQUIRE(a.size() == b.size());
  REQUIRE(std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
}

TEST_CASE("spectral_edit applies ops in order (non-commutative)", "[spectral_edit]") {
  const int samples = kSampleRate / 2;
  std::vector<float> sine = generate_sine(samples, 1000.0f, 0.3f);
  Audio audio = Audio::from_vector(std::move(sine), kSampleRate);

  SpectralRegionOp heal{samples / 4, 3 * samples / 4, 0.0f, 0.0f, 0.0f, SpectralEditMode::Heal};
  SpectralRegionOp gain{samples / 4, 3 * samples / 4, 0.0f, 0.0f, 20.0f, SpectralEditMode::Gain};

  SpectralRegionOp heal_then_gain[2] = {heal, gain};
  SpectralRegionOp gain_then_heal[2] = {gain, heal};

  Audio a = spectral_edit(audio, default_config(), heal_then_gain, 2);
  Audio b = spectral_edit(audio, default_config(), gain_then_heal, 2);

  REQUIRE(a.size() == b.size());
  float max_diff = 0.0f;
  for (size_t i = 0; i < a.size(); ++i) {
    max_diff = std::max(max_diff, std::abs(a.data()[i] - b.data()[i]));
  }
  REQUIRE(max_diff > 1e-4f);  // order is honored: heal-then-gain != gain-then-heal
}

TEST_CASE("spectral_edit clamps out-of-range regions without error", "[spectral_edit]") {
  const int samples = kSampleRate / 2;
  std::vector<float> sine = generate_sine(samples, 1000.0f, 0.3f);
  Audio audio = Audio::from_vector(std::move(sine), kSampleRate);

  SpectralRegionOp op;
  op.start_sample = samples - 100;
  op.end_sample = samples + 100000;  // past end
  op.low_hz = 1000.0f;
  op.high_hz = 1000000.0f;  // above nyquist
  op.mode = SpectralEditMode::Mute;

  Audio out;
  REQUIRE_NOTHROW(out = spectral_edit(audio, default_config(), &op, 1));
  REQUIRE(out.size() == audio.size());
}

TEST_CASE("spectral_edit rejects invalid parameters", "[spectral_edit]") {
  const int samples = kSampleRate / 2;
  std::vector<float> sine = generate_sine(samples, 1000.0f, 0.3f);
  Audio audio = Audio::from_vector(std::move(sine), kSampleRate);
  SpectralRegionOp op{0, samples, 0.0f, 0.0f, 0.0f, SpectralEditMode::Mute};

  SECTION("empty audio") {
    Audio empty;
    REQUIRE_THROWS_AS(spectral_edit(empty, default_config(), nullptr, 0), SonareException);
  }
  SECTION("n_fft not a power of two") {
    SpectralEditConfig cfg = default_config();
    cfg.n_fft = 2000;
    REQUIRE_THROWS_AS(spectral_edit(audio, cfg, nullptr, 0), SonareException);
  }
  SECTION("hop_length exceeds n_fft/2") {
    SpectralEditConfig cfg = default_config();
    cfg.hop_length = cfg.n_fft;  // > n_fft/2
    REQUIRE_THROWS_AS(spectral_edit(audio, cfg, nullptr, 0), SonareException);
  }
  SECTION("null ops with non-zero count") {
    REQUIRE_THROWS_AS(spectral_edit(audio, default_config(), nullptr, 3), SonareException);
  }
  SECTION("valid call with the same audio succeeds") {
    REQUIRE_NOTHROW(spectral_edit(audio, default_config(), &op, 1));
  }
}
