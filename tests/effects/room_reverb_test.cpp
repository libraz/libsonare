#include "effects/reverb/room_reverb.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "acoustic/image_source.h"
#include "acoustic/late_reverb.h"
#include "acoustic/rir_synthesizer.h"
#include "acoustic/room_model.h"
#include "core/audio.h"
#include "effects/reverb/convolution_reverb.h"
#include "metering/lufs.h"
#include "util/exception.h"

using sonare::Audio;
using sonare::ErrorCode;
using sonare::SonareException;
using sonare::acoustic::AirAbsorption;
using sonare::effects::reverb::ConvolutionReverb;
using sonare::effects::reverb::ConvolutionReverbConfig;
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

constexpr int kBlock = 256;
constexpr int kLevelSampleRate = 48000;

// Deterministic white noise: a full-band excitation, so the measured wet level
// reflects the impulse response's own energy rather than the test signal's
// spectral overlap with it.
std::vector<float> noise(size_t count) {
  std::vector<float> samples(count);
  std::uint32_t state = 0x1234567u;
  for (size_t i = 0; i < count; ++i) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    samples[i] = (static_cast<float>(state) / 2147483648.0f - 1.0f) * 0.25f;
  }
  return samples;
}

template <typename Processor>
std::vector<float> render(Processor& processor, std::vector<float> buffer) {
  for (size_t offset = 0; offset + kBlock <= buffer.size(); offset += kBlock) {
    float* block = buffer.data() + offset;
    processor.process(&block, 1, kBlock);
  }
  return buffer;
}

float integrated_lufs(const std::vector<float>& samples) {
  return sonare::metering::lufs(Audio::from_vector(std::vector<float>(samples), kLevelSampleRate))
      .integrated_lufs;
}

float rms_db(const std::vector<float>& samples) {
  double energy = 0.0;
  for (float sample : samples) energy += static_cast<double>(sample) * sample;
  return 10.0f *
         std::log10(static_cast<float>(energy / static_cast<double>(samples.size())) + 1e-30f);
}

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

TEST_CASE("a RoomReverb capped below the direct arrival is still audible",
          "[effects][reverb][acoustic]") {
  // A max_seconds shorter than the source->listener flight time used to end the
  // synthesized RIR before the direct sound was written, so the insert rendered
  // digital silence -- quieter than the dry passthrough its validation exists to
  // prevent, and with no error on any surface.
  RoomReverbConfig config = valid_config();
  config.max_seconds = 0.005f;  // ~5 ms, well inside the ~15 ms direct arrival

  const sonare::acoustic::SourceListener placement{config.source, config.listener};
  const float direct_dist = length(placement.listener - placement.source);
  const int direct_sample =
      static_cast<int>(std::lround(direct_dist / sonare::acoustic::kSoundSpeed * kLevelSampleRate));
  REQUIRE(static_cast<int>(std::ceil(config.max_seconds * kLevelSampleRate)) < direct_sample);

  RoomReverb reverb(config);  // dry_wet = 1.0 => fully wet
  reverb.prepare(static_cast<double>(kLevelSampleRate), kBlock);
  REQUIRE(reverb.ir_size() > direct_sample);

  std::vector<float> buffer(static_cast<size_t>(kBlock) * 8, 0.0f);
  buffer[0] = 1.0f;
  const std::vector<float> out = render(reverb, std::move(buffer));
  double energy = 0.0;
  for (float sample : out) energy += static_cast<double>(sample) * sample;
  REQUIRE(energy > 0.0);
}

TEST_CASE("RoomReverb::prepare reports a refused synthesis instead of going inert",
          "[effects][reverb][acoustic][numeric]") {
  // The host sample rate is the one synthesis input the constructor cannot see.
  // A rate synthesis refuses returns an Error diagnostic plus an empty RIR; the
  // engine used to load that empty IR and prepare a silently inert insert.
  RoomReverb reverb(valid_config());
  REQUIRE_THROWS_AS(reverb.prepare(100.0, kBlock), SonareException);
  try {
    reverb.prepare(100.0, kBlock);
  } catch (const SonareException& error) {
    REQUIRE(error.code() == ErrorCode::InvalidParameter);
    // The refusal carries the synthesizer's own diagnostic rather than a
    // generic message, which is the point of not discarding the diagnostics.
    REQUIRE(std::string(error.what()).find("acoustic.invalid_sample_rate") != std::string::npos);
  }

  // A supported rate still prepares normally.
  REQUIRE_NOTHROW(reverb.prepare(44100.0, kBlock));
}

TEST_CASE("room reverb wet level matches the convolution reverb at the same dryWet",
          "[effects][reverb][acoustic]") {
  // One dryWet value must mean one audible mix depth across the reverb engines.
  // The geometric RIR carries a physical 1/(4*pi*d) attenuation, so loading it
  // unscaled put the room engine roughly 20 dB under its own base class and made
  // an engine swap in a mastering chain a large, undocumented level change.
  const std::vector<float> input = noise(static_cast<size_t>(kLevelSampleRate) * 2);

  RoomReverbConfig room_config = valid_config();
  room_config.max_seconds = 1.0f;
  room_config.dry_wet = 1.0f;
  RoomReverb room(room_config);
  room.prepare(static_cast<double>(kLevelSampleRate), kBlock);
  const std::vector<float> wet_room = render(room, input);

  ConvolutionReverbConfig conv_config;
  conv_config.decay_sec = 1.0f;
  conv_config.dry_wet = 1.0f;
  ConvolutionReverb convolution(conv_config);
  convolution.prepare(static_cast<double>(kLevelSampleRate), kBlock);
  const std::vector<float> wet_convolution = render(convolution, input);

  // Unit energy on both sides equalizes broadband level outright; the residual
  // LUFS difference is the K-weighted spectral tilt of a real room against
  // white decaying noise, not a gain-staging difference.
  REQUIRE(std::fabs(rms_db(wet_room) - rms_db(wet_convolution)) < 1.0f);
  REQUIRE(std::fabs(integrated_lufs(wet_room) - integrated_lufs(wet_convolution)) < 3.0f);

  // Non-vacuity: the same RIR at its physical scale -- what the engine used to
  // load -- is far outside that bound, so the assertions above are testing the
  // normalization rather than an inherent property of the two IRs.
  sonare::acoustic::ShoeboxRoom shoebox =
      sonare::acoustic::uniform_shoebox(room_config.dims, room_config.absorption);
  sonare::acoustic::RirSynthConfig synth;
  synth.ism_order = room_config.ism_order;
  synth.seed = room_config.seed;
  synth.max_seconds = room_config.max_seconds;
  const Audio physical =
      sonare::acoustic::synthesize_rir(
          shoebox, sonare::acoustic::SourceListener{room_config.source, room_config.listener},
          kLevelSampleRate, synth)
          .rir;
  ConvolutionReverb unnormalized;
  unnormalized.suppress_default_ir_synthesis();
  unnormalized.prepare(static_cast<double>(kLevelSampleRate), kBlock);
  unnormalized.load_ir(physical.data(), static_cast<int>(physical.size()));
  unnormalized.set_parameter(0, 1.0f);
  const std::vector<float> wet_physical = render(unnormalized, input);
  REQUIRE(rms_db(wet_convolution) - rms_db(wet_physical) > 10.0f);
}

TEST_CASE("the normalizing IR loader refuses an impulse response with no energy",
          "[effects][reverb][numeric]") {
  // There is no unit-energy form of silence, and convolving with it is digital
  // silence rather than the dry passthrough an empty IR gives.
  ConvolutionReverb reverb;
  reverb.prepare(static_cast<double>(kLevelSampleRate), kBlock);
  const std::vector<float> silence(1000, 0.0f);
  REQUIRE_THROWS_AS(reverb.load_ir_unit_energy(silence.data(), static_cast<int>(silence.size())),
                    SonareException);
  REQUIRE_THROWS_AS(reverb.load_ir_unit_energy(nullptr, 8), SonareException);

  // A usable IR loads and is scaled to unit energy.
  std::vector<float> impulse(1000, 0.0f);
  impulse[3] = 0.001f;
  REQUIRE_NOTHROW(reverb.load_ir_unit_energy(impulse.data(), static_cast<int>(impulse.size())));
  REQUIRE(reverb.ir_size() == static_cast<int>(impulse.size()));
}
