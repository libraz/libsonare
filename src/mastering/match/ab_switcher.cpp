#include "mastering/match/ab_switcher.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "mastering/common/loudness_measure.h"
#include "util/db.h"
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

void validate_selection(ABSelection selection) {
  if (selection != ABSelection::A && selection != ABSelection::B) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid A/B selection");
  }
}

Audio ab_switch(const Audio& a, const Audio& b, ABSelection selection) {
  validate_pair(a, b);
  validate_selection(selection);
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

LoudnessMatchedPair ab_match_loudness(const Audio& a, const Audio& b) {
  validate_pair(a, b);
  const float a_lufs = common::measure_lufs(a);
  const float b_lufs = common::measure_lufs(b);

  // No cap: a bare headroom clamp would leave a peak-normalized `b` at its own
  // loudness, which defeats the only reason this function exists. The caller
  // gets the post-gain true peak below instead of a decision made for it.
  const float gain_db = std::isfinite(a_lufs) && std::isfinite(b_lufs) ? a_lufs - b_lufs : 0.0f;

  std::vector<float> matched(b.data(), b.data() + b.size());
  const float gain = db_to_linear(gain_db);
  for (auto& sample : matched) {
    sample *= gain;
  }
  Audio matched_audio = Audio::from_vector(std::move(matched), b.sample_rate());
  const float matched_true_peak_dbtp = common::measure_true_peak_dbtp(matched_audio);

  return {a, std::move(matched_audio), a_lufs, b_lufs, gain_db, matched_true_peak_dbtp};
}

}  // namespace sonare::mastering::match
