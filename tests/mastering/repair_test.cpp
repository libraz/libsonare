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

TEST_CASE("Declick interpolates isolated spikes", "[mastering][repair]") {
  const auto result = declick(make_audio({0.1f, 1.0f, 0.1f}), {0.8f, 4.0f});

  REQUIRE_THAT(result[1], WithinAbs(0.1f, 0.001f));
}

TEST_CASE("Declick interpolates short click clusters", "[mastering][repair]") {
  const auto result = declick(make_audio({0.1f, 1.0f, 1.0f, 0.2f}), {0.8f, 4.0f, 4});

  REQUIRE_THAT(result[1], WithinAbs(0.133333f, 0.001f));
  REQUIRE_THAT(result[2], WithinAbs(0.166667f, 0.001f));
}

TEST_CASE("Decrackle median-filters small impulses", "[mastering][repair]") {
  const auto result = decrackle(make_audio({0.1f, 0.8f, 0.12f}), {0.2f});

  REQUIRE_THAT(result[1], WithinAbs(0.12f, 0.001f));
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

TEST_CASE("Repair helpers validate inputs", "[mastering][repair]") {
  const Audio empty;
  REQUIRE_THROWS(trim_silence(empty));
  REQUIRE_THROWS(declick(make_audio({0.0f}), {0.0f, 1.0f}));
  REQUIRE_THROWS(decrackle(make_audio({0.0f}), {0.0f}));
  REQUIRE_THROWS(declip(make_audio({0.0f}), {2.0f}));
  REQUIRE_THROWS(dehum(make_audio({0.0f}), {0.0f, 1, 10.0f}));
  DenoiseClassicalConfig bad_config{};
  bad_config.n_fft = 0;
  REQUIRE_THROWS(denoise_classical(make_audio({0.0f}), bad_config));
  REQUIRE_THROWS(dereverb_classical(make_audio({0.0f}), {0.0f, 2.0f}));
}
