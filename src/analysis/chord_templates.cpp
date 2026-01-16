#include "analysis/chord_templates.h"

#include <algorithm>
#include <cmath>

namespace sonare {

namespace {

/// @brief Creates a chord pattern from intervals.
std::array<float, 12> create_pattern(PitchClass root, const std::vector<int>& intervals) {
  std::array<float, 12> pattern = {};
  int root_idx = static_cast<int>(root);

  for (int interval : intervals) {
    int idx = (root_idx + interval) % 12;
    pattern[idx] = 1.0f;
  }

  return pattern;
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
  if (denom < 1e-10f) {
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

  if (max_chroma < 1e-10f) {
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
      quality == ChordQuality::Major7 || quality == ChordQuality::Augmented) {
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
             quality == ChordQuality::Diminished) {
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
  if (quality == ChordQuality::Diminished || quality == ChordQuality::Augmented) {
    quality_penalty = 0.05f;
  }

  return cosine_sim + root_bonus + third_bonus + fifth_bonus - penalty - quality_penalty;
}

float ChordTemplate::correlate(const std::array<float, 12>& chroma) const {
  return correlate(chroma.data());
}

ChordTemplate create_major_template(PitchClass root) {
  ChordTemplate tmpl;
  tmpl.root = root;
  tmpl.quality = ChordQuality::Major;
  tmpl.pattern = create_pattern(root, {0, 4, 7});  // Root, major 3rd, perfect 5th
  return tmpl;
}

ChordTemplate create_minor_template(PitchClass root) {
  ChordTemplate tmpl;
  tmpl.root = root;
  tmpl.quality = ChordQuality::Minor;
  tmpl.pattern = create_pattern(root, {0, 3, 7});  // Root, minor 3rd, perfect 5th
  return tmpl;
}

ChordTemplate create_diminished_template(PitchClass root) {
  ChordTemplate tmpl;
  tmpl.root = root;
  tmpl.quality = ChordQuality::Diminished;
  tmpl.pattern = create_pattern(root, {0, 3, 6});  // Root, minor 3rd, diminished 5th
  return tmpl;
}

ChordTemplate create_augmented_template(PitchClass root) {
  ChordTemplate tmpl;
  tmpl.root = root;
  tmpl.quality = ChordQuality::Augmented;
  tmpl.pattern = create_pattern(root, {0, 4, 8});  // Root, major 3rd, augmented 5th
  return tmpl;
}

ChordTemplate create_dominant7_template(PitchClass root) {
  ChordTemplate tmpl;
  tmpl.root = root;
  tmpl.quality = ChordQuality::Dominant7;
  tmpl.pattern = create_pattern(root, {0, 4, 7, 10});  // Root, major 3rd, perfect 5th, minor 7th
  return tmpl;
}

ChordTemplate create_major7_template(PitchClass root) {
  ChordTemplate tmpl;
  tmpl.root = root;
  tmpl.quality = ChordQuality::Major7;
  tmpl.pattern = create_pattern(root, {0, 4, 7, 11});  // Root, major 3rd, perfect 5th, major 7th
  return tmpl;
}

ChordTemplate create_minor7_template(PitchClass root) {
  ChordTemplate tmpl;
  tmpl.root = root;
  tmpl.quality = ChordQuality::Minor7;
  tmpl.pattern = create_pattern(root, {0, 3, 7, 10});  // Root, minor 3rd, perfect 5th, minor 7th
  return tmpl;
}

ChordTemplate create_sus2_template(PitchClass root) {
  ChordTemplate tmpl;
  tmpl.root = root;
  tmpl.quality = ChordQuality::Sus2;
  tmpl.pattern = create_pattern(root, {0, 2, 7});  // Root, major 2nd, perfect 5th
  return tmpl;
}

ChordTemplate create_sus4_template(PitchClass root) {
  ChordTemplate tmpl;
  tmpl.root = root;
  tmpl.quality = ChordQuality::Sus4;
  tmpl.pattern = create_pattern(root, {0, 5, 7});  // Root, perfect 4th, perfect 5th
  return tmpl;
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
  templates.reserve(12 * 9);

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
