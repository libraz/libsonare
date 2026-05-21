#include "mastering/common/envelope_follower.h"

#include <algorithm>
#include <cmath>

namespace sonare::mastering::common {

void EnvelopeFollower::prepare(double sample_rate, float attack_ms, float release_ms) {
  sample_rate_ = sample_rate;
  attack_coeff_ = coefficient(sample_rate_, attack_ms);
  release_coeff_ = coefficient(sample_rate_, release_ms);
}

void EnvelopeFollower::reset(float value) { envelope_ = std::max(0.0f, value); }

float EnvelopeFollower::process(float input) {
  const float detector = std::abs(input);
  const float coeff = detector > envelope_ ? attack_coeff_ : release_coeff_;
  envelope_ = coeff * envelope_ + (1.0f - coeff) * detector;
  return envelope_;
}

float EnvelopeFollower::coefficient(double sample_rate, float time_ms) {
  const float clamped_ms = std::max(time_ms, 0.0f);
  if (clamped_ms == 0.0f || sample_rate <= 0.0) return 0.0f;

  const double samples = sample_rate * static_cast<double>(clamped_ms) * 0.001;
  return static_cast<float>(std::exp(-1.0 / samples));
}

}  // namespace sonare::mastering::common
