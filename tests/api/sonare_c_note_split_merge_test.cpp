/// @file sonare_c_note_split_merge_test.cpp
/// @brief Tests for the note-editing C API that reshapes a note set:
///        sonare_decompose_note_pitch, sonare_split_note and sonare_merge_notes.
///
/// Tracks are hand-built at 16 kHz with 10 ms frames rather than measured with
/// pYIN, so every expected span, offset and curve value is predictable. The
/// source tone changes amplitude every frame, which is what makes a misaligned
/// amplitude slice visible: on a flat tone every frame carries the same RMS and
/// a slice pointing at a neighbour reads correct.

#include <sonare/sonare_c.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "util/constants.h"

#ifdef SONARE_WITH_PITCH_EDITOR

namespace {

constexpr int kSampleRate = 16000;
constexpr float kFrameRate = 100.0f;  // 10 ms frames
constexpr size_t kSamplesPerFrame = 160;
constexpr size_t kFrames = 40;
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

SonareNoteObject* poisoned_notes() {
  return reinterpret_cast<SonareNoteObject*>(static_cast<std::uintptr_t>(0x1));
}

float* poisoned_floats() { return reinterpret_cast<float*>(static_cast<std::uintptr_t>(0x1)); }

// --- The source every split and merge case starts from ---------------------

/// @brief A 400 Hz tone whose amplitude steps up once per frame.
/// @details 400 Hz at 16 kHz is 40 samples a period and a frame is four of
///          them, so each frame's RMS is exactly its own amplitude over root
///          two -- distinct for every frame, which is what a slice comparison
///          needs to be able to fail.
std::vector<float> stepped_tone(size_t frames) {
  std::vector<float> out(frames * kSamplesPerFrame, 0.0f);
  for (size_t frame = 0; frame < frames; ++frame) {
    const float amplitude = 0.1f + 0.02f * static_cast<float>(frame);
    for (size_t k = 0; k < kSamplesPerFrame; ++k) {
      const size_t i = frame * kSamplesPerFrame + k;
      out[i] = amplitude * static_cast<float>(std::sin(sonare::constants::kTwoPiD * 400.0 *
                                                       static_cast<double>(i) / kSampleRate));
    }
  }
  return out;
}

struct Source {
  std::vector<float> samples;
  std::vector<float> f0;
  std::vector<int32_t> voiced;
};

/// @brief One voiced run over the whole track.
Source plain_source() {
  Source source;
  source.samples = stepped_tone(kFrames);
  source.f0.assign(kFrames, 400.0f);
  source.voiced.assign(kFrames, 1);
  return source;
}

/// @brief Two unvoiced gaps, so the segmenter emits three notes of 10, 13 and
///        13 frames with two frames between each pair.
Source gapped_source() {
  Source source = plain_source();
  for (size_t i = 10; i < 12; ++i) {
    source.f0[i] = 0.0f;
    source.voiced[i] = 0;
  }
  for (size_t i = 25; i < 27; ++i) {
    source.f0[i] = 0.0f;
    source.voiced[i] = 0;
  }
  return source;
}

SonareError extract_from(const Source& source, SonareNoteObjectsResult* out) {
  return sonare_extract_notes(source.samples.data(), source.samples.size(), kSampleRate,
                              source.f0.data(), nullptr, source.voiced.data(), source.f0.size(),
                              kFrameRate, nullptr, out);
}

SonareError split_from(const Source& source, const SonareNoteObject* notes, size_t note_count,
                       const float* envelopes, size_t envelope_count, size_t index, int32_t frame,
                       SonareNoteObjectsResult* out) {
  return sonare_split_note(source.samples.data(), source.samples.size(), kSampleRate,
                           source.f0.data(), nullptr, source.voiced.data(), source.f0.size(),
                           kFrameRate, nullptr, notes, note_count, envelopes, envelope_count, index,
                           frame, out);
}

SonareError merge_from(const Source& source, const SonareNoteObject* notes, size_t note_count,
                       const float* envelopes, size_t envelope_count, size_t first, size_t last,
                       SonareNoteObjectsResult* out) {
  return sonare_merge_notes(source.samples.data(), source.samples.size(), kSampleRate,
                            source.f0.data(), nullptr, source.voiced.data(), source.f0.size(),
                            kFrameRate, nullptr, notes, note_count, envelopes, envelope_count,
                            first, last, out);
}

// --- Reading a result ------------------------------------------------------

/// @brief Every note's amplitude slice is its own frame span, and the slices
///        pack the pool in note order with neither gap nor overlap.
void require_packed_amplitude(const SonareNoteObjectsResult& result) {
  int64_t running = 0;
  for (size_t i = 0; i < result.count; ++i) {
    const SonareNoteObject& note = result.notes[i];
    INFO("note " << i);
    REQUIRE(note.frame_end > note.frame_start);
    REQUIRE(note.amplitude_offset == running);
    running += static_cast<int64_t>(note.frame_end - note.frame_start);
  }
  REQUIRE(result.amplitude_count == static_cast<size_t>(running));
}

/// @brief The amplitude values note @p index addresses through its own offset.
std::vector<float> amplitude_slice(const SonareNoteObjectsResult& result, size_t index) {
  REQUIRE(index < result.count);
  const SonareNoteObject& note = result.notes[index];
  REQUIRE(note.amplitude_offset >= 0);
  REQUIRE(note.frame_end > note.frame_start);
  const size_t offset = static_cast<size_t>(note.amplitude_offset);
  const size_t span = static_cast<size_t>(note.frame_end - note.frame_start);
  REQUIRE(offset + span <= result.amplitude_count);
  REQUIRE(result.amplitude != nullptr);
  return std::vector<float>(result.amplitude + offset, result.amplitude + offset + span);
}

/// @brief The envelope points note @p index addresses through its own offset.
std::vector<float> envelope_slice(const SonareNoteObjectsResult& result, size_t index) {
  REQUIRE(index < result.count);
  const SonareNoteEdit& edit = result.notes[index].edit;
  REQUIRE(edit.envelope_offset >= 0);
  REQUIRE(edit.envelope_count > 0);
  const size_t offset = static_cast<size_t>(edit.envelope_offset);
  REQUIRE(offset + edit.envelope_count <= result.envelope_count);
  REQUIRE(result.envelopes != nullptr);
  return std::vector<float>(result.envelopes + offset,
                            result.envelopes + offset + edit.envelope_count);
}

/// @brief Bit-exact equality. Split and merge re-derive every note, the
///        pass-through ones included, from the same audio and the same track, so
///        a tolerance here would hide a curve that came out of a different
///        computation.
void require_same_values(const std::vector<float>& actual, const std::vector<float>& expected) {
  REQUIRE(actual.size() == expected.size());
  for (size_t i = 0; i < expected.size(); ++i) {
    INFO("entry " << i);
    REQUIRE(actual[i] == expected[i]);
  }
}

/// @brief No two entries agree, so comparing a slice against the wrong one
///        cannot pass. The companion every value comparison below needs.
void require_distinct(const std::vector<float>& values) {
  std::vector<float> sorted = values;
  std::sort(sorted.begin(), sorted.end());
  REQUIRE(sorted.size() > 1);
  for (size_t i = 1; i < sorted.size(); ++i) {
    REQUIRE(sorted[i] - sorted[i - 1] > 1.0e-4f);
  }
}

void require_same_measurements(const SonareNoteObject& actual, const SonareNoteObject& expected) {
  // Re-derived from the same audio and the same track, so these come back equal
  // rather than close.
  REQUIRE(actual.onset_sample == expected.onset_sample);
  REQUIRE(actual.offset_sample == expected.offset_sample);
  REQUIRE(actual.frame_start == expected.frame_start);
  REQUIRE(actual.frame_end == expected.frame_end);
  REQUIRE(actual.median_hz == expected.median_hz);
  REQUIRE(actual.median_cents == expected.median_cents);
  REQUIRE(actual.f0_stability == expected.f0_stability);
}

// --- Building a pitch curve whose decomposition is known -------------------

struct CurveComponent {
  double hz;
  double cents;
};

/// @brief F0 values whose cents against @p centre_hz are exactly the sum of
///        @p components.
std::vector<float> injected_f0(float centre_hz, size_t frames,
                               const std::vector<CurveComponent>& components) {
  std::vector<float> values(frames, 0.0f);
  for (size_t i = 0; i < frames; ++i) {
    const double t = static_cast<double>(i) / kFrameRate;
    double cents = 0.0;
    for (const CurveComponent& component : components) {
      cents += component.cents * std::sin(sonare::constants::kTwoPiD * component.hz * t);
    }
    values[i] = static_cast<float>(static_cast<double>(centre_hz) *
                                   std::pow(2.0, cents / sonare::constants::kCentsPerOctave));
  }
  return values;
}

float cents_above(float hz, float centre_hz) {
  return static_cast<float>(sonare::constants::kCentsPerOctave *
                            std::log2(static_cast<double>(hz) / static_cast<double>(centre_hz)));
}

}  // namespace

// --- sonare_decompose_note_pitch -------------------------------------------

TEST_CASE("sonare_decompose_note_pitch splits a curve into two parts that add back up to it",
          "[c_api][note_split_merge]") {
  constexpr size_t kCurveFrames = 400;
  constexpr float kCentre = 196.0f;
  const std::vector<CurveComponent> components = {{0.5, 60.0}, {5.5, 40.0}};
  const std::vector<float> f0 = injected_f0(kCentre, kCurveFrames, components);

  // Poisoned on the way in: the result is cleared before anything is written.
  SonarePitchDecompositionResult out{};
  out.centre_hz = 99.0f;
  out.drift_cents = poisoned_floats();
  out.vibrato_cents = poisoned_floats();
  out.count = 7;

  REQUIRE(sonare_decompose_note_pitch(f0.data(), f0.size(), kFrameRate, kCentre, 3.0f, &out) ==
          SONARE_OK);
  REQUIRE(out.count == kCurveFrames);
  REQUIRE(out.centre_hz == kCentre);
  REQUIRE(out.drift_cents != nullptr);
  REQUIRE(out.vibrato_cents != nullptr);

  float worst = 0.0f;
  float drift_peak = 0.0f;
  float vibrato_peak = 0.0f;
  for (size_t i = 0; i < kCurveFrames; ++i) {
    const float cents = cents_above(f0[i], kCentre);
    worst = std::max(worst, std::abs(out.drift_cents[i] + out.vibrato_cents[i] - cents));
    drift_peak = std::max(drift_peak, std::abs(out.drift_cents[i]));
    vibrato_peak = std::max(vibrato_peak, std::abs(out.vibrato_cents[i]));
  }
  REQUIRE(worst < 1.0e-3f);

  // Two zero curves satisfy the sum as well, so both have to carry something.
  // 0.5 Hz and 5.5 Hz sit either side of the 3 Hz cut, so each does.
  REQUIRE(drift_peak > 10.0f);
  REQUIRE(vibrato_peak > 10.0f);
  sonare_free_pitch_decomposition(&out);
}

TEST_CASE("the sonare_decompose_note_pitch cutoff takes its default at 0",
          "[c_api][note_split_merge]") {
  constexpr size_t kCurveFrames = 400;
  constexpr float kCentre = 220.0f;
  const std::vector<CurveComponent> components = {{0.5, 60.0}, {5.5, 40.0}};
  const std::vector<float> f0 = injected_f0(kCentre, kCurveFrames, components);

  auto decomposed_at = [&](float cutoff_hz) {
    SonarePitchDecompositionResult out{};
    REQUIRE(sonare_decompose_note_pitch(f0.data(), f0.size(), kFrameRate, kCentre, cutoff_hz,
                                        &out) == SONARE_OK);
    REQUIRE(out.count == kCurveFrames);
    std::vector<float> both(out.drift_cents, out.drift_cents + out.count);
    both.insert(both.end(), out.vibrato_cents, out.vibrato_cents + out.count);
    sonare_free_pitch_decomposition(&out);
    return both;
  };

  const std::vector<float> zeroed = decomposed_at(0.0f);
  const std::vector<float> explicit_default = decomposed_at(3.0f);
  REQUIRE(zeroed == explicit_default);

  // Non-vacuity: the cutoff does decide the split, so the equality above is the
  // default being applied rather than an argument nobody reads.
  REQUIRE(decomposed_at(8.0f) != explicit_default);
}

TEST_CASE("sonare_decompose_note_pitch reports a note with no usable pitch as an empty result",
          "[c_api][note_split_merge]") {
  constexpr size_t kCurveFrames = 200;
  constexpr float kCentre = 220.0f;
  const std::vector<CurveComponent> components = {{5.0, 30.0}};
  const std::vector<float> f0 = injected_f0(kCentre, kCurveFrames, components);

  // A measurement that came up empty is not a bad argument, so it is reported
  // rather than rejected.
  auto require_empty = [](const std::vector<float>& curve, float median_hz) {
    SonarePitchDecompositionResult out{};
    out.centre_hz = 99.0f;
    out.drift_cents = poisoned_floats();
    out.vibrato_cents = poisoned_floats();
    out.count = 7;
    REQUIRE(sonare_decompose_note_pitch(curve.data(), curve.size(), kFrameRate, median_hz, 3.0f,
                                        &out) == SONARE_OK);
    REQUIRE(out.centre_hz == 0.0f);
    REQUIRE(out.drift_cents == nullptr);
    REQUIRE(out.vibrato_cents == nullptr);
    REQUIRE(out.count == 0);
  };

  // Not one usable frame to hold anything from.
  require_empty(std::vector<float>(kCurveFrames, 0.0f), kCentre);
  // A curve, but no centre to measure it against.
  require_empty(f0, 0.0f);

  // The same curve with a centre is not an empty measurement, so neither of the
  // two above passes by emptying every call.
  SonarePitchDecompositionResult usable{};
  REQUIRE(sonare_decompose_note_pitch(f0.data(), f0.size(), kFrameRate, kCentre, 3.0f, &usable) ==
          SONARE_OK);
  REQUIRE(usable.count == kCurveFrames);
  REQUIRE(usable.centre_hz == kCentre);
  sonare_free_pitch_decomposition(&usable);
}

TEST_CASE("sonare_decompose_note_pitch rejects malformed arguments and clears its output",
          "[c_api][note_split_merge]") {
  constexpr size_t kCurveFrames = 200;
  constexpr float kCentre = 220.0f;
  const std::vector<CurveComponent> components = {{5.0, 30.0}};
  const std::vector<float> f0 = injected_f0(kCentre, kCurveFrames, components);

  auto rejects = [](auto&& call) {
    SonarePitchDecompositionResult out{};
    out.centre_hz = 99.0f;
    out.drift_cents = poisoned_floats();
    out.vibrato_cents = poisoned_floats();
    out.count = 7;
    REQUIRE(call(&out) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out.centre_hz == 0.0f);
    REQUIRE(out.drift_cents == nullptr);
    REQUIRE(out.vibrato_cents == nullptr);
    REQUIRE(out.count == 0);
  };

  REQUIRE(sonare_decompose_note_pitch(f0.data(), f0.size(), kFrameRate, kCentre, 3.0f, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);

  rejects([&](SonarePitchDecompositionResult* out) {
    return sonare_decompose_note_pitch(nullptr, kCurveFrames, kFrameRate, kCentre, 3.0f, out);
  });
  rejects([&](SonarePitchDecompositionResult* out) {
    return sonare_decompose_note_pitch(f0.data(), 0, kFrameRate, kCentre, 3.0f, out);
  });

  // A frame carrying no pitch is spelled zero, negative or non-finite, and all
  // four spellings are read the same way rather than refused. The frame rate and
  // the centre below are still refused, so this is not a blanket acceptance.
  for (const float no_pitch : {kNaN, kInf, -kInf, -1.0f}) {
    CAPTURE(no_pitch);
    std::vector<float> track = f0;
    track[7] = no_pitch;
    SonarePitchDecompositionResult out{};
    REQUIRE(sonare_decompose_note_pitch(track.data(), track.size(), kFrameRate, kCentre, 3.0f,
                                        &out) == SONARE_OK);
    sonare_free_pitch_decomposition(&out);
  }

  for (const float bad_rate : {0.0f, -100.0f, kNaN, kInf}) {
    rejects([&](SonarePitchDecompositionResult* out) {
      return sonare_decompose_note_pitch(f0.data(), f0.size(), bad_rate, kCentre, 3.0f, out);
    });
  }

  // 0 is the no-pitch spelling, so only a value that cannot be a centre at all
  // is rejected.
  for (const float bad_median : {-1.0f, kNaN, kInf, -kInf}) {
    rejects([&](SonarePitchDecompositionResult* out) {
      return sonare_decompose_note_pitch(f0.data(), f0.size(), kFrameRate, bad_median, 3.0f, out);
    });
  }

  // 0 is the default spelling, so only a value that cannot be a cutoff is.
  for (const float bad_cutoff : {-1.0f, -3.0f, kNaN, kInf, -kInf}) {
    rejects([&](SonarePitchDecompositionResult* out) {
      return sonare_decompose_note_pitch(f0.data(), f0.size(), kFrameRate, kCentre, bad_cutoff,
                                         out);
    });
  }

  // Positive control: the same call with nothing poisoned succeeds.
  SonarePitchDecompositionResult ok{};
  REQUIRE(sonare_decompose_note_pitch(f0.data(), f0.size(), kFrameRate, kCentre, 3.0f, &ok) ==
          SONARE_OK);
  REQUIRE(ok.count == kCurveFrames);
  sonare_free_pitch_decomposition(&ok);
}

TEST_CASE("sonare_free_pitch_decomposition clears the result it releases",
          "[c_api][note_split_merge]") {
  sonare_free_pitch_decomposition(nullptr);

  // A zeroed result owns nothing, so releasing it is a no-op rather than a free
  // of an uninitialized pointer.
  SonarePitchDecompositionResult empty{};
  sonare_free_pitch_decomposition(&empty);
  REQUIRE(empty.drift_cents == nullptr);
  REQUIRE(empty.vibrato_cents == nullptr);
  REQUIRE(empty.count == 0);

  constexpr size_t kCurveFrames = 120;
  constexpr float kCentre = 220.0f;
  const std::vector<CurveComponent> components = {{5.0, 30.0}};
  const std::vector<float> f0 = injected_f0(kCentre, kCurveFrames, components);

  SonarePitchDecompositionResult out{};
  REQUIRE(sonare_decompose_note_pitch(f0.data(), f0.size(), kFrameRate, kCentre, 3.0f, &out) ==
          SONARE_OK);
  REQUIRE(out.drift_cents != nullptr);
  REQUIRE(out.vibrato_cents != nullptr);

  sonare_free_pitch_decomposition(&out);
  REQUIRE(out.centre_hz == 0.0f);
  REQUIRE(out.drift_cents == nullptr);
  REQUIRE(out.vibrato_cents == nullptr);
  REQUIRE(out.count == 0);

  // Releasing the same result twice is safe: the second call has nothing left.
  sonare_free_pitch_decomposition(&out);
  REQUIRE(out.drift_cents == nullptr);
  REQUIRE(out.vibrato_cents == nullptr);
  REQUIRE(out.count == 0);
}

// --- sonare_split_note ------------------------------------------------------

TEST_CASE("sonare_split_note keeps every note's amplitude slice pointing at its own frames",
          "[c_api][note_split_merge]") {
  const Source source = gapped_source();

  SonareNoteObjectsResult extracted{};
  REQUIRE(extract_from(source, &extracted) == SONARE_OK);
  REQUIRE(extracted.count == 3);
  REQUIRE(extracted.notes[0].frame_start == 0);
  REQUIRE(extracted.notes[0].frame_end == 10);
  REQUIRE(extracted.notes[1].frame_start == 12);
  REQUIRE(extracted.notes[1].frame_end == 25);
  REQUIRE(extracted.notes[2].frame_start == 27);
  REQUIRE(extracted.notes[2].frame_end == 40);
  require_packed_amplitude(extracted);
  REQUIRE(extracted.amplitude_count == 36);
  require_distinct(
      std::vector<float>(extracted.amplitude, extracted.amplitude + extracted.amplitude_count));

  const std::vector<float> first_before = amplitude_slice(extracted, 0);
  const std::vector<float> middle_before = amplitude_slice(extracted, 1);
  const std::vector<float> last_before = amplitude_slice(extracted, 2);

  SonareNoteObjectsResult split{};
  REQUIRE(split_from(source, extracted.notes, extracted.count, nullptr, 0, 1, 18, &split) ==
          SONARE_OK);
  REQUIRE(split.count == 4);

  // The cut lands where it was asked for and the spans stay contiguous.
  REQUIRE(split.notes[1].frame_start == 12);
  REQUIRE(split.notes[1].frame_end == 18);
  REQUIRE(split.notes[2].frame_start == 18);
  REQUIRE(split.notes[2].frame_end == 25);
  REQUIRE(split.notes[1].offset_sample == split.notes[2].onset_sample);
  REQUIRE(split.notes[1].onset_sample == extracted.notes[1].onset_sample);
  REQUIRE(split.notes[2].offset_sample == extracted.notes[1].offset_sample);

  // Every slice, the untouched notes included, is its own span at the running
  // offset -- and holds its own values. A wrapper that cannot supply a
  // pass-through note's curve produces offsets that look right and point at a
  // neighbour's data, which only the values catch.
  require_packed_amplitude(split);
  REQUIRE(split.amplitude_count == extracted.amplitude_count);
  require_same_values(amplitude_slice(split, 0), first_before);
  require_same_values(amplitude_slice(split, 3), last_before);

  std::vector<float> halves = amplitude_slice(split, 1);
  const std::vector<float> tail = amplitude_slice(split, 2);
  halves.insert(halves.end(), tail.begin(), tail.end());
  require_same_values(halves, middle_before);

  // The untouched notes are re-derived rather than copied, and come back equal.
  require_same_measurements(split.notes[0], extracted.notes[0]);
  require_same_measurements(split.notes[3], extracted.notes[2]);

  // No note carried an envelope in, so the result owns no pool.
  REQUIRE(split.envelopes == nullptr);
  REQUIRE(split.envelope_count == 0);

  sonare_free_note_objects(&split);
  sonare_free_note_objects(&extracted);
}

TEST_CASE("sonare_split_note normalizes a time_stretch_ratio of 0 to 1",
          "[c_api][note_split_merge]") {
  const Source source = plain_source();

  SonareNoteObjectsResult extracted{};
  REQUIRE(extract_from(source, &extracted) == SONARE_OK);
  REQUIRE(extracted.count == 1);

  // A zeroed edit spells the identity ratio as 0, and the split re-derives its
  // notes, so what comes back is the 1.0 spelling of the same edit.
  extracted.notes[0].edit = SonareNoteEdit{};
  REQUIRE(extracted.notes[0].edit.time_stretch_ratio == 0.0f);

  SonareNoteObjectsResult split{};
  REQUIRE(split_from(source, extracted.notes, extracted.count, nullptr, 0, 0, 20, &split) ==
          SONARE_OK);
  REQUIRE(split.count == 2);
  for (size_t i = 0; i < split.count; ++i) {
    INFO("half " << i);
    REQUIRE(split.notes[i].edit.time_stretch_ratio == 1.0f);
    REQUIRE(split.notes[i].edit.gain_db == 0.0f);
    REQUIRE(split.notes[i].edit.muted == 0);
    REQUIRE(split.notes[i].edit.envelope_count == 0);
  }
  sonare_free_note_objects(&split);
  sonare_free_note_objects(&extracted);
}

TEST_CASE("sonare_split_note cuts the source note's envelope and returns its own pool",
          "[c_api][note_split_merge]") {
  const Source source = plain_source();

  SonareNoteObjectsResult extracted{};
  REQUIRE(extract_from(source, &extracted) == SONARE_OK);
  REQUIRE(extracted.count == 1);
  REQUIRE(extracted.notes[0].frame_start == 0);
  REQUIRE(extracted.notes[0].frame_end == 40);

  SECTION("a two-point envelope is cut at the same proportion as the span") {
    // The note's points sit at offset 1; the 3.0 in front of them must not be
    // read, and cannot appear in the result's own pool either.
    const std::vector<float> pool = {3.0f, 0.25f, 1.0f};
    extracted.notes[0].edit.envelope_offset = 1;
    extracted.notes[0].edit.envelope_count = 2;

    SonareNoteObjectsResult split{};
    REQUIRE(split_from(source, extracted.notes, extracted.count, pool.data(), pool.size(), 0, 20,
                       &split) == SONARE_OK);
    REQUIRE(split.count == 2);
    REQUIRE(split.envelopes != nullptr);
    REQUIRE(split.envelope_count > 0);

    const std::vector<float> first = envelope_slice(split, 0);
    const std::vector<float> second = envelope_slice(split, 1);

    // The cut keeps the source's own points and inserts one at the cut, so both
    // halves are exact rather than approximate. Frame 20 of a [0, 40) note is
    // position 0.5, and a 0.25 -> 1.0 ramp read endpoint-anchored parts at
    // 0.25 + 0.75 * 0.5. Every value and every product here is exact in binary.
    REQUIRE(first.size() == 2);
    REQUIRE(second.size() == 2);
    REQUIRE(first[0] == 0.25f);
    REQUIRE(first[1] == 0.625f);
    REQUIRE(second[0] == 0.625f);
    REQUIRE(second[1] == 1.0f);

    // Nothing outside the source note's own two points reached the result.
    for (size_t i = 0; i < split.envelope_count; ++i) {
      INFO("point " << i);
      REQUIRE(split.envelopes[i] >= 0.25f);
      REQUIRE(split.envelopes[i] <= 1.0f);
    }
    sonare_free_note_objects(&split);
  }

  SECTION("a one-entry envelope is a constant, so both halves get it unchanged") {
    const std::vector<float> pool = {0.5f};
    extracted.notes[0].edit.envelope_offset = 0;
    extracted.notes[0].edit.envelope_count = 1;

    SonareNoteObjectsResult split{};
    REQUIRE(split_from(source, extracted.notes, extracted.count, pool.data(), pool.size(), 0, 20,
                       &split) == SONARE_OK);
    REQUIRE(split.count == 2);
    for (size_t i = 0; i < split.count; ++i) {
      INFO("half " << i);
      const std::vector<float> slice = envelope_slice(split, i);
      REQUIRE(slice.size() == 1);
      REQUIRE(slice[0] == 0.5f);
    }
    sonare_free_note_objects(&split);
  }

  sonare_free_note_objects(&extracted);
}

TEST_CASE("sonare_free_note_objects releases the envelope pool a split result owns",
          "[c_api][note_split_merge]") {
  const Source source = plain_source();

  SonareNoteObjectsResult extracted{};
  REQUIRE(extract_from(source, &extracted) == SONARE_OK);
  REQUIRE(extracted.count == 1);
  const std::vector<float> pool = {0.25f, 1.0f};
  extracted.notes[0].edit.envelope_count = 2;

  SonareNoteObjectsResult split{};
  REQUIRE(split_from(source, extracted.notes, extracted.count, pool.data(), pool.size(), 0, 20,
                     &split) == SONARE_OK);
  REQUIRE(split.notes != nullptr);
  REQUIRE(split.amplitude != nullptr);
  REQUIRE(split.envelopes != nullptr);

  sonare_free_note_objects(&split);
  REQUIRE(split.notes == nullptr);
  REQUIRE(split.count == 0);
  REQUIRE(split.amplitude == nullptr);
  REQUIRE(split.amplitude_count == 0);
  REQUIRE(split.envelopes == nullptr);
  REQUIRE(split.envelope_count == 0);

  // Releasing the same result twice is safe: the second call has nothing left.
  sonare_free_note_objects(&split);
  REQUIRE(split.envelopes == nullptr);
  REQUIRE(split.envelope_count == 0);

  sonare_free_note_objects(&extracted);
}

TEST_CASE("sonare_split_note rejects an out-of-range index and a frame outside the note",
          "[c_api][note_split_merge]") {
  const Source source = gapped_source();

  SonareNoteObjectsResult extracted{};
  REQUIRE(extract_from(source, &extracted) == SONARE_OK);
  REQUIRE(extracted.count == 3);
  const std::vector<SonareNoteObject> notes(extracted.notes, extracted.notes + extracted.count);
  sonare_free_note_objects(&extracted);

  auto rejects = [&](auto&& call) {
    SonareNoteObjectsResult out{};
    out.notes = poisoned_notes();
    out.count = 7;
    out.amplitude = poisoned_floats();
    out.amplitude_count = 7;
    out.envelopes = poisoned_floats();
    out.envelope_count = 7;
    REQUIRE(call(&out) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out.notes == nullptr);
    REQUIRE(out.count == 0);
    REQUIRE(out.amplitude == nullptr);
    REQUIRE(out.amplitude_count == 0);
    REQUIRE(out.envelopes == nullptr);
    REQUIRE(out.envelope_count == 0);
  };

  REQUIRE(split_from(source, notes.data(), notes.size(), nullptr, 0, 1, 18, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);

  for (const size_t index : {notes.size(), notes.size() + 4}) {
    rejects([&](SonareNoteObjectsResult* out) {
      return split_from(source, notes.data(), notes.size(), nullptr, 0, index, 18, out);
    });
  }
  rejects([&](SonareNoteObjectsResult* out) {
    return split_from(source, nullptr, notes.size(), nullptr, 0, 1, 18, out);
  });
  rejects([&](SonareNoteObjectsResult* out) {
    return split_from(source, notes.data(), 0, nullptr, 0, 0, 18, out);
  });

  // Strictly inside the note's own span, so neither of its boundaries is a legal
  // cut and neither is a frame belonging to another note.
  for (const int32_t frame : {12, 25, 5, 30, -1, 100}) {
    INFO("frame " << frame);
    rejects([&](SonareNoteObjectsResult* out) {
      return split_from(source, notes.data(), notes.size(), nullptr, 0, 1, frame, out);
    });
  }

  // A note the set cannot describe: an empty span, and one running past the
  // track the whole set is re-derived against.
  std::vector<SonareNoteObject> empty_span = notes;
  empty_span[1].frame_end = empty_span[1].frame_start;
  rejects([&](SonareNoteObjectsResult* out) {
    return split_from(source, empty_span.data(), empty_span.size(), nullptr, 0, 0, 5, out);
  });

  std::vector<SonareNoteObject> beyond = notes;
  beyond[2].frame_end = static_cast<int32_t>(kFrames) + 1;
  rejects([&](SonareNoteObjectsResult* out) {
    return split_from(source, beyond.data(), beyond.size(), nullptr, 0, 0, 5, out);
  });

  // The track arguments the extractor itself rejects.
  rejects([&](SonareNoteObjectsResult* out) {
    return sonare_split_note(nullptr, 0, kSampleRate, source.f0.data(), nullptr,
                             source.voiced.data(), source.f0.size(), kFrameRate, nullptr,
                             notes.data(), notes.size(), nullptr, 0, 1, 18, out);
  });
  rejects([&](SonareNoteObjectsResult* out) {
    return sonare_split_note(source.samples.data(), source.samples.size(), kSampleRate, nullptr,
                             nullptr, source.voiced.data(), source.f0.size(), kFrameRate, nullptr,
                             notes.data(), notes.size(), nullptr, 0, 1, 18, out);
  });
  rejects([&](SonareNoteObjectsResult* out) {
    return sonare_split_note(source.samples.data(), source.samples.size(), kSampleRate,
                             source.f0.data(), nullptr, source.voiced.data(), 0, kFrameRate,
                             nullptr, notes.data(), notes.size(), nullptr, 0, 1, 18, out);
  });
  rejects([&](SonareNoteObjectsResult* out) {
    return sonare_split_note(source.samples.data(), source.samples.size(), kSampleRate,
                             source.f0.data(), nullptr, nullptr, source.f0.size(), kFrameRate,
                             nullptr, notes.data(), notes.size(), nullptr, 0, 1, 18, out);
  });
  for (const float bad_rate : {0.0f, -100.0f, kNaN, kInf}) {
    rejects([&](SonareNoteObjectsResult* out) {
      return sonare_split_note(source.samples.data(), source.samples.size(), kSampleRate,
                               source.f0.data(), nullptr, source.voiced.data(), source.f0.size(),
                               bad_rate, nullptr, notes.data(), notes.size(), nullptr, 0, 1, 18,
                               out);
    });
  }

  // Positive controls: one frame in from either end of the note is legal, and
  // both halves keep a span.
  for (const int32_t frame : {13, 24}) {
    SonareNoteObjectsResult out{};
    INFO("frame " << frame);
    REQUIRE(split_from(source, notes.data(), notes.size(), nullptr, 0, 1, frame, &out) ==
            SONARE_OK);
    REQUIRE(out.count == 4);
    REQUIRE(out.notes[1].frame_end == frame);
    REQUIRE(out.notes[2].frame_start == frame);
    sonare_free_note_objects(&out);
  }
}

// --- sonare_merge_notes -----------------------------------------------------

TEST_CASE("sonare_merge_notes spans the gap it joins over and keeps every slice aligned",
          "[c_api][note_split_merge]") {
  const Source source = gapped_source();

  SonareNoteObjectsResult extracted{};
  REQUIRE(extract_from(source, &extracted) == SONARE_OK);
  REQUIRE(extracted.count == 3);
  const std::vector<float> first_before = amplitude_slice(extracted, 0);
  const std::vector<float> middle_before = amplitude_slice(extracted, 1);
  const std::vector<float> last_before = amplitude_slice(extracted, 2);

  SonareNoteObjectsResult merged{};
  REQUIRE(merge_from(source, extracted.notes, extracted.count, nullptr, 0, 0, 1, &merged) ==
          SONARE_OK);
  // last - first notes go away, so the count drops by exactly one here.
  REQUIRE(merged.count == 2);
  REQUIRE(merged.notes[0].frame_start == 0);
  REQUIRE(merged.notes[0].frame_end == 25);
  REQUIRE(merged.notes[0].onset_sample == extracted.notes[0].onset_sample);
  REQUIRE(merged.notes[0].offset_sample == extracted.notes[1].offset_sample);
  require_same_measurements(merged.notes[1], extracted.notes[2]);

  // The merged note's curve covers the gap the segmenter cut at, so the pool
  // grew by the two frames neither neighbour carried.
  require_packed_amplitude(merged);
  REQUIRE(merged.amplitude_count == 38);

  const std::vector<float> joined = amplitude_slice(merged, 0);
  REQUIRE(joined.size() == 25);
  require_same_values(std::vector<float>(joined.begin(), joined.begin() + 10), first_before);
  require_same_values(std::vector<float>(joined.begin() + 12, joined.end()), middle_before);
  // The gap's own amplitude lives in the audio, not in either neighbour.
  REQUIRE(joined[10] > 0.0f);
  REQUIRE(joined[11] > 0.0f);
  require_same_values(amplitude_slice(merged, 1), last_before);

  REQUIRE(merged.envelopes == nullptr);
  REQUIRE(merged.envelope_count == 0);

  sonare_free_note_objects(&merged);
  sonare_free_note_objects(&extracted);
}

TEST_CASE("sonare_merge_notes takes the first note's edit", "[c_api][note_split_merge]") {
  const Source source = gapped_source();

  SonareNoteObjectsResult extracted{};
  REQUIRE(extract_from(source, &extracted) == SONARE_OK);
  REQUIRE(extracted.count == 3);
  extracted.notes[0].edit.gain_db = -3.0f;
  extracted.notes[0].edit.pitch_shift_semitones = 2.0f;
  extracted.notes[1].edit.gain_db = 9.0f;
  extracted.notes[1].edit.muted = 1;

  SonareNoteObjectsResult merged{};
  REQUIRE(merge_from(source, extracted.notes, extracted.count, nullptr, 0, 0, 1, &merged) ==
          SONARE_OK);
  REQUIRE(merged.count == 2);
  REQUIRE(merged.notes[0].edit.gain_db == -3.0f);
  REQUIRE(merged.notes[0].edit.pitch_shift_semitones == 2.0f);
  // The second note's edit does not survive: the rule is the first note's edit,
  // not a merge of the two.
  REQUIRE(merged.notes[0].edit.muted == 0);
  sonare_free_note_objects(&merged);
  sonare_free_note_objects(&extracted);
}

TEST_CASE("a split undone by a merge returns the spans and the curves it started from",
          "[c_api][note_split_merge]") {
  const Source source = gapped_source();

  SonareNoteObjectsResult extracted{};
  REQUIRE(extract_from(source, &extracted) == SONARE_OK);
  REQUIRE(extracted.count == 3);
  const std::vector<float> before(extracted.amplitude,
                                  extracted.amplitude + extracted.amplitude_count);

  SonareNoteObjectsResult split{};
  REQUIRE(split_from(source, extracted.notes, extracted.count, nullptr, 0, 1, 18, &split) ==
          SONARE_OK);
  REQUIRE(split.count == 4);

  SonareNoteObjectsResult rejoined{};
  REQUIRE(merge_from(source, split.notes, split.count, split.envelopes, split.envelope_count, 1, 2,
                     &rejoined) == SONARE_OK);
  REQUIRE(rejoined.count == extracted.count);
  require_packed_amplitude(rejoined);
  REQUIRE(rejoined.amplitude_count == extracted.amplitude_count);

  for (size_t i = 0; i < rejoined.count; ++i) {
    INFO("note " << i);
    require_same_measurements(rejoined.notes[i], extracted.notes[i]);
  }
  require_same_values(
      std::vector<float>(rejoined.amplitude, rejoined.amplitude + rejoined.amplitude_count),
      before);

  sonare_free_note_objects(&rejoined);
  sonare_free_note_objects(&split);
  sonare_free_note_objects(&extracted);
}

TEST_CASE("sonare_merge_notes rejects a first/last pair that is not an ascending in-range run",
          "[c_api][note_split_merge]") {
  const Source source = gapped_source();

  SonareNoteObjectsResult extracted{};
  REQUIRE(extract_from(source, &extracted) == SONARE_OK);
  REQUIRE(extracted.count == 3);
  const std::vector<SonareNoteObject> notes(extracted.notes, extracted.notes + extracted.count);
  sonare_free_note_objects(&extracted);

  auto rejects = [&](auto&& call) {
    SonareNoteObjectsResult out{};
    out.notes = poisoned_notes();
    out.count = 7;
    out.amplitude = poisoned_floats();
    out.amplitude_count = 7;
    out.envelopes = poisoned_floats();
    out.envelope_count = 7;
    REQUIRE(call(&out) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out.notes == nullptr);
    REQUIRE(out.count == 0);
    REQUIRE(out.amplitude == nullptr);
    REQUIRE(out.amplitude_count == 0);
    REQUIRE(out.envelopes == nullptr);
    REQUIRE(out.envelope_count == 0);
  };

  REQUIRE(merge_from(source, notes.data(), notes.size(), nullptr, 0, 0, 1, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);

  // A run of one is not a merge, and a run cannot run backwards.
  rejects([&](SonareNoteObjectsResult* out) {
    return merge_from(source, notes.data(), notes.size(), nullptr, 0, 1, 1, out);
  });
  rejects([&](SonareNoteObjectsResult* out) {
    return merge_from(source, notes.data(), notes.size(), nullptr, 0, 2, 1, out);
  });

  // last indexes past the end, with and without first inside the list.
  rejects([&](SonareNoteObjectsResult* out) {
    return merge_from(source, notes.data(), notes.size(), nullptr, 0, 0, notes.size(), out);
  });
  rejects([&](SonareNoteObjectsResult* out) {
    return merge_from(source, notes.data(), notes.size(), nullptr, 0, notes.size(),
                      notes.size() + 1, out);
  });
  rejects([&](SonareNoteObjectsResult* out) {
    return merge_from(source, nullptr, notes.size(), nullptr, 0, 0, 1, out);
  });
  rejects([&](SonareNoteObjectsResult* out) {
    return merge_from(source, notes.data(), 0, nullptr, 0, 0, 1, out);
  });

  // A note the set cannot describe, the same two ways a split rejects.
  std::vector<SonareNoteObject> empty_span = notes;
  empty_span[2].frame_end = empty_span[2].frame_start;
  rejects([&](SonareNoteObjectsResult* out) {
    return merge_from(source, empty_span.data(), empty_span.size(), nullptr, 0, 0, 1, out);
  });

  std::vector<SonareNoteObject> beyond = notes;
  beyond[2].frame_end = static_cast<int32_t>(kFrames) + 1;
  rejects([&](SonareNoteObjectsResult* out) {
    return merge_from(source, beyond.data(), beyond.size(), nullptr, 0, 0, 1, out);
  });

  // The track arguments the extractor itself rejects.
  rejects([&](SonareNoteObjectsResult* out) {
    return sonare_merge_notes(nullptr, 0, kSampleRate, source.f0.data(), nullptr,
                              source.voiced.data(), source.f0.size(), kFrameRate, nullptr,
                              notes.data(), notes.size(), nullptr, 0, 0, 1, out);
  });
  rejects([&](SonareNoteObjectsResult* out) {
    return sonare_merge_notes(source.samples.data(), source.samples.size(), kSampleRate, nullptr,
                              nullptr, source.voiced.data(), source.f0.size(), kFrameRate, nullptr,
                              notes.data(), notes.size(), nullptr, 0, 0, 1, out);
  });
  rejects([&](SonareNoteObjectsResult* out) {
    return sonare_merge_notes(source.samples.data(), source.samples.size(), kSampleRate,
                              source.f0.data(), nullptr, source.voiced.data(), 0, kFrameRate,
                              nullptr, notes.data(), notes.size(), nullptr, 0, 0, 1, out);
  });
  rejects([&](SonareNoteObjectsResult* out) {
    return sonare_merge_notes(source.samples.data(), source.samples.size(), kSampleRate,
                              source.f0.data(), nullptr, nullptr, source.f0.size(), kFrameRate,
                              nullptr, notes.data(), notes.size(), nullptr, 0, 0, 1, out);
  });
  for (const float bad_rate : {0.0f, -100.0f, kNaN, kInf}) {
    rejects([&](SonareNoteObjectsResult* out) {
      return sonare_merge_notes(source.samples.data(), source.samples.size(), kSampleRate,
                                source.f0.data(), nullptr, source.voiced.data(), source.f0.size(),
                                bad_rate, nullptr, notes.data(), notes.size(), nullptr, 0, 0, 1,
                                out);
    });
  }

  // Positive control: merging the whole run collapses the list to one note over
  // the whole span, so none of the above passes by rejecting every merge.
  SonareNoteObjectsResult all{};
  REQUIRE(merge_from(source, notes.data(), notes.size(), nullptr, 0, 0, 2, &all) == SONARE_OK);
  REQUIRE(all.count == 1);
  REQUIRE(all.notes[0].frame_start == notes[0].frame_start);
  REQUIRE(all.notes[0].frame_end == notes[2].frame_end);
  sonare_free_note_objects(&all);
}

#else

TEST_CASE("the note reshaping C API reports NOT_SUPPORTED without the pitch editor",
          "[c_api][note_split_merge]") {
  const std::vector<float> samples(1600, 0.25f);
  const std::vector<float> f0(10, 440.0f);
  const std::vector<int32_t> voiced(10, 1);
  SonareNoteObject notes[1] = {};
  notes[0].offset_sample = 1600;
  notes[0].frame_end = 10;
  notes[0].median_hz = 440.0f;

  SonarePitchDecompositionResult decomposition{};
  REQUIRE(sonare_decompose_note_pitch(f0.data(), f0.size(), 100.0f, 440.0f, 3.0f, &decomposition) ==
          SONARE_ERROR_NOT_SUPPORTED);
  sonare_free_pitch_decomposition(&decomposition);

  SonareNoteObjectsResult out{};
  REQUIRE(sonare_split_note(samples.data(), samples.size(), 16000, f0.data(), nullptr,
                            voiced.data(), f0.size(), 100.0f, nullptr, notes, 1, nullptr, 0, 0, 5,
                            &out) == SONARE_ERROR_NOT_SUPPORTED);
  REQUIRE(sonare_merge_notes(samples.data(), samples.size(), 16000, f0.data(), nullptr,
                             voiced.data(), f0.size(), 100.0f, nullptr, notes, 1, nullptr, 0, 0, 1,
                             &out) == SONARE_ERROR_NOT_SUPPORTED);
}

#endif
