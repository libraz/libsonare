#include "analysis/downbeat_analyzer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

#include "analysis/chord_analyzer.h"
#include "filters/iir.h"
#include "util/constants.h"
#include "util/exception.h"

namespace sonare {
namespace {

std::vector<float> normalize_to_unit(std::vector<float> values, size_t target_size) {
  values.resize(target_size, 0.0f);
  float max_value = 0.0f;
  for (float value : values) {
    if (std::isfinite(value)) {
      max_value = std::max(max_value, value);
    }
  }
  if (max_value <= constants::kEpsilon) {
    return std::vector<float>(target_size, 0.0f);
  }
  for (float& value : values) {
    value = std::isfinite(value) ? std::clamp(value / max_value, 0.0f, 1.0f) : 0.0f;
  }
  return values;
}

float observation_score(size_t beat_index, int position, int downbeat_phase, int numerator,
                        const DownbeatObservations& observations,
                        const std::vector<float>& beat_strengths,
                        const std::vector<float>& low_frequency,
                        const std::vector<float>& chord_changes) {
  const bool is_downbeat = position == 0;
  const bool is_secondary_strong =
      (numerator == 4 && position == 2) || (numerator == 6 && position == 3);
  float score = 0.0f;
  if (is_downbeat) {
    score += observations.beat_strength_weight * beat_strengths[beat_index];
    score += observations.low_frequency_weight * low_frequency[beat_index];
    score += observations.chord_change_weight * chord_changes[beat_index];
  } else if (is_secondary_strong) {
    score += 0.35f * observations.beat_strength_weight * beat_strengths[beat_index];
    score += 0.25f * observations.low_frequency_weight * low_frequency[beat_index];
  } else {
    score -= 0.15f * observations.chord_change_weight * chord_changes[beat_index];
  }

  const int expected_position =
      (static_cast<int>(beat_index) - downbeat_phase + numerator) % numerator;
  if (position == expected_position) {
    score += observations.phase_prior_weight;
  }
  return score;
}

float median_beat_interval(const std::vector<Beat>& beats) {
  std::vector<float> intervals;
  intervals.reserve(beats.size() > 1 ? beats.size() - 1 : 0);
  for (size_t i = 1; i < beats.size(); ++i) {
    const float interval = beats[i].time - beats[i - 1].time;
    if (std::isfinite(interval) && interval > 1e-4f) {
      intervals.push_back(interval);
    }
  }
  if (intervals.empty()) {
    return 0.5f;
  }

  const size_t mid = intervals.size() / 2;
  std::nth_element(intervals.begin(), intervals.begin() + static_cast<long>(mid), intervals.end());
  return std::max(intervals[mid], 1e-4f);
}

std::vector<float> tempo_state_centers(const std::vector<Beat>& beats, int state_count) {
  state_count = std::clamp(state_count, 1, 9);
  const float median_interval = median_beat_interval(beats);
  std::vector<float> centers(static_cast<size_t>(state_count), median_interval);
  if (state_count == 1) {
    return centers;
  }

  const float min_ratio = 0.70f;
  const float max_ratio = 1.35f;
  for (int i = 0; i < state_count; ++i) {
    const float alpha = static_cast<float>(i) / static_cast<float>(state_count - 1);
    centers[static_cast<size_t>(i)] =
        median_interval * std::pow(max_ratio / min_ratio, alpha) * min_ratio;
  }
  return centers;
}

float tempo_observation_score(const std::vector<Beat>& beats, size_t beat_index, float tempo_center,
                              float weight) {
  if (beat_index == 0 || weight <= 0.0f || tempo_center <= 1e-4f) {
    return 0.0f;
  }
  const float interval = beats[beat_index].time - beats[beat_index - 1].time;
  if (!std::isfinite(interval) || interval <= 1e-4f) {
    return -weight;
  }

  const float log_ratio = std::log(interval / tempo_center);
  return -weight * std::min(std::abs(log_ratio) / std::log(1.20f), 3.0f);
}

float tempo_transition_score(int previous_tempo, int tempo, float weight) {
  if (weight <= 0.0f) {
    return 0.0f;
  }
  return -weight * static_cast<float>(std::abs(tempo - previous_tempo));
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

DownbeatResult estimate_downbeats(const std::vector<Beat>& beats,
                                  const TimeSignature& time_signature, int downbeat_phase,
                                  const DownbeatObservations& observations) {
  DownbeatResult result;
  result.time_signature = time_signature;

  const int numerator = std::max(1, time_signature.numerator);
  if (beats.empty()) {
    result.confidence = time_signature.confidence;
    return result;
  }
  if (numerator <= 1) {
    result.beat_indices.push_back(0);
    result.downbeats.push_back(beats.front());
    result.confidence = time_signature.confidence;
    return result;
  }

  downbeat_phase = ((downbeat_phase % numerator) + numerator) % numerator;
  std::vector<float> beat_strengths =
      observations.beat_strengths.empty() ? std::vector<float>{} : observations.beat_strengths;
  if (beat_strengths.empty()) {
    beat_strengths.reserve(beats.size());
    for (const auto& beat : beats) {
      beat_strengths.push_back(beat.strength);
    }
  }

  beat_strengths = normalize_to_unit(std::move(beat_strengths), beats.size());
  std::vector<float> low_frequency =
      normalize_to_unit(observations.low_frequency_energy, beats.size());
  std::vector<float> chord_changes = normalize_to_unit(observations.chord_changes, beats.size());

  const float neg_inf = -std::numeric_limits<float>::infinity();
  const std::vector<float> tempo_centers =
      tempo_state_centers(beats, observations.tempo_state_count);
  const int tempo_state_count = static_cast<int>(tempo_centers.size());
  const int state_count = numerator * tempo_state_count;
  auto state_index = [tempo_state_count](int position, int tempo) {
    return position * tempo_state_count + tempo;
  };
  auto state_position = [tempo_state_count](int state) { return state / tempo_state_count; };
  auto state_tempo = [tempo_state_count](int state) { return state % tempo_state_count; };

  std::vector<std::vector<float>> score(beats.size(), std::vector<float>(state_count, neg_inf));
  std::vector<std::vector<int>> backpointer(beats.size(), std::vector<int>(state_count, -1));

  for (int position = 0; position < numerator; ++position) {
    const float base = observation_score(0, position, downbeat_phase, numerator, observations,
                                         beat_strengths, low_frequency, chord_changes);
    for (int tempo = 0; tempo < tempo_state_count; ++tempo) {
      score[0][state_index(position, tempo)] = base;
    }
  }

  for (size_t beat = 1; beat < beats.size(); ++beat) {
    for (int position = 0; position < numerator; ++position) {
      const int expected_previous = (position - 1 + numerator) % numerator;
      const float base_observation =
          observation_score(beat, position, downbeat_phase, numerator, observations, beat_strengths,
                            low_frequency, chord_changes);
      for (int tempo = 0; tempo < tempo_state_count; ++tempo) {
        float best_previous_score = neg_inf;
        int best_previous_state = -1;
        for (int previous_tempo = 0; previous_tempo < tempo_state_count; ++previous_tempo) {
          const int previous_state = state_index(expected_previous, previous_tempo);
          const float transition =
              tempo_transition_score(previous_tempo, tempo, observations.tempo_transition_weight);
          const float candidate = score[beat - 1][previous_state] + transition;
          if (candidate > best_previous_score) {
            best_previous_score = candidate;
            best_previous_state = previous_state;
          }
        }

        const float tempo_observation =
            tempo_observation_score(beats, beat, tempo_centers[static_cast<size_t>(tempo)],
                                    observations.tempo_observation_weight);
        const int state = state_index(position, tempo);
        score[beat][state] = best_previous_score + base_observation + tempo_observation;
        backpointer[beat][state] = best_previous_state;
      }
    }
  }

  int best_state = 0;
  float best_score = score.back()[0];
  float second_score = neg_inf;
  std::vector<float> best_score_by_position(static_cast<size_t>(numerator), neg_inf);
  for (int state = 0; state < state_count; ++state) {
    const int position = state_position(state);
    best_score_by_position[static_cast<size_t>(position)] =
        std::max(best_score_by_position[static_cast<size_t>(position)], score.back()[state]);
    if (score.back()[state] > best_score) {
      best_score = score.back()[state];
      best_state = state;
    }
  }
  for (int position = 0; position < numerator; ++position) {
    const float position_score = best_score_by_position[static_cast<size_t>(position)];
    if (position_score < best_score && position_score > second_score) {
      second_score = position_score;
    }
  }

  std::vector<int> positions(beats.size(), 0);
  int state = best_state;
  for (size_t beat = beats.size(); beat-- > 0;) {
    positions[beat] = state_position(state);
    state =
        backpointer[beat][state] >= 0
            ? backpointer[beat][state]
            : state_index((state_position(state) - 1 + numerator) % numerator, state_tempo(state));
  }

  for (size_t beat = 0; beat < beats.size(); ++beat) {
    if (positions[beat] == 0) {
      result.beat_indices.push_back(static_cast<int>(beat));
      result.downbeats.push_back(beats[beat]);
    }
  }

  const float margin =
      std::isfinite(second_score)
          ? std::clamp((best_score - second_score) / std::max(1.0f, std::abs(best_score)), 0.0f,
                       1.0f)
          : 0.0f;
  result.confidence =
      std::clamp(0.5f * time_signature.confidence + 0.5f * (0.5f + margin), 0.0f, 1.0f);
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
    int center = beats[beat_index].frame;
    if (center < 0 || center >= n_frames) {
      center = static_cast<int>(std::round(beats[beat_index].time * static_cast<float>(sr) /
                                           static_cast<float>(hop_length)));
    }
    center = std::clamp(center, 0, n_frames - 1);

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
