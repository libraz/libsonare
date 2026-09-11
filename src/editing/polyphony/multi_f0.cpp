#include "editing/polyphony/multi_f0.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <vector>

#include "core/spectrum.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/math_utils.h"

namespace sonare::editing::polyphony {
namespace {

using sonare::constants::kCentsPerOctave;

float cents_between(float hz, float reference_hz) {
  return kCentsPerOctave * std::log2(hz / reference_hz);
}

/// A ridge while it is still taking candidates; @c peak is the running maximum
/// the salience rule is read against.
struct LiveRidge {
  int frame_start = 0;
  std::vector<float> f0_hz;
  std::vector<float> salience;
  float peak = 0.0f;
};

}  // namespace

MultiF0Estimator::MultiF0Estimator(const CentAxis& spectrum_axis, const MultiF0Config& config)
    : kernel_(spectrum_axis, config.salience), config_(config) {
  // Bounded above as well as below: the iteration count is derived from it, so
  // "at least one" leaves the loop bound to overflow on a large value.
  SONARE_CHECK(config.max_polyphony >= 1 && config.max_polyphony <= 64,
               ErrorCode::InvalidParameter);
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

    // Parabolic interpolation over the peak and its neighbours, so a returned F0
    // is not confined to the axis.
    const float below = salience_[static_cast<size_t>(std::max(peak - 1, 0))];
    const float centre = salience_[static_cast<size_t>(peak)];
    const float above = salience_[static_cast<size_t>(std::min(peak + 1, n_f0_bins - 1))];
    const float denominator = below - 2.0f * centre + above;
    const float offset =
        denominator != 0.0f ? std::clamp(0.5f * (below - above) / denominator, -1.0f, 1.0f) : 0.0f;
    const float f0_hz = kernel_.f0_axis().hz_at(static_cast<float>(peak) + offset);

    // Subtracted before the separation rule runs, so a rejected peak is gone
    // rather than left to be taken again.
    const float removed = kernel_.subtract(residual_.data(), peak, config_.subtraction_factor);

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
  for (const std::vector<F0Candidate>& frame : frames) {
    for (const F0Candidate& candidate : frame) {
      // The salience too: a NaN would make the candidate ordering below
      // intransitive rather than merely arbitrary.
      SONARE_CHECK(std::isfinite(candidate.f0_hz) && candidate.f0_hz > 0.0f &&
                       std::isfinite(candidate.salience),
                   ErrorCode::InvalidParameter);
    }
  }

  std::vector<LiveRidge> finished;
  std::vector<LiveRidge> active;
  for (size_t frame = 0; frame < frames.size(); ++frame) {
    // Descending salience with ties to the lower F0, so the same input always
    // produces the same ridges.
    std::vector<F0Candidate> ordered = frames[frame];
    std::sort(ordered.begin(), ordered.end(), [](const F0Candidate& a, const F0Candidate& b) {
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
        finished.push_back(std::move(ridge));
        continue;
      }
      const F0Candidate& taken = ordered[static_cast<size_t>(best)];
      claimed[static_cast<size_t>(best)] = true;
      ridge.f0_hz.push_back(taken.f0_hz);
      ridge.salience.push_back(taken.salience);
      ridge.peak = std::max(ridge.peak, taken.salience);
      survivors.push_back(std::move(ridge));
    }
    for (size_t i = 0; i < ordered.size(); ++i) {
      if (claimed[i]) continue;
      LiveRidge started;
      started.frame_start = static_cast<int>(frame);
      started.f0_hz.push_back(ordered[i].f0_hz);
      started.salience.push_back(ordered[i].salience);
      started.peak = ordered[i].salience;
      survivors.push_back(std::move(started));
    }
    active = std::move(survivors);
  }
  finished.insert(finished.end(), std::make_move_iterator(active.begin()),
                  std::make_move_iterator(active.end()));

  const double frame_ms =
      1000.0 * static_cast<double>(hop_length) / static_cast<double>(sample_rate);
  std::vector<F0Ridge> ridges;
  ridges.reserve(finished.size());
  for (LiveRidge& live : finished) {
    if (static_cast<double>(live.f0_hz.size()) * frame_ms < config.min_duration_ms) continue;
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
  std::stable_sort(ridges.begin(), ridges.end(), [](const F0Ridge& a, const F0Ridge& b) {
    return a.frame_start != b.frame_start ? a.frame_start < b.frame_start
                                          : a.median_hz < b.median_hz;
  });
  return ridges;
}

float MultiF0Track::frame_rate_hz() const noexcept {
  if (hop_length <= 0 || sample_rate <= 0) return 0.0f;
  return static_cast<float>(sample_rate) / static_cast<float>(hop_length);
}

MultiF0Track extract_multi_f0(const Audio& audio, const MultiF0ExtractorConfig& config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);

  const Spectrogram spectrogram = Spectrogram::compute(audio, config.stft);
  const CentSpectrum spectrum = compute_cent_spectrum(spectrogram, config.spectrum);
  const MultiF0Estimator estimator(spectrum.axis, config.estimation);

  MultiF0Track track;
  track.n_frames = spectrum.n_frames;
  track.hop_length = spectrum.hop_length;
  track.sample_rate = spectrum.sample_rate;
  track.polyphony.assign(static_cast<size_t>(spectrum.n_frames), 0);

  std::vector<std::vector<F0Candidate>> frames(static_cast<size_t>(spectrum.n_frames));
  for (int frame = 0; frame < spectrum.n_frames; ++frame) {
    frames[static_cast<size_t>(frame)] = estimator.estimate(spectrum.column(frame));
    track.polyphony[static_cast<size_t>(frame)] =
        static_cast<int>(frames[static_cast<size_t>(frame)].size());
  }

  track.ridges = track_f0_ridges(frames, spectrum.hop_length, spectrum.sample_rate, config.ridges);

  // Centre padding puts the last frame's end past the audio, so every ridge
  // reaching it would otherwise report a span that cannot be sliced.
  const auto n_samples = static_cast<int64_t>(audio.size());
  for (F0Ridge& ridge : track.ridges) {
    ridge.onset_sample = std::min(ridge.onset_sample, n_samples);
    ridge.offset_sample = std::min(ridge.offset_sample, n_samples);
  }
  return track;
}

}  // namespace sonare::editing::polyphony
