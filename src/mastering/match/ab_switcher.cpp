#include "mastering/match/ab_switcher.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "mastering/common/loudness_measure.h"
#include "metering/lufs.h"
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

void validate_stereo_pair(const StereoAudioPair& pair, const char* role) {
  if (pair.left.empty() || pair.right.empty()) {
    throw SonareException(ErrorCode::InvalidParameter,
                          std::string(role) + " stereo channels must not be empty");
  }
  if (pair.left.size() != pair.right.size()) {
    throw SonareException(ErrorCode::InvalidParameter,
                          std::string(role) + " stereo channel lengths must match");
  }
  if (pair.left.sample_rate() != pair.right.sample_rate()) {
    throw SonareException(ErrorCode::InvalidParameter,
                          std::string(role) + " stereo sample rates must match");
  }
  validate_offline_audio_input(pair.left.data(), pair.left.size(), pair.left.sample_rate());
  validate_offline_audio_input(pair.right.data(), pair.right.size(), pair.right.sample_rate());
}

std::vector<float> interleave_stereo(const StereoAudioPair& pair) {
  std::vector<float> interleaved(pair.left.size() * 2);
  for (std::size_t index = 0; index < pair.left.size(); ++index) {
    interleaved[index * 2] = pair.left[index];
    interleaved[index * 2 + 1] = pair.right[index];
  }
  return interleaved;
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

StereoAudioPair ab_crossfade_stereo(const StereoAudioPair& a, const StereoAudioPair& b, float mix) {
  validate_stereo_pair(a, "a");
  validate_stereo_pair(b, "b");
  if (a.left.sample_rate() != b.left.sample_rate()) {
    throw SonareException(ErrorCode::InvalidParameter, "stereo sample rates must match");
  }
  if (!(mix >= 0.0f && mix <= 1.0f)) {
    throw SonareException(ErrorCode::InvalidParameter, "mix must be in [0, 1]");
  }

  const std::size_t size = std::min(a.left.size(), b.left.size());
  std::vector<float> left(size);
  std::vector<float> right(size);
  for (std::size_t index = 0; index < size; ++index) {
    left[index] = linear_crossfade(a.left[index], b.left[index], mix);
    right[index] = linear_crossfade(a.right[index], b.right[index], mix);
  }
  return {Audio::from_vector(std::move(left), a.left.sample_rate()),
          Audio::from_vector(std::move(right), a.left.sample_rate())};
}

LoudnessMatchedPair ab_match_loudness(const Audio& a, const Audio& b) {
  validate_pair(a, b);
  const float a_lufs = common::measure_lufs(a);
  const metering::LufsGainToTarget solved =
      metering::gain_to_integrated_lufs(b.data(), b.size(), 1, b.sample_rate(), a_lufs);
  const float b_lufs = solved.measured_lufs;

  // No cap: a bare headroom clamp would leave a peak-normalized `b` at its own
  // loudness, which defeats the only reason this function exists. The caller
  // gets the post-gain true peak below instead of a decision made for it.
  const float gain_db = solved.gain_db;

  std::vector<float> matched(b.data(), b.data() + b.size());
  const float gain = db_to_linear(gain_db);
  for (auto& sample : matched) {
    sample *= gain;
  }
  Audio matched_audio = Audio::from_vector(std::move(matched), b.sample_rate());
  const float matched_true_peak_dbtp = common::measure_true_peak_dbtp(matched_audio);

  return {a, std::move(matched_audio), a_lufs, b_lufs, gain_db, matched_true_peak_dbtp};
}

StereoLoudnessMatchedPair ab_match_loudness_stereo(const StereoAudioPair& a,
                                                   const StereoAudioPair& b) {
  validate_stereo_pair(a, "reference");
  validate_stereo_pair(b, "source");
  if (a.left.sample_rate() != b.left.sample_rate()) {
    throw SonareException(ErrorCode::InvalidParameter, "stereo sample rates must match");
  }

  const int sample_rate = a.left.sample_rate();
  const std::vector<float> reference_interleaved = interleave_stereo(a);
  const std::vector<float> source_interleaved = interleave_stereo(b);
  const float reference_lufs =
      common::measure_lufs_interleaved(reference_interleaved.data(), a.left.size(), 2, sample_rate);
  const metering::LufsGainToTarget solved = metering::gain_to_integrated_lufs(
      source_interleaved.data(), b.left.size(), 2, sample_rate, reference_lufs);
  const float source_lufs = solved.measured_lufs;
  const float gain_db = solved.gain_db;

  std::vector<float> matched_left(b.left.begin(), b.left.end());
  std::vector<float> matched_right(b.right.begin(), b.right.end());
  const float gain = db_to_linear(gain_db);
  for (std::size_t index = 0; index < matched_left.size(); ++index) {
    matched_left[index] *= gain;
    matched_right[index] *= gain;
  }
  const float matched_true_peak_dbtp = common::measure_true_peak_dbtp_stereo_planar(
      matched_left.data(), matched_right.data(), matched_left.size());

  StereoAudioPair matched{Audio::from_vector(std::move(matched_left), sample_rate),
                          Audio::from_vector(std::move(matched_right), sample_rate)};
  return {a, std::move(matched), reference_lufs, source_lufs, gain_db, matched_true_peak_dbtp};
}

}  // namespace sonare::mastering::match
