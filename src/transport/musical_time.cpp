#include "transport/musical_time.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sonare::transport {

double note_length_ppq(int denominator, NoteModifier modifier) noexcept {
  const double base = 4.0 / static_cast<double>(std::max(denominator, 1));
  switch (modifier) {
    case NoteModifier::kDotted:
      return base * 1.5;
    case NoteModifier::kTriplet:
      return base * (2.0 / 3.0);
    case NoteModifier::kStraight:
    default:
      return base;
  }
}

int64_t ppq_duration_to_samples(double ppq, double bpm, double sample_rate) noexcept {
  if (bpm <= 0.0 || sample_rate <= 0.0 || !std::isfinite(ppq) || !std::isfinite(bpm) ||
      !std::isfinite(sample_rate)) {
    return 0;
  }
  const long double samples = static_cast<long double>(ppq) *
                              static_cast<long double>(sample_rate) * 60.0L /
                              static_cast<long double>(bpm);
  constexpr long double kMin = static_cast<long double>(std::numeric_limits<int64_t>::min());
  constexpr long double kMax = static_cast<long double>(std::numeric_limits<int64_t>::max());
  if (samples <= kMin) return std::numeric_limits<int64_t>::min();
  if (samples >= kMax) return std::numeric_limits<int64_t>::max();
  return static_cast<int64_t>(std::llround(samples));
}

double samples_to_ppq_duration(int64_t samples, double bpm, double sample_rate) noexcept {
  if (sample_rate <= 0.0 || bpm <= 0.0) return 0.0;
  return static_cast<double>(samples) * bpm / (sample_rate * 60.0);
}

}  // namespace sonare::transport
