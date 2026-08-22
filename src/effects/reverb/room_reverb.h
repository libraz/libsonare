#pragma once

/// @file room_reverb.h
/// @brief Geometry-driven convolution reverb (the 5th reverb engine): synthesizes
///        a room impulse response from shoebox dimensions and uniform wall
///        absorption, then convolves the signal with it.

#include "acoustic/geometry.h"
#include "acoustic/late_reverb.h"  // AirAbsorption
#include "acoustic/room_types.h"
#include "effects/reverb/convolution_reverb.h"

namespace sonare::effects::reverb {

/// @brief Configuration for the geometry-driven room reverb.
///
/// Walls share one broadband absorption coefficient (the scalar-parameter insert
/// path); per-wall frequency-dependent materials are reachable through the
/// struct/binding API, not this engine's JSON params.
struct RoomReverbConfig {
  sonare::RoomDimensions dims{7.0f, 5.0f, 3.0f};  ///< room size (m)
  sonare::acoustic::Vec3 source{1.0f, 1.0f, 1.2f};
  sonare::acoustic::Vec3 listener{5.0f, 4.0f, 1.7f};
  float absorption = 0.2f;   ///< uniform wall absorption, clamped to [0, 1)
  int ism_order = 3;         ///< image-source order for early reflections
  unsigned seed = 1u;        ///< deterministic late-tail noise seed
  float max_seconds = 0.0f;  ///< RIR length cap (s); 0 = auto from the RT60
  float dry_wet = 0.35f;     ///< wet mix (send-style default)
  /// Disabled by default so an existing config's RIR is unchanged byte for
  /// byte. When enabled, the late tail's per-band RT60 gains the ISO 9613-1
  /// atmospheric-absorption term (see acoustic::shoebox_reverb_time); `air`
  /// supplies the temperature/humidity and defaults to the ISO reference
  /// climate.
  bool air_absorption_enabled = false;
  sonare::acoustic::AirAbsorption air{};
};

/// @brief Convolution reverb whose impulse response is synthesized from room
///        geometry at prepare() time.
///
/// The RIR is (re)synthesized in prepare() at the actual processing sample rate
/// (the convolution runs 1:1 with the IR samples), so the reverberation time is
/// correct on any host rate. process()/reset()/set_parameter() are inherited
/// from ConvolutionReverb and stay allocation-free on the audio thread. The
/// constructor rejects with ErrorCode::InvalidParameter every configuration RIR
/// synthesis would refuse — invalid room dimensions or source/listener placement,
/// a negative image-source order, a `max_seconds` outside
/// [0, acoustic::kMaxRirSeconds], and non-physical air temperature/humidity while
/// air absorption is enabled — because a refused synthesis returns an empty IR,
/// which the convolution path renders as silent dry passthrough. The host sample
/// rate is the one synthesis input the constructor cannot see; prepare() throws
/// the same ErrorCode::InvalidParameter when synthesis refuses it, rather than
/// discarding the diagnostics and preparing an inaudible insert.
///
/// The synthesized RIR is loaded at unit energy, not at its physical
/// 1/(4*pi*d) scale, so `dryWet` means the same mix depth here as on the plain
/// convolution reverb (see ConvolutionReverb::load_ir_unit_energy). Offline
/// consumers that want the physical scale call acoustic::synthesize_rir
/// directly, which is unchanged.
class RoomReverb : public ConvolutionReverb {
 public:
  explicit RoomReverb(RoomReverbConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;

 private:
  RoomReverbConfig config_{};
};

}  // namespace sonare::effects::reverb
