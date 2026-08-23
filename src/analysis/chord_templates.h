#pragma once

/// @file chord_templates.h
/// @brief Chord templates for chord recognition.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "util/types.h"

namespace sonare {

/// @brief Largest number of chord tones any ChordQuality is spelled with.
inline constexpr size_t kMaxChordIntervals = 5;

/// @brief The semitone intervals above the root that spell one chord quality.
struct ChordIntervals {
  std::array<int, kMaxChordIntervals> semitones{};  ///< Valid entries are [0, count)
  size_t count = 0;                                 ///< Number of chord tones
};

/// @brief Returns the interval spelling every template of @p quality is built from.
/// @param quality Chord quality
/// @return Intervals above the root; @c count is 0 for ChordQuality::Unknown
/// @details Single source of truth for what notes a quality contains. The
///          template patterns are generated from this table, and consumers that
///          need the pitch-class set of a chord (shared-note counting, chord
///          confusability) derive it from the same entries instead of assuming
///          every non-minor quality is a major triad.
ChordIntervals chord_quality_intervals(ChordQuality quality);

/// @brief Returns the triad @p quality is built on, or @p quality itself when it has none.
/// @param quality Chord quality
/// @return @c Major, @c Minor, @c Diminished or @c Augmented for a quality that
///         spells a third; @p quality unchanged for every suspended spelling,
///         the eleventh (whose third is omitted) and @c Unknown.
/// @details Derived from @ref chord_quality_intervals rather than from a list,
///          so a quality added to the interval table is classified without a
///          second table having to be kept in step. Consumers that reason about
///          major-vs-minor function (diatonic scoring, cadence detection, the
///          template matcher's third emphasis) share this one answer.
ChordQuality chord_quality_triad_base(ChordQuality quality);

/// @brief True when @p quality spells both a major third and a minor seventh.
/// @details Those two tones are the tritone that gives a dominant its pull, so
///          this is what separates a real V7 from a 7sus4 or an eleventh, both
///          of which omit the third. Also derived from the interval table.
bool chord_quality_is_dominant_seventh(ChordQuality quality);

/// @brief Returns the pitch classes of a chord as a 12-bit mask.
/// @param root Root pitch class (0-11); anything else yields 0
/// @param quality ChordQuality enumerator value; anything else yields 0
/// @return Bit @c i set when pitch class @c i is a chord tone
uint16_t chord_pitch_class_mask(int root, int quality);

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

/// @brief Creates an add9 chord template for a given root.
ChordTemplate create_add9_template(PitchClass root);

/// @brief Creates a minor add9 chord template for a given root.
ChordTemplate create_minor_add9_template(PitchClass root);

/// @brief Creates a diminished 7th chord template for a given root.
ChordTemplate create_dim7_template(PitchClass root);

/// @brief Creates a half-diminished 7th / minor 7 flat 5 chord template for a given root.
ChordTemplate create_half_dim7_template(PitchClass root);

/// @brief Creates a major 9th chord template for a given root.
ChordTemplate create_major9_template(PitchClass root);

/// @brief Creates a dominant 9th chord template for a given root.
ChordTemplate create_dominant9_template(PitchClass root);

/// @brief Creates a sus2 add4 chord template for a given root.
ChordTemplate create_sus2_add4_template(PitchClass root);

/// @brief Creates the template for any quality at a given root.
/// @details The generic form of the per-quality factories above; the extended
///          vocabulary is built through this rather than through eight more
///          one-line functions.
ChordTemplate create_chord_template(PitchClass root, ChordQuality quality);

/// @brief The qualities @ref generate_all_chord_templates adds beyond the base set.
/// @details Sixth chords, the minor-major seventh, 7sus4 and the dominant
///          extensions (11th, 13th, altered ninths). Several of them are
///          pitch-class-identical to a base quality rooted elsewhere — a maj6
///          and the m7 a minor third below spell the same four notes — so
///          telling them apart needs the bass evidence the analyzer folds into
///          its root scoring, not the chroma alone.
const std::vector<ChordQuality>& extended_chord_qualities();

/// @brief Transposes a chord template by a given number of semitones.
/// @param tmpl Original template
/// @param semitones Semitones to transpose (positive = up)
/// @return Transposed template
ChordTemplate transpose_template(const ChordTemplate& tmpl, int semitones);

/// @brief Generates all chord templates (triads, 7ths, suspended, and extensions).
/// @return Vector of all chord templates: 12 roots × (16 base qualities +
///         @ref extended_chord_qualities)
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
/// @brief Converts chord quality to string.
std::string chord_quality_to_string(ChordQuality quality);

/// @brief Converts pitch class to string.
std::string pitch_class_to_string(PitchClass pc);

}  // namespace sonare
