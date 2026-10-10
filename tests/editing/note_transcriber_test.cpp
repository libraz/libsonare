/// @file note_transcriber_test.cpp
/// @brief Tests for the audio-to-note bridge: the sequence a synthesized take
///        transcribes back to, and the two conversions the bridge owns on top of
///        the chains it reads -- Hz to a MIDI note number, and a measured level
///        to a velocity.
///
/// The round-trip case is the headline, and it is stated against material whose
/// note numbers are known by construction: equal-tempered sines at exact
/// frequencies, separated by silence so the segmenter has an onset to cut on. A
/// second sequence one whole tone away runs beside it, so the file records that
/// the expectation discriminates rather than that it merely held once.

#include "editing/note_model/note_transcriber.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "core/audio.h"
#include "editing/polyphony/f0_salience.h"
#include "feature/pitch.h"
#include "util/constants.h"
#include "util/exception.h"

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using namespace sonare::editing::note_model;

namespace {

constexpr int kSampleRate = 22050;
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

/// The tracker's hop at this rate. Every span the segmenter emits starts on a
/// frame boundary, so an onset can be no closer to the true one than this.
constexpr int64_t kHopSamples = 512;
/// Two hops: the boundary the run starts on plus the frame the voicing decision
/// settles in. The sequence below lands 478 to 836 samples early against it.
constexpr int64_t kOnsetToleranceSamples = 2 * kHopSamples;

/// The default floor, and one shallow enough to move every measured velocity.
constexpr float kDeepFloorDb = -48.0f;
constexpr float kShallowFloorDb = -12.0f;

constexpr int kToneSamples = kSampleRate / 4;       // 0.25 s
constexpr int kGapSamples = kSampleRate * 6 / 100;  // 0.06 s
constexpr float kEdgeSamples = 0.005f * kSampleRate;

float hz_for_midi(int note) {
  return sonare::constants::kA4Hz *
         std::pow(2.0f, static_cast<float>(note - 69) / sonare::constants::kSemitonesPerOctave);
}

/// @brief A tone with raised-cosine edges, so the level rises into the note
///        rather than stepping into it.
void append_tone(std::vector<float>& into, float hz, float amplitude, int samples) {
  for (int i = 0; i < samples; ++i) {
    const float from_start = static_cast<float>(i);
    const float from_end = static_cast<float>(samples - 1 - i);
    const float edge = std::min(1.0f, std::min(from_start, from_end) / kEdgeSamples);
    const float envelope = 0.5f - 0.5f * std::cos(sonare::constants::kTwoPi * 0.5f * edge);
    into.push_back(amplitude * envelope *
                   static_cast<float>(std::sin(sonare::constants::kTwoPiD * hz *
                                               static_cast<double>(i) / kSampleRate)));
  }
}

void append_silence(std::vector<float>& into, int samples) {
  into.insert(into.end(), static_cast<size_t>(samples), 0.0f);
}

/// @brief A take whose note numbers and onsets are known by construction.
struct Sequence {
  std::vector<float> samples;
  std::vector<int> notes;
  std::vector<int64_t> onsets;

  sonare::Audio audio() const {
    return sonare::Audio::from_buffer(samples.data(), samples.size(), kSampleRate);
  }
};

/// @brief @p midi_notes as separated tones, after a lead-in silence so an onset
///        of 0 is wrong for every note rather than right for the first.
Sequence sequence_of(const std::vector<int>& midi_notes, float amplitude = 0.5f) {
  Sequence sequence;
  append_silence(sequence.samples, kGapSamples);
  for (const int note : midi_notes) {
    sequence.onsets.push_back(static_cast<int64_t>(sequence.samples.size()));
    sequence.notes.push_back(note);
    append_tone(sequence.samples, hz_for_midi(note), amplitude, kToneSamples);
    append_silence(sequence.samples, kGapSamples);
  }
  return sequence;
}

/// @brief One sustained tone, for the cases that are about a config field rather
///        than about segmentation.
sonare::Audio sustained(int midi_note, float amplitude = 0.5f) {
  std::vector<float> samples;
  append_tone(samples, hz_for_midi(midi_note), amplitude, kSampleRate / 2);
  return sonare::Audio::from_buffer(samples.data(), samples.size(), kSampleRate);
}

/// @brief The note numbers, as ints so a failed comparison prints numbers.
std::vector<int> note_numbers(const std::vector<TranscribedNote>& notes) {
  std::vector<int> numbers;
  numbers.reserve(notes.size());
  for (const TranscribedNote& note : notes) numbers.push_back(note.note);
  return numbers;
}

/// @brief Every field of a transcribed note, so "the same notes" is one call.
void require_same_notes(const std::vector<TranscribedNote>& actual,
                        const std::vector<TranscribedNote>& expected) {
  REQUIRE(actual.size() == expected.size());
  for (size_t i = 0; i < expected.size(); ++i) {
    INFO("note " << i);
    REQUIRE(actual[i].onset_sample == expected[i].onset_sample);
    REQUIRE(actual[i].offset_sample == expected[i].offset_sample);
    REQUIRE(actual[i].note == expected[i].note);
    REQUIRE(actual[i].velocity == expected[i].velocity);
    REQUIRE(actual[i].median_hz == expected[i].median_hz);
  }
}

/// @brief The linear peak RMS a decibel level reads as.
float linear_of(float db) { return std::pow(10.0f, db / 20.0f); }

}  // namespace

// --- Round trip on synthesized material -----------------------------------

TEST_CASE(
    "transcribe_notes returns the sequence a take was built from, and says so of one take only",
    "[note_transcriber]") {
  const Sequence intended = sequence_of({60, 64, 67, 72});
  const std::vector<TranscribedNote> notes = transcribe_notes(intended.audio());

  INFO("transcribed " << notes.size() << " notes");
  REQUIRE(notes.size() == intended.notes.size());
  REQUIRE(note_numbers(notes) == intended.notes);

  for (size_t i = 0; i < notes.size(); ++i) {
    INFO("note " << i << ": onset " << notes[i].onset_sample << ", intended " << intended.onsets[i]
                 << ", tolerance " << kOnsetToleranceSamples << " samples (two tracker hops of "
                 << kHopSamples << " at " << kSampleRate << " Hz)");
    CHECK(std::llabs(notes[i].onset_sample - intended.onsets[i]) <= kOnsetToleranceSamples);
    CHECK(notes[i].offset_sample > notes[i].onset_sample);
    // The pitch the note number was rounded from, so a right number reached by a
    // wrong measurement is still a failure.
    CHECK_THAT(notes[i].median_hz, WithinRel(hz_for_midi(intended.notes[i]), 0.02f));
    // -9 dBFS of peak RMS against the -48 dB floor is roughly velocity 103; a
    // measurement that collapsed to the floor would land at 1.
    INFO("velocity " << static_cast<int>(notes[i].velocity));
    CHECK(notes[i].velocity > 64);
    CHECK(notes[i].velocity <= 127);
  }

  SECTION("a different sequence is a different answer") {
    // The control. Without it the expectation above is compatible with a
    // transcriber that answers with a fixed sequence, or with one whose note
    // numbers come from the fixture rather than from the audio.
    const Sequence other = sequence_of({62, 65, 69, 74});
    const std::vector<TranscribedNote> other_notes = transcribe_notes(other.audio());
    REQUIRE(note_numbers(other_notes) == other.notes);
    REQUIRE(note_numbers(other_notes) != intended.notes);
  }
}

TEST_CASE("transcribe_notes answers the same audio identically", "[note_transcriber]") {
  const Sequence sequence = sequence_of({60, 64, 67, 72});
  TranscribeConfig config;
  config.fixed_velocity = 0;

  const std::vector<TranscribedNote> first = transcribe_notes(sequence.audio(), config);
  const std::vector<TranscribedNote> second = transcribe_notes(sequence.audio(), config);
  REQUIRE(!first.empty());
  require_same_notes(second, first);
}

// --- midi_note_for_hz -----------------------------------------------------

TEST_CASE("midi_note_for_hz places a frequency on the equal-tempered grid", "[note_transcriber]") {
  const float a4 = sonare::constants::kA4Hz;

  REQUIRE(midi_note_for_hz(a4, a4) == 69);
  REQUIRE(midi_note_for_hz(261.6256f, a4) == 60);
  REQUIRE(midi_note_for_hz(523.2511f, a4) == 72);

  SECTION("an octave is twelve") {
    for (const int note : {24, 36, 48, 60, 72, 84, 96}) {
      INFO("MIDI " << note);
      REQUIRE(midi_note_for_hz(hz_for_midi(note), a4) == note);
      REQUIRE(midi_note_for_hz(2.0f * hz_for_midi(note), a4) == note + 12);
      REQUIRE(midi_note_for_hz(0.5f * hz_for_midi(note), a4) == note - 12);
    }
  }

  SECTION("rounding takes the nearest note, both ways") {
    // A quarter tone is 50 cents; a third of a semitone lands inside the nearest
    // note's half, so these are the two sides of one boundary rather than a
    // restatement of the exact case.
    const float third_of_a_semitone = std::pow(2.0f, 1.0f / 36.0f);
    REQUIRE(midi_note_for_hz(a4 * third_of_a_semitone, a4) == 69);
    REQUIRE(midi_note_for_hz(a4 / third_of_a_semitone, a4) == 69);
    const float two_thirds = std::pow(2.0f, 2.0f / 36.0f);
    REQUIRE(midi_note_for_hz(a4 * two_thirds, a4) == 70);
    REQUIRE(midi_note_for_hz(a4 / two_thirds, a4) == 68);
  }

  SECTION("a negative is the only no-note spelling") {
    const float a4_reference = a4;
    // Enumerated one by one: an infinity compares normally and would pass a bare
    // ordering test that a NaN fails, so neither stands in for the other.
    REQUIRE(midi_note_for_hz(0.0f, a4_reference) == -1);
    REQUIRE(midi_note_for_hz(-440.0f, a4_reference) == -1);
    REQUIRE(midi_note_for_hz(kNaN, a4_reference) == -1);
    REQUIRE(midi_note_for_hz(kInf, a4_reference) == -1);
    REQUIRE(midi_note_for_hz(-kInf, a4_reference) == -1);

    // Below MIDI 0 (8.1758 Hz) and above MIDI 127 (12543.85 Hz).
    REQUIRE(midi_note_for_hz(4.0f, a4_reference) == -1);
    REQUIRE(midi_note_for_hz(20000.0f, a4_reference) == -1);
    // The two notes just inside those ends are notes, which is what says the
    // refusals above are about the range rather than about the whole extreme.
    REQUIRE(midi_note_for_hz(hz_for_midi(0), a4_reference) == 0);
    REQUIRE(midi_note_for_hz(hz_for_midi(127), a4_reference) == 127);
  }

  SECTION("an unusable reference refuses every frequency") {
    for (const float reference : {0.0f, -440.0f, kNaN, kInf, -kInf}) {
      INFO("reference " << reference);
      REQUIRE(midi_note_for_hz(a4, reference) == -1);
      REQUIRE(midi_note_for_hz(261.6256f, reference) == -1);
    }
  }
}

// --- velocity_for_peak_rms ------------------------------------------------

TEST_CASE("velocity_for_peak_rms maps a level onto the velocity range", "[note_transcriber]") {
  constexpr float kFloorDb = kDeepFloorDb;

  SECTION("both ends") {
    REQUIRE(velocity_for_peak_rms(1.0f, kFloorDb) == 127);
    // Above full scale saturates rather than running past 127.
    REQUIRE(velocity_for_peak_rms(4.0f, kFloorDb) == 127);
    REQUIRE(velocity_for_peak_rms(linear_of(kFloorDb), kFloorDb) == 1);
    REQUIRE(velocity_for_peak_rms(linear_of(kFloorDb - 12.0f), kFloorDb) == 1);
  }

  SECTION("a level midway in dB is midway in velocity") {
    // Linear in decibels, so half the floor is half the span: 1 + 0.5 * 126 = 64.
    // Tolerance 1, which is the rounding of the map's own lround and nothing more.
    const int midpoint = velocity_for_peak_rms(linear_of(0.5f * kFloorDb), kFloorDb);
    INFO("velocity at " << 0.5f * kFloorDb << " dBFS: " << midpoint);
    REQUIRE(std::abs(midpoint - 64) <= 1);

    // A quarter and three quarters up, to the same tolerance, so the map is
    // pinned along its length rather than at one interior point.
    REQUIRE(std::abs(velocity_for_peak_rms(linear_of(0.75f * kFloorDb), kFloorDb) - 33) <= 1);
    REQUIRE(std::abs(velocity_for_peak_rms(linear_of(0.25f * kFloorDb), kFloorDb) - 95) <= 1);
  }

  SECTION("monotonic in level") {
    int previous = 0;
    int strictly_increased = 0;
    for (int db = -60; db <= 6; ++db) {
      const int velocity = velocity_for_peak_rms(linear_of(static_cast<float>(db)), kFloorDb);
      INFO(db << " dBFS -> " << velocity);
      REQUIRE(velocity >= previous);
      REQUIRE(velocity >= 1);
      REQUIRE(velocity <= 127);
      if (velocity > previous) ++strictly_increased;
      previous = velocity;
    }
    // Ordered is not enough: a constant answer is non-decreasing too.
    REQUIRE(strictly_increased > 40);
  }

  SECTION("a level that is not a level is the floor") {
    for (const float peak : {0.0f, -0.5f, kNaN, kInf, -kInf}) {
      INFO("peak " << peak);
      REQUIRE(velocity_for_peak_rms(peak, kFloorDb) == 1);
    }
  }

  SECTION("a floor that is not a floor is the floor") {
    // The contract is "finite and negative"; 0 has no span to map onto and a
    // positive floor would invert the map.
    for (const float floor_db : {0.0f, 6.0f, kNaN, kInf, -kInf}) {
      INFO("floor " << floor_db);
      REQUIRE(velocity_for_peak_rms(1.0f, floor_db) == 1);
    }
  }
}

// --- Config fields that must reach the answer -----------------------------

TEST_CASE("reference_hz moves the note numbers it is measured against", "[note_transcriber]") {
  // The field is read by the rounding and by the segmenter, and a transcriber
  // that stored it and then measured against 440 anyway would pass every other
  // case in this file.
  const sonare::Audio audio = sustained(69);

  TranscribeConfig at_a440;
  const std::vector<TranscribedNote> standard = transcribe_notes(audio, at_a440);
  REQUIRE(standard.size() == 1);
  REQUIRE(standard[0].note == 69);

  TranscribeConfig a_semitone_up;
  a_semitone_up.reference_hz = sonare::constants::kA4Hz * std::pow(2.0f, 1.0f / 12.0f);
  const std::vector<TranscribedNote> raised = transcribe_notes(audio, a_semitone_up);
  REQUIRE(raised.size() == 1);
  // The same tone measured against a reference a semitone higher is a semitone
  // lower on the grid.
  REQUIRE(raised[0].note == 68);

  TranscribeConfig a_semitone_down;
  a_semitone_down.reference_hz = sonare::constants::kA4Hz / std::pow(2.0f, 1.0f / 12.0f);
  const std::vector<TranscribedNote> lowered = transcribe_notes(audio, a_semitone_down);
  REQUIRE(lowered.size() == 1);
  REQUIRE(lowered[0].note == 70);

  // The measured pitch is the same take in all three: only the grid moved.
  CHECK_THAT(raised[0].median_hz, WithinRel(standard[0].median_hz, 1.0e-3f));
  CHECK_THAT(lowered[0].median_hz, WithinRel(standard[0].median_hz, 1.0e-3f));
}

TEST_CASE("polyphonic fmin and fmax constrain the selected tracker range", "[note_transcriber]") {
  const sonare::Audio audio = sustained(69);

  TranscribeConfig inclusive;
  inclusive.source = TranscribeSource::kPolyphonic;
  inclusive.fmin = 400.0f;
  inclusive.fmax = 500.0f;
  const std::vector<TranscribedNote> in_range = transcribe_notes(audio, inclusive);
  REQUIRE(!in_range.empty());
  bool found_a4 = false;
  for (const TranscribedNote& note : in_range) {
    INFO("median " << note.median_hz);
    CHECK(note.median_hz >= inclusive.fmin);
    CHECK(note.median_hz <= inclusive.fmax);
    if (note.note == 69) found_a4 = true;
  }
  REQUIRE(found_a4);

  TranscribeConfig exclusive = inclusive;
  exclusive.fmin = 600.0f;
  exclusive.fmax = 700.0f;
  const std::vector<TranscribedNote> out_of_range = transcribe_notes(audio, exclusive);
  for (const TranscribedNote& note : out_of_range) {
    INFO("median " << note.median_hz);
    CHECK(note.median_hz >= exclusive.fmin);
    CHECK(note.median_hz <= exclusive.fmax);
    CHECK(note.note != 69);
  }
}

TEST_CASE("the F0 range resolver uses the selected source and explicit endpoints",
          "[note_transcriber]") {
  const sonare::PitchConfig mono_defaults;
  const sonare::editing::polyphony::SalienceConfig poly_defaults;

  TranscribeConfig monophonic;
  const TranscribeF0Range mono_range = resolve_transcribe_f0_range(monophonic);
  CHECK(mono_range.fmin == mono_defaults.fmin);
  CHECK(mono_range.fmax == mono_defaults.fmax);

  TranscribeConfig polyphonic;
  polyphonic.source = TranscribeSource::kPolyphonic;
  const TranscribeF0Range poly_range = resolve_transcribe_f0_range(polyphonic);
  CHECK(poly_range.fmin == poly_defaults.f0_min_hz);
  CHECK(poly_range.fmax == poly_defaults.f0_max_hz);

  monophonic.fmin = 80.0f;
  monophonic.fmax = 0.0f;
  const TranscribeF0Range mono_partial = resolve_transcribe_f0_range(monophonic);
  CHECK(mono_partial.fmin == 80.0f);
  CHECK(mono_partial.fmax == mono_defaults.fmax);

  polyphonic.fmin = 0.0f;
  polyphonic.fmax = 1500.0f;
  const TranscribeF0Range poly_partial = resolve_transcribe_f0_range(polyphonic);
  CHECK(poly_partial.fmin == poly_defaults.f0_min_hz);
  CHECK(poly_partial.fmax == 1500.0f);
}

TEST_CASE("omitted tracker bounds resolve from the selected source", "[note_transcriber]") {
  const sonare::PitchConfig mono_defaults;
  const sonare::editing::polyphony::SalienceConfig poly_defaults;

  for (const int midi_note : {33, 35}) {
    INFO("MIDI " << midi_note);
    const sonare::Audio audio = sustained(midi_note);

    TranscribeConfig poly_default;
    poly_default.source = TranscribeSource::kPolyphonic;
    poly_default.fmin = 0.0f;
    poly_default.fmax = 0.0f;
    const std::vector<TranscribedNote> default_notes = transcribe_notes(audio, poly_default);

    TranscribeConfig poly_explicit = poly_default;
    poly_explicit.fmin = poly_defaults.f0_min_hz;
    poly_explicit.fmax = poly_defaults.f0_max_hz;
    const std::vector<TranscribedNote> explicit_notes = transcribe_notes(audio, poly_explicit);
    REQUIRE(!explicit_notes.empty());
    require_same_notes(default_notes, explicit_notes);

    bool found_target = false;
    for (const TranscribedNote& note : default_notes) {
      if (note.note == midi_note) found_target = true;
    }
    REQUIRE(found_target);

    // The monophonic lower bound is the exclusion boundary for both A1 and B1.
    // A polyphonic default that accidentally inherited the monophonic range
    // would drop these notes before it could report them.
    TranscribeConfig mono_boundary = poly_default;
    mono_boundary.fmin = mono_defaults.fmin;
    mono_boundary.fmax = poly_defaults.f0_max_hz;
    const std::vector<TranscribedNote> boundary_notes = transcribe_notes(audio, mono_boundary);
    for (const TranscribedNote& note : boundary_notes) {
      INFO("boundary median " << note.median_hz);
      CHECK(note.median_hz >= mono_boundary.fmin);
      CHECK(note.median_hz <= mono_boundary.fmax);
      CHECK(note.note != midi_note);
    }
  }
}

TEST_CASE("a partial tracker range resolves and validates against source defaults",
          "[note_transcriber]") {
  const sonare::PitchConfig mono_defaults;
  const sonare::editing::polyphony::SalienceConfig poly_defaults;
  const sonare::Audio audio = sustained(69);

  SECTION("polyphonic omitted endpoints use salience defaults") {
    TranscribeConfig expected;
    expected.source = TranscribeSource::kPolyphonic;
    expected.fmin = poly_defaults.f0_min_hz;
    expected.fmax = poly_defaults.f0_max_hz;
    const std::vector<TranscribedNote> explicit_notes = transcribe_notes(audio, expected);
    REQUIRE(!explicit_notes.empty());

    TranscribeConfig omitted_low = expected;
    omitted_low.fmin = 0.0f;
    require_same_notes(transcribe_notes(audio, omitted_low), explicit_notes);

    TranscribeConfig omitted_high = expected;
    omitted_high.fmax = 0.0f;
    require_same_notes(transcribe_notes(audio, omitted_high), explicit_notes);
  }

  SECTION("the same partial values resolve against monophonic defaults") {
    TranscribeConfig expected;
    expected.fmin = mono_defaults.fmin;
    expected.fmax = mono_defaults.fmax;
    const std::vector<TranscribedNote> explicit_notes = transcribe_notes(audio, expected);
    REQUIRE(!explicit_notes.empty());

    TranscribeConfig omitted_low = expected;
    omitted_low.fmin = 0.0f;
    require_same_notes(transcribe_notes(audio, omitted_low), explicit_notes);

    TranscribeConfig omitted_high = expected;
    omitted_high.fmax = 0.0f;
    require_same_notes(transcribe_notes(audio, omitted_high), explicit_notes);
  }

  SECTION("ordering is checked after the source default is resolved") {
    TranscribeConfig polyphonic;
    polyphonic.source = TranscribeSource::kPolyphonic;
    polyphonic.fmin = 1800.0f;
    polyphonic.fmax = 0.0f;
    REQUIRE_THROWS_AS(transcribe_notes(audio, polyphonic), sonare::SonareException);

    TranscribeConfig monophonic;
    monophonic.fmin = 1800.0f;
    monophonic.fmax = 0.0f;
    REQUIRE_NOTHROW(transcribe_notes(audio, monophonic));
  }
}

TEST_CASE("fixed_velocity replaces the measurement and velocity_floor_db changes it",
          "[note_transcriber]") {
  const Sequence sequence = sequence_of({60, 64, 67, 72});

  TranscribeConfig measured;
  const std::vector<TranscribedNote> measured_notes = transcribe_notes(sequence.audio(), measured);
  REQUIRE(measured_notes.size() == sequence.notes.size());

  SECTION("a fixed velocity is every note's velocity") {
    for (const int fixed : {1, 42, 127}) {
      INFO("fixed_velocity " << fixed);
      TranscribeConfig config;
      config.fixed_velocity = fixed;
      const std::vector<TranscribedNote> notes = transcribe_notes(sequence.audio(), config);
      REQUIRE(notes.size() == measured_notes.size());
      for (const TranscribedNote& note : notes) REQUIRE(note.velocity == fixed);
    }

    // And the notes themselves are untouched, so the field replaced the velocity
    // rather than a different measurement having produced it.
    TranscribeConfig config;
    config.fixed_velocity = 42;
    const std::vector<TranscribedNote> notes = transcribe_notes(sequence.audio(), config);
    for (size_t i = 0; i < notes.size(); ++i) {
      REQUIRE(notes[i].onset_sample == measured_notes[i].onset_sample);
      REQUIRE(notes[i].note == measured_notes[i].note);
    }
    // 42 is not what the measurement answers here, so the case is not passing on
    // a coincidence.
    bool any_measured_42 = false;
    for (const TranscribedNote& note : measured_notes) {
      if (note.velocity == 42) any_measured_42 = true;
    }
    REQUIRE_FALSE(any_measured_42);
  }

  SECTION("a higher floor compresses the same level into a lower velocity") {
    TranscribeConfig shallow;
    shallow.velocity_floor_db = kShallowFloorDb;
    const std::vector<TranscribedNote> notes = transcribe_notes(sequence.audio(), shallow);
    REQUIRE(notes.size() == measured_notes.size());
    for (size_t i = 0; i < notes.size(); ++i) {
      INFO("note " << i << ": " << static_cast<int>(measured_notes[i].velocity) << " at -48 dB, "
                   << static_cast<int>(notes[i].velocity) << " at -12 dB");
      REQUIRE(notes[i].velocity < measured_notes[i].velocity);
      // Both velocities have to be the exposed map over ONE measurement, so the
      // level is recovered from the -48 dB answer and pushed back through the map
      // at -12 dB. The recovered level carries the first velocity's rounding,
      // which is 48/126 dB and reads as +-2 velocity steps at the steeper floor.
      const float recovered_db =
          kDeepFloorDb +
          (static_cast<float>(measured_notes[i].velocity) - 1.0f) / 126.0f * -kDeepFloorDb;
      const int predicted = velocity_for_peak_rms(linear_of(recovered_db), kShallowFloorDb);
      INFO("recovered " << recovered_db << " dBFS, predicting " << predicted);
      REQUIRE(std::abs(static_cast<int>(notes[i].velocity) - predicted) <= 3);
    }
  }
}

// --- Rejections -----------------------------------------------------------

TEST_CASE("transcribe_notes refuses audio and config it cannot read", "[note_transcriber]") {
  const sonare::Audio audio = sustained(69);

  auto refused = [&](const TranscribeConfig& config) {
    REQUIRE_THROWS_AS(transcribe_notes(audio, config), sonare::SonareException);
    try {
      transcribe_notes(audio, config);
    } catch (const sonare::SonareException& error) {
      REQUIRE(error.code() == sonare::ErrorCode::InvalidParameter);
    }
  };

  SECTION("empty audio") {
    const sonare::Audio empty;
    REQUIRE(empty.empty());
    REQUIRE_THROWS_AS(transcribe_notes(empty), sonare::SonareException);
    try {
      transcribe_notes(empty);
    } catch (const sonare::SonareException& error) {
      REQUIRE(error.code() == sonare::ErrorCode::InvalidParameter);
    }
  }

  SECTION("a range with nothing in it") {
    TranscribeConfig equal;
    equal.fmin = 440.0f;
    equal.fmax = 440.0f;
    refused(equal);

    TranscribeConfig inverted;
    inverted.fmin = 2093.0f;
    inverted.fmax = 65.0f;
    refused(inverted);
  }

  SECTION("a velocity floor that is not finite and negative") {
    for (const float floor_db : {0.0f, 6.0f, kNaN, kInf, -kInf}) {
      INFO("velocity_floor_db " << floor_db);
      TranscribeConfig config;
      config.velocity_floor_db = floor_db;
      refused(config);
    }
  }

  SECTION("a fixed velocity outside {0} U [1, 127]") {
    for (const int fixed : {-1, 128, 255}) {
      INFO("fixed_velocity " << fixed);
      TranscribeConfig config;
      config.fixed_velocity = fixed;
      refused(config);
    }
  }

  SECTION("a non-finite config field") {
    for (const float bad : {kNaN, kInf, -kInf}) {
      INFO("value " << bad);
      TranscribeConfig reference;
      reference.reference_hz = bad;
      refused(reference);

      TranscribeConfig low;
      low.fmin = bad;
      refused(low);

      TranscribeConfig high;
      high.fmax = bad;
      refused(high);

      TranscribeConfig shortest;
      shortest.min_note_ms = bad;
      refused(shortest);

      TranscribeConfig threshold;
      threshold.segmentation_threshold_cents = bad;
      refused(threshold);
    }
  }

  SECTION("a non-positive reference, negative range or threshold") {
    TranscribeConfig reference;
    reference.reference_hz = 0.0f;
    refused(reference);

    TranscribeConfig low;
    low.fmin = -1.0f;
    refused(low);

    TranscribeConfig high;
    high.fmax = -1.0f;
    refused(high);

    TranscribeConfig shortest;
    shortest.min_note_ms = -1.0f;
    refused(shortest);

    TranscribeConfig threshold;
    threshold.segmentation_threshold_cents = 0.0f;
    refused(threshold);
  }

  SECTION("a source that is neither chain") {
    TranscribeConfig config;
    config.source = static_cast<TranscribeSource>(7);
    refused(config);
  }

  // The defaults are accepted, so every refusal above is attributable to the one
  // field it changed.
  REQUIRE_NOTHROW(transcribe_notes(audio));
}

// --- The polyphonic limits ------------------------------------------------

namespace {

constexpr int kPolyRate = 44100;
constexpr int kPolyPartials = 10;

/// @brief Adds a harmonic tone at @p f0_hz to @p into from @p start for @p length samples,
///        scaled per sample by @p gain.
template <typename Gain>
void add_harmonic(std::vector<float>& into, double f0_hz, size_t start, size_t length, Gain gain) {
  const double nyquist = 0.5 * kPolyRate;
  for (size_t i = 0; i < length && start + i < into.size(); ++i) {
    const double t = static_cast<double>(i) / kPolyRate;
    double sample = 0.0;
    for (int h = 1; h <= kPolyPartials; ++h) {
      const double hz = f0_hz * h;
      if (hz >= nyquist) break;
      sample += 0.25 / h * std::sin(sonare::constants::kTwoPiD * hz * t + 0.37 * h * h);
    }
    into[start + i] += static_cast<float>(sample * gain(t));
  }
}

/// @brief One pitch struck at 0 s and again at 3x the level at 0.45 s while the first
///        still sounds. The second strike rises over 20 ms: a hard step breaks the ridge
///        on its own, which would leave reattack_ratio nothing to decide.
sonare::Audio restruck_tone() {
  constexpr double kRestrikeSec = 0.45;
  constexpr double kDecayPerSec = 1.5;
  constexpr double kAttackSec = 0.02;
  std::vector<float> samples(static_cast<size_t>(0.9 * kPolyRate), 0.0f);
  add_harmonic(samples, hz_for_midi(64), 0, samples.size(), [&](double t) {
    double gain = std::exp(-kDecayPerSec * t);
    if (t >= kRestrikeSec) {
      const double since = t - kRestrikeSec;
      gain += 3.0 * std::min(1.0, since / kAttackSec) * std::exp(-kDecayPerSec * since);
    }
    return gain;
  });
  return sonare::Audio::from_buffer(samples.data(), samples.size(), kPolyRate);
}

/// @brief A held tone and, apart from it, one 100 ms tone with short raised edges.
sonare::Audio held_and_short() {
  constexpr double kEdgeSec = 0.005;
  constexpr double kShortSec = 0.1;
  std::vector<float> samples(static_cast<size_t>(1.0 * kPolyRate), 0.0f);
  add_harmonic(samples, hz_for_midi(64), 0, samples.size(), [](double) { return 1.0; });
  const auto edged = [](double t) {
    const double edge = std::min(1.0, std::min(t, kShortSec - t) / kEdgeSec);
    return std::max(0.0, edge);
  };
  add_harmonic(samples, hz_for_midi(71), static_cast<size_t>(0.4 * kPolyRate),
               static_cast<size_t>(kShortSec * kPolyRate), edged);
  return sonare::Audio::from_buffer(samples.data(), samples.size(), kPolyRate);
}

size_t count_of(const std::vector<TranscribedNote>& notes, int midi_note) {
  return static_cast<size_t>(std::count_if(
      notes.begin(), notes.end(), [&](const TranscribedNote& n) { return n.note == midi_note; }));
}

}  // namespace

TEST_CASE("the polyphonic limits resolve per source", "[note_transcriber]") {
  TranscribeConfig monophonic;
  const TranscribePolyphonyLimits mono = resolve_transcribe_polyphony_limits(monophonic);
  CHECK(mono.min_note_ms == 30.0f);

  TranscribeConfig polyphonic;
  polyphonic.source = TranscribeSource::kPolyphonic;
  const TranscribePolyphonyLimits poly = resolve_transcribe_polyphony_limits(polyphonic);
  CHECK(poly.min_note_ms == 100.0f);
  CHECK(poly.max_polyphony == 10);
  CHECK(poly.min_frame_peak_ratio == 0.20f);
  CHECK(poly.min_ridge_peak_ratio == 0.10f);
  CHECK(poly.reattack_ratio == 2.0f);

  SECTION("a negative ratio is a real 0 and a negative reattack is off") {
    polyphonic.min_frame_peak_ratio = -1.0f;
    polyphonic.min_ridge_peak_ratio = -0.5f;
    polyphonic.reattack_ratio = -1.0f;
    const TranscribePolyphonyLimits floored = resolve_transcribe_polyphony_limits(polyphonic);
    CHECK(floored.min_frame_peak_ratio == 0.0f);
    CHECK(floored.min_ridge_peak_ratio == 0.0f);
    CHECK(floored.reattack_ratio == 0.0f);
  }

  SECTION("a given value is copied") {
    polyphonic.min_note_ms = 45.0f;
    polyphonic.max_polyphony = 3;
    polyphonic.min_frame_peak_ratio = 0.3f;
    polyphonic.min_ridge_peak_ratio = 0.4f;
    polyphonic.reattack_ratio = 1.5f;
    const TranscribePolyphonyLimits given = resolve_transcribe_polyphony_limits(polyphonic);
    CHECK(given.min_note_ms == 45.0f);
    CHECK(given.max_polyphony == 3);
    CHECK(given.min_frame_peak_ratio == 0.3f);
    CHECK(given.min_ridge_peak_ratio == 0.4f);
    CHECK(given.reattack_ratio == 1.5f);
  }
}

TEST_CASE("min_note_ms is the shortest polyphonic note kept", "[note_transcriber]") {
  const sonare::Audio audio = held_and_short();
  constexpr int kShortNote = 71;

  TranscribeConfig config;
  config.source = TranscribeSource::kPolyphonic;
  config.min_note_ms = 40.0f;
  const std::vector<TranscribedNote> kept = transcribe_notes(audio, config);
  INFO("kept " << kept.size() << " notes");
  REQUIRE(count_of(kept, 64) >= 1);
  REQUIRE(count_of(kept, kShortNote) == 1);

  config.min_note_ms = 300.0f;
  const std::vector<TranscribedNote> dropped = transcribe_notes(audio, config);
  CHECK(count_of(dropped, 64) >= 1);
  CHECK(count_of(dropped, kShortNote) == 0);
}

TEST_CASE("a re-struck pitch is two notes by default and one with the split off",
          "[note_transcriber]") {
  const sonare::Audio audio = restruck_tone();
  TranscribeConfig config;
  config.source = TranscribeSource::kPolyphonic;
  const std::vector<TranscribedNote> split = transcribe_notes(audio, config);
  INFO("default notes " << split.size());
  CHECK(count_of(split, 64) == 2);

  config.reattack_ratio = -1.0f;
  const std::vector<TranscribedNote> whole = transcribe_notes(audio, config);
  INFO("split-off notes " << whole.size());
  CHECK(count_of(whole, 64) == 1);
}

TEST_CASE("the polyphonic limits refuse what they cannot read", "[note_transcriber]") {
  const sonare::Audio audio = sustained(69);

  auto refused_naming = [&](const TranscribeConfig& config, const std::string& field) {
    try {
      (void)transcribe_notes(audio, config);
      FAIL("accepted a config naming " << field);
    } catch (const sonare::SonareException& error) {
      CHECK(error.code() == sonare::ErrorCode::InvalidParameter);
      CHECK(std::string(error.what()).find(field) != std::string::npos);
    }
  };

  SECTION("a monophonic config carrying any of them, negative included") {
    for (const float value : {-1.0f, 0.3f}) {
      INFO("value " << value);
      TranscribeConfig frame;
      frame.min_frame_peak_ratio = value;
      refused_naming(frame, "min_frame_peak_ratio");
      TranscribeConfig ridge;
      ridge.min_ridge_peak_ratio = value;
      refused_naming(ridge, "min_ridge_peak_ratio");
    }
    for (const float value : {-1.0f, 2.5f}) {
      TranscribeConfig reattack;
      reattack.reattack_ratio = value;
      refused_naming(reattack, "reattack_ratio");
    }
    TranscribeConfig voices;
    voices.max_polyphony = 4;
    refused_naming(voices, "max_polyphony");
  }

  SECTION("a polyphonic value outside its domain") {
    TranscribeConfig base;
    base.source = TranscribeSource::kPolyphonic;
    for (const int voices : {-1, 65}) {
      TranscribeConfig config = base;
      config.max_polyphony = voices;
      refused_naming(config, "max_polyphony");
    }
    for (const float ratio : {1.5f, kNaN, kInf, -kInf}) {
      TranscribeConfig frame = base;
      frame.min_frame_peak_ratio = ratio;
      refused_naming(frame, "min_frame_peak_ratio");
      TranscribeConfig ridge = base;
      ridge.min_ridge_peak_ratio = ratio;
      refused_naming(ridge, "min_ridge_peak_ratio");
    }
    for (const float ratio : {0.5f, 1.0f, kNaN, kInf, -kInf}) {
      TranscribeConfig reattack = base;
      reattack.reattack_ratio = ratio;
      refused_naming(reattack, "reattack_ratio");
    }
  }
}
