/// @file sonare_c_engine_articulation_test.cpp
/// @brief The articulation C ABI: the per-channel round trip, the three
///        refusals it has to keep apart, and that a mode set through C reaches
///        the sound.
///
/// The last one is the point, and it is worth more here than it is for a
/// setter that returns a value: a slurred note and a retriggered one are the
/// same two pitches, so every call can return SONARE_OK with the mode dropped
/// on the floor and nothing in the transcript would say so. One case renders
/// both and requires them to differ; another takes the engine that declines to
/// be carried and requires the fallback counter to move, which is the only
/// thing separating a refusal from a mode that was never set.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "c_api/sonare_c_engine_internal.h"
#include "sonare_c_test_helpers.h"

namespace {

#if defined(SONARE_WITH_ARRANGEMENT)

/// An engine with a NativeSynth on destination @p dest running @p engine_mode.
SonareRealtimeEngine* make_synth_engine(uint32_t dest, int engine_mode) {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 16, 16) == SONARE_OK);
  SonareSynthPatch patch{};
  patch.struct_version = 1;
  patch.engine_mode = engine_mode;
  REQUIRE(sonare_engine_set_synth_instrument(engine, dest, &patch) == SONARE_OK);
  return engine;
}

std::vector<float> render_blocks(SonareRealtimeEngine* engine, int blocks) {
  std::vector<float> out;
  std::vector<float> left(128, 0.0f);
  std::vector<float> right(128, 0.0f);
  float* channels[] = {left.data(), right.data()};
  for (int block = 0; block < blocks; ++block) {
    REQUIRE(sonare_engine_process(engine, channels, 2, 128) == SONARE_OK);
    out.insert(out.end(), left.begin(), left.end());
  }
  return out;
}

/// C4 held, then G4 on top of it, then the LATE note-off of C4 — the order a
/// player's slur actually sends, and the one a mode that ignores the overlap
/// cannot be told apart from by the first half alone.
std::vector<float> render_slur(uint32_t dest, int engine_mode, int articulation,
                               uint32_t* out_fallbacks) {
  SonareRealtimeEngine* engine = make_synth_engine(dest, engine_mode);
  REQUIRE(sonare_engine_set_articulation(engine, dest, 0, articulation) == SONARE_OK);
  REQUIRE(sonare_engine_push_midi_note_on(engine, dest, 0, 0, 60, 100, -1) == SONARE_OK);
  std::vector<float> out = render_blocks(engine, 96);
  REQUIRE(sonare_engine_push_midi_note_on(engine, dest, 0, 0, 67, 100, -1) == SONARE_OK);
  REQUIRE(sonare_engine_push_midi_note_off(engine, dest, 0, 0, 60, 0, -1) == SONARE_OK);
  const std::vector<float> tail = render_blocks(engine, 128);
  out.insert(out.end(), tail.begin(), tail.end());
  if (out_fallbacks != nullptr) {
    REQUIRE(sonare_engine_legato_fallback_count(engine, dest, out_fallbacks) == SONARE_OK);
  }
  sonare_engine_destroy(engine);
  return out;
}

#endif  // defined(SONARE_WITH_ARRANGEMENT)

/// Entries in a '\n'-separated name table.
size_t name_count(const char* names) {
  if (names == nullptr || *names == '\0') return 0;
  size_t count = 1;
  for (const char* p = names; *p != '\0'; ++p) {
    if (*p == '\n') ++count;
  }
  return count;
}

}  // namespace

TEST_CASE("the articulation name table matches its C count", "[c_api][articulation]") {
  const char* names = sonare_synth_enum_names(SONARE_SYNTH_ENUM_ARTICULATION);
  REQUIRE(names != nullptr);
#if defined(SONARE_WITH_ARRANGEMENT)
  REQUIRE(name_count(names) == SONARE_ARTICULATION_COUNT);
  // Named rather than counted alone: a table that lost an entry and gained an
  // empty one keeps its count, and these names are what a surface spells the
  // ordinals as.
  REQUIRE(std::string(names) == "poly\nmono-retrigger\nmono-legato");
#else
  REQUIRE(std::string(names).empty());
#endif
}

#if defined(SONARE_WITH_ARRANGEMENT)

TEST_CASE("articulation round-trips per channel", "[c_api][articulation]") {
  SonareRealtimeEngine* engine = make_synth_engine(3, SONARE_SYNTH_ENGINE_REED);

  int mode = -1;
  REQUIRE(sonare_engine_articulation(engine, 3, 0, &mode) == SONARE_OK);
  REQUIRE(mode == SONARE_ARTICULATION_POLY);

  REQUIRE(sonare_engine_set_articulation(engine, 3, 0, SONARE_ARTICULATION_MONO_LEGATO) ==
          SONARE_OK);
  REQUIRE(sonare_engine_articulation(engine, 3, 0, &mode) == SONARE_OK);
  REQUIRE(mode == SONARE_ARTICULATION_MONO_LEGATO);

  // A second channel is untouched by the first, so the mode is per channel
  // rather than per instrument -- a host slurring one part must not slur the
  // rest of the rack.
  REQUIRE(sonare_engine_articulation(engine, 3, 1, &mode) == SONARE_OK);
  REQUIRE(mode == SONARE_ARTICULATION_POLY);
  REQUIRE(sonare_engine_set_articulation(engine, 3, 1, SONARE_ARTICULATION_MONO_RETRIGGER) ==
          SONARE_OK);
  REQUIRE(sonare_engine_articulation(engine, 3, 1, &mode) == SONARE_OK);
  REQUIRE(mode == SONARE_ARTICULATION_MONO_RETRIGGER);
  REQUIRE(sonare_engine_articulation(engine, 3, 0, &mode) == SONARE_OK);
  REQUIRE(mode == SONARE_ARTICULATION_MONO_LEGATO);

  // Nothing has been asked for and declined yet, so the counter starts where a
  // later case can see it move.
  uint32_t fallbacks = 123;
  REQUIRE(sonare_engine_legato_fallback_count(engine, 3, &fallbacks) == SONARE_OK);
  REQUIRE(fallbacks == 0);

  sonare_engine_destroy(engine);
}

TEST_CASE("articulation keeps its three refusals apart", "[c_api][articulation]") {
  SonareRealtimeEngine* engine = make_synth_engine(3, SONARE_SYNTH_ENGINE_REED);

  // An ordinal past the enum is refused rather than clamped: kPoly substituted
  // for a misspelled kMonoLegato plays every note and slurs none of them, which
  // the caller cannot tell from a request that took.
  REQUIRE(sonare_engine_set_articulation(engine, 3, 0, SONARE_ARTICULATION_COUNT) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_articulation(engine, 3, 0, -1) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_articulation(engine, 3, 16, SONARE_ARTICULATION_POLY) ==
          SONARE_ERROR_INVALID_PARAMETER);
  int mode = 123;
  REQUIRE(sonare_engine_articulation(engine, 3, 16, &mode) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_articulation(engine, 3, 0, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  // None of the above took: the mode is still what the instrument started at.
  REQUIRE(sonare_engine_articulation(engine, 3, 0, &mode) == SONARE_OK);
  REQUIRE(mode == SONARE_ARTICULATION_POLY);

  // A destination nothing is bound to, and a destination holding an instrument
  // with no articulation of its own, are different answers. Both would
  // otherwise read as "the call worked" to a caller that only checks for OK.
  uint32_t fallbacks = 123;
  REQUIRE(sonare_engine_set_articulation(engine, 5, 0, SONARE_ARTICULATION_MONO_LEGATO) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_articulation(engine, 5, 0, &mode) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_legato_fallback_count(engine, 5, &fallbacks) ==
          SONARE_ERROR_INVALID_PARAMETER);
  // Out-parameters are defined on the failure path rather than left at whatever
  // the caller had.
  REQUIRE(mode == SONARE_ARTICULATION_POLY);
  REQUIRE(fallbacks == 0);

  SonareEngineBuiltinSynthConfig builtin{};
  REQUIRE(sonare_engine_set_builtin_instrument(engine, 5, &builtin) == SONARE_OK);
  REQUIRE(sonare_engine_set_articulation(engine, 5, 0, SONARE_ARTICULATION_MONO_LEGATO) ==
          SONARE_ERROR_NOT_SUPPORTED);
  REQUIRE(sonare_engine_articulation(engine, 5, 0, &mode) == SONARE_ERROR_NOT_SUPPORTED);
  // The counter refuses on the same terms rather than answering zero. All
  // three of these are asked together by a host, and a zero here would read as
  // "every slur took" on an instrument that never had a slur to take.
  REQUIRE(sonare_engine_legato_fallback_count(engine, 5, &fallbacks) == SONARE_ERROR_NOT_SUPPORTED);
  REQUIRE(fallbacks == 0);

  sonare_engine_destroy(engine);
}

TEST_CASE("an articulation set through the C ABI reaches the sound", "[c_api][articulation]") {
  uint32_t slurred_fallbacks = 123;
  uint32_t retriggered_fallbacks = 123;
  const std::vector<float> slurred =
      render_slur(3, SONARE_SYNTH_ENGINE_REED, SONARE_ARTICULATION_MONO_LEGATO, &slurred_fallbacks);
  const std::vector<float> retriggered = render_slur(
      3, SONARE_SYNTH_ENGINE_REED, SONARE_ARTICULATION_MONO_RETRIGGER, &retriggered_fallbacks);

  // The renders carry energy, so "they differ" is not two kinds of silence.
  float peak = 0.0f;
  for (const float s : retriggered) peak = std::max(peak, std::abs(s));
  REQUIRE(peak > 0.0f);
  REQUIRE(slurred.size() == retriggered.size());
  REQUIRE(slurred != retriggered);
  // A reed accepts the carry, so nothing was refused on either side.
  REQUIRE(slurred_fallbacks == 0);
  REQUIRE(retriggered_fallbacks == 0);

  // And the same call twice is bit-identical, so the difference above is the
  // articulation rather than anything free-running in the render.
  REQUIRE(render_slur(3, SONARE_SYNTH_ENGINE_REED, SONARE_ARTICULATION_MONO_RETRIGGER, nullptr) ==
          retriggered);
}

TEST_CASE("an engine that declines to be carried is counted through the C ABI",
          "[c_api][articulation]") {
  // A struck string cannot be slurred: its exciter is spent before the second
  // sample. The note still sounds, so the counter is the only evidence.
  uint32_t declined = 0;
  const std::vector<float> audio =
      render_slur(3, SONARE_SYNTH_ENGINE_PIANO, SONARE_ARTICULATION_MONO_LEGATO, &declined);
  REQUIRE(declined >= 1);

  float peak = 0.0f;
  for (const float s : audio) peak = std::max(peak, std::abs(s));
  REQUIRE(peak > 0.0f);

  // The same phrase under the default mode refuses nothing, so the count above
  // is the request being declined rather than a counter that only ever rises.
  uint32_t poly = 123;
  render_slur(3, SONARE_SYNTH_ENGINE_PIANO, SONARE_ARTICULATION_POLY, &poly);
  REQUIRE(poly == 0);
}

#endif  // defined(SONARE_WITH_ARRANGEMENT)
