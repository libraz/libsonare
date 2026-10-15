/// @file channel_voice_predicate_test.cpp
/// @brief sonare::midi::is_channel_voice over channel-voice and non-channel-voice UMPs.

#include <catch2/catch_test_macros.hpp>

#include "midi/ump.h"

TEST_CASE("is_channel_voice accepts MIDI 1.0 and MIDI 2.0 channel voice messages", "[midi]") {
  using namespace sonare::midi;
  REQUIRE(is_channel_voice(make_midi1_note_on(0, 0, 60, 100)));
  REQUIRE(is_channel_voice(make_midi2_control_change(0, 1, 7, 0x80000000u)));
}

TEST_CASE("is_channel_voice rejects utility, system and sysex messages", "[midi]") {
  using namespace sonare::midi;
  Ump utility;
  utility.word_count = 1;  // type 0x0, status 0: NOOP
  REQUIRE_FALSE(is_channel_voice(utility));

  Ump system;
  system.words[0] = 0x10F80000u;  // type 0x1, MIDI timing clock
  system.word_count = 1;
  REQUIRE_FALSE(is_channel_voice(system));

  REQUIRE_FALSE(is_channel_voice(make_sysex_handle(0, 1)));
}
