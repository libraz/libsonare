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
#include <vector>

#include "core/audio.h"
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

  SECTION("a non-positive reference, range or threshold") {
    TranscribeConfig reference;
    reference.reference_hz = 0.0f;
    refused(reference);

    TranscribeConfig low;
    low.fmin = 0.0f;
    refused(low);

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
