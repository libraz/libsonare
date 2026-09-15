#include "mastering/repair/declip.h"

#include <Eigen/Dense>
#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "util/constants.h"
#include "util/exception.h"
#include "util/lpc.h"
#include "util/validated.h"

namespace sonare::mastering::repair {
namespace {

using sonare::constants::kEpsilon;

float cubic_hermite(float y0, float y1, float y2, float y3, float t) {
  const float m1 = 0.5f * (y2 - y0);
  const float m2 = 0.5f * (y3 - y1);
  const float t2 = t * t;
  const float t3 = t2 * t;
  return (2.0f * t3 - 3.0f * t2 + 1.0f) * y1 + (t3 - 2.0f * t2 + t) * m1 +
         (-2.0f * t3 + 3.0f * t2) * y2 + (t3 - t2) * m2;
}

bool has_cubic_context(size_t start, size_t end, size_t size) {
  return start >= 2 && end + 1 < size;
}

bool can_use_lpc(size_t size, int order) {
  return order > 0 && size > static_cast<size_t>(order + 2);
}

float interpolate_fallback(const std::vector<float>& samples, size_t start, size_t end, size_t j) {
  const float left = start > 0 ? samples[start - 1] : (end < samples.size() ? samples[end] : 0.0f);
  const float right = end < samples.size() ? samples[end] : left;
  const size_t length = end - start;
  const float t = static_cast<float>(j - start + 1) / static_cast<float>(length + 1);
  return has_cubic_context(start, end, samples.size())
             ? cubic_hermite(samples[start - 2], left, right, samples[end + 1], t)
             : left + (right - left) * t;
}

/// @brief Fills [start, end) with the cubic / linear interpolation fallback.
/// @details Used for clipped runs longer than @ref kDeclipMaxLpcGapSamples, where
/// the Janssen solver's dense matrices would grow without bound. Reads only
/// samples outside the run, so writing in place is safe and every position sees
/// the same endpoints.
void interpolate_region(std::vector<float>& samples, size_t start, size_t end) {
  for (size_t j = start; j < end; ++j) {
    samples[j] = interpolate_fallback(samples, start, end, j);
  }
}

/// @brief Janssen (1986) constrained AR interpolation for a single clipped segment.
///
/// Reference: A. J. E. M. Janssen, "Adaptive interpolation of discrete-time signals
/// that can be modeled as autoregressive processes," IEEE Trans. ASSP, vol. 34, no. 2,
/// pp. 317–330, Apr. 1986. DOI: 10.1109/TASSP.1986.1164824
///
/// Outline (cf. Section III): initialise the unknowns x_u by interpolation, then
/// for each outer round estimate AR(p) coefficients over a bounded window by
/// Burg's method (minimum-phase, stable), build the prediction-error filter
/// matrix A, partition it into unknown and known columns, and solve
/// `(A_u^T A_u + lambda I) x_u = -A_u^T A_k y_k` by LDLT — the Tikhonov term
/// covering a gap wider than the model's effective rank. Re-estimating from
/// progressively better x_u converges in 2-5 rounds on music.
///
/// @pre `end - start <= kDeclipMaxLpcGapSamples`. Both dense matrices are sized
/// from the gap, so the caller routes longer runs to interpolate_region instead.
///
/// The original formulation's clipping-consistency constraint
/// (|x_u[i]| >= clip_threshold) is omitted so this stays a general gap filler: a
/// spike that exceeds the threshold without being a clipping event should be
/// interpolated smoothly, not anchored at it.
void reconstruct_region_janssen(std::vector<float>& samples, size_t start, size_t end,
                                const DeclipConfig& config) {
  const size_t gap = end - start;
  const size_t requested_order = static_cast<size_t>(std::max(config.lpc_order, 0));
  // The order-derived term is caller-controlled and would otherwise pull the whole
  // input into the solver; cap it so the context is a function of the caps alone.
  const size_t context_radius = std::min(
      {samples.size(), kDeclipMaxLpcContextRadius, std::max<size_t>(4 * requested_order, 8 * gap)});
  const size_t context_start = start > context_radius ? start - context_radius : 0;
  const size_t context_end = std::min(samples.size(), end + context_radius);
  std::vector<float> context(samples.begin() + static_cast<std::ptrdiff_t>(context_start),
                             samples.begin() + static_cast<std::ptrdiff_t>(context_end));
  const size_t local_start = start - context_start;
  const size_t local_end = end - context_start;

  // Step 1 – cubic / linear initialisation of the unknown region. Keep a copy of
  // the interpolated baseline so the final LPC estimate can be blended against it
  // (config.lpc_blend); the fallback gets weight (1 - lpc_blend).
  std::vector<float> baseline(gap);
  for (size_t j = local_start; j < local_end; ++j) {
    context[j] = interpolate_fallback(context, local_start, local_end, j);
    baseline[j - local_start] = context[j];
  }
  std::copy(context.begin() + static_cast<std::ptrdiff_t>(local_start),
            context.begin() + static_cast<std::ptrdiff_t>(local_end),
            samples.begin() + static_cast<std::ptrdiff_t>(start));

  const size_t n = context.size();
  const int max_order = std::min(config.lpc_order, static_cast<int>(std::max<size_t>(1, n / 4)));
  // Skip LPC refinement when there isn't enough context or model order for it to
  // outperform the cubic / linear baseline. Short signals (n < 32) or very low
  // AR orders (< 4) tend to push the interpolated values toward the global mean
  // and undo the smooth shape produced by the cubic Hermite step.
  constexpr size_t kMinLpcSignalLength = 32;
  constexpr int kMinLpcOrder = 4;
  if (n < kMinLpcSignalLength || max_order < kMinLpcOrder) return;
  if (!can_use_lpc(n, max_order)) return;

  const int nu = static_cast<int>(gap);
  const int nrows = static_cast<int>(n) - max_order;  // number of filter-output rows
  if (nrows <= 0) return;

  const int iters = std::max(config.iterations, 1);
  // Blend weight for the LPC estimate vs. the Step-1 interpolation baseline,
  // clamped to [0, 1]. blend == 1 reproduces the pure-LPC behaviour; blend == 0
  // leaves the cubic / linear interpolation untouched.
  const float blend = std::clamp(config.lpc_blend, 0.0f, 1.0f);
  Eigen::MatrixXf au(nrows, nu);
  Eigen::VectorXf rhs(nrows);
  Eigen::MatrixXf ata(nu, nu);
  Eigen::VectorXf at_rhs(nu);

  for (int outer = 0; outer < iters; ++outer) {
    // Step 2a – AR estimation from the bounded local context.
    const auto model = sonare::lpc_burg(context.data(), n, max_order);
    const int p = max_order;

    // Step 2b – build A_u and A_k vectors for the normal equations.
    // A is (nrows x n) Toeplitz.  Row r (r = 0..nrows-1) acts at sample r+p:
    //   (A x)[r] = x[r+p] + a[1]*x[r+p-1] + ... + a[p]*x[r]
    // We only need A_u (nrows x nu) and the product A_k * y_k.

    // A_u: each column j corresponds to the contiguous local gap sample.
    au.setZero();

    // rhs = -A_k y_k, initialised as -A * samples (all columns),
    // then we subtract A_u * x_u to isolate A_k * y_k.
    rhs.setZero();

    for (int r = 0; r < nrows; ++r) {
      // Compute (A * samples)[r] = sum_k a[k] * samples[r+p-k] for k=0..p
      double Ax_r = 0.0;
      for (int k = 0; k <= p; ++k) {
        Ax_r += static_cast<double>(model.ar[static_cast<size_t>(k)]) *
                context[static_cast<size_t>(r + p - k)];
      }
      rhs[r] = -static_cast<float>(Ax_r);  // start with -A*samples

      // Now fill A_u columns for this row and adjust rhs to isolate A_k*y_k.
      for (int j = 0; j < nu; ++j) {
        const int col = static_cast<int>(local_start) + j;
        // A[r, col] is the coefficient for position col in row r.
        // Row r acts at time r+p; A[r, col] = a[r+p - col] if 0 <= r+p-col <= p, else 0.
        const int lag = (r + p) - col;
        if (lag >= 0 && lag <= p) {
          const float a_lag = model.ar[static_cast<size_t>(lag)];
          au(r, j) = a_lag;
          // rhs = -A_k*y_k = -(A*x)[r] + A_u*x_u → we need to add back A_u[r,j]*x_u[j]
          rhs[r] += a_lag * context[static_cast<size_t>(col)];
        }
      }
    }
    // rhs now holds -A_k * y_k (correctly isolating known-column contributions).

    // Step 2d – solve (A_u^T A_u + lambda I) x_u = A_u^T rhs
    // Tikhonov regularization lambda guards rank-deficient systems (gap > model rank).
    const float lambda = static_cast<float>(nrows) * kEpsilon;
    ata.noalias() = au.transpose() * au;
    ata.diagonal().array() += lambda;
    at_rhs.noalias() = au.transpose() * rhs;

    Eigen::LDLT<Eigen::MatrixXf> solver(ata);
    if (solver.info() != Eigen::Success) break;  // numerical failure: keep current estimate

    const Eigen::VectorXf x_u = solver.solve(at_rhs);
    if (!x_u.allFinite()) break;

    // Step 2e – write the LDLT estimate back into the unknown positions, blended
    // against the Step-1 interpolation baseline by config.lpc_blend. No clipping
    // constraint is applied: see the function-level note for why we treat declip
    // as a general gap-filler rather than a strict clipping-consistency
    // reconstructor.
    for (int j = 0; j < nu; ++j) {
      const size_t idx = local_start + static_cast<size_t>(j);
      context[idx] = blend * x_u[j] + (1.0f - blend) * baseline[idx - local_start];
    }
  }

  std::copy(context.begin() + static_cast<std::ptrdiff_t>(local_start),
            context.begin() + static_cast<std::ptrdiff_t>(local_end),
            samples.begin() + static_cast<std::ptrdiff_t>(start));
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
  if (run.end - run.start > kDeclipMaxLpcGapSamples) {
    interpolate_region(samples, run.start, run.end);
    ++report.interpolated_runs;
  } else {
    reconstruct_region_janssen(samples, run.start, run.end, config);
    ++report.lpc_reconstructed_runs;
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
