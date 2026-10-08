#include "mastering/repair/declip.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "core/stereo_pair.h"
#include "mastering/common/noise_profile.h"
#include "mastering/repair/run_union.h"
#include "util/db.h"
#include "util/exception.h"
#include "util/lpc.h"
#include "util/numeric_validation.h"
#include "util/peak.h"
#include "util/validated.h"

namespace sonare::mastering::repair {
namespace {

/// The caps and solver knobs this module hands the shared Janssen interpolator.
ArInterpolateParams ar_params(const DeclipConfig& config) {
  ArInterpolateParams params;
  params.order = config.lpc_order;
  params.iterations = config.iterations;
  params.blend = config.lpc_blend;
  params.max_gap = kDeclipMaxLpcGapSamples;
  params.max_context_radius = kDeclipMaxLpcContextRadius;
  return params;
}

struct ClipRun {
  size_t start = 0;
  size_t end = 0;
};

/// Collects the maximal runs at or past @p clip_threshold, ascending. This is
/// the module's only definition of a clipped sample; the public detector and
/// the repair both read it so they cannot disagree about what to act on.
std::vector<ClipRun> scan_clipped_runs(const std::vector<float>& samples, float clip_threshold) {
  std::vector<ClipRun> runs;
  size_t i = 0;
  while (i < samples.size()) {
    if (std::abs(samples[i]) < clip_threshold) {
      ++i;
      continue;
    }
    const size_t start = i;
    while (i < samples.size() && std::abs(samples[i]) >= clip_threshold) ++i;
    runs.push_back({start, i});
  }
  return runs;
}

/// A clipped run has no length limit of its own — a sustained full-scale passage
/// is one run — while the Janssen solver's matrices are sized from the gap.
/// Over-long runs take the interpolation fallback so peak memory and per-run
/// compute stay bounded by the caps rather than by the input.
void repair_run(std::vector<float>& samples, const ClipRun& run, const DeclipConfig& config,
                DeclipReport& report) {
  if (ar_interpolate_region(samples.data(), samples.size(), run.start, run.end,
                            ar_params(config))) {
    ++report.lpc_reconstructed_runs;
  } else {
    interpolate_gap(samples.data(), samples.size(), run.start, run.end);
    ++report.interpolated_runs;
  }
  report.repaired_samples += run.end - run.start;
}

/// Finds the maximal runs of bit-identical samples at or above
/// kDeclipFlatRunAbsoluteFloor, takes the channel's flat level as the largest run
/// level once the kReferenceIgnoredTopEvents highest runs are set aside, and
/// counts the runs within kDeclipFlatRunLevelWindowDb of it. The equality is
/// exact on purpose: a clipper writes one ceiling value into every sample it pins
/// and a later gain scales them all by the same factor, so the run stays exactly
/// level, while an unclipped float apex never repeats a sample. Louder audio
/// without runs therefore cannot move the level.
void scan_flat_runs(const std::vector<float>& samples, ClipDetection& detection) {
  struct FlatRun {
    float level;
    size_t length;
  };
  std::vector<FlatRun> candidates;
  size_t i = 0;
  while (i < samples.size()) {
    const float value = samples[i];
    size_t j = i + 1;
    while (j < samples.size() && samples[j] == value) ++j;
    const size_t length = j - i;
    if (length >= kDeclipMinFlatRunSamples && numeric::finite(value) &&
        std::abs(value) >= kDeclipFlatRunAbsoluteFloor) {
      candidates.push_back({std::abs(value), length});
    }
    i = j;
  }
  if (candidates.empty()) return;

  std::vector<float> levels;
  levels.reserve(candidates.size());
  for (const FlatRun& run : candidates) levels.push_back(run.level);
  const float flat_level = max_excluding_top(std::move(levels), kReferenceIgnoredTopEvents);

  const float lower = flat_level * db_to_linear(-kDeclipFlatRunLevelWindowDb);
  const float upper = flat_level * db_to_linear(kDeclipFlatRunLevelWindowDb);
  for (const FlatRun& run : candidates) {
    if (run.level < lower || run.level > upper) continue;
    ++detection.flat_run_count;
    detection.flat_sample_count += run.length;
    detection.longest_flat_run_samples = std::max(detection.longest_flat_run_samples, run.length);
  }
  detection.flat_level = flat_level;
}

ClipDetection to_detection(const std::vector<float>& samples, const std::vector<ClipRun>& runs) {
  ClipDetection detection;
  detection.run_count = runs.size();
  for (const ClipRun& run : runs) {
    const size_t length = run.end - run.start;
    detection.sample_count += length;
    detection.longest_run_samples = std::max(detection.longest_run_samples, length);
  }
  detection.sample_fraction =
      static_cast<float>(detection.sample_count) / static_cast<float>(samples.size());
  scan_flat_runs(samples, detection);
  return detection;
}

/// Merges two ascending, non-overlapping run lists into the ascending region set
/// both channels reconstruct over. Runs that merely touch stay separate, as two
/// adjacent runs already do in one channel.
std::vector<ClipRun> union_runs(const std::vector<ClipRun>& a, const std::vector<ClipRun>& b) {
  return union_sorted_runs(a, b);
}

bool any_at_or_past(const std::vector<float>& samples, const ClipRun& run, float clip_threshold) {
  for (size_t j = run.start; j < run.end; ++j) {
    if (std::abs(samples[j]) >= clip_threshold) return true;
  }
  return false;
}

bool any_below(const std::vector<float>& samples, const ClipRun& run, float clip_threshold) {
  for (size_t j = run.start; j < run.end; ++j) {
    if (std::abs(samples[j]) < clip_threshold) return true;
  }
  return false;
}

/// Reconstructs @p applied in one channel, skipping the runs this channel has no
/// clipped sample in. Ascending order matters: a run's context reads the
/// already-reconstructed samples before it.
DeclipReport repair_channel(std::vector<float>& samples, const std::vector<ClipRun>& applied,
                            const DeclipConfig& config) {
  DeclipReport report;
  for (const ClipRun& run : applied) {
    if (!any_at_or_past(samples, run, config.clip_threshold)) continue;
    const bool reaches_past_own = any_below(samples, run, config.clip_threshold);
    repair_run(samples, run, config, report);
    if (reaches_past_own) ++report.linked_runs;
  }
  return report;
}

}  // namespace

void validate_config(const DeclipConfig& config) {
  // Two-sided comparisons in this form reject NaN and both infinities on their
  // own: every comparison against a non-finite value is false.
  if (!(config.clip_threshold > 0.0f) || !(config.clip_threshold <= 1.0f)) {
    throw SonareException(ErrorCode::InvalidParameter, "clip_threshold must be in (0, 1]");
  }
  if (config.lpc_order < 0 || config.lpc_order > kDeclipMaxLpcOrder) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "lpc_order must be in [0, " + std::to_string(kDeclipMaxLpcOrder) + "]");
  }
  if (config.iterations < 1 || config.iterations > kDeclipMaxIterations) {
    throw SonareException(
        ErrorCode::InvalidParameter,
        "iterations must be in [1, " + std::to_string(kDeclipMaxIterations) + "]");
  }
  if (!(config.lpc_blend >= 0.0f) || !(config.lpc_blend <= 1.0f)) {
    throw SonareException(ErrorCode::InvalidParameter, "lpc_blend must be in [0, 1]");
  }
}

ClipDetection detect_clipping(const float* samples, size_t size, int sample_rate,
                              const DeclipConfig& config) {
  const auto validated = Validated<DeclipConfig>::make(config);
  if (sample_rate <= 0) {
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  }
  if (samples == nullptr || size == 0) return {};
  // Keep null/empty compatibility; reject non-finite values before scan_clipped_runs.
  validate_offline_audio_input(samples, size, sample_rate);

  const std::vector<float> buffer(samples, samples + size);
  return to_detection(buffer, scan_clipped_runs(buffer, validated->clip_threshold));
}

Audio declip(const Audio& audio, const DeclipConfig& config) {
  return declip(audio, config, nullptr);
}

Audio declip(const Audio& audio, const DeclipConfig& config, DeclipReport* report) {
  if (audio.empty()) throw SonareException(ErrorCode::InvalidParameter, "audio must not be empty");
  const auto validated = Validated<DeclipConfig>::make(config);
  validate_offline_audio_input(audio.data(), audio.size(), audio.sample_rate());

  std::vector<float> samples(audio.data(), audio.data() + audio.size());
  const std::vector<ClipRun> runs = scan_clipped_runs(samples, validated->clip_threshold);
  // Before the repair overwrites the samples the flat-top scan reads, and only
  // when a caller asked: the chain's declip stage passes no report at all.
  const ClipDetection detected = report != nullptr ? to_detection(samples, runs) : ClipDetection{};
  DeclipReport pass = repair_channel(samples, runs, validated.get());
  if (report != nullptr) {
    pass.detected = detected;
    *report = pass;
  }
  return Audio::from_vector(std::move(samples), audio.sample_rate());
}

DeclipStereoResult declip_stereo(const Audio& left, const Audio& right,
                                 const DeclipConfig& config) {
  require_stereo_pair(left, right);
  const Audio* channels[2] = {&left, &right};
  std::vector<Audio> out;
  const std::vector<DeclipReport> reports = declip_linked(channels, 2, &out, config);

  DeclipStereoResult result;
  result.left_report = reports[0];
  result.right_report = reports[1];
  result.left = std::move(out[0]);
  result.right = std::move(out[1]);
  return result;
}

std::vector<DeclipReport> declip_linked(const Audio* const* channels, size_t channel_count,
                                        std::vector<Audio>* out, const DeclipConfig& config) {
  const auto validated = Validated<DeclipConfig>::make(config);
  if (out == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter, "declip output must not be null");
  }
  if (channels == nullptr || channel_count == 0 || channels[0] == nullptr || channels[0]->empty()) {
    throw SonareException(ErrorCode::InvalidParameter, "audio must not be empty");
  }
  common::validate_linked_channels(channels, channel_count);
  const int sample_rate = channels[0]->sample_rate();
  for (size_t c = 0; c < channel_count; ++c) {
    validate_offline_audio_input(channels[c]->data(), channels[c]->size(), sample_rate);
  }

  std::vector<std::vector<float>> samples;
  std::vector<ClipDetection> detected;
  samples.reserve(channel_count);
  detected.reserve(channel_count);
  std::vector<ClipRun> applied;
  for (size_t c = 0; c < channel_count; ++c) {
    samples.emplace_back(channels[c]->data(), channels[c]->data() + channels[c]->size());
    const std::vector<ClipRun> runs = scan_clipped_runs(samples[c], validated->clip_threshold);
    detected.push_back(to_detection(samples[c], runs));
    applied = c == 0 ? runs : union_runs(applied, runs);
  }

  std::vector<DeclipReport> reports;
  reports.reserve(channel_count);
  out->clear();
  out->reserve(channel_count);
  for (size_t c = 0; c < channel_count; ++c) {
    reports.push_back(repair_channel(samples[c], applied, validated.get()));
    reports.back().detected = detected[c];
    out->push_back(Audio::from_vector(std::move(samples[c]), sample_rate));
  }
  return reports;
}

}  // namespace sonare::mastering::repair
