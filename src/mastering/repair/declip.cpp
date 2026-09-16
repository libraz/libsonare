#include "mastering/repair/declip.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "util/exception.h"
#include "util/lpc.h"
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

ClipDetection to_detection(const std::vector<ClipRun>& runs, size_t size) {
  ClipDetection detection;
  detection.run_count = runs.size();
  for (const ClipRun& run : runs) {
    const size_t length = run.end - run.start;
    detection.sample_count += length;
    detection.longest_run_samples = std::max(detection.longest_run_samples, length);
  }
  detection.sample_fraction = static_cast<float>(detection.sample_count) / static_cast<float>(size);
  return detection;
}

/// Merges two ascending, non-overlapping run lists into the ascending region set
/// both channels reconstruct over. Runs that merely touch stay separate, as two
/// adjacent runs already do in one channel.
std::vector<ClipRun> union_runs(const std::vector<ClipRun>& a, const std::vector<ClipRun>& b) {
  std::vector<ClipRun> merged;
  merged.reserve(a.size() + b.size());
  merged.insert(merged.end(), a.begin(), a.end());
  merged.insert(merged.end(), b.begin(), b.end());
  std::sort(merged.begin(), merged.end(),
            [](const ClipRun& lhs, const ClipRun& rhs) { return lhs.start < rhs.start; });

  std::vector<ClipRun> result;
  for (const ClipRun& run : merged) {
    if (!result.empty() && run.start < result.back().end) {
      result.back().end = std::max(result.back().end, run.end);
      continue;
    }
    result.push_back(run);
  }
  return result;
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

void require_stereo_pair(const Audio& left, const Audio& right) {
  if (left.empty() || right.empty()) {
    throw SonareException(ErrorCode::InvalidParameter, "audio must not be empty");
  }
  if (left.size() != right.size()) {
    throw SonareException(ErrorCode::InvalidParameter, "stereo channels must have the same length");
  }
  if (left.sample_rate() != right.sample_rate()) {
    throw SonareException(ErrorCode::InvalidParameter, "stereo channels must share a sample rate");
  }
}

}  // namespace

void validate_config(const DeclipConfig& config) {
  // Two-sided comparisons in this form reject NaN and both infinities on their
  // own: every comparison against a non-finite value is false.
  if (!(config.clip_threshold > 0.0f) || !(config.clip_threshold <= 1.0f)) {
    throw SonareException(ErrorCode::InvalidParameter, "clip_threshold must be in (0, 1]");
  }
  if (config.lpc_order < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "lpc_order must be non-negative");
  }
  if (config.iterations < 1) {
    throw SonareException(ErrorCode::InvalidParameter, "iterations must be positive");
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

  const std::vector<float> buffer(samples, samples + size);
  return to_detection(scan_clipped_runs(buffer, validated->clip_threshold), size);
}

Audio declip(const Audio& audio, const DeclipConfig& config) {
  return declip(audio, config, nullptr);
}

Audio declip(const Audio& audio, const DeclipConfig& config, DeclipReport* report) {
  if (audio.empty()) throw SonareException(ErrorCode::InvalidParameter, "audio must not be empty");
  const auto validated = Validated<DeclipConfig>::make(config);

  std::vector<float> samples(audio.data(), audio.data() + audio.size());
  const std::vector<ClipRun> runs = scan_clipped_runs(samples, validated->clip_threshold);
  const ClipDetection detected = to_detection(runs, samples.size());
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
  const auto validated = Validated<DeclipConfig>::make(config);
  const int sample_rate = left.sample_rate();

  std::vector<float> left_samples(left.data(), left.data() + left.size());
  std::vector<float> right_samples(right.data(), right.data() + right.size());
  const std::vector<ClipRun> left_runs = scan_clipped_runs(left_samples, validated->clip_threshold);
  const std::vector<ClipRun> right_runs =
      scan_clipped_runs(right_samples, validated->clip_threshold);
  const ClipDetection left_detected = to_detection(left_runs, left_samples.size());
  const ClipDetection right_detected = to_detection(right_runs, right_samples.size());
  const std::vector<ClipRun> applied = union_runs(left_runs, right_runs);

  DeclipStereoResult result;
  result.left_report = repair_channel(left_samples, applied, validated.get());
  result.right_report = repair_channel(right_samples, applied, validated.get());
  result.left_report.detected = left_detected;
  result.right_report.detected = right_detected;
  result.left = Audio::from_vector(std::move(left_samples), sample_rate);
  result.right = Audio::from_vector(std::move(right_samples), sample_rate);
  return result;
}

}  // namespace sonare::mastering::repair
