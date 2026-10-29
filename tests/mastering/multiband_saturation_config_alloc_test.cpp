#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <vector>

#include "mastering/multiband/multiband_saturation.h"
#include "support/alloc_guard.h"

namespace {

using sonare::mastering::multiband::MultibandSaturation;
using sonare::mastering::multiband::MultibandSaturationConfig;
using sonare::mastering::multiband::SaturationBandConfig;
using sonare::mastering::multiband::SaturationType;
using sonare::test::AllocationGuard;

std::size_t scalar_update_allocations(SaturationType type) {
  MultibandSaturationConfig config;
  config.crossover.cutoffs_hz.clear();
  config.bands = {SaturationBandConfig{6.0f, 0.5f, 0.0f, true, type}};
  MultibandSaturation processor(config);
  processor.prepare(48000.0, 64, 1);

  MultibandSaturationConfig next = config;
  const std::array<float, 3> output_gains{-1.0f, -2.0f, -3.0f};
  const std::array<float, 3> drives{6.5f, 7.0f, 7.5f};
  const std::array<float, 3> mixes{0.4f, 0.6f, 0.5f};
  AllocationGuard guard;
  for (float output_gain : output_gains) {
    next.bands[0].output_gain_db = output_gain;
    processor.set_config(next);
  }
  for (float drive : drives) {
    next.bands[0].drive_db = drive;
    processor.set_config(next);
  }
  for (float mix : mixes) {
    next.bands[0].mix = mix;
    processor.set_config(next);
  }
  return guard.count();
}

}  // namespace

TEST_CASE("Multiband saturation scalar config edits skip latency metadata allocations",
          "[mastering][multiband][config][allocation]") {
  const std::size_t tape_allocations = scalar_update_allocations(SaturationType::Tape);
  const std::size_t soft_clip_allocations = scalar_update_allocations(SaturationType::SoftClip);
  CAPTURE(tape_allocations, soft_clip_allocations);
  // Both processors take the same ConfigDiff snapshot allocations. Tape's dry/wet
  // path has fixed latency while these scalar fields change, so it should need no
  // additional metadata allocation compared with a processor without that path.
  REQUIRE(soft_clip_allocations > 0);
  REQUIRE(tape_allocations == soft_clip_allocations);
}

TEST_CASE("Multiband saturation rebuilds compensation when a band is enabled or changes type",
          "[mastering][multiband][config]") {
  MultibandSaturationConfig config;
  config.crossover.cutoffs_hz.clear();
  config.bands = {SaturationBandConfig{6.0f, 1.0f, 0.0f, true, SaturationType::Tube}};
  MultibandSaturation processor(config);
  processor.prepare(48000.0, 64, 1);
  const int tube_latency = processor.latency_samples_q8();
  REQUIRE(tube_latency > 0);

  config.bands[0].enabled = false;
  processor.set_config(config);
  REQUIRE(processor.latency_samples_q8() == 0);

  config.bands[0].enabled = true;
  config.bands[0].type = SaturationType::Tape;
  processor.set_config(config);
  REQUIRE(processor.latency_samples_q8() == 0);

  config.bands[0].type = SaturationType::Tube;
  processor.set_config(config);
  REQUIRE(processor.latency_samples_q8() == tube_latency);

  config.crossover.cutoffs_hz = {1000.0f};
  config.bands.push_back(SaturationBandConfig{6.0f, 1.0f, 0.0f, true, SaturationType::Tape});
  processor.set_config(config);
  REQUIRE(processor.parameter_descriptors().size() == 2 * MultibandSaturation::kBandStride);
  REQUIRE(processor.set_parameter(MultibandSaturation::kBandStride + 2, -3.0f));
}
