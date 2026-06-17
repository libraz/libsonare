/// @file insert_automation_c_api_test.cpp
/// @brief Tests for the C-API that schedules sample-accurate insert-parameter
///        automation on a scene-loaded mixer's channel strips.

#if defined(SONARE_WITH_MIXING) && defined(SONARE_WITH_GRAPH)

#include <sonare/sonare_c.h>

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

#include "mixing/api/scene.h"

namespace {

// A scene with a single strip carrying a pre-fader compressor insert. The
// compressor's threshold defaults high (-3 dB) so the baseline pass barely
// compresses; lowering threshold_db (param 0) via automation forces heavy gain
// reduction, which is easy to observe at the master output.
std::string make_compressor_scene() {
  sonare::mixing::api::Scene scene;
  sonare::mixing::api::Strip strip;
  strip.id = "comp";
  sonare::mixing::api::Insert insert;
  insert.slot = sonare::mixing::api::InsertSlot::PreFader;
  insert.processor_name = "dynamics.compressor";
  // High threshold, high ratio, fast attack/release for a prompt, observable
  // gain change when the threshold is automated down.
  insert.params_json = "{\"thresholdDb\":-3.0,\"ratio\":8.0,\"attackMs\":1.0,\"releaseMs\":20.0}";
  strip.inserts.push_back(insert);
  scene.strips.push_back(strip);
  scene.buses.push_back({"master", "master"});
  scene.connections.push_back({"comp", "master"});
  return sonare::mixing::api::scene_to_json(scene);
}

// Total energy across both stereo channels of a processed block.
double block_energy(const std::vector<float>& left, const std::vector<float>& right) {
  double sum = 0.0;
  for (size_t i = 0; i < left.size(); ++i) {
    sum += static_cast<double>(left[i]) * left[i] + static_cast<double>(right[i]) * right[i];
  }
  return sum;
}

// Processes `blocks` blocks of a constant loud tone through the mixer and
// returns the summed master-output energy.
double run_mixer_energy(SonareMixer* mixer, int block, int blocks) {
  std::vector<float> in_l(static_cast<size_t>(block), 0.8f);
  std::vector<float> in_r(static_cast<size_t>(block), 0.8f);
  double energy = 0.0;
  for (int b = 0; b < blocks; ++b) {
    const float* il[] = {in_l.data()};
    const float* ir[] = {in_r.data()};
    std::vector<float> out_l(static_cast<size_t>(block), 0.0f);
    std::vector<float> out_r(static_cast<size_t>(block), 0.0f);
    REQUIRE(sonare_mixer_process_stereo(mixer, il, ir, 1, out_l.data(), out_r.data(),
                                        static_cast<size_t>(block)) == SONARE_OK);
    energy += block_energy(out_l, out_r);
  }
  return energy;
}

}  // namespace

TEST_CASE("C-API schedules insert-parameter automation on a scene-loaded mixer",
          "[mixing][automation]") {
  constexpr int kSr = 48000;
  constexpr int kBlock = 256;
  constexpr int kBlocks = 8;

  const std::string scene_json = make_compressor_scene();

  // Baseline: no automation. The compressor barely engages at -3 dB threshold,
  // so a loud tone passes through with high energy.
  SonareMixer* baseline = sonare_mixer_from_scene_json(scene_json.c_str(), kSr, kBlock);
  REQUIRE(baseline != nullptr);
  REQUIRE(sonare_mixer_strip_count(baseline) > 0);
  const double baseline_energy = run_mixer_energy(baseline, kBlock, kBlocks);
  sonare_mixer_destroy(baseline);

  // Automated: drop the compressor threshold (param 0) to -48 dB at sample 0,
  // forcing heavy gain reduction and substantially lower output energy.
  SonareMixer* automated = sonare_mixer_from_scene_json(scene_json.c_str(), kSr, kBlock);
  REQUIRE(automated != nullptr);

  SonareStrip* strip = sonare_mixer_strip_by_id(automated, "comp");
  REQUIRE(strip != nullptr);
  REQUIRE(sonare_mixer_strip_at(automated, 0) == strip);

  REQUIRE(sonare_strip_schedule_insert_automation(strip, /*insert_index=*/0, /*param_id=*/0,
                                                  /*sample_pos=*/0, /*value=*/-48.0f,
                                                  /*curve=*/0) == SONARE_OK);
  REQUIRE(sonare_mixer_compile(automated) == SONARE_OK);
  const double automated_energy = run_mixer_energy(automated, kBlock, kBlocks);
  sonare_mixer_destroy(automated);

  // The automation must take effect: heavy compression yields clearly lower
  // master energy than the un-automated baseline.
  REQUIRE(baseline_energy > 0.0);
  REQUIRE(automated_energy < 0.5 * baseline_energy);
}

TEST_CASE("C-API insert-automation handles bad arguments", "[mixing][automation]") {
  constexpr int kSr = 48000;
  constexpr int kBlock = 256;

  const std::string scene_json = make_compressor_scene();
  SonareMixer* mixer = sonare_mixer_from_scene_json(scene_json.c_str(), kSr, kBlock);
  REQUIRE(mixer != nullptr);

  const size_t count = sonare_mixer_strip_count(mixer);
  REQUIRE(count == 1);

  // Lookups that must fail.
  REQUIRE(sonare_mixer_strip_by_id(mixer, "nonexistent") == nullptr);
  REQUIRE(sonare_mixer_strip_at(mixer, count) == nullptr);

  SonareStrip* strip = sonare_mixer_strip_at(mixer, 0);
  REQUIRE(strip != nullptr);

  // NULL strip handle.
  REQUIRE(sonare_strip_schedule_insert_automation(nullptr, 0, 0, 0, 0.0f, 0) ==
          SONARE_ERROR_INVALID_PARAMETER);
  // Out-of-range insert index (only one insert on this strip).
  REQUIRE(sonare_strip_schedule_insert_automation(strip, /*insert_index=*/5, 0, 0, -24.0f, 0) ==
          SONARE_ERROR_INVALID_PARAMETER);
  // Unknown curve value.
  REQUIRE(sonare_strip_schedule_insert_automation(strip, 0, 0, 0, -24.0f, /*curve=*/99) ==
          SONARE_ERROR_INVALID_PARAMETER);
  // Outlandish parameter ids are rejected before they can become silent no-ops.
  REQUIRE(sonare_strip_schedule_insert_automation(strip, 0, 1000000u, 0, -24.0f, /*curve=*/0) ==
          SONARE_ERROR_INVALID_PARAMETER);
  // Valid linear, exponential, hold, and s-curve curves all succeed.
  REQUIRE(sonare_strip_schedule_insert_automation(strip, 0, 0, 0, -24.0f, /*curve=*/0) ==
          SONARE_OK);
  REQUIRE(sonare_strip_schedule_insert_automation(strip, 0, 0, 128, -24.0f, /*curve=*/1) ==
          SONARE_OK);
  REQUIRE(sonare_strip_schedule_insert_automation(strip, 0, 0, 256, -24.0f, /*curve=*/2) ==
          SONARE_OK);
  REQUIRE(sonare_strip_schedule_insert_automation(strip, 0, 0, 384, -24.0f, /*curve=*/3) ==
          SONARE_OK);

  sonare_mixer_destroy(mixer);
}

#endif  // SONARE_WITH_MIXING && SONARE_WITH_GRAPH
