#include "editing/polyphony/multi_f0.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <vector>

#include "core/spectrum.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/insertion_sort.h"
#include "util/math_utils.h"

namespace sonare::editing::polyphony {
namespace {

using sonare::constants::kCentsPerOctave;

float cents_between(float hz, float reference_hz) {
  return kCentsPerOctave * std::log2(hz / reference_hz);
}

/// A ridge while it is still taking candidates; @c peak is the running maximum
/// the salience rule is read against, @c gap the frames since it last took one.
struct LiveRidge {
  int frame_start = 0;
  std::vector<float> f0_hz;
  std::vector<float> salience;
  /// F0Candidate::attack_frames of each frame's candidate, 0 on a filled frame.
  std::vector<float> attack;
  float peak = 0.0f;
  int gap = 0;
  /// Leading frames place_onsets filled in, which carry no candidate of their own.
  int filled = 0;
};

/// Appends @p taken after the gap frames, filled linearly from the last value.
void take_after_gap(LiveRidge& ridge, const F0Candidate& taken) {
  const float from_hz = ridge.f0_hz.back();
  const float from_salience = ridge.salience.back();
  const float steps = static_cast<float>(ridge.gap + 1);
  for (int i = 1; i <= ridge.gap; ++i) {
    const float t = static_cast<float>(i) / steps;
    ridge.f0_hz.push_back(from_hz + t * (taken.f0_hz - from_hz));
    ridge.salience.push_back(from_salience + t * (taken.salience - from_salience));
    ridge.attack.push_back(0.0f);
  }
  ridge.gap = 0;
  ridge.f0_hz.push_back(taken.f0_hz);
  ridge.salience.push_back(taken.salience);
  ridge.attack.push_back(taken.attack_frames);
  ridge.peak = std::max(ridge.peak, taken.salience);
}

/// Re-attack rise window: holds a struck attack but too little of a 1-2 Hz
/// beat's climb to reach the ratio.
constexpr float kReattackWindowMs = 35.0f;

/// @brief Cuts each ridge where its salience re-attacks, per
///        RidgeConfig::reattack_ratio; a ratio of 0 returns at once.
void split_reattacks(std::vector<LiveRidge>& ridges, double frame_ms, const RidgeConfig& config) {
  if (config.reattack_ratio == 0.0f) return;
  const int window =
      std::max(2, static_cast<int>(std::lround(static_cast<double>(kReattackWindowMs) / frame_ms)));
  // The min_duration_ms filter's own comparison, so no cut leaves a fragment it drops.
  const auto long_enough = [&](int frames) {
    return !(static_cast<double>(frames) * frame_ms < config.min_duration_ms);
  };

  std::vector<LiveRidge> split;
  split.reserve(ridges.size());
  for (LiveRidge& ridge : ridges) {
    const std::vector<float>& s = ridge.salience;
    const int end = static_cast<int>(s.size());
    std::vector<int> cuts;
    int frag_start = 0;
    // Filled frames are not followed evidence, so the first fragment's length excludes them.
    const auto frag_long_enough = [&](int frames) {
      return long_enough(frames - (frag_start == 0 ? ridge.filled : 0));
    };
    int t = frag_start + 2;
    while (t + window - 1 <= end - 1) {
      // Clipped at the fragment start so a cut never reads its pre-attack valley back.
      const int valley_from = std::max(frag_start, t - window - 1);
      const auto valley_at = std::min_element(s.begin() + valley_from, s.begin() + t - 1);
      const auto summit_at = std::max_element(s.begin() + frag_start, s.begin() + t - 1);
      // The level held before the climb is the window's highest frame, not its
      // lowest: a frame in which another candidate took this pitch's partials
      // first reads a fraction of the level, and a dip of one frame is not a decay.
      const float before = *std::max_element(s.begin() + valley_from, s.begin() + t - 1);
      const float crest = *std::max_element(s.begin() + t, s.begin() + t + window);
      // Only a valley after the fragment's summit: the note's own rising attack is not a re-attack.
      if (*valley_at > 0.0f && crest > config.reattack_ratio * before && summit_at < valley_at) {
        const int first = std::max(frag_start + 1, t - 2);
        int cut = first;
        const int last = std::min(end - 1, t + 2);
        for (int k = cut + 1; k <= last; ++k) {
          if (s[static_cast<size_t>(k)] - s[static_cast<size_t>(k - 1)] >
              s[static_cast<size_t>(cut)] - s[static_cast<size_t>(cut - 1)]) {
            cut = k;
          }
        }
        // Back from the largest rise to where the rise began, which sits nearer the strike.
        while (cut > first && s[static_cast<size_t>(cut - 2)] < s[static_cast<size_t>(cut - 1)]) {
          --cut;
        }
        if (frag_long_enough(cut - frag_start) && long_enough(end - cut)) {
          cuts.push_back(cut);
          frag_start = cut;
          t = cut + 2;
          continue;
        }
      }
      ++t;
    }

    if (cuts.empty()) {
      split.push_back(std::move(ridge));
      continue;
    }
    cuts.push_back(end);
    int from = 0;
    for (const int to : cuts) {
      LiveRidge fragment;
      fragment.frame_start = ridge.frame_start + from;
      fragment.filled = from == 0 ? ridge.filled : 0;
      fragment.f0_hz.assign(ridge.f0_hz.begin() + from, ridge.f0_hz.begin() + to);
      fragment.salience.assign(s.begin() + from, s.begin() + to);
      fragment.peak = *std::max_element(fragment.salience.begin(), fragment.salience.end());
      split.push_back(std::move(fragment));
      from = to;
    }
  }
  ridges = std::move(split);
}

/// @brief Moves each ridge's start to the strike its attack reading places, per
///        RidgeConfig::attack_window_ms; a window of 0 returns at once.
void place_onsets(std::vector<LiveRidge>& ridges, double frame_ms, const RidgeConfig& config) {
  if (config.attack_window_ms == 0.0f) return;
  const int window = std::max(
      1, static_cast<int>(std::lround(static_cast<double>(config.attack_window_ms) / frame_ms)));
  for (size_t i = 0; i < ridges.size(); ++i) {
    LiveRidge& ridge = ridges[i];
    const int size = static_cast<int>(ridge.f0_hz.size());
    // Read half a window in, where the reading's own window brackets the attack.
    const int read = std::min(window / 2, size - 1);
    const float back = ridge.attack[static_cast<size_t>(read)];
    if (!(back > 0.0f)) continue;
    int start = ridge.frame_start + read - static_cast<int>(std::lround(back));
    start = std::clamp(start, 0, ridge.frame_start + size - 1);
    // Never back into a ridge this one could have continued.
    for (size_t j = 0; j < ridges.size(); ++j) {
      const LiveRidge& other = ridges[j];
      if (j == i || other.frame_start >= ridge.frame_start) continue;
      const int other_end = other.frame_start + static_cast<int>(other.f0_hz.size());
      if (other_end > start && std::abs(cents_between(other.f0_hz.back(), ridge.f0_hz.front())) <=
                                   config.max_jump_cents) {
        start = std::max(start, other_end);
      }
    }
    if (start > ridge.frame_start) {
      const int drop = std::min(start - ridge.frame_start, size - 1);
      ridge.f0_hz.erase(ridge.f0_hz.begin(), ridge.f0_hz.begin() + drop);
      ridge.salience.erase(ridge.salience.begin(), ridge.salience.begin() + drop);
      ridge.attack.erase(ridge.attack.begin(), ridge.attack.begin() + drop);
      ridge.frame_start += drop;
    } else if (start < ridge.frame_start) {
      const size_t fill = static_cast<size_t>(ridge.frame_start - start);
      ridge.f0_hz.insert(ridge.f0_hz.begin(), fill, ridge.f0_hz.front());
      ridge.salience.insert(ridge.salience.begin(), fill, ridge.salience.front());
      ridge.attack.insert(ridge.attack.begin(), fill, 0.0f);
      ridge.frame_start = start;
      ridge.filled = static_cast<int>(fill);
    }
  }
}

/// Deepest sub-harmonic position read for the note a peak may stand on: the
/// octave, the twelfth and the double octave. Reading deeper promoted bass
/// images in textures and cost more real notes than it recovered.
constexpr int kSubharmonicsChecked = 4;
/// Harmonics of a taken note whose peak is read as that note's residual rather
/// than as a note: the octave and the twelfth, which is where a piano's
/// partials leave one. Measured on rendered repertoire, suppressing the double
/// octave and beyond lost melody notes over a bass and recovered few ghosts.
constexpr int kResidualHarmonics = 3;
/// What explaining_subharmonic returns for a peak standing on a note already taken.
constexpr int kExplainedByTaken = -1;

/// @p config with its F0 ceiling raised an octave, the axis the estimator searches.
SalienceConfig search_range(const SalienceConfig& config) {
  // The kernel checks the raised ceiling, so the configured one is checked here.
  SONARE_CHECK(std::isfinite(config.f0_max_hz) && config.f0_max_hz > config.f0_min_hz,
               ErrorCode::InvalidParameter);
  SalienceConfig search = config;
  search.f0_max_hz = 2.0f * config.f0_max_hz;
  return search;
}

}  // namespace

MultiF0Estimator::MultiF0Estimator(const CentAxis& spectrum_axis, const MultiF0Config& config)
    : kernel_(spectrum_axis, search_range(config.salience)), config_(config) {
  // Bounded above as well as below because the iteration count is derived from
  // it; what 64 refuses is on MultiF0Config::max_polyphony.
  SONARE_CHECK(config.max_polyphony >= 1 && config.max_polyphony <= kMaxPolyphonyVoices,
               ErrorCode::InvalidParameter);
  // The last bin an axis ending at the configured ceiling would have carried.
  report_max_hz_ =
      kernel_.f0_axis().hz_at(std::ceil(kernel_.f0_axis().bin_at(config.salience.f0_max_hz)));
  SONARE_CHECK(std::isfinite(config.min_frame_peak_ratio) && config.min_frame_peak_ratio >= 0.0f &&
                   config.min_frame_peak_ratio <= 1.0f,
               ErrorCode::InvalidParameter);
  SONARE_CHECK(std::isfinite(config.min_separation_cents) && config.min_separation_cents >= 0.0f,
               ErrorCode::InvalidParameter);
  SONARE_CHECK(std::isfinite(config.subtraction_factor) && config.subtraction_factor > 0.0f &&
                   config.subtraction_factor <= 1.0f,
               ErrorCode::InvalidParameter);

  residual_.resize(static_cast<size_t>(kernel_.spectrum_axis().n_bins));
  salience_.resize(static_cast<size_t>(kernel_.f0_axis().n_bins));
}

std::vector<F0Candidate> MultiF0Estimator::estimate(const float* column) const {
  const int n_spectrum_bins = kernel_.spectrum_axis().n_bins;
  const int n_f0_bins = kernel_.f0_axis().n_bins;
  residual_.assign(column, column + n_spectrum_bins);
  salience_.assign(static_cast<size_t>(n_f0_bins), 0.0f);

  // The share denominator is the column as it arrived, so every candidate of one
  // frame is measured against the same total.
  double total = 0.0;
  for (int bin = 0; bin < n_spectrum_bins; ++bin) total += column[bin];

  std::vector<F0Candidate> candidates;
  float first_peak = 0.0f;
  bool have_first_peak = false;
  // Bounded so that a column whose peaks never fall cannot spin on the
  // separation rule, which accepts nothing and still iterates.
  const int max_iterations = config_.max_polyphony * 3;
  for (int iteration = 0;
       iteration < max_iterations && static_cast<int>(candidates.size()) < config_.max_polyphony;
       ++iteration) {
    kernel_.evaluate(residual_.data(), salience_.data());
    const int peak =
        static_cast<int>(std::max_element(salience_.begin(), salience_.end()) - salience_.begin());
    if (!have_first_peak) {
      first_peak = salience_[static_cast<size_t>(peak)];
      have_first_peak = true;
    }
    if (!(first_peak > 0.0f) ||
        salience_[static_cast<size_t>(peak)] < config_.min_frame_peak_ratio * first_peak) {
      break;
    }
    // Parabolic interpolation over a bin and its neighbours, so a returned F0
    // is not confined to the axis.
    const auto refined_hz = [&](int bin) {
      const float below = salience_[static_cast<size_t>(std::max(bin - 1, 0))];
      const float centre = salience_[static_cast<size_t>(bin)];
      const float above = salience_[static_cast<size_t>(std::min(bin + 1, n_f0_bins - 1))];
      const float denominator = below - 2.0f * centre + above;
      const float offset = denominator != 0.0f
                               ? std::clamp(0.5f * (below - above) / denominator, -1.0f, 1.0f)
                               : 0.0f;
      return kernel_.f0_axis().hz_at(static_cast<float>(bin) + offset);
    };

    // The frame cut above read the peak; what is taken may be the note under it.
    const int explained = explaining_subharmonic(peak, refined_hz(peak), candidates);
    if (explained == kExplainedByTaken) {
      // Cleared so the loop moves on to the next voice rather than meeting it again.
      kernel_.subtract(residual_.data(), peak, config_.subtraction_factor);
      continue;
    }
    const int taken_bin = explained;
    const float centre = salience_[static_cast<size_t>(taken_bin)];
    const float f0_hz = refined_hz(taken_bin);

    // Subtracted before the separation rule runs, so a rejected peak is gone
    // rather than left to be taken again.
    const float removed = kernel_.subtract(residual_.data(), taken_bin, config_.subtraction_factor);

    // Over the configured range: found as itself, not handed back as its sub-octave.
    if (f0_hz > report_max_hz_) continue;

    const bool too_close =
        std::any_of(candidates.begin(), candidates.end(), [&](const F0Candidate& taken) {
          return std::abs(cents_between(f0_hz, taken.f0_hz)) < config_.min_separation_cents;
        });
    if (too_close) continue;

    F0Candidate candidate;
    candidate.f0_hz = f0_hz;
    candidate.salience = centre;
    candidate.harmonic_share =
        total > 0.0 ? std::clamp(static_cast<float>(removed / total), 0.0f, 1.0f) : 0.0f;
    candidates.push_back(candidate);
  }

  return candidates;
}

int MultiF0Estimator::explaining_subharmonic(int peak, float peak_hz,
                                             const std::vector<F0Candidate>& taken) const {
  const int n_f0_bins = kernel_.f0_axis().n_bins;
  const float cents_per_bin = kernel_.f0_axis().cents_per_bin;
  const float bins_per_octave = kCentsPerOctave / cents_per_bin;
  for (int every = kSubharmonicsChecked; every >= 2; --every) {
    const int middle = static_cast<int>(std::lround(
        static_cast<float>(peak) - std::log2(static_cast<float>(every)) * bins_per_octave));
    if (middle < 0) continue;
    // A note already taken this frame explains the peak as what its subtraction
    // left -- the partials over the kernel's reach, and the stretched ones -- when
    // the peak lies within the bin the kernel reads that partial over.
    const bool taken_here =
        std::any_of(taken.begin(), taken.end(), [&](const F0Candidate& candidate) {
          return std::abs(cents_between(peak_hz, static_cast<float>(every) * candidate.f0_hz)) <=
                 cents_per_bin;
        });
    if (taken_here) return every <= kResidualHarmonics ? kExplainedByTaken : peak;
    // The local maximum about the ideal position, so a stretched series still lands.
    int best = -1;
    for (int bin = std::max(0, middle - 1); bin <= std::min(middle + 1, n_f0_bins - 1); ++bin) {
      if (best < 0 || salience_[static_cast<size_t>(bin)] > salience_[static_cast<size_t>(best)]) {
        best = bin;
      }
    }
    const float whole = salience_[static_cast<size_t>(best)];
    if (!(whole > 0.0f)) continue;
    // The peak has to sit on this candidate's partial, within the bin the kernel
    // reads it over; a note a quarter-tone off the octave is its own note.
    const float best_hz = kernel_.f0_axis().hz_at(static_cast<float>(best));
    if (std::abs(cents_between(peak_hz, static_cast<float>(every) * best_hz)) > cents_per_bin) {
      continue;
    }
    // Three readings, because a texture fills any low bin with other notes'
    // partials: the unshared sum has to be most of the candidate's own, to rival
    // the peak, and to rest on a fundamental of its own rather than on partials
    // that are another note's.
    const float own = kernel_.evaluate_excluding(residual_.data(), best, every);
    if (own < kSubharmonicOwnShare * whole) continue;
    if (own < kSubharmonicPeakShare * salience_[static_cast<size_t>(peak)]) continue;
    if (kernel_.partial_term(residual_.data(), best, 1) < kSubharmonicFundamentalShare * own) {
      continue;
    }
    return best;
  }
  return peak;
}

std::vector<F0Ridge> track_f0_ridges(const std::vector<std::vector<F0Candidate>>& frames,
                                     int hop_length, int sample_rate, const RidgeConfig& config) {
  SONARE_CHECK(hop_length > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(sample_rate > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(std::isfinite(config.max_jump_cents) && config.max_jump_cents > 0.0f,
               ErrorCode::InvalidParameter);
  SONARE_CHECK(std::isfinite(config.min_ridge_peak_ratio) && config.min_ridge_peak_ratio >= 0.0f &&
                   config.min_ridge_peak_ratio <= 1.0f,
               ErrorCode::InvalidParameter);
  SONARE_CHECK(std::isfinite(config.min_duration_ms) && config.min_duration_ms >= 0.0f,
               ErrorCode::InvalidParameter);
  SONARE_CHECK(std::isfinite(config.reattack_ratio) &&
                   (config.reattack_ratio == 0.0f || config.reattack_ratio > 1.0f),
               ErrorCode::InvalidParameter);
  SONARE_CHECK(std::isfinite(config.max_gap_ms) && config.max_gap_ms >= 0.0f,
               ErrorCode::InvalidParameter);
  SONARE_CHECK(std::isfinite(config.attack_window_ms) && config.attack_window_ms >= 0.0f,
               ErrorCode::InvalidParameter);
  for (const std::vector<F0Candidate>& frame : frames) {
    for (const F0Candidate& candidate : frame) {
      // The salience too: a NaN would make the candidate ordering below
      // intransitive rather than merely arbitrary.
      SONARE_CHECK(std::isfinite(candidate.f0_hz) && candidate.f0_hz > 0.0f &&
                       std::isfinite(candidate.salience) && std::isfinite(candidate.attack_frames),
                   ErrorCode::InvalidParameter);
    }
  }

  const double frame_ms =
      1000.0 * static_cast<double>(hop_length) / static_cast<double>(sample_rate);
  const int max_gap_frames =
      static_cast<int>(std::floor(static_cast<double>(config.max_gap_ms) / frame_ms));

  std::vector<LiveRidge> finished;
  std::vector<LiveRidge> active;
  for (size_t frame = 0; frame < frames.size(); ++frame) {
    // Descending salience with ties to the lower F0, so the same input always
    // produces the same ridges.
    std::vector<F0Candidate> ordered = frames[frame];
    insertion_sort(ordered.begin(), ordered.end(), [](const F0Candidate& a, const F0Candidate& b) {
      return a.salience != b.salience ? a.salience > b.salience : a.f0_hz < b.f0_hz;
    });
    std::vector<bool> claimed(ordered.size(), false);

    std::vector<LiveRidge> survivors;
    survivors.reserve(active.size() + ordered.size());
    // Ridges are offered in the order they started.
    for (LiveRidge& ridge : active) {
      int best = -1;
      float best_distance = 0.0f;
      for (size_t i = 0; i < ordered.size(); ++i) {
        if (claimed[i]) continue;
        const float distance = std::abs(cents_between(ordered[i].f0_hz, ridge.f0_hz.back()));
        if (best < 0 || distance < best_distance) {
          best = static_cast<int>(i);
          best_distance = distance;
        }
      }
      const bool takes =
          best >= 0 && best_distance <= config.max_jump_cents &&
          ordered[static_cast<size_t>(best)].salience >= config.min_ridge_peak_ratio * ridge.peak;
      if (!takes) {
        // A gap frame keeps the ridge in the running; it is filled once it resumes.
        if (ridge.gap < max_gap_frames) {
          ++ridge.gap;
          survivors.push_back(std::move(ridge));
        } else {
          finished.push_back(std::move(ridge));
        }
        continue;
      }
      const F0Candidate& taken = ordered[static_cast<size_t>(best)];
      claimed[static_cast<size_t>(best)] = true;
      take_after_gap(ridge, taken);
      survivors.push_back(std::move(ridge));
    }
    for (size_t i = 0; i < ordered.size(); ++i) {
      if (claimed[i]) continue;
      LiveRidge started;
      started.frame_start = static_cast<int>(frame);
      started.f0_hz.push_back(ordered[i].f0_hz);
      started.salience.push_back(ordered[i].salience);
      started.attack.push_back(ordered[i].attack_frames);
      started.peak = ordered[i].salience;
      survivors.push_back(std::move(started));
    }
    active = std::move(survivors);
  }
  finished.insert(finished.end(), std::make_move_iterator(active.begin()),
                  std::make_move_iterator(active.end()));

  place_onsets(finished, frame_ms, config);
  split_reattacks(finished, frame_ms, config);
  std::vector<F0Ridge> ridges;
  ridges.reserve(finished.size());
  for (LiveRidge& live : finished) {
    // Measured over the frames the tracker followed: a filled frame is not evidence.
    const auto followed = static_cast<double>(live.f0_hz.size()) - live.filled;
    if (followed * frame_ms < config.min_duration_ms) continue;
    F0Ridge ridge;
    ridge.frame_start = live.frame_start;
    ridge.f0_hz = std::move(live.f0_hz);
    ridge.salience = std::move(live.salience);
    // Frame to sample the way the rest of the analysis framing converts it: a
    // centred frame begins at its index times the hop.
    ridge.onset_sample = static_cast<int64_t>(ridge.frame_start) * hop_length;
    ridge.offset_sample = static_cast<int64_t>(ridge.frame_end()) * hop_length;
    ridge.median_hz = sonare::median(ridge.f0_hz.data(), ridge.f0_hz.size());
    ridges.push_back(std::move(ridge));
  }
  insertion_sort(ridges.begin(), ridges.end(), [](const F0Ridge& a, const F0Ridge& b) {
    return a.frame_start != b.frame_start ? a.frame_start < b.frame_start
                                          : a.median_hz < b.median_hz;
  });
  return ridges;
}

float MultiF0Track::frame_rate_hz() const noexcept {
  if (hop_length <= 0 || sample_rate <= 0) return 0.0f;
  return static_cast<float>(sample_rate) / static_cast<float>(hop_length);
}

namespace {

/// The last few STFT magnitude columns, for reading an attack without phase placement.
class MagnitudeHistory {
 public:
  MagnitudeHistory(int n_bins, int length)
      : n_bins_(n_bins), columns_(static_cast<size_t>(length) * static_cast<size_t>(n_bins)) {}

  /// Appends @p spec's frame @p frame as the newest column.
  void push(const Spectrogram& spec, int frame) {
    const int length = static_cast<int>(columns_.size()) / n_bins_;
    newest_ = (newest_ + 1) % length;
    count_ = std::min(count_ + 1, length);
    float* column = columns_.data() + static_cast<size_t>(newest_) * static_cast<size_t>(n_bins_);
    const std::complex<float>* data = spec.complex_data();
    const size_t n_frames = static_cast<size_t>(spec.n_frames());
    for (int bin = 0; bin < n_bins_; ++bin) {
      column[bin] =
          std::abs(data[static_cast<size_t>(bin) * n_frames + static_cast<size_t>(frame)]);
    }
  }

  int count() const noexcept { return count_; }

  /// The column @p back frames before the newest, which must be under count().
  const float* at(int back) const noexcept {
    const int length = static_cast<int>(columns_.size()) / n_bins_;
    const int index = (newest_ - back + length) % length;
    return columns_.data() + static_cast<size_t>(index) * static_cast<size_t>(n_bins_);
  }

 private:
  int n_bins_;
  std::vector<float> columns_;
  int newest_ = -1;
  int count_ = 0;
};

/// @brief F0Candidate::attack_frames for @p f0_hz over @p history.
/// @details The magnitude at the first @p n_harmonics partial bins is summed per
///          column; where the newest-to-oldest span doubles, the latest crossing
///          of the level halfway between its lowest and highest column is
///          interpolated between the two frames it falls between.
float attack_frames(const MagnitudeHistory& history, float f0_hz, float bin_hz, int n_harmonics,
                    int n_bins) {
  const int count = history.count();
  if (count < 2) return 0.0f;
  std::vector<float> summed(static_cast<size_t>(count), 0.0f);
  for (int back = 0; back < count; ++back) {
    const float* column = history.at(back);
    for (int harmonic = 1; harmonic <= n_harmonics; ++harmonic) {
      const int bin = static_cast<int>(std::lround(harmonic * f0_hz / bin_hz));
      if (bin >= n_bins) break;
      summed[static_cast<size_t>(back)] += column[bin];
    }
  }
  const float lowest = *std::min_element(summed.begin(), summed.end());
  const float highest = *std::max_element(summed.begin(), summed.end());
  if (!(highest > 2.0f * lowest)) return 0.0f;
  const float half = 0.5f * (lowest + highest);
  // Newest first: the latest column at or over the half level with one under it
  // before, so an earlier event's rise still in the history is not the one read.
  for (int back = 0; back + 1 < count; ++back) {
    const float above = summed[static_cast<size_t>(back)];
    const float below = summed[static_cast<size_t>(back + 1)];
    if (above < half || below >= half) continue;
    return static_cast<float>(back) + (above - half) / (above - below);
  }
  return 0.0f;
}

/// Partials the attack is read over. Measured on a C2 struck under an E5, whose
/// fundamental is C2's tenth partial: over all 20 the sum never doubles across
/// the history and the strike is not read; over 8 it is read 36 ms after the strike.
constexpr int kAttackPartials = 8;
/// Analysis windows the attack is read back over. One misses a note admitted
/// 60-100 ms after its strike (a bass note under a chord); three reached earlier
/// events and raised octave-below ghosts on rendered repertoire.
constexpr int kAttackHistoryWindows = 2;

/// Reads the attack of every candidate in @p frame against @p history.
void read_attacks(std::vector<F0Candidate>& candidates, const MagnitudeHistory& history,
                  float bin_hz, int n_bins) {
  for (F0Candidate& candidate : candidates) {
    candidate.attack_frames =
        attack_frames(history, candidate.f0_hz, bin_hz, kAttackPartials, n_bins);
  }
}

/// The track the per-frame candidates make, over @p audio's framing.
MultiF0Track track_from_frames(const Audio& audio,
                               const std::vector<std::vector<F0Candidate>>& frames, int hop_length,
                               const RidgeConfig& config) {
  MultiF0Track track;
  track.n_frames = static_cast<int>(frames.size());
  track.hop_length = hop_length;
  track.sample_rate = audio.sample_rate();
  track.polyphony.resize(frames.size());
  for (size_t frame = 0; frame < frames.size(); ++frame) {
    track.polyphony[frame] = static_cast<int>(frames[frame].size());
  }

  track.ridges = track_f0_ridges(frames, hop_length, audio.sample_rate(), config);

  // Centre padding puts the last frame's end past the audio, so every ridge
  // reaching it would otherwise report a span that cannot be sliced.
  const auto n_samples = static_cast<int64_t>(audio.size());
  for (F0Ridge& ridge : track.ridges) {
    ridge.onset_sample = std::min(ridge.onset_sample, n_samples);
    ridge.offset_sample = std::min(ridge.offset_sample, n_samples);
  }
  return track;
}

}  // namespace

MultiF0Track extract_multi_f0(const Audio& audio, const Spectrogram& spec,
                              const MultiF0ExtractorConfig& config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  // Checked rather than trusted: a spectrogram of another framing would put every
  // ridge on frames it never measured.
  validate_reused_geometry(spec, polyphony_stft_at_rate(config.stft, audio.sample_rate()),
                           audio.sample_rate(), audio.size());

  const CentSpectrum spectrum = compute_cent_spectrum(spec, config.spectrum);
  const MultiF0Estimator estimator(spectrum.axis, config.estimation);
  const bool read_attack = config.ridges.attack_window_ms != 0.0f;
  const float bin_hz = static_cast<float>(spec.sample_rate()) / static_cast<float>(spec.n_fft());
  MagnitudeHistory history(spec.n_bins(),
                           kAttackHistoryWindows * (spec.n_fft() / spec.hop_length()) + 1);

  std::vector<std::vector<F0Candidate>> frames(static_cast<size_t>(spectrum.n_frames));
  for (int frame = 0; frame < spectrum.n_frames; ++frame) {
    std::vector<F0Candidate>& candidates = frames[static_cast<size_t>(frame)];
    candidates = estimator.estimate(spectrum.column(frame));
    if (read_attack) {
      history.push(spec, frame);
      read_attacks(candidates, history, bin_hz, spec.n_bins());
    }
  }
  return track_from_frames(audio, frames, spectrum.hop_length, config.ridges);
}

MultiF0Track extract_multi_f0(const Audio& audio, const MultiF0ExtractorConfig& config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  const StftConfig stft = polyphony_stft_at_rate(config.stft, audio.sample_rate());

  // The cent spectrum reads one frame back for its phase advance, so each tile
  // repeats the previous tile's last frame and that column is not estimated twice.
  std::vector<std::vector<F0Candidate>> frames(
      static_cast<size_t>(stft_frame_count(audio.size(), stft)));
  std::optional<MultiF0Estimator> estimator;
  const bool read_attack = config.ridges.attack_window_ms != 0.0f;
  const float bin_hz = static_cast<float>(audio.sample_rate()) / static_cast<float>(stft.n_fft);
  // The history runs across tiles, so only a tile's new frames are pushed.
  MagnitudeHistory history(stft.n_fft / 2 + 1,
                           kAttackHistoryWindows * (stft.n_fft / stft.hop_length) + 1);
  for_each_stft_tile(audio, stft, 1, [&](const Spectrogram& tile, int first_frame, int discard) {
    const CentSpectrum spectrum = compute_cent_spectrum(tile, config.spectrum);
    if (!estimator) estimator.emplace(spectrum.axis, config.estimation);
    for (int frame = discard; frame < spectrum.n_frames; ++frame) {
      std::vector<F0Candidate>& candidates = frames[static_cast<size_t>(first_frame + frame)];
      candidates = estimator->estimate(spectrum.column(frame));
      if (read_attack) {
        history.push(tile, frame);
        read_attacks(candidates, history, bin_hz, tile.n_bins());
      }
    }
  });
  return track_from_frames(audio, frames, stft.hop_length, config.ridges);
}

}  // namespace sonare::editing::polyphony
