/// @file core_common_fill_test.cpp
/// @brief Unit tests for the internal C-ABI result fillers in core_common.cpp.

#include <catch2/catch_test_macros.hpp>

#include "c_api/core_internal.h"

TEST_CASE("fill_chord_result nulls the pointer on an empty result", "[c_api]") {
  // Own the empty-result contract: even when the caller hands in a struct whose
  // pointer field is stale, an empty chord list must leave chords == nullptr so a
  // blind sonare_free_chord_analysis_result cannot double-free.
  SonareChordAnalysisResult out;
  out.chords = reinterpret_cast<SonareChord*>(static_cast<std::uintptr_t>(0x1));
  out.chord_count = 42;

  fill_chord_result({}, &out);

  REQUIRE(out.chords == nullptr);
  REQUIRE(out.chord_count == 0);
}

TEST_CASE("fill_chord_result populates a non-empty result", "[c_api]") {
  std::vector<Chord> chords;
  Chord chord;
  chord.root = PitchClass::C;
  chord.quality = ChordQuality::Major;
  chord.start = 0.0f;
  chord.end = 1.0f;
  chord.confidence = 0.9f;
  chord.bass = PitchClass::C;
  chords.push_back(chord);

  SonareChordAnalysisResult out = {};
  fill_chord_result(chords, &out);

  REQUIRE(out.chord_count == 1);
  REQUIRE(out.chords != nullptr);
  REQUIRE(out.chords[0].start == 0.0f);

  sonare_free_chord_analysis_result(&out);
  REQUIRE(out.chords == nullptr);
  REQUIRE(out.chord_count == 0);
}
