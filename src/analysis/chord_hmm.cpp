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

/// @brief How strongly a transition behaves as a cadence.
/// @details Root motion alone cannot tell a resolving cadence from a modal
/// shuffle: V7 -> I and v -> i are the same two scale degrees. Grading the
/// motion by the qualities that actually carry it keeps the strongest harmonic
/// cue in the model instead of discarding it after the template already
/// measured it.
enum class CadenceStrength {
  None,
  /// Cadential root motion, but the chord standing on it is not the one the
  /// cadence is built from (a minor v where the dominant belongs, say).
  Weak,
  /// The cadence spelled as expected.
  Full,
  /// A dominant seventh resolving: the tritone is present and it pulls.
  Dominant,
};

CadenceStrength grade(bool quality_matches) {
  return quality_matches ? CadenceStrength::Full : CadenceStrength::Weak;
}

CadenceStrength cadence_strength(const ChordTemplate& from, const ChordTemplate& to,
                                 const ChordHmmConfig& config) {
  if (!config.use_key_context) {
    return CadenceStrength::None;
  }

  const int from_degree = interval(config.key_root, from.root);
  const int to_degree = interval(config.key_root, to.root);
  const ChordQuality from_base = chord_quality_triad_base(from.quality);
  const ChordQuality to_base = chord_quality_triad_base(to.quality);
  const bool from_is_dominant7 = chord_quality_is_dominant_seventh(from.quality);

  if (config.key_mode == Mode::Minor) {
    if (from_degree == 7 && to_degree == 0) {  // V/v -> i
      if (to_base != ChordQuality::Minor) {
        return CadenceStrength::Weak;
      }
      // The raised leading tone is what makes the fifth degree a dominant in a
      // minor key; the natural-minor v has no leading tone and no pull.
      if (from_is_dominant7) {
        return CadenceStrength::Dominant;
      }
      return grade(from_base == ChordQuality::Major);
    }
    if (from_degree == 5 && to_degree == 0) {  // iv -> i
      return grade(from_base == ChordQuality::Minor && to_base == ChordQuality::Minor);
    }
    if (from_degree == 2 && to_degree == 7) {  // ii(dim) -> v/V
      return grade(from_base == ChordQuality::Diminished || from_base == ChordQuality::Minor);
    }
    if (from_degree == 10 && to_degree == 0) {  // VII -> i
      return grade(from_base == ChordQuality::Major && to_base == ChordQuality::Minor);
    }
    return CadenceStrength::None;
  }

  if (from_degree == 7 && to_degree == 0) {  // V -> I
    if (to_base != ChordQuality::Major) {
      return CadenceStrength::Weak;
    }
    if (from_is_dominant7) {
      return CadenceStrength::Dominant;
    }
    return grade(from_base == ChordQuality::Major);
  }
  if (from_degree == 5 && to_degree == 0) {  // IV -> I
    return grade(from_base == ChordQuality::Major && to_base == ChordQuality::Major);
  }
  if (from_degree == 2 && to_degree == 7) {  // ii -> V
    return grade(from_base == ChordQuality::Minor);
  }
  if (from_degree == 9 && to_degree == 2) {  // vi -> ii
    return grade(from_base == ChordQuality::Minor && to_base == ChordQuality::Minor);
  }
  return CadenceStrength::None;
}

float cadence_logp(CadenceStrength strength, const ChordHmmConfig& config) {
  switch (strength) {
    case CadenceStrength::Dominant:
      return config.dominant_cadential_transition_logp;
    case CadenceStrength::Full:
      return config.cadential_transition_logp;
    case CadenceStrength::Weak:
      return config.weak_cadential_transition_logp;
    case CadenceStrength::None:
      break;
  }
  return config.remote_transition_logp;
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
  const CadenceStrength strength = cadence_strength(templates[from_idx], templates[to_idx], config);
  if (strength != CadenceStrength::None) {
    // A weak cadence must never score below an ordinary related transition:
    // the motion is still one a progression makes, so grading its quality may
    // withdraw the cadence bonus but must not turn it into a penalty.
    const float logp = cadence_logp(strength, config);
    if (strength == CadenceStrength::Weak) {
      return std::max(logp, config.related_transition_logp);
    }
    return logp;
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
