#pragma once

/// @file note_transcriber.h
/// @brief Turns audio into pitched, velocity-bearing note events.
///
/// A bridge, not a detector. The spans and the measured curves come from the
/// chains that already produce them -- pYIN cut by NoteSegmenter for one line at
/// a time, the multi-F0 chain for overlapping ones -- and this file decides only
/// the three conversions a note event needs on top of them: which chain answers,
/// what MIDI note a median F0 lands on, and what velocity a measured level
/// becomes.
///
/// Output is in SAMPLES and carries no musical time. Placing notes on a bar grid
/// needs a tempo, which is the caller's, so the conversion to PPQ happens above
/// this file rather than inside it. Quantizing them is likewise not here: the
/// MIDI-FX bake already owns that grid.
///
/// Offline / control-plane only. Deterministic: no clock and no randomness, so
/// the same audio and config always yield the same notes.

#include <cstdint>
#include <vector>

#include "core/audio.h"
#include "util/constants.h"

namespace sonare::editing::note_model {

/// @brief Which chain the notes are read from.
enum class TranscribeSource : uint32_t {
  /// pYIN's F0 track cut by NoteSegmenter. One note sounds at a time; a chord
  /// arrives as whichever line the tracker followed.
  kMonophonic = 0,
  /// The multi-F0 chain. Overlapping notes are expected, and the cost is a full
  /// STFT plus a mask per tracked ridge.
  kPolyphonic = 1,
};

/// @brief What the transcription reads and how it converts what it finds.
struct TranscribeConfig {
  TranscribeSource source = TranscribeSource::kMonophonic;

  /// Tuning reference the MIDI note number is measured against. A take recorded
  /// against a different reference lands a full semitone out at roughly 26 Hz of
  /// error here, so it is worth passing a measured value rather than the default.
  float reference_hz = constants::kA4Hz;

  /// Monophonic tracker range in Hz. The polyphonic chain sets its own range
  /// from its framing and ignores both.
  float fmin = 65.0f;
  float fmax = 2093.0f;

  /// Shortest span kept as a note.
  float min_note_ms = 30.0f;
  /// Pitch movement, in cents, that ends one note and starts the next.
  float segmentation_threshold_cents = 50.0f;

  /// Level that maps to velocity 1. A note's PEAK per-frame RMS is taken in
  /// dBFS and mapped linearly from [velocity_floor_db, 0] onto [1, 127],
  /// clamped at both ends. Must be finite and negative.
  float velocity_floor_db = -48.0f;

  /// 1..127 gives every note that velocity and skips the measurement entirely;
  /// 0 measures. Anything else is rejected.
  int fixed_velocity = 0;
};

/// @brief One transcribed note.
struct TranscribedNote {
  /// Span in source samples, [onset_sample, offset_sample).
  int64_t onset_sample = 0;
  int64_t offset_sample = 0;
  /// MIDI note number, 0..127.
  uint8_t note = 0;
  /// 1..127. Never 0, because a MIDI 1.0 note-on at velocity 0 is a note-off.
  uint8_t velocity = 1;
  /// The median F0 the note number was rounded from, in Hz. Carried so a caller
  /// can recover the note's deviation from equal temperament without re-running
  /// the tracker.
  float median_hz = 0.0f;
};

/// @brief The MIDI note number @p hz lands on under @p reference_hz.
/// @return -1 when @p hz is not finite and positive, when @p reference_hz is
///         not finite and positive, or when the result falls outside 0..127.
///         A negative return is the only "no note here" spelling.
int midi_note_for_hz(float hz, float reference_hz) noexcept;

/// @brief The velocity a linear peak RMS maps to under @p velocity_floor_db.
/// @details Exposed so a caller can reproduce the mapping instead of inferring
///          it from the output. A non-finite or non-positive @p peak_rms is
///          velocity 1, which is what the floor means.
uint8_t velocity_for_peak_rms(float peak_rms, float velocity_floor_db) noexcept;

/// @brief Transcribes @p audio into note events.
/// @details Returned in (onset_sample, note) order, which is total: two notes of
///          the same pitch cannot share an onset in either chain.
///
///          Finding no notes is not an error. Silence, and material whose
///          register the chain cannot resolve, transcribe to an empty vector.
/// @throws SonareException(InvalidParameter) on empty @p audio, on a non-finite
///         or out-of-range config field, on a @c fixed_velocity outside
///         {0} U [1, 127], on a
///         @c velocity_floor_db that is not finite and negative, or on
///         @c fmin >= @c fmax.
std::vector<TranscribedNote> transcribe_notes(const Audio& audio,
                                              const TranscribeConfig& config = {});

}  // namespace sonare::editing::note_model
