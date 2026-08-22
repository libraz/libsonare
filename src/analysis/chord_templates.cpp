#include "analysis/chord_templates.h"

#include <algorithm>
#include <cmath>

#include "util/constants.h"

namespace sonare {

using sonare::constants::kEpsilon;

namespace {

/// @brief Creates a chord pattern from a quality's interval spelling.
std::array<float, 12> create_pattern(PitchClass root, const ChordIntervals& intervals) {
  std::array<float, 12> pattern = {};
  int root_idx = static_cast<int>(root);

  for (size_t i = 0; i < intervals.count; ++i) {
    int idx = (root_idx + intervals.semitones[i]) % 12;
    pattern[idx] = 1.0f;
  }

  return pattern;
}

/// @brief Builds the template for @p quality rooted at @p root.
ChordTemplate make_template(PitchClass root, ChordQuality quality) {
  ChordTemplate tmpl;
  tmpl.root = root;
  tmpl.quality = quality;
  tmpl.pattern = create_pattern(root, chord_quality_intervals(quality));
  return tmpl;
}

/// @brief Rotates a pattern by semitones.
std::array<float, 12> rotate_pattern(const std::array<float, 12>& pattern, int semitones) {
  std::array<float, 12> rotated;
  for (int i = 0; i < 12; ++i) {
    int src = (i - semitones + 120) % 12;  // +120 to handle negative
    rotated[i] = pattern[src];
  }
  return rotated;
}

}  // namespace

std::string pitch_class_to_string(PitchClass pc) {
  static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  int idx = static_cast<int>(pc);
  if (idx >= 0 && idx < 12) {
    return names[idx];
  }
  return "?";
}

std::string chord_quality_to_string(ChordQuality quality) {
  switch (quality) {
    case ChordQuality::Major:
      return "maj";
    case ChordQuality::Minor:
      return "m";
    case ChordQuality::Diminished:
      return "dim";
    case ChordQuality::Augmented:
      return "aug";
    case ChordQuality::Dominant7:
      return "7";
    case ChordQuality::Major7:
      return "maj7";
    case ChordQuality::Minor7:
      return "m7";
    case ChordQuality::Sus2:
      return "sus2";
    case ChordQuality::Sus4:
      return "sus4";
    case ChordQuality::Add9:
      return "add9";
    case ChordQuality::MinorAdd9:
      return "madd9";
    case ChordQuality::Dim7:
      return "dim7";
    case ChordQuality::HalfDim7:
      return "m7b5";
    case ChordQuality::Major9:
      return "maj9";
    case ChordQuality::Dominant9:
      return "9";
    case ChordQuality::Sus2Add4:
      return "sus2add4";
    default:
      return "";
  }
}

std::string ChordTemplate::to_string() const {
  return pitch_class_to_string(root) + chord_quality_to_string(quality);
}

float ChordTemplate::correlate(const float* chroma) const {
  // Compute weighted correlation between chroma and pattern
  // Using cosine similarity with root and third emphasis

  // First compute basic dot product and norms
  float dot = 0.0f;
  float chroma_norm_sq = 0.0f;
  float pattern_norm_sq = 0.0f;

  for (int i = 0; i < 12; ++i) {
    dot += chroma[i] * pattern[i];
    chroma_norm_sq += chroma[i] * chroma[i];
    pattern_norm_sq += pattern[i] * pattern[i];
  }

  float denom = std::sqrt(chroma_norm_sq * pattern_norm_sq);
  if (denom < constants::kEpsilon) {
    return 0.0f;
  }

  float cosine_sim = dot / denom;

  int root_idx = static_cast<int>(root);

  // Find max chroma value for normalization
  float max_chroma = 0.0f;
  for (int i = 0; i < 12; ++i) {
    if (chroma[i] > max_chroma) {
      max_chroma = chroma[i];
    }
  }

  if (max_chroma < constants::kEpsilon) {
    return cosine_sim;
  }

  // Root emphasis: if root is prominent, add bonus
  float root_weight = chroma[root_idx];
  float root_ratio = root_weight / max_chroma;
  float root_bonus = 0.0f;
  if (root_ratio >= 0.4f) {
    root_bonus = 0.1f * root_ratio;  // Up to 0.1 bonus
  }

  // Third note emphasis - this distinguishes chords that share root/fifth
  // Major third is at +4 semitones, minor third is at +3 semitones
  // The third is the most important note for chord quality discrimination
  float third_bonus = 0.0f;
  if (quality == ChordQuality::Major || quality == ChordQuality::Dominant7 ||
      quality == ChordQuality::Major7 || quality == ChordQuality::Augmented ||
      quality == ChordQuality::Add9 || quality == ChordQuality::Major9 ||
      quality == ChordQuality::Dominant9) {
    // Major third at +4
    int third_idx = (root_idx + 4) % 12;
    float third_ratio = chroma[third_idx] / max_chroma;
    if (third_ratio >= 0.3f) {
      third_bonus = 0.08f * third_ratio;
    }
    // Penalize if minor third is stronger than major third
    int minor_third_idx = (root_idx + 3) % 12;
    if (chroma[minor_third_idx] > chroma[third_idx] * 1.2f) {
      third_bonus -= 0.05f;
    }
  } else if (quality == ChordQuality::Minor || quality == ChordQuality::Minor7 ||
             quality == ChordQuality::Diminished || quality == ChordQuality::MinorAdd9 ||
             quality == ChordQuality::Dim7 || quality == ChordQuality::HalfDim7) {
    // Minor third at +3
    int third_idx = (root_idx + 3) % 12;
    float third_ratio = chroma[third_idx] / max_chroma;
    if (third_ratio >= 0.3f) {
      third_bonus = 0.08f * third_ratio;
    }
    // Penalize if major third is stronger than minor third
    int major_third_idx = (root_idx + 4) % 12;
    if (chroma[major_third_idx] > chroma[third_idx] * 1.2f) {
      third_bonus -= 0.05f;
    }
  }

  // Fifth note check - perfect fifth at +7 semitones
  // If fifth is present, it confirms the chord
  float fifth_bonus = 0.0f;
  int fifth_idx = (root_idx + 7) % 12;
  float fifth_ratio = chroma[fifth_idx] / max_chroma;
  if (fifth_ratio >= 0.25f) {
    fifth_bonus = 0.03f * fifth_ratio;
  }

  // Penalize notes that shouldn't be in the chord
  float penalty = 0.0f;
  for (int i = 0; i < 12; ++i) {
    if (pattern[i] < 0.5f && chroma[i] > max_chroma * 0.5f) {
      // Strong note that's not in the chord pattern
      penalty += 0.02f;
    }
  }

  // Penalize diminished/augmented chords slightly (they're less common)
  float quality_penalty = 0.0f;
  if (quality == ChordQuality::Diminished || quality == ChordQuality::Augmented ||
      quality == ChordQuality::Dim7 || quality == ChordQuality::HalfDim7) {
    quality_penalty = 0.05f;
  }

  return cosine_sim + root_bonus + third_bonus + fifth_bonus - penalty - quality_penalty;
}

float ChordTemplate::correlate(const std::array<float, 12>& chroma) const {
  return correlate(chroma.data());
}

ChordIntervals chord_quality_intervals(ChordQuality quality) {
  switch (quality) {
    case ChordQuality::Major:
      return {{{0, 4, 7}}, 3};  // Root, major 3rd, perfect 5th
    case ChordQuality::Minor:
      return {{{0, 3, 7}}, 3};  // Root, minor 3rd, perfect 5th
    case ChordQuality::Diminished:
      return {{{0, 3, 6}}, 3};  // Root, minor 3rd, diminished 5th
    case ChordQuality::Augmented:
      return {{{0, 4, 8}}, 3};  // Root, major 3rd, augmented 5th
    case ChordQuality::Dominant7:
      return {{{0, 4, 7, 10}}, 4};  // Root, major 3rd, perfect 5th, minor 7th
    case ChordQuality::Major7:
      return {{{0, 4, 7, 11}}, 4};  // Root, major 3rd, perfect 5th, major 7th
    case ChordQuality::Minor7:
      return {{{0, 3, 7, 10}}, 4};  // Root, minor 3rd, perfect 5th, minor 7th
    case ChordQuality::Sus2:
      return {{{0, 2, 7}}, 3};  // Root, major 2nd, perfect 5th
    case ChordQuality::Sus4:
      return {{{0, 5, 7}}, 3};  // Root, perfect 4th, perfect 5th
    case ChordQuality::Add9:
      return {{{0, 4, 7, 14}}, 4};  // Root, major 3rd, 5th, 9th
    case ChordQuality::MinorAdd9:
      return {{{0, 3, 7, 14}}, 4};  // Root, minor 3rd, 5th, 9th
    case ChordQuality::Dim7:
      return {{{0, 3, 6, 9}}, 4};  // Root, minor 3rd, dim 5th, dim 7th
    case ChordQuality::HalfDim7:
      return {{{0, 3, 6, 10}}, 4};  // Root, minor 3rd, dim 5th, minor 7th
    case ChordQuality::Major9:
      return {{{0, 4, 7, 11, 14}}, 5};  // Root, 3rd, 5th, maj 7th, 9th
    case ChordQuality::Dominant9:
      return {{{0, 4, 7, 10, 14}}, 5};  // Root, 3rd, 5th, min 7th, 9th
    case ChordQuality::Sus2Add4:
      return {{{0, 2, 5, 7}}, 4};  // Root, 2nd, 4th, 5th
    case ChordQuality::Unknown:
      break;
  }
  /// Unknown has no spelling: an empty set shares no note with any chord, which
  /// is the honest answer for "no chord was identified".
  return {};
}

uint16_t chord_pitch_class_mask(int root, int quality) {
  if (root < 0 || root > 11 || quality < 0 || quality > static_cast<int>(ChordQuality::Sus2Add4)) {
    return 0;
  }
  const ChordIntervals intervals = chord_quality_intervals(static_cast<ChordQuality>(quality));
  uint16_t mask = 0;
  for (size_t i = 0; i < intervals.count; ++i) {
    mask = static_cast<uint16_t>(mask | (1u << ((root + intervals.semitones[i]) % 12)));
  }
  return mask;
}

ChordTemplate create_major_template(PitchClass root) {
  return make_template(root, ChordQuality::Major);
}

ChordTemplate create_minor_template(PitchClass root) {
  return make_template(root, ChordQuality::Minor);
}

ChordTemplate create_diminished_template(PitchClass root) {
  return make_template(root, ChordQuality::Diminished);
}

ChordTemplate create_augmented_template(PitchClass root) {
  return make_template(root, ChordQuality::Augmented);
}

ChordTemplate create_dominant7_template(PitchClass root) {
  return make_template(root, ChordQuality::Dominant7);
}

ChordTemplate create_major7_template(PitchClass root) {
  return make_template(root, ChordQuality::Major7);
}

ChordTemplate create_minor7_template(PitchClass root) {
  return make_template(root, ChordQuality::Minor7);
}

ChordTemplate create_sus2_template(PitchClass root) {
  return make_template(root, ChordQuality::Sus2);
}

ChordTemplate create_sus4_template(PitchClass root) {
  return make_template(root, ChordQuality::Sus4);
}

ChordTemplate create_add9_template(PitchClass root) {
  return make_template(root, ChordQuality::Add9);
}

ChordTemplate create_minor_add9_template(PitchClass root) {
  return make_template(root, ChordQuality::MinorAdd9);
}

ChordTemplate create_dim7_template(PitchClass root) {
  return make_template(root, ChordQuality::Dim7);
}

ChordTemplate create_half_dim7_template(PitchClass root) {
  return make_template(root, ChordQuality::HalfDim7);
}

ChordTemplate create_major9_template(PitchClass root) {
  return make_template(root, ChordQuality::Major9);
}

ChordTemplate create_dominant9_template(PitchClass root) {
  return make_template(root, ChordQuality::Dominant9);
}

ChordTemplate create_sus2_add4_template(PitchClass root) {
  return make_template(root, ChordQuality::Sus2Add4);
}

ChordTemplate transpose_template(const ChordTemplate& tmpl, int semitones) {
  ChordTemplate transposed;
  transposed.root = static_cast<PitchClass>((static_cast<int>(tmpl.root) + semitones + 12) % 12);
  transposed.quality = tmpl.quality;
  transposed.pattern = rotate_pattern(tmpl.pattern, semitones);
  return transposed;
}

std::vector<ChordTemplate> generate_all_chord_templates() {
  std::vector<ChordTemplate> templates;
  templates.reserve(12 * 16);

  for (int root = 0; root < 12; ++root) {
    PitchClass pc = static_cast<PitchClass>(root);

    templates.push_back(create_major_template(pc));
    templates.push_back(create_minor_template(pc));
    templates.push_back(create_diminished_template(pc));
    templates.push_back(create_augmented_template(pc));
    templates.push_back(create_dominant7_template(pc));
    templates.push_back(create_major7_template(pc));
    templates.push_back(create_minor7_template(pc));
    templates.push_back(create_sus2_template(pc));
    templates.push_back(create_sus4_template(pc));
    templates.push_back(create_add9_template(pc));
    templates.push_back(create_minor_add9_template(pc));
    templates.push_back(create_dim7_template(pc));
    templates.push_back(create_half_dim7_template(pc));
    templates.push_back(create_major9_template(pc));
    templates.push_back(create_dominant9_template(pc));
    templates.push_back(create_sus2_add4_template(pc));
  }

  return templates;
}

std::vector<ChordTemplate> generate_triad_templates() {
  std::vector<ChordTemplate> templates;
  templates.reserve(12 * 4);

  for (int root = 0; root < 12; ++root) {
    PitchClass pc = static_cast<PitchClass>(root);

    templates.push_back(create_major_template(pc));
    templates.push_back(create_minor_template(pc));
    templates.push_back(create_diminished_template(pc));
    templates.push_back(create_augmented_template(pc));
  }

  return templates;
}

std::vector<ChordTemplate> generate_seventh_templates() {
  std::vector<ChordTemplate> templates;
  templates.reserve(12 * 3);

  for (int root = 0; root < 12; ++root) {
    PitchClass pc = static_cast<PitchClass>(root);

    templates.push_back(create_dominant7_template(pc));
    templates.push_back(create_major7_template(pc));
    templates.push_back(create_minor7_template(pc));
  }

  return templates;
}

std::pair<ChordTemplate, float> find_best_chord(const float* chroma,
                                                const std::vector<ChordTemplate>& templates) {
  if (templates.empty()) {
    return {ChordTemplate{}, -1.0f};
  }

  float best_score = -2.0f;
  size_t best_idx = 0;

  for (size_t i = 0; i < templates.size(); ++i) {
    float score = templates[i].correlate(chroma);
    if (score > best_score) {
      best_score = score;
      best_idx = i;
    }
  }

  return {templates[best_idx], best_score};
}

std::pair<ChordTemplate, float> find_best_chord(const std::array<float, 12>& chroma,
                                                const std::vector<ChordTemplate>& templates) {
  return find_best_chord(chroma.data(), templates);
}

}  // namespace sonare
