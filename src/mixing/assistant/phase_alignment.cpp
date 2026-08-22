/// @file phase_alignment.cpp
/// @brief Pairwise time and polarity measurement for the mixing assistant.

#include "mixing/assistant/phase_alignment.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include "core/fft.h"
#include "util/constants.h"

namespace sonare::mixing::assistant {
namespace {

using sonare::constants::kEpsilon;

/// Number of energy blocks one analysis window is divided into. The excerpt's
/// position is resolved to this granularity, so 16 blocks put a 1 s window
/// within ~60 ms of the joint activity peak while the envelope scan stays
/// negligible next to the correlation it feeds.
constexpr std::size_t kEnergyBlocksPerWindow = 16;

/// Shortest overlap, in samples, a correlation is trusted over. Below this the
/// peak is decided by the handful of samples that happen to line up rather than
/// by the material, which produces a confident-looking answer from nothing.
constexpr int kMinCorrelationSamples = 256;

/// How much larger one `|r|` has to be before it displaces a peak found at a
/// smaller offset. The products come out of a float transform, so two lags
/// carrying the same correlation on paper differ in the last digit or two;
/// without a margin the documented "a tie resolves to the smallest offset"
/// would be settled by rounding noise instead. Set above the transform's worst
/// measured deviation from an exact double correlation over the default
/// geometry -- 2.2e-7 in `r`, on a pure tone, where every lag a period apart is
/// a near-tie -- and far below any difference that means something, since `r`
/// lives in [-1, 1] and a pair is only acted on above 0.5.
constexpr float kPeakTieTolerance = 1.0e-6f;

/// Cost counters for the call in progress. Thread-local rather than atomic:
/// the pass is documented offline / control-thread only, and two threads
/// analysing different mixes should each read their own figures rather than
/// each other's.
thread_local PhaseAlignmentCost g_cost;

/// One track's activity over time, at the granularity the window search needs.
struct ActivityEnvelope {
  /// False when the track cannot be measured at all: unusable profile, null
  /// buffer, non-positive sample rate, no frames, or silence throughout.
  bool measurable = false;
  std::size_t block_size = 1;
  /// Per-block energy scaled so the loudest block reads 1. Scaling each track
  /// to its own peak is what lets the two curves be compared: an absolute
  /// energy comparison would just pick whichever window the louder track likes.
  std::vector<float> activity;
};

/// Mono sum of one frame. Stereo folds to `0.5 * (L + R)`; the correlation is
/// normalized afterwards, so the factor changes nothing but keeps the excerpt
/// in the same range as a mono track's.
float mono_sample(const TrackInput& track, std::size_t frame) noexcept {
  const float left = track.left[frame];
  if (track.right == nullptr) return left;
  return 0.5f * (left + track.right[frame]);
}

std::size_t window_samples_for(int sample_rate, float window_sec) noexcept {
  const double samples = static_cast<double>(window_sec) * static_cast<double>(sample_rate);
  return static_cast<std::size_t>(std::max(1.0, std::floor(samples)));
}

ActivityEnvelope build_envelope(const TrackInput& track, bool usable, float window_sec) {
  ActivityEnvelope envelope;
  if (!usable || track.left == nullptr || track.frame_count == 0 || track.sample_rate <= 0) {
    return envelope;
  }

  const std::size_t window = window_samples_for(track.sample_rate, window_sec);
  envelope.block_size = std::max<std::size_t>(1, window / kEnergyBlocksPerWindow);
  const std::size_t block_count =
      (track.frame_count + envelope.block_size - 1) / envelope.block_size;

  std::vector<double> energy(block_count, 0.0);
  double peak = 0.0;
  for (std::size_t block = 0; block < block_count; ++block) {
    const std::size_t begin = block * envelope.block_size;
    const std::size_t end = std::min(begin + envelope.block_size, track.frame_count);
    double sum = 0.0;
    for (std::size_t frame = begin; frame < end; ++frame) {
      const double value = mono_sample(track, frame);
      sum += value * value;
    }
    energy[block] = sum;
    peak = std::max(peak, sum);
  }
  // A track with no energy anywhere has nothing to correlate against, and
  // normalising by its peak would divide by zero. NaN input lands here too:
  // the comparison is false for a NaN peak, so the track drops out unmeasured.
  if (!(peak > kEpsilon)) return envelope;

  envelope.activity.resize(block_count);
  for (std::size_t block = 0; block < block_count; ++block) {
    envelope.activity[block] = static_cast<float>(energy[block] / peak);
  }
  envelope.measurable = true;
  return envelope;
}

/// First sample of the excerpt both tracks of a pair are measured over.
/// @details The two tracks share one window position, so the lag the
///          correlation finds is a real arrival difference rather than an
///          artefact of comparing two different passages. Among the candidate
///          positions the one maximising the summed `min(activity)` wins, which
///          is the stretch where both tracks are simultaneously at their most
///          active — a sum would instead be satisfied by one loud track over the
///          other's silence, where there is no relationship to measure.
std::size_t choose_window_start(const ActivityEnvelope& reference, const ActivityEnvelope& target,
                                std::size_t shared_frames, std::size_t window_samples) {
  if (shared_frames <= window_samples) return 0;

  const std::size_t block_size = reference.block_size;
  const std::size_t shared_blocks =
      std::min({reference.activity.size(), target.activity.size(), shared_frames / block_size});
  const std::size_t window_blocks = std::max<std::size_t>(1, window_samples / block_size);
  if (shared_blocks <= window_blocks) return 0;

  const auto joint = [&](std::size_t block) {
    return std::min(reference.activity[block], target.activity[block]);
  };

  double running = 0.0;
  for (std::size_t block = 0; block < window_blocks; ++block) running += joint(block);
  double best = running;
  std::size_t best_block = 0;
  for (std::size_t block = window_blocks; block < shared_blocks; ++block) {
    running += joint(block) - joint(block - window_blocks);
    if (running > best) {
      best = running;
      best_block = block + 1 - window_blocks;
    }
  }

  return std::min(best_block * block_size, shared_frames - window_samples);
}

std::vector<float> mono_excerpt(const TrackInput& track, std::size_t start, std::size_t count) {
  std::vector<float> excerpt(count, 0.0f);
  for (std::size_t i = 0; i < count; ++i) {
    excerpt[i] = mono_sample(track, start + i);
  }
  return excerpt;
}

struct CorrelationPeak {
  int lag = 0;
  float value = 0.0f;
  bool measured = false;
};

/// Smallest power of two that is at least @p value, and never below 2.
int transform_size_for(int value) noexcept {
  int size = 2;
  while (size < value) size *= 2;
  return size;
}

/// Transform plans, kept for the lifetime of one call.
/// @details Building a plan is linear in the transform size and every pair of a
///          normal session asks for the same size, so allocating one per pair
///          would cost more than the transform it serves. Tracks of different
///          lengths or sample rates produce a handful of sizes at most, which a
///          linear scan resolves faster than a hash would.
class TransformCache {
 public:
  FFT& plan(int size) {
    for (const auto& entry : plans_) {
      if (entry.first == size) return *entry.second;
    }
    plans_.emplace_back(size, std::make_unique<FFT>(size));
    return *plans_.back().second;
  }

 private:
  std::vector<std::pair<int, std::unique_ptr<FFT>>> plans_;
};

/// Buffers the transform reuses across pairs, so the quadratic loop allocates
/// once per call rather than once per pair.
struct CorrelationScratch {
  std::vector<float> reference;
  std::vector<float> target;
  std::vector<std::complex<float>> reference_spectrum;
  std::vector<std::complex<float>> target_spectrum;
  std::vector<float> products;
};

/// Unnormalized correlation products for every lag in `[-lag_range, lag_range]`.
/// @details `products[lag + lag_range]` is
///          `sum_i reference[lag_range + i] * target[lag_range + lag + i]` over
///          the core segment, which is the same sum the lag-by-lag form
///          computes. It is taken through one transform pair instead: the
///          direct form costs `(2 * lag_range + 1) * core` multiply-adds, which
///          at the default geometry is around 130 million per pair and is what
///          made a full mix's worth of pairs a minutes-long wait.
///
///          This is not a coarse stage. The transform evaluates every lag the
///          direct search evaluates, at full sample resolution, and the
///          reported correlation is recomputed in double at the winning lag —
///          so the transform decides only which lag wins, never what the
///          measurement says.
///
///          The transform length only has to reach the target excerpt's length:
///          the reference contributes `core` samples starting at index 0, so
///          the largest index any in-range lag touches is `core - 1 + 2 *
///          lag_range`, one short of that length. Nothing wraps into the
///          reported range, which is what a circular correlation has to be kept
///          away from.
void correlation_products(const std::vector<float>& reference, const std::vector<float>& target,
                          int core, int lag_range, TransformCache& cache,
                          CorrelationScratch& scratch) {
  const int length = core + 2 * lag_range;
  const int size = transform_size_for(length);
  FFT& fft = cache.plan(size);
  const int bins = fft.n_bins();

  scratch.reference.assign(static_cast<std::size_t>(size), 0.0f);
  scratch.target.assign(static_cast<std::size_t>(size), 0.0f);
  std::copy(reference.begin() + lag_range, reference.begin() + lag_range + core,
            scratch.reference.begin());
  std::copy(target.begin(), target.begin() + length, scratch.target.begin());

  scratch.reference_spectrum.resize(static_cast<std::size_t>(bins));
  scratch.target_spectrum.resize(static_cast<std::size_t>(bins));
  fft.forward(scratch.reference.data(), scratch.reference_spectrum.data());
  fft.forward(scratch.target.data(), scratch.target_spectrum.data());
  for (int bin = 0; bin < bins; ++bin) {
    const std::size_t index = static_cast<std::size_t>(bin);
    scratch.target_spectrum[index] *= std::conj(scratch.reference_spectrum[index]);
  }
  scratch.products.assign(static_cast<std::size_t>(size), 0.0f);
  fft.inverse(scratch.target_spectrum.data(), scratch.products.data());
}

/// Exact product at one lag, in double.
/// @details The reported correlation is this, not the transform's float
///          estimate of it: the transform ranks the lags and this says what the
///          winner measures, so a value a caller acts on carries the same
///          precision the lag-by-lag form gave it.
double exact_product(const std::vector<float>& reference, const std::vector<float>& target,
                     int core, int lag_range, int lag) {
  // The one core-length product pass the search makes per pair. Counted here
  // because this is where the work happens; a search that goes back to one pass
  // per lag has to count each of them or PhaseAlignmentCost stops describing
  // anything.
  ++g_cost.core_product_passes;
  double product = 0.0;
  const int offset = lag_range + lag;
  for (int i = 0; i < core; ++i) {
    product += static_cast<double>(reference[static_cast<std::size_t>(lag_range + i)]) *
               static_cast<double>(target[static_cast<std::size_t>(offset + i)]);
  }
  return product;
}

/// Strongest `|r|` over `[-lag_range, lag_range]`, both excerpts sharing one
/// time origin and one length.
/// @details The reference contribution is fixed to the core segment
///          `[lag_range, lag_range + core)` so that every lag reads the target
///          entirely from inside the excerpt. Both sides are normalized by
///          their own energy over exactly the samples that entered the product,
///          which is what keeps `r` in `[-1, 1]` and keeps a loud track from
///          winning every lag on amplitude alone.
CorrelationPeak best_correlation(const std::vector<float>& reference,
                                 const std::vector<float>& target, int lag_range,
                                 TransformCache& cache, CorrelationScratch& scratch) {
  CorrelationPeak peak;
  const int length = static_cast<int>(reference.size());
  const int core = length - 2 * lag_range;
  if (core < kMinCorrelationSamples) return peak;

  const int base = lag_range;
  double reference_energy = 0.0;
  for (int i = 0; i < core; ++i) {
    const double value = reference[static_cast<std::size_t>(base + i)];
    reference_energy += value * value;
  }
  // Silence (or a non-finite excerpt, for which the comparison is false) is
  // dropped rather than divided by: one NaN escaping here would poison every
  // subsequent maximum.
  if (!(reference_energy > kEpsilon)) return peak;

  correlation_products(reference, target, core, lag_range, cache, scratch);

  // Energy of the target's lag window, slid rather than rescanned: one sample
  // leaves the front and one enters at the back per lag, which turns an O(core)
  // pass per lag into O(1). Seeded at the lowest lag, whose window starts at 0.
  double target_energy = 0.0;
  for (int i = 0; i < core; ++i) {
    const double value = target[static_cast<std::size_t>(i)];
    target_energy += value * value;
  }

  std::vector<float> correlation(static_cast<std::size_t>(2 * lag_range + 1), 0.0f);
  std::vector<double> denominators(static_cast<std::size_t>(2 * lag_range + 1), 0.0);
  for (int lag = -lag_range; lag <= lag_range; ++lag) {
    const int offset = base + lag;
    if (lag > -lag_range) {
      const double leaving = target[static_cast<std::size_t>(offset - 1)];
      const double entering = target[static_cast<std::size_t>(offset + core - 1)];
      target_energy += entering * entering - leaving * leaving;
    }
    const std::size_t slot = static_cast<std::size_t>(lag + lag_range);
    // The slid sum can drift a hair below zero on a near-silent window; clamp
    // before the square root so the guard below sees a real number.
    const double denominator = std::sqrt(reference_energy * std::max(target_energy, 0.0));
    if (!(denominator > kEpsilon)) continue;
    denominators[slot] = denominator;
    correlation[slot] = static_cast<float>(
        std::clamp(static_cast<double>(scratch.products[slot]) / denominator, -1.0, 1.0));
  }

  // Peak of |r|, not of r: a polarity-opposed pair's true peak is negative, and
  // searching the signed maximum would settle for whatever weak positive bump
  // sits nearby instead. Candidates are visited by increasing |lag| so a tie
  // resolves to the smallest offset rather than to whichever end was scanned
  // first.
  float best_magnitude = -1.0f;
  for (int magnitude = 0; magnitude <= lag_range; ++magnitude) {
    const int candidates[2] = {-magnitude, magnitude};
    const int candidate_count = (magnitude == 0) ? 1 : 2;
    for (int candidate = 0; candidate < candidate_count; ++candidate) {
      const int lag = candidates[candidate];
      const float value = correlation[static_cast<std::size_t>(lag + lag_range)];
      if (std::abs(value) > best_magnitude + kPeakTieTolerance) {
        best_magnitude = std::abs(value);
        peak.lag = lag;
        peak.value = value;
      }
    }
  }

  // The winner's value is retaken exactly; the transform only chose it.
  const std::size_t winner = static_cast<std::size_t>(peak.lag + lag_range);
  if (denominators[winner] > kEpsilon) {
    peak.value = static_cast<float>(std::clamp(
        exact_product(reference, target, core, lag_range, peak.lag) / denominators[winner], -1.0,
        1.0));
  }
  peak.measured = true;
  return peak;
}

void measure_pair(const TrackInput& reference_track, const ActivityEnvelope& reference_envelope,
                  const TrackInput& target_track, const ActivityEnvelope& target_envelope,
                  const PhaseAlignmentConfig& config, TransformCache& cache,
                  CorrelationScratch& scratch, PairAlignment& pair) {
  if (!reference_envelope.measurable || !target_envelope.measurable) return;
  // A lag counted in samples only means the same thing on both sides when both
  // sides count samples at the same rate.
  if (reference_track.sample_rate != target_track.sample_rate) return;

  const int sample_rate = reference_track.sample_rate;
  const std::size_t shared_frames = std::min(reference_track.frame_count, target_track.frame_count);
  const std::size_t window = window_samples_for(sample_rate, config.analysis_window_sec);
  // A track shorter than the window is measured over its real length.
  const std::size_t region = std::min(shared_frames, window);

  const double requested_lag =
      static_cast<double>(config.max_lag_ms) * 0.001 * static_cast<double>(sample_rate);
  int lag_range = static_cast<int>(std::lround(requested_lag));
  // The search eats lag_range samples off each end of the excerpt, so a short
  // excerpt buys its lag range out of the overlap that pays for the estimate.
  const int region_limit = (static_cast<int>(region) - kMinCorrelationSamples) / 2;
  lag_range = std::min(lag_range, region_limit);
  if (lag_range < 0) return;

  const std::size_t start =
      choose_window_start(reference_envelope, target_envelope, shared_frames, region);
  const std::vector<float> reference = mono_excerpt(reference_track, start, region);
  const std::vector<float> target = mono_excerpt(target_track, start, region);

  const CorrelationPeak peak = best_correlation(reference, target, lag_range, cache, scratch);
  if (!peak.measured) return;
  ++g_cost.measured_pairs;

  pair.lag_samples = peak.lag;
  pair.correlation = peak.value;
  pair.polarity_opposed = peak.value < 0.0f;
  pair.related = std::abs(peak.value) >= config.min_abs_correlation;
}

PhaseAlignmentConfig sanitize(const PhaseAlignmentConfig& config) {
  PhaseAlignmentConfig sanitized;
  sanitized.max_lag_ms = std::clamp(config.max_lag_ms, 0.0f, kMaxSearchLagMs);
  sanitized.min_abs_correlation = std::clamp(config.min_abs_correlation, 0.0f, 1.0f);
  sanitized.analysis_window_sec =
      std::clamp(config.analysis_window_sec, kMinAnalysisWindowSec, kMaxAnalysisWindowSec);
  // A NaN survives std::clamp, so the defaults are restored explicitly.
  if (!std::isfinite(sanitized.max_lag_ms)) sanitized.max_lag_ms = kDefaultMaxLagMs;
  if (!std::isfinite(sanitized.min_abs_correlation)) {
    sanitized.min_abs_correlation = kDefaultMinAbsCorrelation;
  }
  if (!std::isfinite(sanitized.analysis_window_sec)) {
    sanitized.analysis_window_sec = kDefaultAnalysisWindowSec;
  }
  return sanitized;
}

}  // namespace

PhaseAlignmentCost last_phase_alignment_cost() noexcept { return g_cost; }

std::vector<PairAlignment> analyze_phase_alignment(const std::vector<TrackInput>& tracks,
                                                   const std::vector<TrackProfile>& profiles,
                                                   const PhaseAlignmentConfig& config) {
  // Reset first, so a call that returns early still describes itself rather
  // than leaving the previous call's figures readable as its own.
  g_cost = PhaseAlignmentCost{};

  std::vector<PairAlignment> alignments;
  const std::size_t track_count = tracks.size();
  if (track_count < 2) return alignments;
  alignments.reserve(track_count * (track_count - 1) / 2);

  const PhaseAlignmentConfig sanitized = sanitize(config);

  // Envelopes are built once per track rather than once per pair: the pass is
  // quadratic in track count and this part of it does not have to be.
  std::vector<ActivityEnvelope> envelopes(track_count);
  for (std::size_t index = 0; index < track_count; ++index) {
    const bool usable = index < profiles.size() && profiles[index].usable;
    envelopes[index] = build_envelope(tracks[index], usable, sanitized.analysis_window_sec);
  }

  // One plan set and one set of buffers for the whole quadratic loop.
  TransformCache cache;
  CorrelationScratch scratch;
  for (std::size_t reference = 0; reference < track_count; ++reference) {
    for (std::size_t target = reference + 1; target < track_count; ++target) {
      PairAlignment pair;
      pair.reference_index = static_cast<int>(reference);
      pair.target_index = static_cast<int>(target);
      measure_pair(tracks[reference], envelopes[reference], tracks[target], envelopes[target],
                   sanitized, cache, scratch, pair);
      alignments.push_back(pair);
    }
  }
  return alignments;
}

}  // namespace sonare::mixing::assistant
