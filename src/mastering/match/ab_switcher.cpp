#include "mastering/match/ab_switcher.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "util/dsp_primitives.h"
#include "util/exception.h"

namespace sonare::mastering::match {

namespace {

void validate_pair(const Audio& a, const Audio& b) {
  if (a.empty() || b.empty()) {
    throw SonareException(ErrorCode::InvalidParameter, "audio must not be empty");
  }
  if (a.sample_rate() != b.sample_rate()) {
    throw SonareException(ErrorCode::InvalidParameter, "sample rates must match");
  }
}

}  // namespace

Audio ab_switch(const Audio& a, const Audio& b, ABSelection selection) {
  validate_pair(a, b);
  return selection == ABSelection::A ? a : b;
}

Audio ab_crossfade(const Audio& a, const Audio& b, float mix) {
  validate_pair(a, b);
  if (!(mix >= 0.0f && mix <= 1.0f)) {
    throw SonareException(ErrorCode::InvalidParameter, "mix must be in [0, 1]");
  }
  const size_t size = std::min(a.size(), b.size());
  std::vector<float> samples(size);
  for (size_t i = 0; i < size; ++i) {
    samples[i] = linear_crossfade(a[i], b[i], mix);
  }
  return Audio::from_vector(std::move(samples), a.sample_rate());
}

}  // namespace sonare::mastering::match
