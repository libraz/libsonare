#include "analysis/downbeat_analyzer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

#include "analysis/chord_analyzer.h"
#include "filters/chroma.h"
#include "filters/iir.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/vector_normalize.h"

namespace sonare {

using sonare::constants::kEpsilon;
namespace {

/// Upper edge of the low band whose energy marks a bar start, in Hz.
constexpr double kLowBandHz = 150.0;
/// Low-band window around a beat's attack, in seconds relative to the beat time.
constexpr double kLowWindowStart = -0.02;
constexpr double kLowWindowEnd = 0.10;
constexpr int kChromaBins = 12;

/// Median as the mean of the two middle values for an even count; 0 when empty.
double median_of(std::vector<double> values) {
  if (values.empty()) return 0.0;
  const size_t mid = values.size() / 2;
  std::nth_element(values.begin(), values.begin() + static_cast<long>(mid), values.end());
  const double upper = values[mid];
  if (values.size() % 2 == 1) return upper;
  return 0.5 * (upper + *std::max_element(values.begin(), values.begin() + static_cast<long>(mid)));
}

/// Index of the first frame centred at or after @p seconds, in [0, n_frames].
int first_frame_at(double seconds, int sr, int hop, int n_frames) {
  // The tolerance keeps a beat that sits on a frame centre on that frame despite float rounding.
  const double position = seconds * static_cast<double>(sr) / static_cast<double>(hop);
  const double frame = std::ceil(position - 1e-4);
  return static_cast<int>(std::clamp(frame, 0.0, static_cast<double>(n_frames)));
}

/// Population z-scores of @p values; all zero when empty or of zero variance.
std::vector<double> z_scores(const std::vector<float>& values, size_t count) {
  std::vector<double> z(count, 0.0);
  if (values.empty()) return z;
  double mean = 0.0;
  double scale = 0.0;
  for (float value : values) {
    mean += value;
    scale = std::max(scale, std::abs(static_cast<double>(value)));
  }
  mean /= static_cast<double>(values.size());
  double variance = 0.0;
  for (float value : values) {
    variance += (value - mean) * (value - mean);
  }
  const double stddev = std::sqrt(variance / static_cast<double>(values.size()));
  // A spread at float rounding level is a constant feature.
  if (!(stddev > 1e-6 * scale)) return z;
  for (size_t i = 0; i < count; ++i) {
    z[i] = (values[i] - mean) / stddev;
  }
  return z;
}

}  // namespace

DownbeatResult estimate_downbeats(const std::vector<Beat>& beats,
                                  const TimeSignature& time_signature, int downbeat_phase) {
  DownbeatResult result;
  result.time_signature = time_signature;
  result.confidence = time_signature.confidence;

  const int numerator = std::max(1, time_signature.numerator);
  if (beats.empty()) {
    return result;
  }

  downbeat_phase = ((downbeat_phase % numerator) + numerator) % numerator;
  for (size_t i = 0; i < beats.size(); ++i) {
    if ((static_cast<int>(i) - downbeat_phase + numerator) % numerator == 0) {
      result.beat_indices.push_back(static_cast<int>(i));
      result.downbeats.push_back(beats[i]);
    }
  }

  return result;
}

DownbeatObservations downbeat_phase_evidence(const std::vector<float>& power, int n_bins,
                                             int n_frames, int sr, int n_fft, int hop,
                                             const std::vector<Beat>& beats) {
  SONARE_CHECK(sr > 0 && hop > 0 && n_fft > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(n_frames >= 0 && n_bins == n_fft / 2 + 1, ErrorCode::InvalidParameter);
  SONARE_CHECK(power.size() == static_cast<size_t>(n_bins) * static_cast<size_t>(n_frames),
               ErrorCode::InvalidParameter);

  DownbeatObservations observations;
  if (beats.empty() || n_frames == 0) {
    return observations;
  }
  const size_t frames = static_cast<size_t>(n_frames);

  std::vector<double> low_band(frames, 0.0);
  for (int bin = 0; bin < n_bins; ++bin) {
    if (static_cast<double>(bin) * sr / n_fft >= kLowBandHz) break;
    const float* row = power.data() + static_cast<size_t>(bin) * frames;
    for (size_t frame = 0; frame < frames; ++frame) {
      low_band[frame] += row[frame];
    }
  }

  const auto filterbank = get_chroma_filterbank_cached(sr, n_fft, ChromaFilterConfig{});
  const std::vector<float> raw_chroma =
      apply_chroma_filterbank(power.data(), n_bins, n_frames, filterbank->data(), kChromaBins);
  const std::vector<float> chroma =
      normalize_matrix(raw_chroma.data(), kChromaBins, n_frames, 0, NormType::Inf);

  std::vector<double> intervals;
  for (size_t i = 1; i < beats.size(); ++i) {
    intervals.push_back(static_cast<double>(beats[i].time) - beats[i - 1].time);
  }
  const double last_interval = intervals.empty() ? 0.5 : median_of(std::move(intervals));

  observations.lf.resize(beats.size(), 0.0f);
  observations.harmonic_change.resize(beats.size(), 0.0f);
  std::array<double, kChromaBins> previous{};
  for (size_t i = 0; i < beats.size(); ++i) {
    const double time = beats[i].time;

    const int lf_first =
        std::min(first_frame_at(time + kLowWindowStart, sr, hop, n_frames), n_frames - 1);
    const int lf_last =
        std::max(first_frame_at(time + kLowWindowEnd, sr, hop, n_frames), lf_first + 1);
    double lf_sum = 0.0;
    for (int frame = lf_first; frame < lf_last; ++frame) {
      lf_sum += low_band[static_cast<size_t>(frame)];
    }
    observations.lf[i] = static_cast<float>(lf_sum / (lf_last - lf_first));

    const double span_end = i + 1 < beats.size() ? beats[i + 1].time : time + last_interval;
    const int span_first = std::min(first_frame_at(time, sr, hop, n_frames), n_frames - 1);
    const int span_last = std::max(first_frame_at(span_end, sr, hop, n_frames), span_first + 1);
    std::array<double, kChromaBins> current{};
    for (int pc = 0; pc < kChromaBins; ++pc) {
      const float* row = chroma.data() + static_cast<size_t>(pc) * frames;
      double sum = 0.0;
      for (int frame = span_first; frame < span_last; ++frame) {
        sum += row[frame];
      }
      current[static_cast<size_t>(pc)] = sum / (span_last - span_first);
    }
    if (i > 0) {
      double dot = 0.0;
      double norm_current = 0.0;
      double norm_previous = 0.0;
      for (size_t pc = 0; pc < current.size(); ++pc) {
        dot += current[pc] * previous[pc];
        norm_current += current[pc] * current[pc];
        norm_previous += previous[pc] * previous[pc];
      }
      const double cosine = dot / (std::sqrt(norm_current) * std::sqrt(norm_previous) + 1e-9);
      observations.harmonic_change[i] = static_cast<float>(1.0 - cosine);
    }
    previous = current;
  }
  return observations;
}

DownbeatResult select_downbeat_phase(const std::vector<Beat>& beats,
                                     const TimeSignature& time_signature,
                                     const DownbeatObservations& observations) {
  SONARE_CHECK(observations.lf.empty() || observations.lf.size() == beats.size(),
               ErrorCode::InvalidParameter);
  SONARE_CHECK(
      observations.harmonic_change.empty() || observations.harmonic_change.size() == beats.size(),
      ErrorCode::InvalidParameter);

  const int numerator = std::max(1, time_signature.numerator);
  if (beats.empty()) {
    return estimate_downbeats(beats, time_signature, 0);
  }
  if (numerator <= 1) {
    DownbeatResult result;
    result.time_signature = time_signature;
    result.confidence = time_signature.confidence;
    result.beat_indices.push_back(0);
    result.downbeats.push_back(beats.front());
    return result;
  }

  int phase = 0;
  double margin = 0.0;
  if (beats.size() >= static_cast<size_t>(numerator)) {
    const std::vector<double> z_lf = z_scores(observations.lf, beats.size());
    const std::vector<double> z_hc = z_scores(observations.harmonic_change, beats.size());
    std::vector<double> position_scores;
    for (int position = 0; position < numerator; ++position) {
      std::vector<double> scores;
      for (size_t i = static_cast<size_t>(position); i < beats.size();
           i += static_cast<size_t>(numerator)) {
        scores.push_back(z_lf[i] + z_hc[i]);
      }
      position_scores.push_back(median_of(std::move(scores)));
    }
    for (int position = 1; position < numerator; ++position) {
      if (position_scores[static_cast<size_t>(position)] >
          position_scores[static_cast<size_t>(phase)]) {
        phase = position;
      }
    }
    double second = -std::numeric_limits<double>::infinity();
    for (int position = 0; position < numerator; ++position) {
      if (position != phase)
        second = std::max(second, position_scores[static_cast<size_t>(position)]);
    }
    margin = std::clamp(position_scores[static_cast<size_t>(phase)] - second, 0.0, 1.0);
  }

  DownbeatResult result = estimate_downbeats(beats, time_signature, phase);
  result.confidence =
      std::clamp(0.5f * time_signature.confidence + 0.5f * static_cast<float>(margin), 0.0f, 1.0f);
  return result;
}

std::vector<float> chord_change_observations(const std::vector<Beat>& beats,
                                             const std::vector<Chord>& chords,
                                             float tolerance_seconds) {
  std::vector<float> observations(beats.size(), 0.0f);
  if (beats.empty() || chords.empty()) {
    return observations;
  }

  for (size_t chord_index = 0; chord_index < chords.size(); ++chord_index) {
    const float start = chords[chord_index].start;
    auto nearest =
        std::min_element(beats.begin(), beats.end(), [start](const Beat& a, const Beat& b) {
          return std::abs(a.time - start) < std::abs(b.time - start);
        });
    if (nearest == beats.end()) {
      continue;
    }
    const float distance = std::abs(nearest->time - start);
    if (distance <= tolerance_seconds) {
      const size_t beat_index = static_cast<size_t>(nearest - beats.begin());
      observations[beat_index] = std::max(observations[beat_index], 1.0f);
    }
  }

  return observations;
}

std::vector<float> low_frequency_energy_observations(const std::vector<Beat>& beats,
                                                     const Audio& audio, float cutoff_hz,
                                                     float window_seconds) {
  SONARE_CHECK(audio.sample_rate() > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(cutoff_hz > 0.0f, ErrorCode::InvalidParameter);
  SONARE_CHECK(cutoff_hz < static_cast<float>(audio.sample_rate()) * 0.5f,
               ErrorCode::InvalidParameter);
  SONARE_CHECK(window_seconds > 0.0f, ErrorCode::InvalidParameter);

  std::vector<float> observations(beats.size(), 0.0f);
  if (beats.empty() || audio.empty()) {
    return observations;
  }

  const auto cascade = lowpass_coeffs_4th(cutoff_hz, audio.sample_rate());
  const std::vector<float> low = apply_cascade_filtfilt(audio.data(), audio.size(), cascade);
  const int half_window =
      std::max(1, static_cast<int>(std::round(0.5f * window_seconds * audio.sample_rate())));

  for (size_t beat_index = 0; beat_index < beats.size(); ++beat_index) {
    const int center = static_cast<int>(std::round(beats[beat_index].time * audio.sample_rate()));
    const int first = std::max(0, center - half_window);
    const int last = std::min(static_cast<int>(low.size()), center + half_window);
    if (first >= last) {
      continue;
    }

    double energy = 0.0;
    for (int i = first; i < last; ++i) {
      energy += static_cast<double>(low[static_cast<size_t>(i)]) * low[static_cast<size_t>(i)];
    }
    observations[beat_index] = static_cast<float>(energy / static_cast<double>(last - first));
  }

  return observations;
}

// Frame-0 beats are often underestimated due to center-padding of the onset
// envelope, so when the first beat is near-silent (below this fraction of the
// second beat's strength) it inherits the second beat's strength.
constexpr float kSilentFirstBeatRatio = 0.1f;

std::vector<float> onset_strength_observations(const std::vector<Beat>& beats,
                                               const std::vector<float>& onset_strength, int sr,
                                               int hop_length, float window_seconds) {
  SONARE_CHECK(sr > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(hop_length > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(window_seconds > 0.0f, ErrorCode::InvalidParameter);

  std::vector<float> observations(beats.size(), 0.0f);
  if (beats.empty() || onset_strength.empty()) {
    return observations;
  }

  const int half_window = std::max(
      1, static_cast<int>(std::round(0.5f * window_seconds * sr / static_cast<float>(hop_length))));
  const int n_frames = static_cast<int>(onset_strength.size());
  for (size_t beat_index = 0; beat_index < beats.size(); ++beat_index) {
    const int center = std::clamp(beats[beat_index].frame, 0, n_frames - 1);

    const int first = std::max(0, center - half_window);
    const int last = std::min(n_frames, center + half_window + 1);
    float max_value = 0.0f;
    float sum = 0.0f;
    for (int frame = first; frame < last; ++frame) {
      const float value = std::max(0.0f, onset_strength[static_cast<size_t>(frame)]);
      max_value = std::max(max_value, value);
      sum += value;
    }

    const float mean = sum / static_cast<float>(std::max(1, last - first));
    observations[beat_index] = 0.7f * max_value + 0.3f * mean;
  }
  if (observations.size() >= 2 && beats.front().frame == 0 &&
      observations.front() < observations[1] * kSilentFirstBeatRatio) {
    observations.front() = observations[1];
  }

  return observations;
}

}  // namespace sonare
