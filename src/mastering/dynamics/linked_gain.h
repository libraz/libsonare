#pragma once

#include <algorithm>
#include <cmath>

#include "util/db.h"

namespace sonare::mastering::dynamics {

/**
 * @brief Applies one linked gain to every channel from a single loudest-channel envelope.
 *
 * Independent per-channel followers would let L/R gain diverge on asymmetric
 * content and rotate the stereo image, so one detector drives all channels.
 *
 * @param excluded_channel Channel left out of detection, or negative for none.
 * @param law Maps the envelope in dB to the applied gain in dB.
 * @param fold Combines the running extreme with each sample's gain in dB.
 * @return The extreme gain in dB over the block (starting from 0 dB).
 */
template <typename Follower, typename GainLaw, typename Fold>
float apply_linked_gain(float* const* channels, int num_channels, int num_samples,
                        int excluded_channel, Follower& follower, GainLaw law, Fold fold) {
  float extreme_db = 0.0f;
  for (int i = 0; i < num_samples; ++i) {
    float linked_level = 0.0f;
    if (excluded_channel < 0) {
      for (int ch = 0; ch < num_channels; ++ch) {
        linked_level = std::max(linked_level, std::abs(channels[ch][i]));
      }
    } else {
      for (int ch = 0; ch < num_channels; ++ch) {
        if (ch == excluded_channel) continue;
        linked_level = std::max(linked_level, std::abs(channels[ch][i]));
      }
    }
    const float envelope = follower.process(linked_level);
    const float gain_db = law(linear_to_db(envelope));
    const float gain = db_to_linear(gain_db);
    for (int ch = 0; ch < num_channels; ++ch) {
      channels[ch][i] *= gain;
    }
    extreme_db = fold(extreme_db, gain_db);
  }
  return extreme_db;
}

}  // namespace sonare::mastering::dynamics
