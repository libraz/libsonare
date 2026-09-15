#include "mastering/repair/declick.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>
#include <vector>

#include "util/exception.h"
#include "util/lpc.h"
#include "util/validated.h"

namespace sonare::mastering::repair {
namespace {

bool can_use_lpc(size_t size, int order) {
  return order > 0 && size > static_cast<size_t>(order + 2);
}

std::optional<LpcResult> fit_lpc(const std::vector<float>& samples, const DeclickConfig& config) {
  if (!can_use_lpc(samples.size(), config.lpc_order)) return std::nullopt;
  const int lpc_order =
      std::min(config.lpc_order, static_cast<int>(std::max<size_t>(1, samples.size() / 4)));
  return sonare::lpc_burg(samples.data(), samples.size(), lpc_order);
}

std::vector<bool> click_mask(const std::vector<float>& samples, const DeclickConfig& config,
                             const LpcResult* lpc_model) {
  std::vector<bool> mask(samples.size(), false);
  for (size_t i = 1; i + 1 < samples.size(); ++i) {
    mask[i] = std::abs(samples[i]) >= config.threshold;
  }

  if (!lpc_model) return mask;
  const auto residual = sonare::lpc_residual(samples.data(), samples.size(), *lpc_model);
  constexpr size_t kRadius = 8;
  for (size_t i = 1; i + 1 < samples.size(); ++i) {
    const size_t begin = i > kRadius ? i - kRadius : 0;
    const size_t end = std::min(samples.size(), i + kRadius + 1);
    double residual_sum = 0.0;
    double sample_sum = 0.0;
    size_t count = 0;
    for (size_t j = begin; j < end; ++j) {
      if (j == i) continue;
      residual_sum += std::abs(residual[j]);
      sample_sum += std::abs(samples[j]);
      ++count;
    }
    const float local_residual =
        count == 0 ? 1.0e-6f : static_cast<float>(residual_sum / static_cast<double>(count));
    const float local_sample =
        count == 0 ? 1.0e-6f : static_cast<float>(sample_sum / static_cast<double>(count));
    if (std::abs(residual[i]) > local_residual * config.residual_ratio &&
        std::abs(samples[i]) > local_sample * 1.5f) {
      mask[i] = true;
    }
  }
  return mask;
}

struct ClickRun {
  size_t start = 0;
  size_t end = 0;
};

/// One channel's click analysis: which runs the repair criteria select, and how
/// many they turned down. Separated from the fill so the public detector and
/// the repair cannot disagree about what counts as a click.
struct ChannelAnalysis {
  std::vector<ClickRun> selected;
  size_t rejected = 0;
  size_t longest_run_samples = 0;
};

ChannelAnalysis analyze_channel(const std::vector<float>& samples, const DeclickConfig& config,
                                const LpcResult* lpc_model) {
  ChannelAnalysis analysis;
  const std::vector<bool> mask = click_mask(samples, config, lpc_model);
  size_t i = 1;
  while (i + 1 < samples.size()) {
    if (!mask[i]) {
      ++i;
      continue;
    }

    const size_t start = i;
    float peak = 0.0f;
    while (i + 1 < samples.size() && mask[i]) {
      peak = std::max(peak, std::abs(samples[i]));
      ++i;
    }
    const size_t end = i;
    const size_t length = end - start;
    const float local = std::max({std::abs(samples[start - 1]), std::abs(samples[end]), 1e-6f});
    if (length <= config.max_click_samples && peak > local * config.neighbor_ratio) {
      analysis.selected.push_back({start, end});
      analysis.longest_run_samples = std::max(analysis.longest_run_samples, length);
    } else {
      ++analysis.rejected;
    }
  }
  return analysis;
}

void interpolate_region(std::vector<float>& output, const std::vector<float>& samples, size_t start,
                        size_t end, const LpcResult* lpc_model) {
  const size_t length = end - start;
  const float left = output[start - 1];
  const float right = samples[end];
  // Linear interpolation anchored to BOTH boundaries; this is the fallback fill
  // and also the boundary-respecting baseline the AR estimate is blended toward.
  for (size_t j = start; j < end; ++j) {
    const float t = static_cast<float>(j - start + 1) / static_cast<float>(length + 1);
    output[j] = left + (right - left) * t;
  }

  if (!lpc_model) return;

  // Forward AR extrapolation from the left context restores the click's spectral
  // detail but, on its own, drifts away from the right boundary. Crossfade the
  // AR prediction (weight 1 at the left edge) toward the linear interpolation
  // (weight 1 at the right edge) so both boundaries are respected. Predict into
  // a scratch buffer first so each step uses the already-blended history rather
  // than raw AR output.
  std::vector<float> ar_fill(length, 0.0f);
  for (size_t j = start; j < end; ++j) {
    double predicted = 0.0;
    const size_t max_k = std::min(lpc_model->ar.size() - 1, j);
    for (size_t k = 1; k <= max_k; ++k) {
      predicted -= static_cast<double>(lpc_model->ar[k]) * output[j - k];
    }
    const float linear = output[j];
    // w: 1 at the first filled sample, decreasing to ~0 near the right anchor.
    const float w = 1.0f - static_cast<float>(j - start + 1) / static_cast<float>(length + 1);
    ar_fill[j - start] = w * static_cast<float>(predicted) + (1.0f - w) * linear;
    output[j] = ar_fill[j - start];
  }
}

/// Fills @p runs in ascending order, which is the order the fill was written
/// for: each region's left anchor is the already-repaired sample before it.
void apply_runs(std::vector<float>& output, const std::vector<float>& samples,
                const std::vector<ClickRun>& runs, const LpcResult* lpc_model) {
  for (const ClickRun& run : runs) {
    interpolate_region(output, samples, run.start, run.end, lpc_model);
  }
}

ClickDetection to_detection(const ChannelAnalysis& analysis, size_t size, int sample_rate) {
  ClickDetection detection;
  detection.count = analysis.selected.size();
  detection.rejected = analysis.rejected;
  detection.longest_run_samples = analysis.longest_run_samples;
  detection.per_second = static_cast<float>(detection.count) * static_cast<float>(sample_rate) /
                         static_cast<float>(size);
  return detection;
}

/// Merges two ascending, non-overlapping run lists into the ascending run set
/// both channels are repaired over. Runs that merely touch stay separate; each
/// then anchors on the other's repaired output, as adjacent runs already do.
std::vector<ClickRun> union_runs(const std::vector<ClickRun>& a, const std::vector<ClickRun>& b) {
  std::vector<ClickRun> merged;
  merged.reserve(a.size() + b.size());
  merged.insert(merged.end(), a.begin(), a.end());
  merged.insert(merged.end(), b.begin(), b.end());
  std::sort(merged.begin(), merged.end(),
            [](const ClickRun& lhs, const ClickRun& rhs) { return lhs.start < rhs.start; });

  std::vector<ClickRun> result;
  for (const ClickRun& run : merged) {
    if (!result.empty() && run.start < result.back().end) {
      result.back().end = std::max(result.back().end, run.end);
      continue;
    }
    result.push_back(run);
  }
  return result;
}

size_t count_linked_runs(const std::vector<ClickRun>& applied, const std::vector<ClickRun>& own) {
  size_t linked = 0;
  for (const ClickRun& run : applied) {
    const bool is_own = std::any_of(own.begin(), own.end(), [&](const ClickRun& candidate) {
      return candidate.start == run.start && candidate.end == run.end;
    });
    if (!is_own) ++linked;
  }
  return linked;
}

DeclickReport to_report(const ChannelAnalysis& analysis, const std::vector<ClickRun>& applied,
                        size_t size, int sample_rate, bool lpc_model_used) {
  DeclickReport report;
  report.detected = to_detection(analysis, size, sample_rate);
  report.repaired_runs = applied.size();
  for (const ClickRun& run : applied) report.repaired_samples += run.end - run.start;
  report.linked_runs = count_linked_runs(applied, analysis.selected);
  report.lpc_model_used = lpc_model_used;
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

void validate_config(const DeclickConfig& config) {
  if (!std::isfinite(config.threshold) || !(config.threshold > 0.0f)) {
    throw SonareException(ErrorCode::InvalidParameter, "threshold must be finite and positive");
  }
  if (!std::isfinite(config.neighbor_ratio) || !(config.neighbor_ratio > 0.0f)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "neighbor_ratio must be finite and positive");
  }
  if (config.max_click_samples == 0) {
    throw SonareException(ErrorCode::InvalidParameter, "max_click_samples must be positive");
  }
  if (config.lpc_order < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "lpc_order must be non-negative");
  }
  if (!std::isfinite(config.residual_ratio) || !(config.residual_ratio > 0.0f)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "residual_ratio must be finite and positive");
  }
}

ClickDetection detect_clicks(const float* samples, size_t size, int sample_rate,
                             const DeclickConfig& config) {
  const auto validated = Validated<DeclickConfig>::make(config);
  if (sample_rate <= 0) {
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  }
  if (samples == nullptr || size == 0) return {};

  const std::vector<float> buffer(samples, samples + size);
  const std::optional<LpcResult> lpc_model = fit_lpc(buffer, validated.get());
  const ChannelAnalysis analysis =
      analyze_channel(buffer, validated.get(), lpc_model ? &*lpc_model : nullptr);
  return to_detection(analysis, size, sample_rate);
}

Audio declick(const Audio& audio, const DeclickConfig& config) {
  return declick(audio, config, nullptr);
}

Audio declick(const Audio& audio, const DeclickConfig& config, DeclickReport* report) {
  if (audio.empty()) throw SonareException(ErrorCode::InvalidParameter, "audio must not be empty");
  const auto validated = Validated<DeclickConfig>::make(config);

  const std::vector<float> samples(audio.data(), audio.data() + audio.size());
  std::vector<float> output = samples;
  const std::optional<LpcResult> lpc_model = fit_lpc(samples, validated.get());
  const LpcResult* model = lpc_model ? &*lpc_model : nullptr;
  const ChannelAnalysis analysis = analyze_channel(samples, validated.get(), model);
  apply_runs(output, samples, analysis.selected, model);
  if (report != nullptr) {
    *report = to_report(analysis, analysis.selected, samples.size(), audio.sample_rate(),
                        lpc_model.has_value());
  }
  return Audio::from_vector(std::move(output), audio.sample_rate());
}

DeclickStereoResult declick_stereo(const Audio& left, const Audio& right,
                                   const DeclickConfig& config) {
  require_stereo_pair(left, right);
  const auto validated = Validated<DeclickConfig>::make(config);
  const int sample_rate = left.sample_rate();

  const std::vector<float> left_samples(left.data(), left.data() + left.size());
  const std::vector<float> right_samples(right.data(), right.data() + right.size());
  const std::optional<LpcResult> left_model = fit_lpc(left_samples, validated.get());
  const std::optional<LpcResult> right_model = fit_lpc(right_samples, validated.get());
  const ChannelAnalysis left_analysis =
      analyze_channel(left_samples, validated.get(), left_model ? &*left_model : nullptr);
  const ChannelAnalysis right_analysis =
      analyze_channel(right_samples, validated.get(), right_model ? &*right_model : nullptr);
  const std::vector<ClickRun> applied = union_runs(left_analysis.selected, right_analysis.selected);

  std::vector<float> left_output = left_samples;
  std::vector<float> right_output = right_samples;
  apply_runs(left_output, left_samples, applied, left_model ? &*left_model : nullptr);
  apply_runs(right_output, right_samples, applied, right_model ? &*right_model : nullptr);

  DeclickStereoResult result;
  result.left_report =
      to_report(left_analysis, applied, left_samples.size(), sample_rate, left_model.has_value());
  result.right_report = to_report(right_analysis, applied, right_samples.size(), sample_rate,
                                  right_model.has_value());
  result.left = Audio::from_vector(std::move(left_output), sample_rate);
  result.right = Audio::from_vector(std::move(right_output), sample_rate);
  return result;
}

}  // namespace sonare::mastering::repair
