#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <random>
#include <vector>

#include "mastering/repair/declick.h"
#include "mastering/repair/declip.h"
#include "mastering/repair/decrackle.h"
#include "mastering/repair/dehum.h"
#include "mastering/repair/denoise_classical.h"
#include "mastering/repair/dereverb_classical.h"
#include "mastering/repair/trim_silence.h"

using Catch::Matchers::WithinAbs;
using namespace sonare;
using namespace sonare::mastering::repair;

namespace {

Audio make_audio(const std::vector<float>& samples) {
  return Audio::from_buffer(samples.data(), samples.size(), 48000);
}

float rms(const Audio& audio) {
  double sum = 0.0;
  for (size_t i = 0; i < audio.size(); ++i) sum += static_cast<double>(audio[i]) * audio[i];
  return audio.empty() ? 0.0f : static_cast<float>(std::sqrt(sum / audio.size()));
}

}  // namespace

TEST_CASE("TrimSilence removes leading and trailing quiet samples", "[mastering][repair]") {
  const auto result = trim_silence(make_audio({0.0f, 0.001f, 0.2f, -0.1f, 0.0f}), {0.01f, 0});

  REQUIRE(result.size() == 2);
  REQUIRE_THAT(result[0], WithinAbs(0.2f, 0.001f));
}

TEST_CASE("TrimSilence supports LUFS-gated trimming", "[mastering][repair]") {
  std::vector<float> samples(100, 0.005f);
  samples.insert(samples.end(), 20, 0.1f);
  samples.insert(samples.end(), 100, 0.005f);

  const auto result =
      trim_silence(make_audio(samples), {0.001f, 0, TrimSilenceMode::LufsGated, -35.0f, 0.1f});

  REQUIRE(result.size() < samples.size());
  REQUIRE(result.size() >= 20);
  REQUIRE(rms(result) > 0.05f);
}

TEST_CASE("Declick interpolates isolated spikes", "[mastering][repair]") {
  const auto result = declick(make_audio({0.1f, 1.0f, 0.1f}), {0.8f, 4.0f});

  REQUIRE_THAT(result[1], WithinAbs(0.1f, 0.001f));
}

TEST_CASE("Declick interpolates short click clusters", "[mastering][repair]") {
  const auto result = declick(make_audio({0.1f, 1.0f, 1.0f, 0.2f}), {0.8f, 4.0f, 4});

  REQUIRE_THAT(result[1], WithinAbs(0.133333f, 0.001f));
  REQUIRE_THAT(result[2], WithinAbs(0.166667f, 0.001f));
}

TEST_CASE("Declick detects sub-threshold impulses with LPC residuals", "[mastering][repair]") {
  std::vector<float> samples(128, 0.0f);
  for (size_t i = 1; i < samples.size(); ++i) {
    samples[i] = 0.92f * samples[i - 1] + (i == 1 ? 0.1f : 0.0f);
  }
  samples[64] = 0.7f;

  const auto result = declick(Audio::from_vector(samples, 48000), {0.8f, 4.0f, 4, 12, 6.0f});

  REQUIRE(std::abs(result[64]) < 0.3f);
  REQUIRE(std::abs(result[63] - samples[63]) < 0.0001f);
  REQUIRE(std::abs(result[65] - samples[65]) < 0.0001f);
}

TEST_CASE("Decrackle median-filters small impulses", "[mastering][repair]") {
  const auto result = decrackle(make_audio({0.1f, 0.8f, 0.12f}), {0.2f});

  REQUIRE_THAT(result[1], WithinAbs(0.12f, 0.001f));
}

TEST_CASE("Decrackle wavelet shrinkage reduces crackle energy", "[mastering][repair]") {
  std::vector<float> samples(256, 0.0f);
  for (size_t i = 0; i < samples.size(); ++i) {
    samples[i] = 0.1f * static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * i / 64.0));
  }
  samples[32] += 0.35f;
  samples[96] -= 0.32f;
  samples[160] += 0.30f;
  const auto input = make_audio(samples);

  const auto result = decrackle(input, {0.08f, DecrackleMode::WaveletShrinkage, 4});

  REQUIRE(rms(result) < rms(input));
  REQUIRE(std::abs(result[32]) < std::abs(input[32]));
}

TEST_CASE("Declip reconstructs clipped samples from neighbors", "[mastering][repair]") {
  const auto result = declip(make_audio({0.2f, 1.0f, 0.4f}), {0.98f});

  REQUIRE_THAT(result[1], WithinAbs(0.3f, 0.001f));
}

TEST_CASE("Declip reconstructs clipped runs as a continuous segment", "[mastering][repair]") {
  const auto result = declip(make_audio({0.4f, 1.0f, 1.0f, 0.7f}), {0.98f});

  REQUIRE_THAT(result[1], WithinAbs(0.5f, 0.001f));
  REQUIRE_THAT(result[2], WithinAbs(0.6f, 0.001f));
}

TEST_CASE("Declip uses cubic context for smoother clipped run reconstruction",
          "[mastering][repair]") {
  const auto result = declip(make_audio({0.1f, 0.2f, 1.0f, 1.0f, 0.7f, 0.9f}), {0.98f});

  REQUIRE(result[2] > 0.34f);
  REQUIRE(result[2] < 0.45f);
  REQUIRE(result[3] > 0.52f);
  REQUIRE(result[3] < 0.62f);
  REQUIRE(result[2] < result[3]);
}

TEST_CASE("Declip allows reconstructed samples beyond clip threshold", "[mastering][repair]") {
  const auto result = declip(make_audio({0.0f, 0.49f, 1.0f, 0.49f, 0.0f}), {0.5f});

  REQUIRE(result[2] > 0.5f);
  REQUIRE_THAT(result[2], WithinAbs(0.55125f, 0.0001f));
}

TEST_CASE("Declip uses LPC reconstruction when enough context is available",
          "[mastering][repair]") {
  std::vector<float> samples(96, 0.0f);
  samples[0] = 1.0f;
  for (size_t i = 1; i < samples.size(); ++i) samples[i] = 0.95f * samples[i - 1];
  samples[20] = 1.0f;
  samples[21] = 1.0f;
  samples[22] = 1.0f;

  const auto result = declip(Audio::from_vector(samples, 48000), {0.9f, 12, 2});

  REQUIRE(std::isfinite(result[20]));
  REQUIRE(std::isfinite(result[21]));
  REQUIRE(std::isfinite(result[22]));
  REQUIRE(result[20] > result[21]);
  REQUIRE(result[21] > result[22]);
  REQUIRE(result[20] < 0.9f);
}

namespace {

// Generate a sine wave + low-amplitude noise, then hard-clip at +/- threshold.
// Returns the original (unclipped) and clipped signals separately so tests can
// measure SDR improvements without re-deriving the reference.
struct ClippedFixture {
  std::vector<float> original;
  std::vector<float> clipped;
};

ClippedFixture make_clipped_sine(size_t n, float freq_hz, float amp, float sample_rate,
                                 float clip_thresh, uint32_t seed) {
  ClippedFixture fx;
  fx.original.resize(n);
  fx.clipped.resize(n);
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> noise(-0.005f, 0.005f);
  constexpr double kPi = 3.14159265358979323846;
  for (size_t i = 0; i < n; ++i) {
    const float t = static_cast<float>(i) / sample_rate;
    const float x = amp * static_cast<float>(std::sin(2.0 * kPi * freq_hz * t)) + noise(rng);
    fx.original[i] = x;
    fx.clipped[i] = std::clamp(x, -clip_thresh, clip_thresh);
  }
  return fx;
}

}  // namespace

TEST_CASE("Declip preserves all unclipped samples exactly", "[mastering][repair]") {
  // Regression test for the bug where Burg was retrained on filled-in samples:
  // even after the LPC step, samples outside any clipped region must remain
  // bit-identical to the input.
  const auto fx = make_clipped_sine(2048, 700.0f, 0.92f, 48000.0f, 0.5f, 0xFACE);
  const auto clipped_audio = Audio::from_buffer(fx.clipped.data(), fx.clipped.size(), 48000);
  const auto result = declip(clipped_audio, {0.5f, 24, 2, 1.0f});

  REQUIRE(result.size() == fx.clipped.size());
  for (size_t i = 0; i < fx.clipped.size(); ++i) {
    if (std::abs(fx.clipped[i]) < 0.5f) {
      REQUIRE(result[i] == fx.clipped[i]);
    }
  }
}

TEST_CASE("Dehum notch filter reduces fundamental tone", "[mastering][repair]") {
  std::vector<float> samples(4800);
  for (size_t i = 0; i < samples.size(); ++i) {
    samples[i] = 0.5f * static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * 50.0 *
                                                    static_cast<double>(i) / 48000.0));
  }
  const auto input = make_audio(samples);
  const auto result = dehum(input, {50.0f, 1, 10.0f});

  REQUIRE(rms(result) < rms(input));
}

TEST_CASE("Dehum adaptive notch follows drifting fundamental", "[mastering][repair]") {
  std::vector<float> samples(8192);
  for (size_t i = 0; i < samples.size(); ++i) {
    const float hz = 49.0f + 2.0f * static_cast<float>(i) / static_cast<float>(samples.size());
    samples[i] = 0.35f * static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * hz *
                                                     static_cast<double>(i) / 48000.0));
  }
  const auto input = make_audio(samples);
  const auto result = dehum(input, {50.0f, 1, 6.0f, true, 2.5f, 0.8f, 1024});

  REQUIRE(rms(result) < rms(input) * 0.8f);
}

namespace {
Audio noisy_tone(int sample_rate, int samples, float tone_freq, float tone_amp, float noise_amp,
                 uint32_t seed) {
  constexpr double kPi = 3.14159265358979323846;
  std::vector<float> data(static_cast<size_t>(samples));
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> noise(-noise_amp, noise_amp);
  for (int i = 0; i < samples; ++i) {
    const double t = static_cast<double>(i) / sample_rate;
    data[static_cast<size_t>(i)] =
        static_cast<float>(tone_amp * std::sin(2.0 * kPi * tone_freq * t)) + noise(rng);
  }
  return Audio::from_vector(std::move(data), sample_rate);
}

float high_frequency_residual_rms(const Audio& a) {
  // First-difference RMS is dominated by broadband noise content; it is a
  // proxy for "how noisy this signal sounds" that is largely insensitive to
  // the low-frequency tone.
  double sum = 0.0;
  for (size_t i = 1; i < a.size(); ++i) {
    const float diff = a[i] - a[i - 1];
    sum += static_cast<double>(diff) * diff;
  }
  return static_cast<float>(std::sqrt(sum / static_cast<double>(a.size())));
}
}  // namespace

TEST_CASE("DenoiseClassical LogMmse reduces broadband noise", "[mastering][repair]") {
  const Audio input = noisy_tone(48000, 48000, 1000.0f, 0.5f, 0.05f, 12345);
  DenoiseClassicalConfig config{};
  config.mode = DenoiseMode::LogMmse;
  const Audio output = denoise_classical(input, config);

  REQUIRE(output.size() == input.size());
  REQUIRE(high_frequency_residual_rms(output) < high_frequency_residual_rms(input) * 0.85f);
}

TEST_CASE("DenoiseClassical MmseStsa reduces broadband noise", "[mastering][repair]") {
  const Audio input = noisy_tone(48000, 48000, 1000.0f, 0.5f, 0.05f, 23456);
  DenoiseClassicalConfig config{};
  config.mode = DenoiseMode::MmseStsa;
  const Audio output = denoise_classical(input, config);

  REQUIRE(high_frequency_residual_rms(output) < high_frequency_residual_rms(input) * 0.85f);
}

TEST_CASE("DenoiseClassical SpectralSubtraction reduces broadband noise", "[mastering][repair]") {
  const Audio input = noisy_tone(48000, 48000, 1000.0f, 0.5f, 0.05f, 34567);
  DenoiseClassicalConfig config{};
  config.mode = DenoiseMode::SpectralSubtraction;
  const Audio output = denoise_classical(input, config);

  REQUIRE(high_frequency_residual_rms(output) < high_frequency_residual_rms(input));
}

TEST_CASE("DenoiseClassical can use IMCRA frame-adaptive noise tracking", "[mastering][repair]") {
  const Audio input = noisy_tone(48000, 48000, 1000.0f, 0.45f, 0.04f, 45678);
  DenoiseClassicalConfig config{};
  config.mode = DenoiseMode::LogMmse;
  config.noise_estimator = DenoiseNoiseEstimator::Imcra;
  const Audio output = denoise_classical(input, config);

  REQUIRE(output.size() == input.size());
  REQUIRE(high_frequency_residual_rms(output) < high_frequency_residual_rms(input) * 0.9f);

  config.noise_estimator = DenoiseNoiseEstimator::Mcra;
  const Audio mcra_output = denoise_classical(input, config);
  REQUIRE(mcra_output.size() == input.size());
}

TEST_CASE("DenoiseClassical returns short inputs unchanged", "[mastering][repair]") {
  const auto result = denoise_classical(make_audio({0.03f, 0.05f}));
  REQUIRE(result.size() == 2);
  REQUIRE_THAT(result[0], WithinAbs(0.03f, 0.001f));
  REQUIRE_THAT(result[1], WithinAbs(0.05f, 0.001f));
}

TEST_CASE("DereverbClassical attenuates low-level tails", "[mastering][repair]") {
  const auto result = dereverb_classical(make_audio({0.5f, 0.04f, 0.02f}), {0.05f, 0.25f});

  REQUIRE_THAT(result[0], WithinAbs(0.5f, 0.001f));
  REQUIRE_THAT(result[1], WithinAbs(0.01f, 0.001f));
  REQUIRE_THAT(result[2], WithinAbs(0.005f, 0.001f));
}

TEST_CASE("DereverbClassical spectral subtraction reduces late decay", "[mastering][repair]") {
  std::vector<float> samples(48000, 0.0f);
  samples[0] = 1.0f;
  for (size_t i = 1; i < samples.size(); ++i) {
    samples[i] =
        0.4f * std::exp(-static_cast<float>(i) / 8000.0f) *
        std::sin(2.0f * 3.14159265358979323846f * 1000.0f * static_cast<float>(i) / 48000.0f);
  }
  const Audio input = Audio::from_vector(samples, 48000);
  const auto output =
      dereverb_classical(input, {0.05f, 0.5f, 1024, 256, 0.25f, 20.0f, 2.0f, 0.02f});

  REQUIRE(output.size() == input.size());
  double in_tail = 0.0;
  double out_tail = 0.0;
  for (size_t i = 12000; i < input.size(); ++i) {
    in_tail += static_cast<double>(input[i]) * input[i];
    out_tail += static_cast<double>(output[i]) * output[i];
  }
  REQUIRE(out_tail < in_tail * 0.8);
}

TEST_CASE("DereverbClassical WPE mode further suppresses predictable late reverb",
          "[mastering][repair]") {
  std::vector<float> samples(48000, 0.0f);
  for (size_t i = 0; i < samples.size(); ++i) {
    const float direct = 0.4f * static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * 700.0 *
                                                            static_cast<double>(i) / 48000.0));
    const float late = i >= 2400 ? 0.25f * samples[i - 2400] : 0.0f;
    samples[i] = direct + late;
  }
  const Audio input = Audio::from_vector(samples, 48000);
  const auto spectral =
      dereverb_classical(input, {0.05f, 0.5f, 1024, 256, 0.4f, 50.0f, 0.8f, 0.05f});
  const auto wpe = dereverb_classical(
      input, {0.05f, 0.5f, 1024, 256, 0.4f, 50.0f, 0.8f, 0.05f, true, 2, 3, 0.5f});

  REQUIRE(wpe.size() == input.size());
  REQUIRE(rms(wpe) < rms(spectral));
}

TEST_CASE("Repair helpers validate inputs", "[mastering][repair]") {
  const Audio empty;
  REQUIRE_THROWS(trim_silence(empty));
  REQUIRE_THROWS(trim_silence(make_audio({0.0f}), {0.0f, 0, TrimSilenceMode::Peak, -60.0f, 0.0f}));
  REQUIRE_THROWS(declick(make_audio({0.0f}), {0.0f, 1.0f}));
  REQUIRE_THROWS(declick(make_audio({0.0f}), {0.8f, 1.0f, 1, -1, 8.0f}));
  REQUIRE_THROWS(decrackle(make_audio({0.0f}), {0.0f}));
  REQUIRE_THROWS(decrackle(make_audio({0.0f}), {0.1f, DecrackleMode::WaveletShrinkage, 0}));
  REQUIRE_THROWS(declip(make_audio({0.0f}), {2.0f}));
  REQUIRE_THROWS(declip(make_audio({0.0f}), {0.98f, -1, 2}));
  REQUIRE_THROWS(dereverb_classical(make_audio({0.0f, 0.1f}), {0.05f, 0.5f, 1024, 256, 0.4f, 50.0f,
                                                               0.8f, 0.05f, true, 0, 3, 0.5f}));
  REQUIRE_THROWS(dehum(make_audio({0.0f}), {0.0f, 1, 10.0f}));
  DenoiseClassicalConfig bad_config{};
  bad_config.n_fft = 0;
  REQUIRE_THROWS(denoise_classical(make_audio({0.0f}), bad_config));
  REQUIRE_THROWS(dereverb_classical(make_audio({0.0f}), {0.0f, 2.0f}));
  REQUIRE_THROWS(dereverb_classical(make_audio({0.0f}), {0.0f, 0.5f, 1000}));
}
