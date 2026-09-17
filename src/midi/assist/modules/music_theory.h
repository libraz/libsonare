#pragma once

/// @file music_theory.h
/// @brief Pitch-class arithmetic shared by the built-in assist modules.
///
/// Deliberately small and deliberately not a music-theory library: it answers
/// only the three questions the modules ask -- which pitch classes a key admits,
/// which ones a chord symbol sounds, and how rough an interval is. Every
/// function is pure, total, and free of allocation beyond its return value.
///
/// An empty pitch-class set means "no constraint stated", never "no pitch class
/// allowed". The modules read it that way, so an unknown key or chord relaxes a
/// rule instead of rejecting everything.

#include <cstdint>
#include <vector>

#include "arrangement/harmonic_timeline.h"

namespace sonare::midi::assist::theory {

/// @brief Pitch classes of the diatonic collection @p mode places on @p tonic_pc.
/// @return Empty for an unknown mode or an out-of-range tonic.
std::vector<uint8_t> scale_pitch_classes(uint8_t tonic_pc, arrangement::KeyMode mode);

/// @brief Pitch classes @p chord sounds: its family's triad, the extension
///        degrees it lists, and its slash bass when it has one.
/// @details An extension's quality follows the family rather than carrying one
///          of its own -- a 7 over a major chord is the major seventh and a 7
///          over a dominant or minor one is the minor seventh -- because the
///          symbol type stores degrees, not alterations.
/// @return Empty for an unknown quality or an out-of-range root.
std::vector<uint8_t> chord_pitch_classes(const arrangement::ChordSymbol& chord);

/// @brief Interval class 0..6 between two MIDI notes: octave-equivalent and
///        inversionally symmetric, so a fifth and a fourth are both 5.
int interval_class(int note_a, int note_b) noexcept;

/// @brief Roughness of an interval class in [0, 1], 0 being the most consonant.
/// @details Ordered by the classical consonance ranking rather than fitted to a
///          measurement: unison and octave, then fifths, thirds and sixths, then
///          seconds and sevenths, with the tritone placed between the last two.
float interval_class_roughness(int interval_class) noexcept;

/// @brief Whether @p midi_note's pitch class is in @p pitch_classes.
/// @return True when @p pitch_classes is empty -- an unstated set constrains
///         nothing.
bool admits(const std::vector<uint8_t>& pitch_classes, int midi_note) noexcept;

/// @brief Moves @p midi_note by @p steps positions through @p pitch_classes.
/// @details A STEP is a position in the set, not a fixed interval, which is what
///          makes "a third below" follow the key instead of alternating between
///          a major and a minor third by accident. @p midi_note is snapped into
///          the set first when it is not already in it.
/// @return -1 when @p pitch_classes is empty, when nothing snaps, or when the
///         result leaves 0..127 -- there is no partial answer to return.
int transpose_scale_steps(const std::vector<uint8_t>& pitch_classes, int midi_note,
                          int steps) noexcept;

/// @brief The note nearest @p midi_note whose pitch class is in @p pitch_classes,
///        searching outward and preferring the lower of two equal distances.
/// @return @p midi_note unchanged when @p pitch_classes is empty, and -1 when no
///         note in [0, 127] qualifies.
int nearest_admitted(const std::vector<uint8_t>& pitch_classes, int midi_note) noexcept;

}  // namespace sonare::midi::assist::theory
