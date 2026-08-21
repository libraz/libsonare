#include "effects/reverb/room_reverb.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <vector>

#include "acoustic/late_reverb.h"
#include "acoustic/rir_synthesizer.h"
#include "util/exception.h"

using sonare::ErrorCode;
using sonare::SonareException;
using sonare::acoustic::AirAbsorption;
using sonare::effects::reverb::RoomReverb;
using sonare::effects::reverb::RoomReverbConfig;

namespace {

RoomReverbConfig valid_config() {
  RoomReverbConfig config;
  config.dims = {8.0f, 6.0f, 3.5f};
  config.source = {1.0f, 1.0f, 1.2f};
  config.listener = {5.0f, 4.0f, 1.7f};
  config.absorption = 0.15f;
  config.max_seconds = 0.5f;
  config.dry_wet = 1.0f;
  return config;
}

// Constructs and reports whether the rejection happened, so a case can assert
// both that an invalid config throws InvalidParameter and that a valid one does
// not. The constructor never calls synthesize_rir (that happens in prepare), so
// a throw here is by construction a rejection made before synthesis.
bool rejects_at_construction(const RoomReverbConfig& config) {
  try {
    RoomReverb reverb(config);
    return false;
  } catch (const SonareException& error) {
    REQUIRE(error.code() == ErrorCode::InvalidParameter);
    return true;
  }
}

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

}  // namespace

TEST_CASE("RoomReverb rejects an out-of-range RIR length cap at construction",
          "[effects][reverb][acoustic][numeric]") {
  REQUIRE_FALSE(rejects_at_construction(valid_config()));

  // 0 keeps its documented meaning (auto length from the longest RT60).
  RoomReverbConfig automatic = valid_config();
  automatic.max_seconds = 0.0f;
  REQUIRE_FALSE(rejects_at_construction(automatic));

  for (float invalid : {-1.0f, sonare::acoustic::kMaxRirSeconds + 1.0f, 1000.0f, kNaN, kInf}) {
    RoomReverbConfig config = valid_config();
    config.max_seconds = invalid;
    REQUIRE(rejects_at_construction(config));
  }
}

TEST_CASE("RoomReverb rejects non-physical air absorption at construction",
          "[effects][reverb][acoustic][numeric]") {
  for (const AirAbsorption& invalid :
       {AirAbsorption{-500.0f, 50.0f}, AirAbsorption{20.0f, 150.0f}, AirAbsorption{20.0f, -1.0f},
        AirAbsorption{kNaN, 50.0f}, AirAbsorption{20.0f, kNaN}}) {
    RoomReverbConfig config = valid_config();
    config.air_absorption_enabled = true;
    config.air = invalid;
    REQUIRE(rejects_at_construction(config));

    // The same values are inert while air absorption is disabled, so a value
    // left behind in a disabled block must not fail the whole config.
    RoomReverbConfig disabled = valid_config();
    disabled.air_absorption_enabled = false;
    disabled.air = invalid;
    REQUIRE_FALSE(rejects_at_construction(disabled));
  }
}

TEST_CASE("RoomReverb rejects a negative image-source order at construction",
          "[effects][reverb][acoustic][numeric]") {
  RoomReverbConfig config = valid_config();
  config.ism_order = -1;
  REQUIRE(rejects_at_construction(config));
}

TEST_CASE("an accepted RoomReverb is never a silent dry passthrough",
          "[effects][reverb][acoustic]") {
  // Every config the constructor accepts must synthesize a usable IR: an empty
  // IR makes ConvolutionReverb::process return early, leaving the input
  // untouched, which is the failure this engine's validation exists to prevent.
  RoomReverb reverb(valid_config());  // dry_wet = 1.0 => fully wet
  constexpr int kBlock = 256;
  reverb.prepare(48000.0, kBlock);

  std::vector<float> buffer(static_cast<size_t>(kBlock) * 8, 0.0f);
  buffer[0] = 1.0f;
  for (size_t offset = 0; offset < buffer.size(); offset += static_cast<size_t>(kBlock)) {
    float* block = buffer.data() + offset;
    reverb.process(&block, 1, kBlock);
  }

  // A dry passthrough would leave the impulse exactly where it was written; the
  // convolution path delays everything by at least one partition.
  REQUIRE(buffer[0] != 1.0f);
  double energy = 0.0;
  for (float sample : buffer) energy += static_cast<double>(sample) * sample;
  REQUIRE(energy > 0.0);
}
