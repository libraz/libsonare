#include "analysis/chord_hmm.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sonare {

namespace {

int interval(PitchClass from, PitchClass to) {
  return (static_cast<int>(to) - static_cast<int>(from) + 12) % 12;
}

bool is_diatonic_root(PitchClass root, PitchClass key_root, Mode mode) {
  static constexpr int kMajorDegrees[] = {0, 2, 4, 5, 7, 9, 11};
  static constexpr int kMinorDegrees[] = {0, 2, 3, 5, 7, 8, 10};
  const int degree = interval(key_root, root);
  const int* degrees = mode == Mode::Minor ? kMinorDegrees : kMajorDegrees;
  for (int i = 0; i < 7; ++i) {
    if (degrees[i] == degree) {
      return true;
    }
  }
  return false;
}

bool cadential_transition(const ChordTemplate& from, const ChordTemplate& to,
                          const ChordHmmConfig& config) {
  if (!config.use_key_context) {
    return false;
  }

  const int from_degree = interval(config.key_root, from.root);
  const int to_degree = interval(config.key_root, to.root);

  if (config.key_mode == Mode::Minor) {
    return (from_degree == 7 && to_degree == 0) ||  // v/V -> i
           (from_degree == 5 && to_degree == 0) ||  // iv -> i
           (from_degree == 2 && to_degree == 7) ||  // ii -> v/V
           (from_degree == 10 && to_degree == 0);   // VII -> i
  }

  return (from_degree == 7 && to_degree == 0) ||  // V -> I
         (from_degree == 5 && to_degree == 0) ||  // IV -> I
         (from_degree == 2 && to_degree == 7) ||  // ii -> V
         (from_degree == 9 && to_degree == 2);    // vi -> ii
}

bool related_transition(const ChordTemplate& from, const ChordTemplate& to,
                        const ChordHmmConfig& config) {
  if (from.root == to.root) {
    return true;
  }

  const int root_motion = interval(from.root, to.root);
  if (root_motion == 5 || root_motion == 7) {
    return true;
  }

  if (!config.use_key_context) {
    return false;
  }

  const bool from_diatonic = is_diatonic_root(from.root, config.key_root, config.key_mode);
  const bool to_diatonic = is_diatonic_root(to.root, config.key_root, config.key_mode);
  if (from_diatonic && to_diatonic) {
    return true;
  }
  return false;
}

float transition_score(int from_idx, int to_idx, const std::vector<ChordTemplate>& templates,
                       const ChordHmmConfig& config) {
  if (from_idx == to_idx) {
    return config.self_transition_logp;
  }
  if (from_idx < 0 || to_idx < 0 || from_idx >= static_cast<int>(templates.size()) ||
      to_idx >= static_cast<int>(templates.size())) {
    return config.remote_transition_logp;
  }
  if (cadential_transition(templates[from_idx], templates[to_idx], config)) {
    return config.cadential_transition_logp;
  }
  return related_transition(templates[from_idx], templates[to_idx], config)
             ? config.related_transition_logp
             : config.remote_transition_logp;
}

std::vector<std::pair<int, float>> normalized_candidates(const ChordHmmObservation& observation,
                                                         const ChordHmmConfig& config) {
  std::vector<std::pair<int, float>> candidates = observation.candidates;
  std::sort(candidates.begin(), candidates.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });
  if (config.beam_width > 0 && static_cast<int>(candidates.size()) > config.beam_width) {
    candidates.resize(config.beam_width);
  }
  return candidates;
}

}  // namespace

std::vector<int> viterbi_chord_sequence(const std::vector<ChordHmmObservation>& observations,
                                        const std::vector<ChordTemplate>& templates,
                                        const ChordHmmConfig& config) {
  if (observations.empty() || templates.empty()) {
    return {};
  }

  std::vector<std::vector<std::pair<int, float>>> beams;
  beams.reserve(observations.size());
  for (const auto& observation : observations) {
    auto candidates = normalized_candidates(observation, config);
    if (candidates.empty()) {
      return {};
    }
    beams.push_back(std::move(candidates));
  }

  std::vector<std::vector<float>> scores(beams.size());
  std::vector<std::vector<int>> backtrack(beams.size());

  // The emission term is a template-correlation score scaled by emission_weight
  // (not a true log-probability); emission_weight is tuned to balance these
  // correlations against the log-domain transition scores accumulated below.
  // Because emission is linear while transitions are log-domain, the effective
  // smoothing depends on the chroma front-end's correlation magnitude scale: if a
  // different front-end (e.g. NNLS vs STFT) shifts those magnitudes, emission_weight
  // must be re-tuned. This is an intentional, deterministic heuristic, not a true
  // MAP decode.
  scores[0].resize(beams[0].size());
  backtrack[0].assign(beams[0].size(), -1);
  for (size_t j = 0; j < beams[0].size(); ++j) {
    scores[0][j] = beams[0][j].second * config.emission_weight;
  }

  for (size_t t = 1; t < beams.size(); ++t) {
    scores[t].assign(beams[t].size(), -std::numeric_limits<float>::infinity());
    backtrack[t].assign(beams[t].size(), -1);

    for (size_t curr = 0; curr < beams[t].size(); ++curr) {
      const int curr_idx = beams[t][curr].first;
      const float emission = beams[t][curr].second * config.emission_weight;

      for (size_t prev = 0; prev < beams[t - 1].size(); ++prev) {
        const int prev_idx = beams[t - 1][prev].first;
        const float score = scores[t - 1][prev] +
                            transition_score(prev_idx, curr_idx, templates, config) + emission;
        if (score > scores[t][curr]) {
          scores[t][curr] = score;
          backtrack[t][curr] = static_cast<int>(prev);
        }
      }
    }
  }

  size_t best = 0;
  for (size_t j = 1; j < scores.back().size(); ++j) {
    if (scores.back()[j] > scores.back()[best]) {
      best = j;
    }
  }

  std::vector<int> sequence(beams.size(), 0);
  int cursor = static_cast<int>(best);
  for (int t = static_cast<int>(beams.size()) - 1; t >= 0; --t) {
    // Defensively clamp the cursor into the valid range for this beam before
    // indexing, guarding against any inconsistent backtrack pointer.
    cursor = std::clamp(cursor, 0, static_cast<int>(beams[static_cast<size_t>(t)].size()) - 1);
    sequence[static_cast<size_t>(t)] =
        beams[static_cast<size_t>(t)][static_cast<size_t>(cursor)].first;
    cursor = backtrack[static_cast<size_t>(t)][static_cast<size_t>(cursor)];
    if (cursor < 0 && t > 0) {
      cursor = 0;
    }
  }

  return sequence;
}

}  // namespace sonare
