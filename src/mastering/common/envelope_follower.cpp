#include "mastering/common/envelope_follower.h"

#include <algorithm>
#include <cmath>

#include "util/dsp_primitives.h"

namespace sonare::mastering::common {

void EnvelopeFollower::prepare(double sample_rate, float attack_ms, float release_ms) {
  sample_rate_ = sample_rate;
  attack_coeff_ = time_to_coefficient(sample_rate_, attack_ms);
  release_coeff_ = time_to_coefficient(sample_rate_, release_ms);
}

void EnvelopeFollower::reset(float value) { envelope_ = std::max(0.0f, value); }

float EnvelopeFollower::process(float input) {
  const float detector = std::abs(input);
  const float coeff = detector > envelope_ ? attack_coeff_ : release_coeff_;
  envelope_ = coeff * envelope_ + (1.0f - coeff) * detector;
  return envelope_;
}

float EnvelopeFollower::smooth_bidirectional(float target, bool attack_when_decreasing) {
  return smooth_bidirectional(target, release_coeff_, attack_when_decreasing);
}

float EnvelopeFollower::smooth_bidirectional(float target, float release_coeff,
                                             bool attack_when_decreasing) {
  const bool use_attack = attack_when_decreasing ? target < envelope_ : target > envelope_;
  const float coeff = use_attack ? attack_coeff_ : release_coeff;
  envelope_ = coeff * envelope_ + (1.0f - coeff) * target;
  return envelope_;
}

}  // namespace sonare::mastering::common
