#pragma once

#include "core/audio.h"

namespace sonare::mastering::repair {

struct DereverbClassicalConfig {
  float threshold = 0.05f;
  float attenuation = 0.5f;
  int n_fft = 1024;
  int hop_length = 256;
  float t60_sec = 0.4f;
  float late_delay_ms = 50.0f;
  float over_subtraction = 1.0f;
  float spectral_floor = 0.08f;
  bool wpe_enabled = false;
  int wpe_iterations = 2;
  int wpe_taps = 3;
  float wpe_strength = 0.7f;
};

Audio dereverb_classical(const Audio& audio, const DereverbClassicalConfig& config = {});

/// @brief Sets the two config fields a room measurement determines, leaving the
///        rest of @p config as the caller has them.
/// @details What the room decides is WHERE the tail is: @p rt60_mid_sec is how
///   long it lasts and @p volume_m3 fixes where it starts, through Polack's
///   mixing time sqrt(V) in ms -- past that the response is the diffuse tail
///   the subtraction targets rather than separable early reflections. How MUCH
///   to remove (attenuation, threshold, over_subtraction, spectral_floor) is
///   taste rather than measurement, so this does not touch it.
///
///   A non-finite or non-positive argument leaves its own field alone, so a
///   partial estimate still configures the half it measured. Takes scalars
///   rather than a RoomEstimate deliberately: the mastering layer does not
///   depend on the acoustic one, and the caller already holds both numbers.
void apply_room_measurement(DereverbClassicalConfig& config, float rt60_mid_sec,
                            float volume_m3) noexcept;

}  // namespace sonare::mastering::repair
