/// @file convert_test.cpp
/// @brief Tests for unit conversion functions.

#include "core/convert.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace sonare;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

TEST_CASE("hz_to_mel / mel_to_hz", "[convert]") {
  SECTION("low frequency (linear region)") {
    float hz = 500.0f;
    float mel = hz_to_mel(hz);
    REQUIRE_THAT(mel, WithinRel(7.5f, 0.01f));
    REQUIRE_THAT(mel_to_hz(mel), WithinAbs(hz, 0.1f));
  }

  SECTION("high frequency (log region)") {
    float hz = 4000.0f;
    float mel = hz_to_mel(hz);
    REQUIRE_THAT(mel_to_hz(mel), WithinAbs(hz, 0.1f));
  }

  SECTION("1000Hz boundary") {
    float mel = hz_to_mel(1000.0f);
    REQUIRE_THAT(mel, WithinRel(15.0f, 0.01f));
  }
}

TEST_CASE("hz_to_midi / midi_to_hz", "[convert]") {
  // A4 = 440Hz = MIDI 69
  REQUIRE_THAT(hz_to_midi(440.0f), WithinAbs(69.0f, 0.01f));
  REQUIRE_THAT(midi_to_hz(69.0f), WithinAbs(440.0f, 0.01f));

  // C4 = 261.63Hz = MIDI 60
  REQUIRE_THAT(hz_to_midi(261.63f), WithinAbs(60.0f, 0.1f));
  REQUIRE_THAT(midi_to_hz(60.0f), WithinAbs(261.63f, 0.1f));
}

TEST_CASE("hz_to_note / note_to_hz", "[convert]") {
  REQUIRE(hz_to_note(440.0f) == "A4");
  REQUIRE(hz_to_note(261.63f) == "C4");
  REQUIRE_THAT(note_to_hz("A4"), WithinAbs(440.0f, 0.01f));
  REQUIRE_THAT(note_to_hz("C4"), WithinAbs(261.63f, 0.1f));
}

TEST_CASE("frames_to_time / time_to_frames", "[convert]") {
  int sr = 22050;
  int hop = 512;

  // 100 frames * 512 / 22050 = 2.3219...
  float time = frames_to_time(100, sr, hop);
  REQUIRE_THAT(time, WithinAbs(2.32f, 0.01f));
  // Round-trip test uses computed time value
  REQUIRE(time_to_frames(time, sr, hop) == 100);
}
