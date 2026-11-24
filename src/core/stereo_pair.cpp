#include "core/stereo_pair.h"

#include "util/parameter_checks.h"

namespace sonare {

void require_stereo_pair(const Audio& left, const Audio& right) {
  if (left.empty()) util::check::refuse("left", util::refusal::kNonEmptyPredicate);
  if (right.empty()) util::check::refuse("right", util::refusal::kNonEmptyPredicate);
  if (left.size() != right.size()) {
    util::check::refuse_relation({"left", "right"}, "{0} and {1} must have the same length");
  }
  if (left.sample_rate() != right.sample_rate()) {
    util::check::refuse_relation({"left", "right"}, "{0} and {1} must share a sample rate");
  }
}

}  // namespace sonare
