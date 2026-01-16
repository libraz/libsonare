#pragma once

/// @file chord_templates.h
/// @brief Chord templates for chord recognition.

#include <array>
#include <string>
#include <vector>

#include "util/types.h"

namespace sonare {

/// @brief Template for a chord type.
struct ChordTemplate {
  PitchClass root;                ///< Root pitch class
  ChordQuality quality;           ///< Chord quality
  std::array<float, 12> pattern;  ///< Chroma pattern (binary or weighted)

  /// @brief Returns chord name as string (e.g., "Cmaj", "Am7").
  std::string to_string() const;

  /// @brief Computes correlation with a chroma vector.
  /// @param chroma Chroma vector [12]
  /// @return Correlation value
  float correlate(const float* chroma) const;

  /// @brief Computes correlation with a chroma array.
  float correlate(const std::array<float, 12>& chroma) const;
};

/// @brief Creates a major chord template for a given root.
/// @param root Root pitch class
/// @return Chord template with pattern [1, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0] rotated
ChordTemplate create_major_template(PitchClass root);

/// @brief Creates a minor chord template for a given root.
/// @param root Root pitch class
/// @return Chord template with pattern [1, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0] rotated
ChordTemplate create_minor_template(PitchClass root);

/// @brief Creates a diminished chord template for a given root.
ChordTemplate create_diminished_template(PitchClass root);

/// @brief Creates an augmented chord template for a given root.
ChordTemplate create_augmented_template(PitchClass root);

/// @brief Creates a dominant 7th chord template for a given root.
ChordTemplate create_dominant7_template(PitchClass root);

/// @brief Creates a major 7th chord template for a given root.
ChordTemplate create_major7_template(PitchClass root);

/// @brief Creates a minor 7th chord template for a given root.
ChordTemplate create_minor7_template(PitchClass root);

/// @brief Creates a sus2 chord template for a given root.
ChordTemplate create_sus2_template(PitchClass root);

/// @brief Creates a sus4 chord template for a given root.
ChordTemplate create_sus4_template(PitchClass root);

/// @brief Transposes a chord template by a given number of semitones.
/// @param tmpl Original template
/// @param semitones Semitones to transpose (positive = up)
/// @return Transposed template
ChordTemplate transpose_template(const ChordTemplate& tmpl, int semitones);

/// @brief Generates all basic chord templates (triads and 7th chords).
/// @return Vector of all chord templates (12 roots × 9 qualities = 108 templates)
std::vector<ChordTemplate> generate_all_chord_templates();

/// @brief Generates only triad templates (major, minor, diminished, augmented).
/// @return Vector of triad templates (12 roots × 4 qualities = 48 templates)
std::vector<ChordTemplate> generate_triad_templates();

/// @brief Generates only 7th chord templates.
/// @return Vector of 7th chord templates (12 roots × 3 qualities = 36 templates)
std::vector<ChordTemplate> generate_seventh_templates();

/// @brief Finds the best matching chord template for a chroma vector.
/// @param chroma Chroma vector [12]
/// @param templates Vector of templates to search
/// @return Pair of (best matching template, correlation score)
std::pair<ChordTemplate, float> find_best_chord(const float* chroma,
                                                const std::vector<ChordTemplate>& templates);

/// @brief Finds the best matching chord template for a chroma array.
std::pair<ChordTemplate, float> find_best_chord(const std::array<float, 12>& chroma,
                                                const std::vector<ChordTemplate>& templates);

/// @brief Finds the best matching chord with key context bias.
/// @param chroma Chroma vector [12]
/// @param templates Vector of templates to search
/// @param key_root Root note of the detected key (0-11, C=0)
/// @param key_minor True if the key is minor
/// @param key_confidence Confidence of key detection (0-1)
/// @return Pair of (best matching template, correlation score)
std::pair<ChordTemplate, float> find_best_chord_with_key(
    const float* chroma, const std::vector<ChordTemplate>& templates,
    int key_root, bool key_minor, float key_confidence);

/// @brief Converts chord quality to string.
std::string chord_quality_to_string(ChordQuality quality);

/// @brief Converts pitch class to string.
std::string pitch_class_to_string(PitchClass pc);

}  // namespace sonare
