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

/// @brief The sentinel a refused ridge returns, and the 0 it is not.
constexpr float kRefused = -1.0f;

/// @brief C3 and a sampled piano's own stretch there, which is inside the register
///        the fit reaches at this chain's framing.
constexpr float kStretchedHz = 130.8128f;
constexpr float kStretchedB = 1.130e-4f;
constexpr int kStretchedPartials = 20;

/// @brief A declared stretch no stage arrives at on its own, so a note's effective
///        stretch says whether a fit or the fallback placed its claims.
constexpr float kDeclaredStretch = 1.0e-5f;

void add_tone(std::vector<float>& into, float f0_hz, double inharmonicity = 0.0,
              int n_partials = kPartials) {
  const double nyquist = 0.5 * static_cast<double>(kSampleRate);
  for (int h = 1; h <= n_partials; ++h) {
    const double hd = static_cast<double>(h);
    const double hz = hd * static_cast<double>(f0_hz) * std::sqrt(1.0 + inharmonicity * hd * hd);
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

/// @brief One stretched note, isolated, at the pitch the fit's reach covers.
std::vector<float> stretched_tone() {
  std::vector<float> samples(kSourceSamples, 0.0f);
  add_tone(samples, kStretchedHz, kStretchedB, kStretchedPartials);
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

/// @brief The per-note stretch the handle reports, read into a buffer four longer
///        than the note count so a writer running past the count is visible.
std::vector<float> inharmonicity_of(SonarePolyphonicAnalysis* analysis) {
  std::vector<float> out(note_count_of(analysis) + 4, -99.0f);
  size_t written = 0;
  REQUIRE(sonare_polyphonic_note_inharmonicity(analysis, out.data(), out.size(), &written) ==
          SONARE_OK);
  for (size_t i = written; i < out.size(); ++i) REQUIRE(out[i] == -99.0f);
  out.resize(written);
  return out;
}

/// @brief The C++ chain over the same audio, which is the oracle for a wrapper
///        that copies rather than computes.
sonare::editing::polyphony::PolyphonicAnalysis chain_over(
    const std::vector<float>& samples,
    const sonare::editing::polyphony::PolyphonicEditConfig& config) {
  return sonare::editing::polyphony::analyze_polyphonic(
      sonare::Audio::from_buffer(samples.data(), samples.size(), kSampleRate), config);
}

/// @brief The core config an isolated stretched tone is fitted under, and the C
///        struct that has to resolve onto it.
sonare::editing::polyphony::PolyphonicEditConfig fitted_core_config() {
  sonare::editing::polyphony::PolyphonicEditConfig config;
  config.extraction.estimation.max_polyphony = 1;
  config.masks.inharmonicity = kDeclaredStretch;
  config.estimate_inharmonicity = true;
  return config;
}

SonarePolyphonicConfig fitted_c_config() {
  SonarePolyphonicConfig config{};
  config.max_polyphony = 1;
  config.inharmonicity = kDeclaredStretch;
  config.estimate_inharmonicity = 1;
  return config;
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

TEST_CASE("the stretch fit crosses as one entry per note, or not at all", "[c_api][polyphony]") {
  // Three answers, not two. An empty array says the fit never ran; -1 says it ran
  // and would not commit; a non-negative value is a measurement. A host handed
  // only the effective stretch could not separate the second from the third,
  // because a refused note carries the declared value and that defaults to the
  // same 0 a fit of the harmonic series returns.
  const std::vector<float> samples = chord();

  SECTION("not asked for") {
    const Handle handle(nullptr, samples);
    REQUIRE(handle.error() == SONARE_OK);
    REQUIRE(note_count_of(handle.get()) > 0);
    // A capacity is offered and nothing is written, so the zero is the fit's
    // absence rather than a buffer the call could not fill.
    std::vector<float> out(8, -99.0f);
    size_t written = 99;
    REQUIRE(sonare_polyphonic_note_inharmonicity(handle.get(), out.data(), out.size(), &written) ==
            SONARE_OK);
    REQUIRE(written == 0);
    for (const float value : out) REQUIRE(value == -99.0f);
  }

  SECTION("asked for") {
    SonarePolyphonicConfig config{};
    config.estimate_inharmonicity = 1;
    config.inharmonicity = kDeclaredStretch;
    const Handle handle(&config, samples);
    REQUIRE(handle.error() == SONARE_OK);

    sonare::editing::polyphony::PolyphonicEditConfig core;
    core.masks.inharmonicity = kDeclaredStretch;
    core.estimate_inharmonicity = true;
    const sonare::editing::polyphony::PolyphonicAnalysis want = chain_over(samples, core);
    REQUIRE(!want.notes.empty());
    REQUIRE(want.inharmonicity_fit.size() == want.notes.size());

    const std::vector<float> got = inharmonicity_of(handle.get());
    // One entry per note whatever happened to each, and exact rather than close:
    // the wrapper copies, so anything but equality means a second code path
    // reached the values.
    REQUIRE(got.size() == note_count_of(handle.get()));
    REQUIRE(got == want.inharmonicity_fit);
    for (size_t i = 0; i < got.size(); ++i) {
      INFO("note " << i << ": " << got[i]);
      REQUIRE(std::isfinite(got[i]));
      REQUIRE((got[i] == kRefused || got[i] >= 0.0f));
    }
  }
}

TEST_CASE("a fitted stretch and a refused one are different answers at the C door",
          "[c_api][polyphony]") {
  // The value case. Without it every assertion about this accessor is about an
  // array's length, and a wrapper that reported the declared value for every note
  // would pass all of them.
  const std::vector<float> samples = stretched_tone();
  const sonare::editing::polyphony::PolyphonicAnalysis want =
      chain_over(samples, fitted_core_config());

  INFO("the chain found " << want.notes.size() << " notes");
  REQUIRE(want.notes.size() == 1);
  REQUIRE(want.inharmonicity_fit.size() == 1);

  SECTION("a fit the material supports") {
    // The fixture's premise, stated against the oracle so a fit that did not reach
    // this material fails as a statement about the material.
    INFO("the chain fitted " << want.inharmonicity_fit[0]);
    REQUIRE(want.inharmonicity_fit[0] != kRefused);
    REQUIRE(want.inharmonicity_fit[0] >= 0.0f);
    REQUIRE(want.inharmonicity_fit[0] != kDeclaredStretch);

    const SonarePolyphonicConfig config = fitted_c_config();
    const Handle handle(&config, samples);
    REQUIRE(handle.error() == SONARE_OK);
    REQUIRE(note_count_of(handle.get()) == want.notes.size());
    const std::vector<float> got = inharmonicity_of(handle.get());
    REQUIRE(got == want.inharmonicity_fit);
  }

  SECTION("a refusal the configuration forces") {
    // More partials than the twenty claims the geometry places, so the refusal is
    // attributable to the threshold rather than to the material.
    SonarePolyphonicConfig config = fitted_c_config();
    config.inharmonicity_min_partials = 128;
    const Handle handle(&config, samples);
    REQUIRE(handle.error() == SONARE_OK);

    const std::vector<float> got = inharmonicity_of(handle.get());
    REQUIRE(got.size() == 1);
    INFO("returned " << got[0]);
    REQUIRE(got[0] == kRefused);
    // Both halves: 0 is a fitted result meaning the harmonic series, and a build
    // returning it here would send a host to the ideal series where it meant to
    // read the declared value.
    REQUIRE(got[0] != 0.0f);

    sonare::editing::polyphony::PolyphonicEditConfig core = fitted_core_config();
    core.inharmonicity.min_partials = 128;
    REQUIRE(got == chain_over(samples, core).inharmonicity_fit);
  }
}

TEST_CASE("the fit's four fields take their defaults at zero and reach the core otherwise",
          "[c_api][polyphony]") {
  // A zeroed struct is the defaults, so a field silently dropped would produce a
  // correct-looking array measured under something the caller did not ask for.
  // Each field is read off both ends: the value it takes at zero, and a value the
  // core answers differently for.
  const std::vector<float> samples = stretched_tone();

  SECTION("zero is the default on all four") {
    SonarePolyphonicConfig config = fitted_c_config();
    config.inharmonicity_min_partials = 0;
    config.inharmonicity_max_residual_bins = 0.0f;
    config.inharmonicity_max_stretch = 0.0f;
    const Handle handle(&config, samples);
    REQUIRE(handle.error() == SONARE_OK);
    REQUIRE(inharmonicity_of(handle.get()) ==
            chain_over(samples, fitted_core_config()).inharmonicity_fit);
  }

  SECTION("each threshold moved past what the material supplies refuses the note") {
    struct NamedForced {
      const char* what;
      SonarePolyphonicConfig config;
      sonare::editing::polyphony::PolyphonicEditConfig core;
    };
    std::vector<NamedForced> forced;
    {
      NamedForced entry{"more partials than the claim count places", fitted_c_config(),
                        fitted_core_config()};
      entry.config.inharmonicity_min_partials = 128;
      entry.core.inharmonicity.min_partials = 128;
      forced.push_back(entry);
    }
    {
      NamedForced entry{"a misfit ceiling under the fit's own rounding", fitted_c_config(),
                        fitted_core_config()};
      entry.config.inharmonicity_max_residual_bins = 1e-6f;
      entry.core.inharmonicity.max_residual_bins = 1e-6f;
      forced.push_back(entry);
    }
    {
      NamedForced entry{"a stretch ceiling under the one synthesised", fitted_c_config(),
                        fitted_core_config()};
      entry.config.inharmonicity_max_stretch = 1e-6f;
      entry.core.inharmonicity.max_inharmonicity = 1e-6f;
      forced.push_back(entry);
    }

    for (const NamedForced& entry : forced) {
      INFO(entry.what);
      const Handle handle(&entry.config, samples);
      REQUIRE(handle.error() == SONARE_OK);
      const std::vector<float> got = inharmonicity_of(handle.get());
      REQUIRE(got.size() == 1);
      INFO("returned " << got[0]);
      REQUIRE(got[0] == kRefused);
      REQUIRE(got[0] != 0.0f);
      REQUIRE(got == chain_over(samples, entry.core).inharmonicity_fit);
    }
  }

  SECTION("a value outside the core's range is refused, and only where the fit runs") {
    // The sharper half of the zero rule: 1 is rejected while 0 is the default 3,
    // so the sentinel cannot be a field the wrapper forwards unchanged. And a
    // field read only when the fit was asked for refuses nobody who did not ask.
    std::vector<std::pair<const char*, SonarePolyphonicConfig>> rejected;
    for (const int32_t partials : {1, 129}) {
      SonarePolyphonicConfig config{};
      config.inharmonicity_min_partials = partials;
      rejected.emplace_back("a partial count outside [2, 128]", config);
    }
    {
      SonarePolyphonicConfig config{};
      config.inharmonicity_max_residual_bins = -1.0f;
      rejected.emplace_back("a negative misfit ceiling", config);
    }
    {
      SonarePolyphonicConfig config{};
      config.inharmonicity_max_stretch = -1.0f;
      rejected.emplace_back("a negative stretch ceiling", config);
    }

    for (const auto& entry : rejected) {
      INFO(entry.first);
      SonarePolyphonicConfig off = entry.second;
      off.estimate_inharmonicity = 0;
      const Handle unread(&off, samples);
      REQUIRE(unread.error() == SONARE_OK);

      SonarePolyphonicConfig on = entry.second;
      on.estimate_inharmonicity = 1;
      SonarePolyphonicAnalysis* out = nullptr;
      REQUIRE(sonare_polyphonic_analyze(samples.data(), samples.size(), kSampleRate, &on, &out) ==
              SONARE_ERROR_INVALID_PARAMETER);
      REQUIRE(out == nullptr);
    }
  }
}

TEST_CASE("the stretch accessor sizes a buffer the way its siblings do", "[c_api][polyphony]") {
  const std::vector<float> samples = chord();
  SonarePolyphonicConfig config{};
  config.estimate_inharmonicity = 1;
  const Handle handle(&config, samples);
  REQUIRE(handle.error() == SONARE_OK);
  const size_t count = note_count_of(handle.get());
  REQUIRE(count > 1);

  float one = -99.0f;
  size_t written = 99;
  // A short buffer is clamped rather than refused, which is how a host reads the
  // first entry without sizing anything.
  REQUIRE(sonare_polyphonic_note_inharmonicity(handle.get(), &one, 1, &written) == SONARE_OK);
  REQUIRE(written == 1);
  REQUIRE(one != -99.0f);

  // A zero capacity writes nothing and needs no buffer, so a host can size one
  // without a separate call.
  written = 99;
  REQUIRE(sonare_polyphonic_note_inharmonicity(handle.get(), nullptr, 0, &written) == SONARE_OK);
  REQUIRE(written == 0);

  // A capacity with no buffer is a caller error rather than an empty read.
  REQUIRE(sonare_polyphonic_note_inharmonicity(handle.get(), nullptr, count, &written) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_polyphonic_note_inharmonicity(nullptr, &one, 1, &written) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_polyphonic_note_inharmonicity(handle.get(), &one, 1, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
}

#endif  // SONARE_WITH_PITCH_EDITOR
