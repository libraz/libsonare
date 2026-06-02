#pragma once

/// @file room_reverb.h
/// @brief Geometry-driven convolution reverb (the 5th reverb engine): synthesizes
///        a room impulse response from shoebox dimensions and uniform wall
///        absorption, then convolves the signal with it.

#include "acoustic/geometry.h"
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
};

/// @brief Convolution reverb whose impulse response is synthesized from room
///        geometry at prepare() time.
///
/// The RIR is (re)synthesized in prepare() at the actual processing sample rate
/// (the convolution runs 1:1 with the IR samples), so the reverberation time is
/// correct on any host rate. process()/reset()/set_parameter() are inherited
/// from ConvolutionReverb and stay allocation-free on the audio thread.
class RoomReverb : public ConvolutionReverb {
 public:
  explicit RoomReverb(RoomReverbConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;

 private:
  RoomReverbConfig config_{};
};

}  // namespace sonare::effects::reverb
