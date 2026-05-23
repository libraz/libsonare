#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <stdexcept>
#include <vector>

#include "mastering/api/chain.h"
#include "mastering/api/named_processor.h"
#include "mastering/api/presets.h"

using Catch::Matchers::WithinAbs;

namespace sonare::mastering::api {
namespace {

std::vector<float> create_preset_fixture(int sample_rate, float seconds) {
  const int count = static_cast<int>(static_cast<float>(sample_rate) * seconds);
  std::vector<float> samples(static_cast<std::size_t>(count));
  for (int index = 0; index < count; ++index) {
    const float t = static_cast<float>(index) / static_cast<float>(sample_rate);
    const float env = 0.55f + 0.35f * std::sin(2.0f * static_cast<float>(M_PI) * 1.7f * t);
    samples[static_cast<std::size_t>(index)] =
        env * (0.18f * std::sin(2.0f * static_cast<float>(M_PI) * 110.0f * t) +
               0.10f * std::sin(2.0f * static_cast<float>(M_PI) * 440.0f * t) +
               0.04f * std::sin(2.0f * static_cast<float>(M_PI) * 1760.0f * t));
  }
  return samples;
}

float mean_abs(const std::vector<float>& samples) {
  double sum = 0.0;
  for (float value : samples) {
    sum += std::abs(value);
  }
  return static_cast<float>(sum / static_cast<double>(samples.size()));
}

float rms(const std::vector<float>& samples) {
  double sum = 0.0;
  for (float value : samples) {
    sum += static_cast<double>(value) * static_cast<double>(value);
  }
  return static_cast<float>(std::sqrt(sum / static_cast<double>(samples.size())));
}

float peak_abs(const std::vector<float>& samples) {
  float peak = 0.0f;
  for (float value : samples) {
    peak = std::max(peak, std::abs(value));
  }
  return peak;
}

}  // namespace

TEST_CASE("preset_names returns all 16 presets", "[mastering][preset]") {
  auto names = preset_names();
  REQUIRE(names.size() == 16);
  const std::vector<std::string> expected = {"pop",       "edm",     "acoustic",  "hipHop",
                                             "aiMusic",   "speech",  "streaming", "youtube",
                                             "broadcast", "podcast", "audiobook", "cinema",
                                             "jpop",      "ambient", "lofi",      "classical"};
  for (const auto& name : expected) {
    REQUIRE(std::find(names.begin(), names.end(), name) != names.end());
  }
}

TEST_CASE("preset_from_string maps known names", "[mastering][preset]") {
  REQUIRE(preset_from_string("pop") == Preset::Pop);
  REQUIRE(preset_from_string("edm") == Preset::EDM);
  REQUIRE(preset_from_string("acoustic") == Preset::Acoustic);
  REQUIRE(preset_from_string("hipHop") == Preset::HipHop);
  REQUIRE(preset_from_string("aiMusic") == Preset::AIMusic);
  REQUIRE(preset_from_string("speech") == Preset::Speech);
  REQUIRE(preset_from_string("streaming") == Preset::Streaming);
  REQUIRE(preset_from_string("youtube") == Preset::YouTube);
  REQUIRE(preset_from_string("broadcast") == Preset::Broadcast);
  REQUIRE(preset_from_string("podcast") == Preset::Podcast);
  REQUIRE(preset_from_string("audiobook") == Preset::Audiobook);
  REQUIRE(preset_from_string("cinema") == Preset::Cinema);
  REQUIRE(preset_from_string("jpop") == Preset::JPop);
  REQUIRE(preset_from_string("ambient") == Preset::Ambient);
  REQUIRE(preset_from_string("lofi") == Preset::Lofi);
  REQUIRE(preset_from_string("classical") == Preset::Classical);
  REQUIRE_THROWS_AS(preset_from_string("invalid"), std::invalid_argument);
}

TEST_CASE("preset_to_string round-trips", "[mastering][preset]") {
  for (const auto& name : preset_names()) {
    Preset preset = preset_from_string(name);
    REQUIRE(std::string(preset_to_string(preset)) == name);
  }
}

TEST_CASE("preset_config(Pop) has expected enabled stages", "[mastering][preset]") {
  auto config = preset_config(Preset::Pop);
  REQUIRE(config.dynamics.compressor.enabled);
  REQUIRE(config.maximizer.true_peak_limiter.enabled);
  REQUIRE(config.loudness.enabled);
  REQUIRE_THAT(config.loudness.target_lufs, WithinAbs(-14.0f, 1e-6f));
}

TEST_CASE("preset_config(AIMusic) enables denoise and air_band", "[mastering][preset]") {
  auto config = preset_config(Preset::AIMusic);
  REQUIRE(config.repair.denoise.enabled);
  REQUIRE(config.spectral.air_band.enabled);
}

TEST_CASE("preset_config(Speech) enables denoise and loudness target=-16", "[mastering][preset]") {
  auto config = preset_config(Preset::Speech);
  REQUIRE(config.repair.denoise.enabled);
  REQUIRE(config.loudness.enabled);
  REQUIRE_THAT(config.loudness.target_lufs, WithinAbs(-16.0f, 1e-6f));
}

TEST_CASE("new platform and genre presets expose planned loudness targets", "[mastering][preset]") {
  struct ExpectedPreset {
    Preset preset;
    float lufs;
    float ceiling;
  };
  const ExpectedPreset expected[] = {
      {Preset::Streaming, -14.0f, -1.0f}, {Preset::YouTube, -14.0f, -1.0f},
      {Preset::Broadcast, -23.0f, -1.0f}, {Preset::Podcast, -16.0f, -1.5f},
      {Preset::Audiobook, -18.0f, -3.0f}, {Preset::Cinema, -27.0f, -2.0f},
      {Preset::JPop, -9.0f, -0.5f},       {Preset::Ambient, -18.0f, -1.0f},
      {Preset::Lofi, -11.0f, -1.0f},      {Preset::Classical, -23.0f, -2.0f},
  };

  for (const auto& item : expected) {
    auto config = preset_config(item.preset);
    REQUIRE(config.loudness.enabled);
    REQUIRE(config.maximizer.true_peak_limiter.enabled);
    REQUIRE_THAT(config.loudness.target_lufs, WithinAbs(item.lufs, 1e-6f));
    REQUIRE_THAT(config.loudness.ceiling_db, WithinAbs(item.ceiling, 1e-6f));
  }
}

TEST_CASE("new presets enable characteristic stages", "[mastering][preset]") {
  REQUIRE(preset_config(Preset::Podcast).dynamics.deesser.enabled);
  REQUIRE(preset_config(Preset::Audiobook).repair.declick.enabled);
  REQUIRE(preset_config(Preset::Ambient).stereo.imager.config.width > 1.0f);
  REQUIRE(preset_config(Preset::Lofi).saturation.tape.enabled);
  REQUIRE(preset_config(Preset::Classical).dynamics.compressor.config.ratio < 1.3f);
}

TEST_CASE("all 16 presets process a deterministic fixture with valid output",
          "[mastering][preset]") {
  constexpr int sample_rate = 44100;
  const auto fixture = create_preset_fixture(sample_rate, 1.25f);

  const auto names = preset_names();
  REQUIRE(names.size() == 16);
  for (const auto& name : names) {
    CAPTURE(name);
    auto result =
        master_audio_mono(preset_from_string(name), fixture.data(), fixture.size(), sample_rate);

    REQUIRE(result.samples.size() == fixture.size());
    REQUIRE_FALSE(result.stages.empty());
    REQUIRE(std::isfinite(result.input_lufs));
    REQUIRE(std::isfinite(result.output_lufs));
    REQUIRE(std::isfinite(result.applied_gain_db));

    const float output_mean_abs = mean_abs(result.samples);
    const float output_rms = rms(result.samples);
    const float output_peak = peak_abs(result.samples);
    REQUIRE(std::isfinite(output_mean_abs));
    REQUIRE(std::isfinite(output_rms));
    REQUIRE(std::isfinite(output_peak));
    REQUIRE(output_mean_abs > 1e-4f);
    REQUIRE(output_rms > 1e-4f);
    REQUIRE(output_peak <= 1.05f);

    bool all_samples_finite = true;
    for (float sample : result.samples) {
      all_samples_finite = all_samples_finite && std::isfinite(sample);
    }
    REQUIRE(all_samples_finite);
  }
}

TEST_CASE("master_audio_mono runs Pop preset on dummy signal", "[mastering][preset]") {
  std::vector<float> samples(44100, 0.1f);
  auto result = master_audio_mono(Preset::Pop, samples.data(), samples.size(), 44100);
  REQUIRE(result.samples.size() == samples.size());
  REQUIRE_FALSE(result.stages.empty());
}

TEST_CASE("master_audio_mono applies overrides", "[mastering][preset]") {
  std::vector<float> samples(44100, 0.1f);
  Param overrides[] = {{"loudness.targetLufs", -10.0}};
  REQUIRE_NOTHROW(
      master_audio_mono(Preset::Pop, samples.data(), samples.size(), 44100, overrides, 1));
}

TEST_CASE("apply_chain_config_overrides updates fields in-place", "[mastering][preset]") {
  auto config = preset_config(Preset::Pop);
  REQUIRE_THAT(config.loudness.target_lufs, WithinAbs(-14.0f, 1e-6f));

  Param overrides[] = {{"loudness.targetLufs", -10.0}};
  apply_chain_config_overrides(config, overrides, 1);

  REQUIRE_THAT(config.loudness.target_lufs, WithinAbs(-10.0f, 1e-6f));
  // Pop preset's compressor was already enabled; it should still be enabled
  // since we didn't touch the dynamics module.
  REQUIRE(config.dynamics.compressor.enabled);
}

TEST_CASE("apply_chain_config_overrides can disable a module via enabled=0",
          "[mastering][preset]") {
  auto config = preset_config(Preset::Pop);
  REQUIRE(config.dynamics.compressor.enabled);

  Param overrides[] = {{"dynamics.compressor.enabled", 0.0}};
  apply_chain_config_overrides(config, overrides, 1);

  REQUIRE_FALSE(config.dynamics.compressor.enabled);
}

TEST_CASE("master_audio_stereo runs EDM preset", "[mastering][preset]") {
  std::vector<float> left(22050, 0.1f);
  std::vector<float> right(22050, -0.1f);
  auto result = master_audio_stereo(Preset::EDM, left.data(), right.data(), left.size(), 44100);
  REQUIRE(result.left.size() == left.size());
  REQUIRE(result.right.size() == right.size());
}

TEST_CASE("preset_config(AIMusic) enables full repair trio", "[mastering][preset]") {
  auto config = preset_config(Preset::AIMusic);
  REQUIRE(config.repair.denoise.enabled);
  REQUIRE(config.repair.declick.enabled);
  REQUIRE(config.repair.dereverb.enabled);
}

TEST_CASE("preset_config(Speech) enables deesser", "[mastering][preset]") {
  auto config = preset_config(Preset::Speech);
  REQUIRE(config.dynamics.deesser.enabled);
}

TEST_CASE("preset_config(Pop) enables transient_shaper", "[mastering][preset]") {
  auto config = preset_config(Preset::Pop);
  REQUIRE(config.dynamics.transient_shaper.enabled);
  REQUIRE_THAT(config.dynamics.transient_shaper.config.attack_gain_db, WithinAbs(2.0f, 1e-6f));
}

}  // namespace sonare::mastering::api
