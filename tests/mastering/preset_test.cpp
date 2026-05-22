#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <stdexcept>
#include <vector>

#include "mastering/api/chain.h"
#include "mastering/api/named_processor.h"
#include "mastering/api/presets.h"

using Catch::Matchers::WithinAbs;

namespace sonare::mastering::api {

TEST_CASE("preset_names returns all 6 presets", "[mastering][preset]") {
  auto names = preset_names();
  REQUIRE(names.size() == 6);
  REQUIRE(std::find(names.begin(), names.end(), "pop") != names.end());
  REQUIRE(std::find(names.begin(), names.end(), "edm") != names.end());
  REQUIRE(std::find(names.begin(), names.end(), "acoustic") != names.end());
  REQUIRE(std::find(names.begin(), names.end(), "hipHop") != names.end());
  REQUIRE(std::find(names.begin(), names.end(), "aiMusic") != names.end());
  REQUIRE(std::find(names.begin(), names.end(), "speech") != names.end());
}

TEST_CASE("preset_from_string maps known names", "[mastering][preset]") {
  REQUIRE(preset_from_string("pop") == Preset::Pop);
  REQUIRE(preset_from_string("edm") == Preset::EDM);
  REQUIRE(preset_from_string("acoustic") == Preset::Acoustic);
  REQUIRE(preset_from_string("hipHop") == Preset::HipHop);
  REQUIRE(preset_from_string("aiMusic") == Preset::AIMusic);
  REQUIRE(preset_from_string("speech") == Preset::Speech);
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
