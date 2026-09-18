/// @file sonare_c_note_targets_test.cpp
/// @brief Tests for the note-target C API: sonare_note_targets_from_smf,
///        sonare_assign_note_targets and their config seeder.
///
/// Notes are hand-built at 48 kHz on half-second spans and every median is a
/// whole MIDI number, so each expected shift is an exact integer rather than a
/// tolerance. The reference melodies are real SMF bytes written by the library's
/// own exporter -- the fixture, not the thing under test.

#include <sonare/sonare_c.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <tuple>
#include <vector>

#if defined(SONARE_WITH_ARRANGEMENT)
#include "midi/midi_clip.h"
#include "midi/smf.h"
#include "midi/ump.h"
#include "transport/tempo_map.h"
#endif

namespace {

constexpr int kSampleRate = 48000;
constexpr int64_t kHalfSecond = kSampleRate / 2;
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
/// 440 Hz is MIDI 69 exactly, so every expected shift below is target - 69.
constexpr float kA4Hz = 440.0f;

SonareNoteObject note_at(int64_t onset_sample, int64_t offset_sample, float median_hz) {
  SonareNoteObject note{};
  note.onset_sample = onset_sample;
  note.offset_sample = offset_sample;
  note.median_hz = median_hz;
  note.edit.time_stretch_ratio = 1.0f;
  return note;
}

SonareNoteTarget target_at(double start_sec, double end_sec, float target_midi) {
  SonareNoteTarget target{};
  target.start_sec = start_sec;
  target.end_sec = end_sec;
  target.target_midi = target_midi;
  return target;
}

#if defined(SONARE_WITH_ARRANGEMENT)

/// A melody clip: one note per (note number, start ppq, length ppq) entry.
sonare::midi::MidiClip melody_clip(const std::vector<std::tuple<uint8_t, double, double>>& notes) {
  sonare::midi::MidiClip clip;
  for (const auto& [note, start_ppq, length_ppq] : notes) {
    clip.add_event({start_ppq, sonare::midi::make_midi1_note_on(0, 0, note, 100), nullptr, 0});
    clip.add_event(
        {start_ppq + length_ppq, sonare::midi::make_midi1_note_off(0, 0, note, 0), nullptr, 0});
  }
  return clip;
}

/// The same two quarter notes every SMF case below reads, at 120 BPM: C4 over
/// 0-0.5 s and G4 over 0.5-1.0 s.
std::vector<uint8_t> two_quarter_notes_smf() {
  sonare::transport::TempoSegment tempo;
  tempo.start_ppq = 0.0;
  tempo.bpm = 120.0;
  const auto exported = sonare::midi::export_smf({melody_clip({{60, 0.0, 1.0}, {67, 1.0, 1.0}})},
                                                 {tempo}, {}, {}, {});
  REQUIRE(exported.ok());
  return exported.bytes;
}

#endif

}  // namespace

#if defined(SONARE_WITH_PITCH_EDITOR)

TEST_CASE("the note-target config seeder reports the core's own defaults",
          "[c_api][note_targets]") {
  SonareNoteTargetAssignConfig config{};
  REQUIRE(sonare_note_target_assign_config_default(&config) == SONARE_OK);
  REQUIRE(config.unmatched_policy == SONARE_NOTE_TARGET_UNMATCHED_LEAVE);
  REQUIRE(config.min_overlap_ratio == 0.5f);
  REQUIRE(config.max_correction_semitones == 12.0f);

  REQUIRE(sonare_note_target_assign_config_default(nullptr) == SONARE_ERROR_INVALID_PARAMETER);
}

TEST_CASE("assignment writes the shift its target asks for", "[c_api][note_targets]") {
  std::vector<SonareNoteObject> notes{note_at(0, kHalfSecond, kA4Hz),
                                      note_at(kHalfSecond, 2 * kHalfSecond, kA4Hz)};
  const std::vector<SonareNoteTarget> targets{target_at(0.0, 0.5, 60.0f),
                                              target_at(0.5, 1.0, 72.0f)};

  size_t assigned = 0;
  REQUIRE(sonare_assign_note_targets(notes.data(), notes.size(), kSampleRate, targets.data(),
                                     targets.size(), nullptr, &assigned) == SONARE_OK);
  REQUIRE(assigned == 2);
  REQUIRE(notes[0].edit.pitch_shift_semitones == Catch::Approx(-9.0).margin(1e-4));
  REQUIRE(notes[1].edit.pitch_shift_semitones == Catch::Approx(3.0).margin(1e-4));
  REQUIRE(notes[0].edit.muted == 0);
}

TEST_CASE("the overlap ratio decides whether a target counts", "[c_api][note_targets]") {
  // The target covers 0.2 s of a 0.5 s note, which is 40%.
  std::vector<SonareNoteObject> notes{note_at(0, kHalfSecond, kA4Hz)};
  const std::vector<SonareNoteTarget> targets{target_at(0.3, 0.7, 72.0f)};

  SonareNoteTargetAssignConfig config{};
  REQUIRE(sonare_note_target_assign_config_default(&config) == SONARE_OK);

  size_t assigned = 0;
  REQUIRE(sonare_assign_note_targets(notes.data(), notes.size(), kSampleRate, targets.data(),
                                     targets.size(), &config, &assigned) == SONARE_OK);
  REQUIRE(assigned == 0);
  REQUIRE(notes[0].edit.pitch_shift_semitones == 0.0f);

  // The same call with the threshold below the measured overlap: the input, the
  // target and the note are unchanged, so only the ratio can explain the answer.
  config.min_overlap_ratio = 0.25f;
  REQUIRE(sonare_assign_note_targets(notes.data(), notes.size(), kSampleRate, targets.data(),
                                     targets.size(), &config, &assigned) == SONARE_OK);
  REQUIRE(assigned == 1);
  REQUIRE(notes[0].edit.pitch_shift_semitones == Catch::Approx(3.0).margin(1e-4));
}

TEST_CASE("each unmatched policy does what it says", "[c_api][note_targets]") {
  // One note over the target, one note a second later with nothing near it.
  const std::vector<SonareNoteTarget> targets{target_at(0.0, 0.5, 72.0f)};
  SonareNoteTargetAssignConfig config{};
  REQUIRE(sonare_note_target_assign_config_default(&config) == SONARE_OK);

  SECTION("leave") {
    std::vector<SonareNoteObject> notes{note_at(0, kHalfSecond, kA4Hz),
                                        note_at(3 * kHalfSecond, 4 * kHalfSecond, kA4Hz)};
    size_t assigned = 0;
    REQUIRE(sonare_assign_note_targets(notes.data(), notes.size(), kSampleRate, targets.data(),
                                       targets.size(), &config, &assigned) == SONARE_OK);
    REQUIRE(assigned == 1);
    REQUIRE(notes[1].edit.pitch_shift_semitones == 0.0f);
    REQUIRE(notes[1].edit.muted == 0);
  }

  SECTION("mute") {
    std::vector<SonareNoteObject> notes{note_at(0, kHalfSecond, kA4Hz),
                                        note_at(3 * kHalfSecond, 4 * kHalfSecond, kA4Hz)};
    config.unmatched_policy = SONARE_NOTE_TARGET_UNMATCHED_MUTE;
    size_t assigned = 0;
    REQUIRE(sonare_assign_note_targets(notes.data(), notes.size(), kSampleRate, targets.data(),
                                       targets.size(), &config, &assigned) == SONARE_OK);
    REQUIRE(assigned == 1);
    REQUIRE(notes[1].edit.muted == 1);
    REQUIRE(notes[1].edit.pitch_shift_semitones == 0.0f);
    // The matched note is not muted, so the policy reached only the other one.
    REQUIRE(notes[0].edit.muted == 0);
  }

  SECTION("nearest") {
    std::vector<SonareNoteObject> notes{note_at(0, kHalfSecond, kA4Hz),
                                        note_at(3 * kHalfSecond, 4 * kHalfSecond, kA4Hz)};
    config.unmatched_policy = SONARE_NOTE_TARGET_UNMATCHED_NEAREST;
    size_t assigned = 0;
    REQUIRE(sonare_assign_note_targets(notes.data(), notes.size(), kSampleRate, targets.data(),
                                       targets.size(), &config, &assigned) == SONARE_OK);
    REQUIRE(assigned == 2);
    REQUIRE(notes[1].edit.pitch_shift_semitones == Catch::Approx(3.0).margin(1e-4));
    REQUIRE(notes[1].edit.muted == 0);
  }
}

TEST_CASE("a note with no measured pitch is never edited, whatever the policy",
          "[c_api][note_targets]") {
  const std::vector<SonareNoteTarget> targets{target_at(0.0, 0.5, 72.0f)};
  SonareNoteTargetAssignConfig config{};
  REQUIRE(sonare_note_target_assign_config_default(&config) == SONARE_OK);

  // Both spellings of "no pitch": the extractor's zero and a track's NaN. Each
  // note lies over the target, so an assignment is available to every one of them
  // and only the missing median can be keeping them out.
  for (const float median_hz : {0.0f, kNaN, -1.0f}) {
    for (const int32_t policy :
         {SONARE_NOTE_TARGET_UNMATCHED_MUTE, SONARE_NOTE_TARGET_UNMATCHED_NEAREST}) {
      std::vector<SonareNoteObject> notes{note_at(0, kHalfSecond, median_hz)};
      config.unmatched_policy = policy;
      size_t assigned = 0;
      REQUIRE(sonare_assign_note_targets(notes.data(), notes.size(), kSampleRate, targets.data(),
                                         targets.size(), &config, &assigned) == SONARE_OK);
      REQUIRE(assigned == 0);
      REQUIRE(notes[0].edit.pitch_shift_semitones == 0.0f);
      REQUIRE(notes[0].edit.muted == 0);
    }
  }
}

TEST_CASE("a correction past the bound saturates rather than being refused",
          "[c_api][note_targets]") {
  std::vector<SonareNoteObject> notes{note_at(0, kHalfSecond, kA4Hz),
                                      note_at(kHalfSecond, 2 * kHalfSecond, kA4Hz)};
  // Two octaves up and two down from MIDI 69.
  const std::vector<SonareNoteTarget> targets{target_at(0.0, 0.5, 93.0f),
                                              target_at(0.5, 1.0, 45.0f)};

  SonareNoteTargetAssignConfig config{};
  REQUIRE(sonare_note_target_assign_config_default(&config) == SONARE_OK);
  config.max_correction_semitones = 12.0f;

  size_t assigned = 0;
  REQUIRE(sonare_assign_note_targets(notes.data(), notes.size(), kSampleRate, targets.data(),
                                     targets.size(), &config, &assigned) == SONARE_OK);
  REQUIRE(assigned == 2);
  REQUIRE(notes[0].edit.pitch_shift_semitones == Catch::Approx(12.0).margin(1e-4));
  REQUIRE(notes[1].edit.pitch_shift_semitones == Catch::Approx(-12.0).margin(1e-4));

  // A zero bound is its own value, not a request for the default: the notes are
  // still counted as assigned and neither one moves.
  notes[0].edit.pitch_shift_semitones = 0.0f;
  notes[1].edit.pitch_shift_semitones = 0.0f;
  config.max_correction_semitones = 0.0f;
  REQUIRE(sonare_assign_note_targets(notes.data(), notes.size(), kSampleRate, targets.data(),
                                     targets.size(), &config, &assigned) == SONARE_OK);
  REQUIRE(assigned == 2);
  REQUIRE(notes[0].edit.pitch_shift_semitones == 0.0f);
  REQUIRE(notes[1].edit.pitch_shift_semitones == 0.0f);
}

TEST_CASE("assignment leaves every field it does not write", "[c_api][note_targets]") {
  // Editing in place only means anything if the rest of the note survives it, and
  // nothing else in the C API rewrites a caller's array.
  std::vector<SonareNoteObject> notes{note_at(0, kHalfSecond, kA4Hz)};
  notes[0].amplitude_offset = 7;
  notes[0].frame_start = 3;
  notes[0].frame_end = 11;
  notes[0].median_cents = 123.0f;
  notes[0].f0_stability = 0.75f;
  notes[0].edit.time_offset_samples = 64;
  notes[0].edit.envelope_offset = 2;
  notes[0].edit.envelope_count = 5;
  notes[0].edit.gain_db = -3.0f;
  notes[0].edit.time_stretch_ratio = 1.5f;
  notes[0].edit.formant_shift_semitones = 2.0f;
  notes[0].edit.vibrato_depth_change = 0.25f;
  notes[0].edit.drift_change = -0.5f;
  const SonareNoteObject before = notes[0];

  const std::vector<SonareNoteTarget> targets{target_at(0.0, 0.5, 72.0f)};
  size_t assigned = 0;
  REQUIRE(sonare_assign_note_targets(notes.data(), notes.size(), kSampleRate, targets.data(),
                                     targets.size(), nullptr, &assigned) == SONARE_OK);
  REQUIRE(assigned == 1);
  REQUIRE(notes[0].edit.pitch_shift_semitones == Catch::Approx(3.0).margin(1e-4));

  REQUIRE(notes[0].onset_sample == before.onset_sample);
  REQUIRE(notes[0].offset_sample == before.offset_sample);
  REQUIRE(notes[0].amplitude_offset == before.amplitude_offset);
  REQUIRE(notes[0].frame_start == before.frame_start);
  REQUIRE(notes[0].frame_end == before.frame_end);
  REQUIRE(notes[0].median_hz == before.median_hz);
  REQUIRE(notes[0].median_cents == before.median_cents);
  REQUIRE(notes[0].f0_stability == before.f0_stability);
  REQUIRE(notes[0].edit.time_offset_samples == before.edit.time_offset_samples);
  REQUIRE(notes[0].edit.envelope_offset == before.edit.envelope_offset);
  REQUIRE(notes[0].edit.envelope_count == before.edit.envelope_count);
  REQUIRE(notes[0].edit.gain_db == before.edit.gain_db);
  REQUIRE(notes[0].edit.time_stretch_ratio == before.edit.time_stretch_ratio);
  REQUIRE(notes[0].edit.formant_shift_semitones == before.edit.formant_shift_semitones);
  REQUIRE(notes[0].edit.vibrato_depth_change == before.edit.vibrato_depth_change);
  REQUIRE(notes[0].edit.drift_change == before.edit.drift_change);
}

TEST_CASE("assignment keeps an edit the caller already made when no target reaches it",
          "[c_api][note_targets]") {
  // LEAVE has to leave the caller's own shift alone rather than reset it, and a
  // note already muted stays muted.
  std::vector<SonareNoteObject> notes{note_at(3 * kHalfSecond, 4 * kHalfSecond, kA4Hz)};
  notes[0].edit.pitch_shift_semitones = -4.0f;
  notes[0].edit.muted = 1;

  const std::vector<SonareNoteTarget> targets{target_at(0.0, 0.5, 72.0f)};
  size_t assigned = 0;
  REQUIRE(sonare_assign_note_targets(notes.data(), notes.size(), kSampleRate, targets.data(),
                                     targets.size(), nullptr, &assigned) == SONARE_OK);
  REQUIRE(assigned == 0);
  REQUIRE(notes[0].edit.pitch_shift_semitones == -4.0f);
  REQUIRE(notes[0].edit.muted == 1);
}

TEST_CASE("assignment refuses arguments it cannot act on", "[c_api][note_targets]") {
  std::vector<SonareNoteObject> notes{note_at(0, kHalfSecond, kA4Hz)};
  const std::vector<SonareNoteTarget> targets{target_at(0.0, 0.5, 72.0f)};
  size_t assigned = 0;

  REQUIRE(sonare_assign_note_targets(notes.data(), notes.size(), kSampleRate, targets.data(),
                                     targets.size(), nullptr,
                                     nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_assign_note_targets(nullptr, 1, kSampleRate, targets.data(), targets.size(),
                                     nullptr, &assigned) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_assign_note_targets(notes.data(), notes.size(), kSampleRate, nullptr, 1, nullptr,
                                     &assigned) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_assign_note_targets(notes.data(), notes.size(), 0, targets.data(), targets.size(),
                                     nullptr, &assigned) == SONARE_ERROR_INVALID_PARAMETER);

  SonareNoteTargetAssignConfig config{};
  REQUIRE(sonare_note_target_assign_config_default(&config) == SONARE_OK);
  config.unmatched_policy = 7;
  REQUIRE(sonare_assign_note_targets(notes.data(), notes.size(), kSampleRate, targets.data(),
                                     targets.size(), &config,
                                     &assigned) == SONARE_ERROR_INVALID_PARAMETER);

  REQUIRE(sonare_note_target_assign_config_default(&config) == SONARE_OK);
  config.min_overlap_ratio = 1.5f;
  REQUIRE(sonare_assign_note_targets(notes.data(), notes.size(), kSampleRate, targets.data(),
                                     targets.size(), &config,
                                     &assigned) == SONARE_ERROR_INVALID_PARAMETER);

  // A non-finite target would be selected and written into the shift: every
  // overlap comparison is false for a NaN, so nothing downstream rejects it.
  for (const SonareNoteTarget bad :
       {target_at(kNaN, 0.5, 72.0f), target_at(0.0, kNaN, 72.0f), target_at(0.0, 0.5, kNaN)}) {
    const std::vector<SonareNoteTarget> one{bad};
    REQUIRE(sonare_assign_note_targets(notes.data(), notes.size(), kSampleRate, one.data(),
                                       one.size(), nullptr,
                                       &assigned) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(notes[0].edit.pitch_shift_semitones == 0.0f);
  }

  // An empty note set and an empty reference are both legitimate.
  REQUIRE(sonare_assign_note_targets(nullptr, 0, kSampleRate, nullptr, 0, nullptr, &assigned) ==
          SONARE_OK);
  REQUIRE(assigned == 0);
}

#else

TEST_CASE("the note-target assignment C API reports NOT_SUPPORTED without the pitch editor",
          "[c_api][note_targets]") {
  SonareNoteTargetAssignConfig config{};
  REQUIRE(sonare_note_target_assign_config_default(&config) == SONARE_ERROR_NOT_SUPPORTED);

  size_t assigned = 0;
  REQUIRE(sonare_assign_note_targets(nullptr, 0, 48000, nullptr, 0, nullptr, &assigned) ==
          SONARE_ERROR_NOT_SUPPORTED);
}

#endif

#if defined(SONARE_WITH_ARRANGEMENT)

TEST_CASE("the SMF reader returns one target per closed note", "[c_api][note_targets]") {
  const std::vector<uint8_t> bytes = two_quarter_notes_smf();

  SonareNoteTarget* targets = nullptr;
  size_t count = 0;
  REQUIRE(sonare_note_targets_from_smf(bytes.data(), bytes.size(), 0, &targets, &count) ==
          SONARE_OK);
  REQUIRE(count == 2);
  REQUIRE(targets != nullptr);
  REQUIRE(targets[0].start_sec == Catch::Approx(0.0).margin(1e-4));
  REQUIRE(targets[0].end_sec == Catch::Approx(0.5).margin(1e-4));
  REQUIRE(targets[0].target_midi == 60.0f);
  REQUIRE(targets[1].start_sec == Catch::Approx(0.5).margin(1e-4));
  REQUIRE(targets[1].end_sec == Catch::Approx(1.0).margin(1e-4));
  REQUIRE(targets[1].target_midi == 67.0f);

  sonare_free_note_targets(targets);
  sonare_free_note_targets(nullptr);
}

TEST_CASE("the SMF reader refuses what it cannot read", "[c_api][note_targets]") {
  const std::vector<uint8_t> bytes = two_quarter_notes_smf();
  SonareNoteTarget* targets = nullptr;
  size_t count = 0;

  // The exporter writes the tempo map as its own track, which carries no MIDI, so
  // the one melody is the only index this file has.
  REQUIRE(sonare_note_targets_from_smf(bytes.data(), bytes.size(), 1, &targets, &count) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(targets == nullptr);
  REQUIRE(count == 0);
  REQUIRE(sonare_note_targets_from_smf(bytes.data(), bytes.size(), -1, &targets, &count) ==
          SONARE_ERROR_INVALID_PARAMETER);

  const std::vector<uint8_t> garbage(64, 0x7F);
  REQUIRE(sonare_note_targets_from_smf(garbage.data(), garbage.size(), 0, &targets, &count) ==
          SONARE_ERROR_INVALID_FORMAT);
  REQUIRE(sonare_note_targets_from_smf(nullptr, 32, 0, &targets, &count) ==
          SONARE_ERROR_INVALID_PARAMETER);
  // An empty buffer is not a readable file, and a NULL one with a zero length has
  // to reach that answer rather than the dereference it looks like.
  REQUIRE(sonare_note_targets_from_smf(bytes.data(), 0, 0, &targets, &count) ==
          SONARE_ERROR_INVALID_FORMAT);
  REQUIRE(sonare_note_targets_from_smf(nullptr, 0, 0, &targets, &count) ==
          SONARE_ERROR_INVALID_FORMAT);
  REQUIRE(sonare_note_targets_from_smf(bytes.data(), bytes.size(), 0, nullptr, &count) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_note_targets_from_smf(bytes.data(), bytes.size(), 0, &targets, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
}

#if defined(SONARE_WITH_PITCH_EDITOR)

TEST_CASE("a take assigned from an SMF ends up at the pitches the file wrote",
          "[c_api][note_targets]") {
  // The whole reach of this surface in one call chain: bytes in, corrected
  // medians out. Both notes were sung at A4, and the file asks for C4 then G4.
  const std::vector<uint8_t> bytes = two_quarter_notes_smf();

  SonareNoteTarget* targets = nullptr;
  size_t count = 0;
  REQUIRE(sonare_note_targets_from_smf(bytes.data(), bytes.size(), 0, &targets, &count) ==
          SONARE_OK);
  REQUIRE(count == 2);

  std::vector<SonareNoteObject> notes{note_at(0, kHalfSecond, kA4Hz),
                                      note_at(kHalfSecond, 2 * kHalfSecond, kA4Hz)};
  size_t assigned = 0;
  REQUIRE(sonare_assign_note_targets(notes.data(), notes.size(), kSampleRate, targets, count,
                                     nullptr, &assigned) == SONARE_OK);
  REQUIRE(assigned == 2);
  // 69 + shift is the pitch the note will render at, and that is what the file
  // said: 60 and 67.
  REQUIRE(69.0f + notes[0].edit.pitch_shift_semitones == Catch::Approx(60.0).margin(1e-4));
  REQUIRE(69.0f + notes[1].edit.pitch_shift_semitones == Catch::Approx(67.0).margin(1e-4));

  sonare_free_note_targets(targets);
}

#endif

#else

TEST_CASE("the SMF reference reader reports NOT_SUPPORTED without the arrangement layer",
          "[c_api][note_targets]") {
  const std::vector<uint8_t> bytes(64, 0x00);
  SonareNoteTarget* targets = nullptr;
  size_t count = 0;
  REQUIRE(sonare_note_targets_from_smf(bytes.data(), bytes.size(), 0, &targets, &count) ==
          SONARE_ERROR_NOT_SUPPORTED);
  REQUIRE(targets == nullptr);
  REQUIRE(count == 0);
}

#endif
