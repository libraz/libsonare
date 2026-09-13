/// @file sonare_c_polyphony_test.cpp
/// @brief Tests for the polyphonic editing C API: the analysis handle, what it
///        reports, the one field a host writes, and the render back to audio.
///
/// The C++ chain is the oracle for everything the wrapper only forwards. Comparing
/// against it is exact rather than tolerant: the wrapper copies, it does not compute,
/// so a render that merely agrees closely has been recomputed somewhere.
///
/// The fixture is a fifth whose partials collide -- E4's 3rd and B4's 2nd land in one
/// bin at this framing -- so the chain the handle wraps has a shared bin to divide
/// and is not exercising its trivial path.

#include <sonare/sonare_c.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <vector>

#include "util/constants.h"

#ifdef SONARE_WITH_PITCH_EDITOR

#include "core/audio.h"
#include "editing/polyphony/polyphonic_edit.h"

namespace {

constexpr int kSampleRate = 44100;
constexpr size_t kSourceSamples = 22050;
constexpr int kPartials = 10;
constexpr float kLowHz = 329.6276f;
constexpr float kHighHz = 493.8833f;

void add_tone(std::vector<float>& into, float f0_hz) {
  const double nyquist = 0.5 * static_cast<double>(kSampleRate);
  for (int h = 1; h <= kPartials; ++h) {
    const double hz = static_cast<double>(h) * static_cast<double>(f0_hz);
    if (hz >= nyquist) break;
    const double phase = 0.37 * static_cast<double>(h) * static_cast<double>(h);
    const float level = 0.25f / static_cast<float>(h);
    for (size_t i = 0; i < into.size(); ++i) {
      into[i] += level * static_cast<float>(
                             std::sin(sonare::constants::kTwoPiD * hz * static_cast<double>(i) /
                                          static_cast<double>(kSampleRate) +
                                      phase));
    }
  }
}

std::vector<float> chord() {
  std::vector<float> samples(kSourceSamples, 0.0f);
  add_tone(samples, kLowHz);
  add_tone(samples, kHighHz);
  return samples;
}

/// @brief Owns a handle for the duration of a scope, so an assertion that fails
///        mid-test still releases it.
class Handle {
 public:
  explicit Handle(const SonarePolyphonicConfig* config, const std::vector<float>& samples) {
    error_ = sonare_polyphonic_analyze(samples.data(), samples.size(), kSampleRate, config, &ptr_);
  }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  ~Handle() { sonare_polyphonic_analysis_destroy(ptr_); }

  SonareError error() const { return error_; }
  SonarePolyphonicAnalysis* get() const { return ptr_; }

 private:
  SonarePolyphonicAnalysis* ptr_ = nullptr;
  SonareError error_ = SONARE_OK;
};

/// @brief Renders a handle and returns the samples, releasing the library's array.
std::vector<float> render(SonarePolyphonicAnalysis* analysis,
                          const SonareNoteRenderConfig* config = nullptr) {
  float* out = nullptr;
  size_t length = 0;
  REQUIRE(sonare_polyphonic_render(analysis, config, &out, &length) == SONARE_OK);
  std::vector<float> samples(out, out + length);
  sonare_free_floats(out);
  return samples;
}

size_t note_count_of(SonarePolyphonicAnalysis* analysis) {
  size_t count = 0;
  REQUIRE(sonare_polyphonic_note_count(analysis, &count) == SONARE_OK);
  return count;
}

std::vector<SonareNoteObject> notes_of(SonarePolyphonicAnalysis* analysis) {
  std::vector<SonareNoteObject> notes(note_count_of(analysis));
  size_t written = 0;
  REQUIRE(sonare_polyphonic_notes(analysis, notes.data(), notes.size(), &written) == SONARE_OK);
  REQUIRE(written == notes.size());
  return notes;
}

}  // namespace

TEST_CASE("the polyphonic handle reports what the chain found", "[c_api][polyphony]") {
  const std::vector<float> samples = chord();
  const Handle handle(nullptr, samples);
  REQUIRE(handle.error() == SONARE_OK);
  REQUIRE(handle.get() != nullptr);

  // Two tones, so the fixture the rest of this file relies on resolved as two notes;
  // otherwise every count below would be comparing one note against itself.
  REQUIRE(note_count_of(handle.get()) == 2);

  int32_t frames = 0;
  REQUIRE(sonare_polyphonic_frame_count(handle.get(), &frames) == SONARE_OK);
  REQUIRE(frames > 1);

  SECTION("every note comes back with the identity edit") {
    for (const SonareNoteObject& note : notes_of(handle.get())) {
      CHECK(note.onset_sample < note.offset_sample);
      CHECK(note.frame_start < note.frame_end);
      CHECK(note.median_hz > 0.0f);
      CHECK(note.edit.pitch_shift_semitones == 0.0f);
      CHECK(note.edit.gain_db == 0.0f);
      CHECK(note.edit.time_offset_samples == 0);
      CHECK(note.edit.muted == 0);
      CHECK(note.edit.envelope_count == 0);
      // No pool to index through, which the header states rather than leaving to be
      // inferred from a zero that might have been an offset.
      CHECK(note.amplitude_offset == 0);
      CHECK(note.edit.envelope_offset == 0);
    }
  }

  SECTION("a buffer shorter than the note count is clamped, not refused") {
    SonareNoteObject one{};
    size_t written = 0;
    REQUIRE(sonare_polyphonic_notes(handle.get(), &one, 1, &written) == SONARE_OK);
    REQUIRE(written == 1);
    // A zero capacity writes nothing and needs no buffer, which is how a caller
    // sizes one without a separate count call.
    written = 99;
    REQUIRE(sonare_polyphonic_notes(handle.get(), nullptr, 0, &written) == SONARE_OK);
    REQUIRE(written == 0);
  }

  SECTION("the three per-note curves span the note and nothing else") {
    const std::vector<SonareNoteObject> notes = notes_of(handle.get());
    for (size_t i = 0; i < notes.size(); ++i) {
      const size_t span = static_cast<size_t>(notes[i].frame_end - notes[i].frame_start);
      REQUIRE(span > 0);
      std::vector<float> curve(span + 4, -1.0f);
      size_t written = 0;

      REQUIRE(sonare_polyphonic_note_f0(handle.get(), i, curve.data(), curve.size(), &written) ==
              SONARE_OK);
      REQUIRE(written == span);
      for (size_t k = 0; k < written; ++k) CHECK(curve[k] > 0.0f);

      REQUIRE(sonare_polyphonic_note_amplitude(handle.get(), i, curve.data(), curve.size(),
                                               &written) == SONARE_OK);
      REQUIRE(written == span);
      for (size_t k = 0; k < written; ++k) CHECK(curve[k] >= 0.0f);

      REQUIRE(sonare_polyphonic_note_salience(handle.get(), i, curve.data(), curve.size(),
                                              &written) == SONARE_OK);
      REQUIRE(written == span);
      // The salience curve is the ridge's, read through the ridge's own start. A
      // misalignment there would leave the zeros this asserts against.
      bool any_positive = false;
      for (size_t k = 0; k < written; ++k) {
        CHECK(curve[k] >= 0.0f);
        if (curve[k] > 0.0f) any_positive = true;
      }
      CHECK(any_positive);
    }
  }

  SECTION("the per-frame voice count is one per frame") {
    std::vector<int32_t> counts(static_cast<size_t>(frames) + 4, -1);
    size_t written = 0;
    REQUIRE(sonare_polyphonic_polyphony(handle.get(), counts.data(), counts.size(), &written) ==
            SONARE_OK);
    REQUIRE(written == static_cast<size_t>(frames));
    for (size_t i = 0; i < written; ++i) CHECK(counts[i] >= 0);
  }
}

TEST_CASE("the polyphonic handle forwards the chain exactly", "[c_api][polyphony]") {
  const std::vector<float> samples = chord();
  const sonare::Audio audio =
      sonare::Audio::from_buffer(samples.data(), samples.size(), kSampleRate);
  const sonare::editing::polyphony::PolyphonicAnalysis want =
      sonare::editing::polyphony::analyze_polyphonic(audio);
  const sonare::Audio wanted_render = sonare::editing::polyphony::render_polyphonic(want);

  const Handle handle(nullptr, samples);
  REQUIRE(handle.error() == SONARE_OK);
  REQUIRE(note_count_of(handle.get()) == want.notes.size());

  const std::vector<float> got = render(handle.get());
  REQUIRE(got.size() == wanted_render.size());
  // Exact: the wrapper copies rather than computes, so anything but equality means a
  // second code path reached the samples.
  for (size_t i = 0; i < got.size(); ++i) REQUIRE(got[i] == wanted_render[i]);

  SECTION("and a config field reaches the stage it belongs to") {
    SonarePolyphonicConfig config{};
    config.hop_length = 1024;
    const Handle wider(&config, samples);
    REQUIRE(wider.error() == SONARE_OK);
    int32_t wide_frames = 0;
    int32_t default_frames = 0;
    REQUIRE(sonare_polyphonic_frame_count(wider.get(), &wide_frames) == SONARE_OK);
    REQUIRE(sonare_polyphonic_frame_count(handle.get(), &default_frames) == SONARE_OK);
    // A longer hop is fewer frames. Without this the whole config struct could be
    // dropped on the floor and every other case here would still pass.
    REQUIRE(wide_frames < default_frames);
  }

  SECTION("and a negative selects the zero a zero cannot") {
    // The sentinel's whole claim: a negative is the value 0, not a refusal and not
    // the default. Measured against the chain asked for 0 directly.
    sonare::editing::polyphony::PolyphonicEditConfig zeroed;
    zeroed.extraction.estimation.min_separation_cents = 0.0f;
    zeroed.extraction.ridges.min_duration_ms = 0.0f;
    const sonare::editing::polyphony::PolyphonicAnalysis direct =
        sonare::editing::polyphony::analyze_polyphonic(audio, zeroed);

    SonarePolyphonicConfig config{};
    config.min_separation_cents = -1.0f;
    config.min_ridge_duration_ms = -1.0f;
    const Handle floored(&config, samples);
    REQUIRE(floored.error() == SONARE_OK);
    REQUIRE(note_count_of(floored.get()) == direct.notes.size());

    const sonare::Audio wanted_floored = sonare::editing::polyphony::render_polyphonic(direct);
    const std::vector<float> got_floored = render(floored.get());
    REQUIRE(got_floored.size() == wanted_floored.size());
    for (size_t i = 0; i < got_floored.size(); ++i) REQUIRE(got_floored[i] == wanted_floored[i]);
  }
}

TEST_CASE("the edit is the one thing a host writes, and it reaches the render",
          "[c_api][polyphony]") {
  const std::vector<float> samples = chord();
  const Handle handle(nullptr, samples);
  REQUIRE(handle.error() == SONARE_OK);
  REQUIRE(note_count_of(handle.get()) == 2);
  const std::vector<float> unedited = render(handle.get());

  SECTION("a pitch shift on one note changes the render") {
    SonareNoteEdit edit{};
    edit.pitch_shift_semitones = 1.0f;
    REQUIRE(sonare_polyphonic_set_note_edit(handle.get(), 0, &edit, nullptr, 0) == SONARE_OK);

    // The edit came back, so the handle holds it rather than having validated and
    // dropped it.
    const std::vector<SonareNoteObject> notes = notes_of(handle.get());
    CHECK(notes[0].edit.pitch_shift_semitones == 1.0f);
    CHECK(notes[1].edit.pitch_shift_semitones == 0.0f);

    const std::vector<float> edited = render(handle.get());
    REQUIRE(edited.size() == unedited.size());
    CHECK(std::equal(edited.begin(), edited.end(), unedited.begin()) == false);
  }

  SECTION("an envelope is copied, so the caller's array need not outlive the call") {
    SonareNoteEdit edit{};
    {
      const std::vector<float> envelope{1.0f, 0.25f, 0.0f};
      REQUIRE(sonare_polyphonic_set_note_edit(handle.get(), 1, &edit, envelope.data(),
                                              envelope.size()) == SONARE_OK);
    }
    const std::vector<SonareNoteObject> notes = notes_of(handle.get());
    CHECK(notes[1].edit.envelope_count == 3);
    // A count whose points cannot be fetched would be a field promising what it
    // cannot deliver, so the points come back and are compared to what went in.
    std::vector<float> back(5, -1.0f);
    size_t written = 0;
    REQUIRE(sonare_polyphonic_note_envelope(handle.get(), 1, back.data(), back.size(), &written) ==
            SONARE_OK);
    REQUIRE(written == 3);
    CHECK(back[0] == 1.0f);
    CHECK(back[1] == 0.25f);
    CHECK(back[2] == 0.0f);
    // The other note never had one, which is what says the read is per note.
    REQUIRE(sonare_polyphonic_note_envelope(handle.get(), 0, back.data(), back.size(), &written) ==
            SONARE_OK);
    CHECK(written == 0);

    const std::vector<float> edited = render(handle.get());
    REQUIRE(edited.size() == unedited.size());
    CHECK(std::equal(edited.begin(), edited.end(), unedited.begin()) == false);
  }

  SECTION("a zero stretch ratio reads as one, the way the by-value door reads it") {
    SonareNoteEdit edit{};
    edit.time_stretch_ratio = 0.0f;
    REQUIRE(sonare_polyphonic_set_note_edit(handle.get(), 0, &edit, nullptr, 0) == SONARE_OK);
    CHECK(notes_of(handle.get())[0].edit.time_stretch_ratio == 1.0f);
    // A zeroed edit is the identity, so the render is untouched.
    const std::vector<float> again = render(handle.get());
    REQUIRE(again.size() == unedited.size());
    CHECK(std::equal(again.begin(), again.end(), unedited.begin()));
  }

  SECTION("an edit is replaced rather than merged") {
    SonareNoteEdit edit{};
    edit.gain_db = -6.0f;
    REQUIRE(sonare_polyphonic_set_note_edit(handle.get(), 0, &edit, nullptr, 0) == SONARE_OK);
    REQUIRE(sonare_polyphonic_set_note_edit(handle.get(), 0, nullptr, nullptr, 0) == SONARE_OK);
    CHECK(notes_of(handle.get())[0].edit.gain_db == 0.0f);
    const std::vector<float> again = render(handle.get());
    CHECK(std::equal(again.begin(), again.end(), unedited.begin()));
  }
}

TEST_CASE("the polyphonic C API refuses what it cannot do", "[c_api][polyphony]") {
  const std::vector<float> samples = chord();

  SECTION("analysis") {
    SonarePolyphonicAnalysis* out = nullptr;
    CHECK(sonare_polyphonic_analyze(samples.data(), samples.size(), kSampleRate, nullptr,
                                    nullptr) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(sonare_polyphonic_analyze(nullptr, 0, kSampleRate, nullptr, &out) ==
          SONARE_ERROR_INVALID_PARAMETER);
    CHECK(out == nullptr);
    CHECK(sonare_polyphonic_analyze(samples.data(), 0, kSampleRate, nullptr, &out) ==
          SONARE_ERROR_INVALID_PARAMETER);

    SonarePolyphonicConfig config{};
    config.struct_version = 2;
    CHECK(sonare_polyphonic_analyze(samples.data(), samples.size(), kSampleRate, &config, &out) ==
          SONARE_ERROR_INVALID_PARAMETER);
    config.struct_version = 0;
    config.max_polyphony = -1;
    CHECK(sonare_polyphonic_analyze(samples.data(), samples.size(), kSampleRate, &config, &out) ==
          SONARE_ERROR_INVALID_PARAMETER);
    CHECK(out == nullptr);
  }

  SECTION("a NULL handle, and a note index the analysis does not have") {
    size_t count = 99;
    int32_t frames = 99;
    float curve = 0.0f;
    CHECK(sonare_polyphonic_note_count(nullptr, &count) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(sonare_polyphonic_frame_count(nullptr, &frames) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(sonare_polyphonic_notes(nullptr, nullptr, 0, &count) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(sonare_polyphonic_note_f0(nullptr, 0, &curve, 1, &count) ==
          SONARE_ERROR_INVALID_PARAMETER);
    CHECK(sonare_polyphonic_render(nullptr, nullptr, nullptr, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
    CHECK(sonare_polyphonic_set_note_edit(nullptr, 0, nullptr, nullptr, 0) ==
          SONARE_ERROR_INVALID_PARAMETER);

    const Handle handle(nullptr, samples);
    REQUIRE(handle.error() == SONARE_OK);
    const size_t past_end = note_count_of(handle.get());
    CHECK(sonare_polyphonic_note_f0(handle.get(), past_end, &curve, 1, &count) ==
          SONARE_ERROR_INVALID_PARAMETER);
    CHECK(count == 0);
    CHECK(sonare_polyphonic_note_amplitude(handle.get(), past_end, &curve, 1, &count) ==
          SONARE_ERROR_INVALID_PARAMETER);
    CHECK(sonare_polyphonic_note_salience(handle.get(), past_end, &curve, 1, &count) ==
          SONARE_ERROR_INVALID_PARAMETER);
    CHECK(sonare_polyphonic_note_envelope(handle.get(), past_end, &curve, 1, &count) ==
          SONARE_ERROR_INVALID_PARAMETER);
    CHECK(sonare_polyphonic_set_note_edit(handle.get(), past_end, nullptr, nullptr, 0) ==
          SONARE_ERROR_INVALID_PARAMETER);
    // An envelope count with no array is a caller error rather than an empty envelope.
    CHECK(sonare_polyphonic_set_note_edit(handle.get(), 0, nullptr, nullptr, 3) ==
          SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("destroying a NULL handle") { sonare_polyphonic_analysis_destroy(nullptr); }
}

#endif  // SONARE_WITH_PITCH_EDITOR
