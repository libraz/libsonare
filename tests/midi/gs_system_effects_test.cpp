/// @file gs_system_effects_test.cpp
/// @brief The GS system-effect value layer: the reset defaults, the byte ->
///        physical-unit conversions, and the macro parameter blocks.
///
/// The expected values here are a second, independent transcription of the
/// SC-8850 Parameter Address Map — never derived from the header — so a wrong
/// default or a wrong DELAY TIME CENTER breakpoint fails by name. The default
/// table's row count is checked against kGsSystemEffectFieldCount so a field
/// added without a test row fails too.

#include "midi/synth/gs_system_effects.h"

#include <algorithm>
#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#if defined(SONARE_MIDI_WITH_FX)
#include "midi/synth/gs_effects.h"
#include "support/alloc_guard.h"
#endif

namespace {

using Catch::Approx;
using sonare::midi::synth::GsChorusMacroParams;
using sonare::midi::synth::GsDelayMacroParams;
using sonare::midi::synth::GsReverbMacroParams;
using sonare::midi::synth::GsSystemEffects;
using sonare::midi::synth::kGsChorusMacros;
using sonare::midi::synth::kGsDelayMacros;
using sonare::midi::synth::kGsPreLpfThruHz;
using sonare::midi::synth::kGsReverbMacros;
using sonare::midi::synth::kGsSystemEffectFieldCount;

using Field = uint8_t GsSystemEffects::*;

struct DefaultRow {
  const char* name;
  Field field;
  int expected;
};

/// Transcribed from the map, not from the header: address, name, reset value.
const std::array<DefaultRow, 27> kResetDefaults{{
    {"40 01 30 REVERB MACRO", &GsSystemEffects::reverb_macro, 4},  // Hall 2, not 0
    {"40 01 31 REVERB CHARACTER", &GsSystemEffects::reverb_character, 4},
    {"40 01 32 REVERB PRE-LPF", &GsSystemEffects::reverb_pre_lpf, 0},
    {"40 01 33 REVERB LEVEL", &GsSystemEffects::reverb_level, 64},
    {"40 01 34 REVERB TIME", &GsSystemEffects::reverb_time, 64},
    {"40 01 35 REVERB DELAY FEEDBACK", &GsSystemEffects::reverb_delay_feedback, 0},
    {"40 01 37 REVERB PREDELAY TIME", &GsSystemEffects::reverb_predelay, 0},
    {"40 01 38 CHORUS MACRO", &GsSystemEffects::chorus_macro, 2},
    {"40 01 39 CHORUS PRE-LPF", &GsSystemEffects::chorus_pre_lpf, 0},
    {"40 01 3A CHORUS LEVEL", &GsSystemEffects::chorus_level, 64},
    {"40 01 3B CHORUS FEEDBACK", &GsSystemEffects::chorus_feedback, 8},
    {"40 01 3C CHORUS DELAY", &GsSystemEffects::chorus_delay, 80},
    {"40 01 3D CHORUS RATE", &GsSystemEffects::chorus_rate, 3},
    {"40 01 3E CHORUS DEPTH", &GsSystemEffects::chorus_depth, 19},
    {"40 01 3F CHORUS SEND TO REVERB", &GsSystemEffects::chorus_send_to_reverb, 0},
    {"40 01 40 CHORUS SEND TO DELAY", &GsSystemEffects::chorus_send_to_delay, 0},
    {"40 01 50 DELAY MACRO", &GsSystemEffects::delay_macro, 0},
    {"40 01 51 DELAY PRE-LPF", &GsSystemEffects::delay_pre_lpf, 0},
    {"40 01 52 DELAY TIME CENTER", &GsSystemEffects::delay_time_center, 0x61},
    {"40 01 53 DELAY TIME RATIO LEFT", &GsSystemEffects::delay_time_ratio_left, 0x01},
    {"40 01 54 DELAY TIME RATIO RIGHT", &GsSystemEffects::delay_time_ratio_right, 0x01},
    {"40 01 55 DELAY LEVEL CENTER", &GsSystemEffects::delay_level_center, 0x7F},
    {"40 01 56 DELAY LEVEL LEFT", &GsSystemEffects::delay_level_left, 0x00},
    {"40 01 57 DELAY LEVEL RIGHT", &GsSystemEffects::delay_level_right, 0x00},
    {"40 01 58 DELAY LEVEL", &GsSystemEffects::delay_level, 0x40},
    {"40 01 59 DELAY FEEDBACK", &GsSystemEffects::delay_feedback, 0x50},  // = +16, not centre
    {"40 01 5A DELAY SEND TO REVERB", &GsSystemEffects::delay_send_to_reverb, 0x00},
}};

/// The manual's DELAY TIME CENTER segments: first value, last value, the time
/// at each end, and the step inside. Transcribed independently of the header.
struct DelaySegment {
  uint8_t lo;
  uint8_t hi;
  double lo_ms;
  double hi_ms;
  double step_ms;
};

const std::array<DelaySegment, 9> kDelaySegments{{
    {0x01, 0x14, 0.1, 2.0, 0.1},
    {0x14, 0x23, 2.0, 5.0, 0.2},
    {0x23, 0x2D, 5.0, 10.0, 0.5},
    {0x2D, 0x37, 10.0, 20.0, 1.0},
    {0x37, 0x46, 20.0, 50.0, 2.0},
    {0x46, 0x50, 50.0, 100.0, 5.0},
    {0x50, 0x5A, 100.0, 200.0, 10.0},
    {0x5A, 0x69, 200.0, 500.0, 20.0},
    {0x69, 0x73, 500.0, 1000.0, 50.0},
}};

std::string byte_name(const char* what, int value) {
  return std::string(what) + " = " + std::to_string(value);
}

std::vector<int> reverb_block(const GsReverbMacroParams& p) {
  return {p.character, p.pre_lpf, p.level, p.time, p.delay_feedback, p.predelay};
}

std::vector<int> chorus_block(const GsChorusMacroParams& p) {
  return {p.pre_lpf, p.level, p.feedback,       p.delay,
          p.rate,    p.depth, p.send_to_reverb, p.send_to_delay};
}

std::vector<int> delay_block(const GsDelayMacroParams& p) {
  return {p.pre_lpf,    p.time_center, p.time_ratio_left, p.time_ratio_right, p.level_center,
          p.level_left, p.level_right, p.level,           p.feedback,         p.send_to_reverb};
}

}  // namespace

TEST_CASE("GS system effects: every address resets to the manual's default", "[midi][synth][gs]") {
  // A field added outside the X-macro list fails the header's static_assert; a
  // field added to it without a row here fails this count.
  REQUIRE(kResetDefaults.size() == kGsSystemEffectFieldCount);

  for (size_t i = 0; i < kResetDefaults.size(); ++i) {
    for (size_t j = i + 1; j < kResetDefaults.size(); ++j) {
      INFO(kResetDefaults[i].name << " and " << kResetDefaults[j].name);
      CHECK(kResetDefaults[i].field != kResetDefaults[j].field);
    }
  }

  const GsSystemEffects fx;
  for (const DefaultRow& row : kResetDefaults) {
    INFO(row.name);
    CHECK(static_cast<int>(fx.*(row.field)) == row.expected);
  }
}

TEST_CASE("GS delay time centre follows the manual's piecewise table", "[midi][synth][gs]") {
  using sonare::midi::synth::gs_delay_time_ms;

  SECTION("every segment endpoint is exact") {
    for (const DelaySegment& seg : kDelaySegments) {
      INFO(byte_name("segment start", seg.lo));
      CHECK(gs_delay_time_ms(seg.lo) == Approx(seg.lo_ms).epsilon(1e-4));
      INFO(byte_name("segment end", seg.hi));
      CHECK(gs_delay_time_ms(seg.hi) == Approx(seg.hi_ms).epsilon(1e-4));
    }
  }

  SECTION("a shared boundary value reads the same from either segment") {
    // The manual's segments share their endpoints (01-14, 14-23, ...). Both
    // readings give the same time, so the duplication carries no ambiguity.
    for (size_t i = 1; i < kDelaySegments.size(); ++i) {
      INFO(byte_name("boundary", kDelaySegments[i].lo));
      CHECK(kDelaySegments[i - 1].hi == kDelaySegments[i].lo);
      CHECK(kDelaySegments[i - 1].hi_ms == Approx(kDelaySegments[i].lo_ms));
      CHECK(gs_delay_time_ms(kDelaySegments[i].lo) ==
            Approx(kDelaySegments[i].lo_ms).epsilon(1e-4));
    }
  }

  SECTION("the step inside a segment is the manual's") {
    for (const DelaySegment& seg : kDelaySegments) {
      for (int v = seg.lo; v < seg.hi; ++v) {
        INFO(byte_name("step above", v));
        const double step = gs_delay_time_ms(static_cast<uint8_t>(v + 1)) -
                            gs_delay_time_ms(static_cast<uint8_t>(v));
        CHECK(step == Approx(seg.step_ms).epsilon(1e-3));
      }
    }
  }

  SECTION("monotone over the whole defined range") {
    for (int v = 0x01; v < 0x73; ++v) {
      INFO(byte_name("value", v));
      CHECK(gs_delay_time_ms(static_cast<uint8_t>(v)) <
            gs_delay_time_ms(static_cast<uint8_t>(v + 1)));
    }
  }

  SECTION("out-of-range bytes clamp to the defined domain") {
    // The value layer is total; refusing an out-of-range byte is the decode
    // layer's job (gs.md: out of range is ignored, never clamped, there).
    CHECK(gs_delay_time_ms(0x00) == Approx(0.1).epsilon(1e-4));
    for (int v = 0x74; v <= 0x7F; ++v) {
      INFO(byte_name("value", v));
      CHECK(gs_delay_time_ms(static_cast<uint8_t>(v)) == Approx(1000.0).epsilon(1e-4));
    }
  }

  SECTION("the power-on default is 340 ms") {
    CHECK(gs_delay_time_ms(GsSystemEffects{}.delay_time_center) == Approx(340.0).epsilon(1e-4));
  }
}

TEST_CASE("GS delay feedback is signed around 0x40", "[midi][synth][gs]") {
  using sonare::midi::synth::gs_delay_feedback_coefficient;
  using sonare::midi::synth::gs_delay_feedback_signed;

  CHECK(gs_delay_feedback_signed(0x00) == -64);
  CHECK(gs_delay_feedback_signed(0x40) == 0);
  CHECK(gs_delay_feedback_signed(0x7F) == 63);
  CHECK(gs_delay_feedback_signed(0x50) == 16);

  // An unsigned reading is monotone too, so sign is what has to be asserted.
  CHECK(gs_delay_feedback_coefficient(0x40) == Approx(0.0));
  CHECK(gs_delay_feedback_coefficient(0x50) > 0.0f);
  CHECK(gs_delay_feedback_coefficient(0x30) < 0.0f);
  CHECK(gs_delay_feedback_coefficient(0x00) < 0.0f);
  CHECK(gs_delay_feedback_coefficient(0x00) ==
        Approx(-gs_delay_feedback_coefficient(0x7F) * (64.0f / 63.0f)).epsilon(1e-5));

  for (int v = 0; v < 0x7F; ++v) {
    INFO(byte_name("value", v));
    CHECK(gs_delay_feedback_signed(static_cast<uint8_t>(v)) <
          gs_delay_feedback_signed(static_cast<uint8_t>(v + 1)));
    CHECK(std::abs(gs_delay_feedback_coefficient(static_cast<uint8_t>(v))) <= 0.9f);
  }

  SECTION("the power-on default is positive and not centre") {
    const GsSystemEffects fx;
    CHECK(gs_delay_feedback_signed(fx.delay_feedback) == 16);
    CHECK(gs_delay_feedback_coefficient(fx.delay_feedback) > 0.0f);
  }
}

TEST_CASE("GS scalar conversions span their documented ranges", "[midi][synth][gs]") {
  using sonare::midi::synth::gs_chorus_delay_ms;
  using sonare::midi::synth::gs_chorus_depth_ms;
  using sonare::midi::synth::gs_chorus_feedback_coefficient;
  using sonare::midi::synth::gs_chorus_rate_hz;
  using sonare::midi::synth::gs_delay_time_ratio_percent;
  using sonare::midi::synth::gs_effect_level;
  using sonare::midi::synth::gs_pre_lpf_cutoff_hz;
  using sonare::midi::synth::gs_reverb_delay_feedback_coefficient;
  using sonare::midi::synth::gs_reverb_predelay_ms;
  using sonare::midi::synth::gs_reverb_time_seconds;

  SECTION("delay time ratio steps by 100/24 percent") {
    CHECK(gs_delay_time_ratio_percent(24) == Approx(100.0).epsilon(1e-5));
    CHECK(gs_delay_time_ratio_percent(0x78) == Approx(500.0).epsilon(1e-5));
    CHECK(gs_delay_time_ratio_percent(0x01) == Approx(100.0 / 24.0).epsilon(1e-5));
    for (int v = 0x01; v < 0x78; ++v) {
      INFO(byte_name("value", v));
      const double step = gs_delay_time_ratio_percent(static_cast<uint8_t>(v + 1)) -
                          gs_delay_time_ratio_percent(static_cast<uint8_t>(v));
      CHECK(step == Approx(100.0 / 24.0).epsilon(1e-4));
    }
    CHECK(gs_delay_time_ratio_percent(0x00) == Approx(gs_delay_time_ratio_percent(0x01)));
    CHECK(gs_delay_time_ratio_percent(0x7F) == Approx(gs_delay_time_ratio_percent(0x78)));
  }

  SECTION("levels are linear over [0, 1]") {
    CHECK(gs_effect_level(0) == Approx(0.0));
    CHECK(gs_effect_level(127) == Approx(1.0));
    CHECK(gs_effect_level(64) == Approx(64.0 / 127.0).epsilon(1e-5));
  }

  SECTION("reverb time rises with the byte and stays inside its endpoints") {
    CHECK(gs_reverb_time_seconds(0) == Approx(0.2).epsilon(1e-4));
    CHECK(gs_reverb_time_seconds(127) == Approx(7.17).epsilon(1e-4));
    // The power-on byte is what the span was set by: a measured SC-8850 decays
    // 1.97 s over 500 Hz-1 kHz at Hall 2. Pinned here so the one point the curve
    // is fitted to cannot move without this saying so.
    CHECK(gs_reverb_time_seconds(64) == Approx(1.97).epsilon(1e-3));
    for (int v = 0; v < 127; ++v) {
      INFO(byte_name("value", v));
      CHECK(gs_reverb_time_seconds(static_cast<uint8_t>(v)) <
            gs_reverb_time_seconds(static_cast<uint8_t>(v + 1)));
    }
  }

  SECTION("predelay is milliseconds one for one") {
    CHECK(gs_reverb_predelay_ms(0) == Approx(0.0));
    CHECK(gs_reverb_predelay_ms(37) == Approx(37.0));
    CHECK(gs_reverb_predelay_ms(127) == Approx(127.0));
  }

  SECTION("unsigned feedbacks rise from zero and stay stable") {
    CHECK(gs_chorus_feedback_coefficient(0) == Approx(0.0));
    CHECK(gs_reverb_delay_feedback_coefficient(0) == Approx(0.0));
    CHECK(gs_chorus_feedback_coefficient(127) <= 0.95f);
    CHECK(gs_reverb_delay_feedback_coefficient(127) <= 0.95f);
    for (int v = 0; v < 127; ++v) {
      INFO(byte_name("value", v));
      CHECK(gs_chorus_feedback_coefficient(static_cast<uint8_t>(v)) <
            gs_chorus_feedback_coefficient(static_cast<uint8_t>(v + 1)));
    }
  }

  SECTION("chorus rate, depth and delay rise with the byte") {
    CHECK(gs_chorus_rate_hz(0) == Approx(0.0));
    CHECK(gs_chorus_rate_hz(127) > 10.0f);
    CHECK(gs_chorus_depth_ms(0) > 0.0f);
    CHECK(gs_chorus_depth_ms(127) == Approx(40.0).epsilon(1e-3));
    CHECK(gs_chorus_delay_ms(127) == Approx(40.0).epsilon(1e-3));
    for (int v = 0; v < 127; ++v) {
      INFO(byte_name("value", v));
      CHECK(gs_chorus_rate_hz(static_cast<uint8_t>(v)) <
            gs_chorus_rate_hz(static_cast<uint8_t>(v + 1)));
      CHECK(gs_chorus_depth_ms(static_cast<uint8_t>(v)) <
            gs_chorus_depth_ms(static_cast<uint8_t>(v + 1)));
    }
  }

  SECTION("pre-LPF 0 is THRU and the cutoff falls from there") {
    CHECK(gs_pre_lpf_cutoff_hz(0) == Approx(kGsPreLpfThruHz));
    for (int v = 0; v < 7; ++v) {
      INFO(byte_name("value", v));
      CHECK(gs_pre_lpf_cutoff_hz(static_cast<uint8_t>(v)) >
            gs_pre_lpf_cutoff_hz(static_cast<uint8_t>(v + 1)));
    }
    // Out of range clamps to the last entry rather than wrapping.
    for (int v = 8; v <= 127; ++v) {
      INFO(byte_name("value", v));
      CHECK(gs_pre_lpf_cutoff_hz(static_cast<uint8_t>(v)) == Approx(gs_pre_lpf_cutoff_hz(7)));
    }
  }
}

TEST_CASE("GS CHARACTER delay time uses the named designed law", "[midi][synth][gs]") {
  using sonare::midi::synth::gs_delay_time_ms;
  using sonare::midi::synth::gs_reverb_delay_time_ms_designed;

  CHECK(gs_reverb_delay_time_ms_designed(0) == Approx(1.0f));
  CHECK(gs_reverb_delay_time_ms_designed(127) == Approx(1000.0f));
  CHECK(gs_reverb_delay_time_ms_designed(64) == Approx(1.0f + 999.0f * 64.0f / 127.0f));
  for (int value = 0; value < 127; ++value) {
    INFO("value " << value);
    CHECK(gs_reverb_delay_time_ms_designed(static_cast<uint8_t>(value)) <
          gs_reverb_delay_time_ms_designed(static_cast<uint8_t>(value + 1)));
  }
  // This law is a deliberate modern design for an unmeasured curve; it must
  // remain distinct from the manual's DELAY TIME CENTER table.
  CHECK(gs_reverb_delay_time_ms_designed(64) != Approx(gs_delay_time_ms(0x61)).margin(1.0e-3f));
}

TEST_CASE("GS macro blocks are distinct and reproduce the power-on state", "[midi][synth][gs]") {
  using sonare::midi::synth::gs_chorus_macro_params;
  using sonare::midi::synth::gs_delay_macro_params;
  using sonare::midi::synth::gs_reverb_macro_params;

  SECTION("no two macros load the same parameter block") {
    for (size_t i = 0; i < kGsReverbMacros.size(); ++i) {
      for (size_t j = i + 1; j < kGsReverbMacros.size(); ++j) {
        INFO("reverb macros " << i << " and " << j);
        CHECK(reverb_block(kGsReverbMacros[i]) != reverb_block(kGsReverbMacros[j]));
      }
    }
    for (size_t i = 0; i < kGsChorusMacros.size(); ++i) {
      for (size_t j = i + 1; j < kGsChorusMacros.size(); ++j) {
        INFO("chorus macros " << i << " and " << j);
        CHECK(chorus_block(kGsChorusMacros[i]) != chorus_block(kGsChorusMacros[j]));
      }
    }
    for (size_t i = 0; i < kGsDelayMacros.size(); ++i) {
      for (size_t j = i + 1; j < kGsDelayMacros.size(); ++j) {
        INFO("delay macros " << i << " and " << j);
        CHECK(delay_block(kGsDelayMacros[i]) != delay_block(kGsDelayMacros[j]));
      }
    }
  }

  SECTION("the default macro of each unit loads exactly the reset defaults") {
    const GsSystemEffects reset;
    GsSystemEffects fx;
    sonare::midi::synth::gs_apply_reverb_macro(fx, reset.reverb_macro);
    sonare::midi::synth::gs_apply_chorus_macro(fx, reset.chorus_macro);
    sonare::midi::synth::gs_apply_delay_macro(fx, reset.delay_macro);
    for (const DefaultRow& row : kResetDefaults) {
      INFO(row.name);
      CHECK(static_cast<int>(fx.*(row.field)) == row.expected);
    }
  }

  SECTION("reverb decay follows the hardware's order Room 1 < ... < Hall 2") {
    for (int m = 0; m < 4; ++m) {
      INFO("reverb macro " << m);
      CHECK(gs_reverb_macro_params(static_cast<uint8_t>(m)).time <
            gs_reverb_macro_params(static_cast<uint8_t>(m + 1)).time);
    }
  }

  SECTION("chorus feedback rises across Chorus 1-4") {
    for (int m = 0; m < 3; ++m) {
      INFO("chorus macro " << m);
      CHECK(gs_chorus_macro_params(static_cast<uint8_t>(m)).feedback <
            gs_chorus_macro_params(static_cast<uint8_t>(m + 1)).feedback);
    }
  }

  SECTION("character 6 and 7 are the delay-type reverbs, and only those") {
    for (int c = 0; c < 8; ++c) {
      INFO("character " << c);
      CHECK(sonare::midi::synth::gs_reverb_character_is_delay(static_cast<uint8_t>(c)) == (c >= 6));
    }
    // A macro selects the algorithm of its own number (the manual is explicit).
    for (size_t m = 0; m < kGsReverbMacros.size(); ++m) {
      INFO("reverb macro " << m);
      CHECK(static_cast<size_t>(kGsReverbMacros[m].character) == m);
    }
  }

  SECTION("Delay to Reverb is the only delay macro that writes SEND TO REVERB") {
    for (size_t m = 0; m < kGsDelayMacros.size(); ++m) {
      INFO("delay macro " << m);
      CHECK((kGsDelayMacros[m].send_to_reverb != 0) == (m == 8));
    }
  }

  SECTION("an out-of-range macro index clamps to the last entry") {
    for (int m = 8; m <= 127; ++m) {
      INFO(byte_name("macro", m));
      CHECK(reverb_block(gs_reverb_macro_params(static_cast<uint8_t>(m))) ==
            reverb_block(kGsReverbMacros.back()));
      CHECK(chorus_block(gs_chorus_macro_params(static_cast<uint8_t>(m))) ==
            chorus_block(kGsChorusMacros.back()));
    }
    for (int m = 10; m <= 127; ++m) {
      INFO(byte_name("macro", m));
      CHECK(delay_block(gs_delay_macro_params(static_cast<uint8_t>(m))) ==
            delay_block(kGsDelayMacros.back()));
    }
    GsSystemEffects fx;
    sonare::midi::synth::gs_apply_delay_macro(fx, 200);
    CHECK(static_cast<int>(fx.delay_macro) == 9);
  }
}

TEST_CASE("GS macro selection overwrites the parameters, and a later write wins",
          "[midi][synth][gs]") {
  using sonare::midi::synth::gs_apply_chorus_macro;
  using sonare::midi::synth::gs_apply_delay_macro;
  using sonare::midi::synth::gs_apply_reverb_macro;

  // Macro x individual parameter x value, exhaustively: 26 macros over the
  // three units, every parameter the macro covers, four probe values each.
  const std::array<uint8_t, 4> kProbes{{0, 1, 64, 127}};

  SECTION("a reverb parameter written after the macro survives, and only it moves") {
    static const std::array<Field, 6> kCovered{{
        &GsSystemEffects::reverb_character,
        &GsSystemEffects::reverb_pre_lpf,
        &GsSystemEffects::reverb_level,
        &GsSystemEffects::reverb_time,
        &GsSystemEffects::reverb_delay_feedback,
        &GsSystemEffects::reverb_predelay,
    }};
    for (int m = 0; m < 8; ++m) {
      const std::vector<int> block = reverb_block(kGsReverbMacros[static_cast<size_t>(m)]);
      for (size_t i = 0; i < kCovered.size(); ++i) {
        for (uint8_t probe : kProbes) {
          GsSystemEffects fx;
          gs_apply_reverb_macro(fx, static_cast<uint8_t>(m));
          fx.*(kCovered[i]) = probe;
          INFO("reverb macro " << m << ", field " << i << ", " << byte_name("probe", probe));
          CHECK(static_cast<int>(fx.*(kCovered[i])) == static_cast<int>(probe));
          CHECK(static_cast<int>(fx.reverb_macro) == m);
          for (size_t j = 0; j < kCovered.size(); ++j) {
            if (j == i) continue;
            INFO("untouched field " << j);
            CHECK(static_cast<int>(fx.*(kCovered[j])) == block[j]);
          }
        }
      }
    }
  }

  SECTION("a macro arriving after an individual parameter overwrites it") {
    static const std::array<Field, 6> kCovered{{
        &GsSystemEffects::reverb_character,
        &GsSystemEffects::reverb_pre_lpf,
        &GsSystemEffects::reverb_level,
        &GsSystemEffects::reverb_time,
        &GsSystemEffects::reverb_delay_feedback,
        &GsSystemEffects::reverb_predelay,
    }};
    for (int m = 0; m < 8; ++m) {
      const GsReverbMacroParams p = kGsReverbMacros[static_cast<size_t>(m)];
      const std::vector<int> expected = reverb_block(p);
      for (size_t i = 0; i < kCovered.size(); ++i) {
        for (uint8_t probe : kProbes) {
          GsSystemEffects fx;
          fx.*(kCovered[i]) = probe;
          gs_apply_reverb_macro(fx, static_cast<uint8_t>(m));
          INFO("reverb macro " << m << ", field " << i << ", " << byte_name("probe", probe));
          CHECK(static_cast<int>(fx.*(kCovered[i])) == expected[i]);
        }
      }
    }
  }

  SECTION("chorus and delay macros write every parameter they cover") {
    for (int m = 0; m < 8; ++m) {
      for (uint8_t probe : kProbes) {
        GsSystemEffects fx;
        fx.chorus_pre_lpf = probe;
        fx.chorus_level = probe;
        fx.chorus_feedback = probe;
        fx.chorus_delay = probe;
        fx.chorus_rate = probe;
        fx.chorus_depth = probe;
        fx.chorus_send_to_reverb = probe;
        fx.chorus_send_to_delay = probe;
        gs_apply_chorus_macro(fx, static_cast<uint8_t>(m));
        INFO("chorus macro " << m << ", " << byte_name("probe", probe));
        CHECK(chorus_block(kGsChorusMacros[static_cast<size_t>(m)]) ==
              std::vector<int>{fx.chorus_pre_lpf, fx.chorus_level, fx.chorus_feedback,
                               fx.chorus_delay, fx.chorus_rate, fx.chorus_depth,
                               fx.chorus_send_to_reverb, fx.chorus_send_to_delay});
      }
    }
    for (int m = 0; m < 10; ++m) {
      for (uint8_t probe : kProbes) {
        GsSystemEffects fx;
        fx.delay_pre_lpf = probe;
        fx.delay_time_center = probe;
        fx.delay_time_ratio_left = probe;
        fx.delay_time_ratio_right = probe;
        fx.delay_level_center = probe;
        fx.delay_level_left = probe;
        fx.delay_level_right = probe;
        fx.delay_level = probe;
        fx.delay_feedback = probe;
        fx.delay_send_to_reverb = probe;
        gs_apply_delay_macro(fx, static_cast<uint8_t>(m));
        INFO("delay macro " << m << ", " << byte_name("probe", probe));
        CHECK(delay_block(kGsDelayMacros[static_cast<size_t>(m)]) ==
              std::vector<int>{fx.delay_pre_lpf, fx.delay_time_center, fx.delay_time_ratio_left,
                               fx.delay_time_ratio_right, fx.delay_level_center,
                               fx.delay_level_left, fx.delay_level_right, fx.delay_level,
                               fx.delay_feedback, fx.delay_send_to_reverb});
      }
    }
  }

  SECTION("selecting one unit's macro leaves the other two alone") {
    GsSystemEffects fx;
    const GsSystemEffects reset;
    gs_apply_reverb_macro(fx, 0);
    CHECK(fx.chorus_delay == reset.chorus_delay);
    CHECK(fx.delay_time_center == reset.delay_time_center);
    gs_apply_delay_macro(fx, 3);
    CHECK(fx.chorus_delay == reset.chorus_delay);
    CHECK(fx.reverb_time == kGsReverbMacros[0].time);
  }
}

#if defined(SONARE_MIDI_WITH_FX)

TEST_CASE("a return level is unity at its reset value, not at full scale", "[midi][synth][gs]") {
  using sonare::midi::synth::gs_effect_level;
  using sonare::midi::synth::gs_return_level;

  // A send is a fraction of a part, so 127 is all of it. A return has no such
  // anchor and the manual gives none: 0-127, no unit, reset 64. Reading 127 as
  // unity would put the state a file never writes 6 dB under the bus this synth
  // was voiced at, which is a claim about our own nominal rather than the
  // manual's. The two mappings must therefore stay distinct functions.
  CHECK(gs_return_level(64) == Approx(1.0f));
  CHECK(gs_effect_level(127) == Approx(1.0f));
  CHECK(gs_return_level(0) == Approx(0.0f));
  CHECK(gs_return_level(127) == Approx(127.0f / 64.0f));  // ~+5.95 dB

  // Strictly monotone over the whole byte, so no part of the range is inert.
  for (int v = 0; v < 127; ++v) {
    INFO("value " << v);
    CHECK(gs_return_level(static_cast<uint8_t>(v)) < gs_return_level(static_cast<uint8_t>(v + 1)));
  }

  // The three that are returns use it; the cross-sends stay on the send map.
  const GsSystemEffects fx;
  CHECK(fx.reverb_level == 64);
  CHECK(fx.chorus_level == 64);
  CHECK(fx.delay_level == 0x40);

#if defined(SONARE_MIDI_WITH_FX)
  // The assertion the reference point actually rests on, and the one a render
  // cannot make: the reset state has to map to the gains the bus shipped with.
  // Comparing a written reset value against an unwritten one cannot see this —
  // both go through the same mapping, so they agree wherever unity is put.
  using sonare::midi::synth::gs_effects_config_from;
  using sonare::midi::synth::GsEffectsConfig;
  const GsEffectsConfig shipped;
  const GsEffectsConfig from_reset = gs_effects_config_from(fx);
  CHECK(from_reset.reverb_level == Approx(shipped.reverb_level));
  CHECK(from_reset.chorus_level == Approx(shipped.chorus_level));
  CHECK(from_reset.delay_level == Approx(shipped.delay_level));
#endif
}

TEST_CASE("GS system effects bridge onto the effect bus config", "[midi][synth][gs]") {
  using sonare::midi::synth::gs_delay_feedback_coefficient;
  using sonare::midi::synth::gs_delay_time_ms;
  using sonare::midi::synth::gs_effects_config_from;
  using sonare::midi::synth::gs_reverb_delay_feedback_coefficient;
  using sonare::midi::synth::gs_reverb_delay_time_ms_designed;
  using sonare::midi::synth::GsEffectsConfig;

  SECTION("a default config is still what the bus shipped with") {
    // The bus now runs what gs_effects_config_from produces, so these are the
    // values a file that writes nothing hears. A separate case checks that the
    // reset state maps onto exactly them.
    const GsEffectsConfig cfg;
    CHECK(cfg.enable_reverb);
    CHECK(cfg.enable_chorus);
    CHECK(cfg.enable_delay);
    CHECK(cfg.reverb_decay == Approx(0.7f));
    CHECK(cfg.reverb_damping == Approx(0.4f));
    CHECK(cfg.chorus_rate_hz == Approx(0.8f));
    CHECK(cfg.chorus_depth_ms == Approx(6.0f));
    CHECK(cfg.delay_time_ms == Approx(340.0f));
    CHECK(cfg.delay_feedback == Approx(0.25f));
    // The fields the value layer needed are inert at their defaults.
    CHECK(cfg.reverb_level == Approx(1.0f));
    CHECK(cfg.reverb_predelay_ms == Approx(0.0f));
    CHECK(cfg.reverb_pre_lpf_hz == Approx(kGsPreLpfThruHz));
    CHECK(cfg.chorus_level == Approx(1.0f));
    CHECK(cfg.chorus_feedback == Approx(0.0f));
    CHECK(cfg.delay_level == Approx(1.0f));
    CHECK(cfg.delay_time_ratio_left == Approx(1.0f));
    CHECK(cfg.delay_time_ratio_right == Approx(1.0f));
    CHECK(cfg.delay_level_center == Approx(1.0f));
    CHECK(cfg.delay_level_left == Approx(0.0f));
    CHECK(cfg.delay_level_right == Approx(0.0f));
    CHECK(cfg.delay_send_to_reverb == Approx(0.0f));
  }

  SECTION("the reset state maps to a hall, a gentle chorus and a 340 ms delay") {
    const GsEffectsConfig cfg = gs_effects_config_from(GsSystemEffects{});
    CHECK(cfg.delay_time_ms == Approx(340.0f).epsilon(1e-4));
    CHECK(cfg.delay_feedback > 0.0f);
    CHECK(cfg.reverb_decay > 0.0f);
    CHECK(cfg.reverb_decay <= 0.98f);
    CHECK(cfg.reverb_damping == Approx(0.4f));  // character 4, Hall 2
    CHECK(cfg.chorus_rate_hz > 0.0f);
    CHECK(cfg.chorus_depth_ms == Approx(6.25f).epsilon(1e-3));
    CHECK(cfg.reverb_pre_lpf_hz == Approx(kGsPreLpfThruHz));
    CHECK(cfg.reverb_character == 4);
    CHECK(cfg.reverb_delay_time_ms == Approx(gs_reverb_delay_time_ms_designed(64)));
    CHECK(cfg.reverb_delay_feedback == Approx(0.0f));
  }

  SECTION("a longer reverb time gives a longer tank decay") {
    GsSystemEffects shorter;
    shorter.reverb_time = 16;
    GsSystemEffects longer;
    longer.reverb_time = 112;
    CHECK(gs_effects_config_from(shorter).reverb_decay <
          gs_effects_config_from(longer).reverb_decay);
  }

  SECTION("character delay controls map without borrowing system delay controls") {
    GsSystemEffects fx;
    fx.reverb_character = 7;
    fx.reverb_time = 12;
    fx.reverb_delay_feedback = 96;
    fx.reverb_predelay = 5;
    fx.delay_time_center = 0x01;
    fx.delay_feedback = 0x7F;
    const GsEffectsConfig cfg = gs_effects_config_from(fx);
    CHECK(cfg.reverb_character == 7);
    CHECK(cfg.reverb_delay_time_ms == Approx(gs_reverb_delay_time_ms_designed(12)));
    CHECK(cfg.reverb_delay_feedback == Approx(gs_reverb_delay_feedback_coefficient(96)));
    CHECK(cfg.reverb_predelay_ms == Approx(5.0f));
    CHECK(cfg.delay_time_ms == Approx(gs_delay_time_ms(0x01)));
    CHECK(cfg.delay_feedback == Approx(gs_delay_feedback_coefficient(0x7F)));
  }

  SECTION("each macro reaches the config") {
    std::vector<float> decays;
    for (int m = 0; m < 8; ++m) {
      GsSystemEffects fx;
      sonare::midi::synth::gs_apply_reverb_macro(fx, static_cast<uint8_t>(m));
      decays.push_back(gs_effects_config_from(fx).reverb_decay);
    }
    for (int m = 0; m < 4; ++m) {
      INFO("reverb macro " << m);
      CHECK(decays[static_cast<size_t>(m)] < decays[static_cast<size_t>(m) + 1]);
    }
  }
}

TEST_CASE("GS delay feedback keeps its sign in the bus and its tail bound", "[midi][synth][gs]") {
  using sonare::midi::synth::GsEffectBus;
  using sonare::midi::synth::GsEffectsConfig;

  constexpr double kSampleRate = 48000.0;
  constexpr int kDelaySamples = 480;                  // 10 ms at 48 kHz.
  constexpr int kSecondEcho = 2 * kDelaySamples + 1;  // feedback state adds one sample per lap.
  constexpr int kRenderSamples = kDelaySamples * 3;
  constexpr int kSettleSamples = 4800;  // 100 ms of silence: ten smoothing time constants.

  auto render_impulse = [&](float feedback, bool via_set_config) {
    GsEffectsConfig initial;
    initial.enable_reverb = false;
    initial.enable_chorus = false;
    initial.enable_delay = true;
    initial.delay_time_ms = 10.0f;
    initial.delay_feedback = via_set_config ? 0.0f : feedback;

    GsEffectBus bus(initial);
    bus.prepare(kSampleRate);

    if (via_set_config) {
      GsEffectsConfig target = initial;
      target.delay_feedback = feedback;
      bus.set_config(target);
    }

    std::array<float, GsEffectBus::kBlockFrames> silence_l{};
    std::array<float, GsEffectBus::kBlockFrames> silence_r{};
    for (int offset = 0; offset < (via_set_config ? kSettleSamples : 0);
         offset += GsEffectBus::kBlockFrames) {
      const int n = std::min(GsEffectBus::kBlockFrames, kSettleSamples - offset);
      bus.begin_chunk();
      bus.render_returns(silence_l.data(), silence_r.data(), n);
    }

    std::vector<float> output(static_cast<size_t>(kRenderSamples), 0.0f);
    std::vector<float> output_r(static_cast<size_t>(kRenderSamples), 0.0f);
    for (int offset = 0; offset < kRenderSamples; offset += GsEffectBus::kBlockFrames) {
      const int n = std::min(GsEffectBus::kBlockFrames, kRenderSamples - offset);
      bus.begin_chunk();
      if (offset == 0) bus.delay_in(0)[0] = 1.0f;
      bus.render_returns(output.data() + offset, output_r.data() + offset, n);
    }

    return std::pair<std::vector<float>, int64_t>{std::move(output), bus.tail_samples(kSampleRate)};
  };

  for (const bool via_set_config : {false, true}) {
    INFO("mode: " << (via_set_config ? "set_config" : "constructor"));
    const auto positive = render_impulse(0.5f, via_set_config);
    const auto negative = render_impulse(-0.5f, via_set_config);
    const auto& positive_output = positive.first;
    const auto& negative_output = negative.first;

    // The first echo does not yet traverse the feedback loop, so both signs
    // must arrive with the same polarity and magnitude.
    CHECK(positive_output[static_cast<size_t>(kDelaySamples)] > 0.0f);
    CHECK(negative_output[static_cast<size_t>(kDelaySamples)] > 0.0f);
    CHECK(std::abs(positive_output[static_cast<size_t>(kDelaySamples)]) ==
          Approx(std::abs(negative_output[static_cast<size_t>(kDelaySamples)])).margin(1.0e-6));

    // The second echo has passed through feedback once, so a negative GS
    // coefficient must invert it while preserving its magnitude.
    CHECK(positive_output[static_cast<size_t>(kSecondEcho)] > 0.0f);
    CHECK(negative_output[static_cast<size_t>(kSecondEcho)] < 0.0f);
    CHECK(std::abs(positive_output[static_cast<size_t>(kSecondEcho)]) ==
          Approx(std::abs(negative_output[static_cast<size_t>(kSecondEcho)])).margin(1.0e-4));

    // Tail duration depends on loop-gain magnitude, not its polarity, and it
    // must include more than the one-echo delay when feedback is nonzero.
    CHECK(positive.second == negative.second);
    CHECK(positive.second > kDelaySamples);
  }

  auto max_after_tail = [](float feedback) {
    constexpr double kLowSampleRate = 1000.0;
    GsEffectsConfig config;
    config.enable_reverb = false;
    config.enable_chorus = false;
    config.enable_delay = true;
    config.delay_time_ms = 10.0f;
    config.delay_feedback = feedback;

    GsEffectBus bus(config);
    bus.prepare(kLowSampleRate);
    const int64_t tail = bus.tail_samples(kLowSampleRate);
    const int samples = static_cast<int>(tail) + 32;
    std::vector<float> output(static_cast<size_t>(samples), 0.0f);
    std::vector<float> output_r(static_cast<size_t>(samples), 0.0f);
    for (int offset = 0; offset < samples; offset += GsEffectBus::kBlockFrames) {
      const int n = std::min(GsEffectBus::kBlockFrames, samples - offset);
      bus.begin_chunk();
      if (offset == 0) bus.delay_in(0)[0] = 1.0f;
      bus.render_returns(output.data() + offset, output_r.data() + offset, n);
    }

    float peak = 0.0f;
    for (int i = static_cast<int>(tail); i < samples; ++i) {
      peak = std::max(peak, std::abs(output[static_cast<size_t>(i)]));
    }
    return std::pair<int64_t, float>{tail, peak};
  };

  for (const float feedback : {0.5f, -0.5f}) {
    INFO("tail feedback: " << feedback);
    const auto [tail, peak] = max_after_tail(feedback);
    CHECK(tail > 10);
    CHECK(peak <= 1.0e-4f);
  }
}

TEST_CASE("GS delay tail remains conservative while feedback is lowered", "[midi][synth][gs]") {
  using sonare::midi::synth::GsEffectBus;
  using sonare::midi::synth::GsEffectsConfig;

  constexpr double kSampleRate = 1000.0;
  constexpr int kDelaySamples = 10;
  constexpr int kImpulseFrames = 64;

  auto tail_peak_after_lowering = [](float feedback) {
    GsEffectsConfig config;
    config.enable_reverb = false;
    config.enable_chorus = false;
    config.enable_delay = true;
    config.delay_time_ms = 10.0f;
    config.delay_feedback = feedback;

    GsEffectBus bus(config);
    bus.prepare(kSampleRate);

    std::array<float, GsEffectBus::kBlockFrames> impulse_l{};
    std::array<float, GsEffectBus::kBlockFrames> impulse_r{};
    bus.begin_chunk();
    bus.delay_in(0)[0] = 1.0f;
    bus.render_returns(impulse_l.data(), impulse_r.data(), kImpulseFrames);

    // Shorten both targets while the old 10 ms delay and high feedback are
    // still in the line; set_config() must leave the bound conservative.
    config.delay_time_ms = 1.0f;
    config.delay_feedback = 0.0f;
    bus.set_config(config);
    const int64_t tail = bus.tail_samples(kSampleRate);
    const int samples = static_cast<int>(tail) + 64;
    std::vector<float> output(static_cast<size_t>(samples), 0.0f);
    std::vector<float> output_r(static_cast<size_t>(samples), 0.0f);
    for (int offset = 0; offset < samples; offset += GsEffectBus::kBlockFrames) {
      const int n = std::min(GsEffectBus::kBlockFrames, samples - offset);
      bus.begin_chunk();
      bus.render_returns(output.data() + offset, output_r.data() + offset, n);
    }

    float peak = 0.0f;
    for (int i = static_cast<int>(tail); i < samples; ++i) {
      peak = std::max(peak, std::abs(output[static_cast<size_t>(i)]));
    }
    return std::pair<int64_t, float>{tail, peak};
  };

  for (const float feedback : {0.9f, -0.9f}) {
    INFO("lowered feedback: " << feedback);
    const auto [tail, peak] = tail_peak_after_lowering(feedback);
    CHECK(tail > kDelaySamples);
    CHECK(peak <= 1.0e-4f);
  }
}

TEST_CASE("GS CHARACTER 6 and 7 use an independent delay return", "[midi][synth][gs]") {
  using sonare::midi::synth::GsEffectBus;
  using sonare::midi::synth::GsEffectsConfig;

  constexpr double kSampleRate = 1000.0;
  constexpr int kDelaySamples = 10;
  constexpr int kFirstEcho = kDelaySamples;
  constexpr int kSecondEcho = 2 * kDelaySamples + 1;
  constexpr int kRenderSamples = 64;

  struct Rendered {
    std::vector<float> left;
    std::vector<float> right;
    int64_t tail = 0;
  };

  auto render = [&](uint8_t character, float feedback, float delay_ms, float predelay_ms,
                    const std::vector<int>& partitions, float impulse_l = 1.0f,
                    float impulse_r = 0.0f) {
    GsEffectsConfig config;
    config.enable_reverb = true;
    config.enable_chorus = false;
    config.enable_delay = false;
    config.reverb_character = character;
    config.reverb_delay_time_ms = delay_ms;
    config.reverb_delay_feedback = feedback;
    config.reverb_predelay_ms = predelay_ms;
    config.reverb_level = 1.0f;

    GsEffectBus bus(config);
    bus.prepare(kSampleRate);
    Rendered output{std::vector<float>(kRenderSamples, 0.0f),
                    std::vector<float>(kRenderSamples, 0.0f), 0};
    int offset = 0;
    for (const int requested : partitions) {
      const int n = std::min(requested, kRenderSamples - offset);
      if (n <= 0) break;
      bus.begin_chunk();
      if (offset == 0) {
        bus.reverb_in(0)[0] = impulse_l;
        bus.reverb_in(1)[0] = impulse_r;
      }
      bus.render_returns(output.left.data() + offset, output.right.data() + offset, n);
      offset += n;
    }
    output.tail = bus.tail_samples(kSampleRate);
    return output;
  };

  SECTION("character 6 is a normal stereo delay and ignores the system delay") {
    const Rendered output = render(6, 0.5f, 10.0f, 0.0f, {kRenderSamples});
    CHECK(output.left[kFirstEcho] > 0.0f);
    CHECK(std::abs(output.right[kFirstEcho]) < 1.0e-6f);
    CHECK(output.left[kSecondEcho] > 0.0f);
    CHECK(output.tail > kDelaySamples);
  }

  SECTION("character 7 alternates the return between left and right") {
    const Rendered output = render(7, 0.5f, 10.0f, 0.0f, {kRenderSamples});
    CHECK(output.left[kFirstEcho] > 0.0f);
    CHECK(std::abs(output.right[kFirstEcho]) < 1.0e-6f);
    CHECK(std::abs(output.left[kSecondEcho]) < 1.0e-6f);
    CHECK(output.right[kSecondEcho] > 0.0f);
  }

  SECTION("character 7 seeds centered and right-only stereo as a mono ping-pong source") {
    const Rendered centered = render(7, 0.5f, 10.0f, 5.0f, {kRenderSamples}, 1.0f, 1.0f);
    CHECK(centered.left[15] > 0.9f);
    CHECK(std::abs(centered.right[15]) < 1.0e-6f);
    CHECK(std::abs(centered.left[26]) < 1.0e-6f);
    CHECK(centered.right[26] > 0.4f);

    const Rendered right_only = render(7, 0.5f, 10.0f, 5.0f, {kRenderSamples}, 0.0f, 1.0f);
    CHECK(right_only.left[15] > 0.4f);  // midpoint seed keeps a right-only send audible.
    CHECK(std::abs(right_only.right[15]) < 1.0e-6f);
    CHECK(std::abs(right_only.left[26]) < 1.0e-6f);
    CHECK(right_only.right[26] > 0.2f);
  }

  SECTION("the reverb pre-delay is part of the independent delay timing") {
    const Rendered output = render(6, 0.0f, 10.0f, 5.0f, {kRenderSamples});
    CHECK(std::abs(output.left[10]) < 1.0e-6f);
    CHECK(output.left[15] > 0.0f);
    CHECK(output.tail >= 15);
  }

  SECTION("pre-delay shifts only the first echo, not the feedback loop") {
    const Rendered output = render(6, 0.5f, 10.0f, 5.0f, {kRenderSamples});
    const Rendered without_predelay = render(6, 0.5f, 10.0f, 0.0f, {kRenderSamples});
    CHECK(output.left[15] > 0.0f);
    CHECK(output.left[26] > 0.0f);  // 15 ms first echo, then 10 ms + one feedback frame.
    CHECK(std::abs(output.left[31]) < 1.0e-6f);
    CHECK(output.tail - without_predelay.tail == 5);
  }

  SECTION("the old unit drains when the character changes") {
    GsEffectsConfig config;
    config.enable_reverb = true;
    config.enable_chorus = false;
    config.enable_delay = false;
    config.reverb_character = 6;
    config.reverb_delay_time_ms = 10.0f;
    config.reverb_delay_feedback = 0.5f;

    GsEffectBus bus(config);
    bus.prepare(kSampleRate);
    std::vector<float> first(12, 0.0f);
    std::vector<float> first_r(12, 0.0f);
    bus.begin_chunk();
    bus.reverb_in(0)[0] = 1.0f;
    bus.render_returns(first.data(), first_r.data(), static_cast<int>(first.size()));

    config.reverb_character = 4;
    bus.set_config(config);
    std::vector<float> drained(32, 0.0f);
    std::vector<float> drained_r(32, 0.0f);
    bus.begin_chunk();
    bus.render_returns(drained.data(), drained_r.data(), static_cast<int>(drained.size()));
    CHECK(drained[9] > 0.0f);  // global sample 21: the previous delay tail
  }

  SECTION("a pending input pre-delay drains after the character changes") {
    GsEffectsConfig config;
    config.enable_reverb = true;
    config.enable_chorus = false;
    config.enable_delay = false;
    config.reverb_character = 6;
    config.reverb_delay_time_ms = 10.0f;
    config.reverb_delay_feedback = 0.0f;
    config.reverb_predelay_ms = 5.0f;

    GsEffectBus bus(config);
    bus.prepare(kSampleRate);
    std::array<float, 2> first{};
    std::array<float, 2> first_r{};
    bus.begin_chunk();
    bus.reverb_in(0)[0] = 1.0f;
    bus.render_returns(first.data(), first_r.data(), static_cast<int>(first.size()));

    config.reverb_character = 4;
    bus.set_config(config);
    std::vector<float> drained(32, 0.0f);
    std::vector<float> drained_r(32, 0.0f);
    bus.begin_chunk();
    bus.render_returns(drained.data(), drained_r.data(), static_cast<int>(drained.size()));
    CHECK(drained[13] > 0.0f);  // global sample 15: input pre-delay 5 + delay 10.
  }

  SECTION("the result is invariant to send block partitions") {
    const Rendered whole = render(7, 0.5f, 10.0f, 0.0f, {kRenderSamples});
    const Rendered split = render(7, 0.5f, 10.0f, 0.0f, {1, 7, 13, 5, 19, 19});
    for (int i = 0; i < kRenderSamples; ++i) {
      INFO("sample " << i);
      CHECK(split.left[static_cast<size_t>(i)] ==
            Approx(whole.left[static_cast<size_t>(i)]).margin(1.0e-6f));
      CHECK(split.right[static_cast<size_t>(i)] ==
            Approx(whole.right[static_cast<size_t>(i)]).margin(1.0e-6f));
    }
  }

  SECTION("prepared character changes do not allocate on the audio path") {
    GsEffectsConfig config;
    config.enable_reverb = true;
    config.enable_chorus = false;
    config.enable_delay = false;
    config.reverb_character = 6;
    config.reverb_delay_time_ms = 10.0f;
    config.reverb_delay_feedback = 0.5f;
    GsEffectBus bus(config);
    bus.prepare(kSampleRate);
    std::array<float, GsEffectBus::kBlockFrames> left{};
    std::array<float, GsEffectBus::kBlockFrames> right{};

    size_t allocations = 0;
    {
      sonare::test::AllocationGuard guard;
      for (int block = 0; block < 4; ++block) {
        bus.begin_chunk();
        if (block == 0) bus.reverb_in(0)[0] = 1.0f;
        bus.render_returns(left.data(), right.data(), 32);
        config.reverb_character = block % 2 == 0 ? 7 : 4;
        bus.set_config(config);
      }
      allocations = guard.count();
    }
    CHECK(allocations == 0);
  }

  SECTION("the host reverb gate owns both character returns") {
    const Rendered output = [&] {
      GsEffectsConfig config;
      config.enable_reverb = false;
      config.enable_chorus = false;
      config.enable_delay = true;
      config.reverb_character = 7;
      config.reverb_delay_time_ms = 10.0f;
      config.reverb_delay_feedback = 0.5f;
      GsEffectBus bus(config);
      bus.prepare(kSampleRate);
      std::vector<float> left(kRenderSamples, 0.0f);
      std::vector<float> right(kRenderSamples, 0.0f);
      bus.begin_chunk();
      bus.reverb_in(0)[0] = 1.0f;
      bus.render_returns(left.data(), right.data(), kRenderSamples);
      return Rendered{std::move(left), std::move(right), bus.tail_samples(kSampleRate)};
    }();
    for (int i = 0; i < kRenderSamples; ++i) {
      CHECK(output.left[static_cast<size_t>(i)] == 0.0f);
      CHECK(output.right[static_cast<size_t>(i)] == 0.0f);
    }
  }
}

TEST_CASE("GS reverb macro transitions preserve each active unit", "[midi][synth][gs]") {
  using sonare::midi::synth::gs_apply_reverb_macro;
  using sonare::midi::synth::gs_effects_config_from;
  using sonare::midi::synth::GsEffectBus;
  using sonare::midi::synth::GsEffectsConfig;
  using sonare::midi::synth::GsSystemEffects;

  constexpr double kSampleRate = 1000.0;
  constexpr int kBlock = 2;
  constexpr int kRender = 64;

  auto macro_config = [](uint8_t macro, float delay_ms, float feedback, float predelay_ms) {
    GsSystemEffects fx;
    gs_apply_reverb_macro(fx, macro);
    GsEffectsConfig config = gs_effects_config_from(fx);
    config.enable_reverb = true;
    config.enable_chorus = false;
    config.enable_delay = false;
    // Keep the mode and GS macro mapping under test, while using short,
    // distinct physical times that make both old echoes fit this fixture.
    config.reverb_delay_time_ms = delay_ms;
    config.reverb_delay_feedback = feedback;
    config.reverb_predelay_ms = predelay_ms;
    config.reverb_level = 1.0f;
    return config;
  };

  auto transition = [&](const GsEffectsConfig& old_config, const GsEffectsConfig& new_config,
                        bool inject_new_input) {
    GsEffectBus bus(old_config);
    bus.prepare(kSampleRate);
    std::vector<float> left(kRender, 0.0f);
    std::vector<float> right(kRender, 0.0f);
    bus.begin_chunk();
    bus.reverb_in(0)[0] = 1.0f;
    bus.render_returns(left.data(), right.data(), kBlock);
    bus.set_config(new_config);
    bus.begin_chunk();
    if (inject_new_input) bus.reverb_in(0)[0] = 1.0f;
    bus.render_returns(left.data() + kBlock, right.data() + kBlock, kRender - kBlock);
    return std::pair<std::vector<float>, std::vector<float>>{std::move(left), std::move(right)};
  };

  SECTION("delay to normal keeps the old first and feedback echoes") {
    const GsEffectsConfig old_config = macro_config(6, 10.0f, 0.5f, 5.0f);
    const GsEffectsConfig new_config = macro_config(4, 40.0f, 0.0f, 0.0f);
    const auto output = transition(old_config, new_config, false);
    CHECK(output.first[15] > 0.9f);  // old pre-delay + TIME.
    CHECK(output.first[26] > 0.4f);  // old TIME + old feedback.
  }

  SECTION("normal delay to panning delay keeps the old line and arms the new one") {
    const GsEffectsConfig old_config = macro_config(6, 10.0f, 0.5f, 5.0f);
    const GsEffectsConfig new_config = macro_config(7, 20.0f, 0.0f, 0.0f);
    const auto output = transition(old_config, new_config, true);
    CHECK(output.first[15] > 0.9f);  // old CHARACTER 6 first echo.
    CHECK(output.first[26] > 0.4f);  // old CHARACTER 6 feedback echo.
    CHECK(output.first[20] > 0.1f);  // new CHARACTER 7 input at its new TIME.
    CHECK(std::abs(output.second[20]) < 1.0e-6f);
  }

  SECTION("normal tank coefficients survive a switch to either delay character") {
    GsEffectsConfig old_config = macro_config(4, 10.0f, 0.0f, 0.0f);
    old_config.reverb_damping = 0.4f;
    old_config.reverb_decay = 0.7f;

    for (const uint8_t macro : {uint8_t{6}, uint8_t{7}}) {
      GsEffectsConfig new_config = macro_config(macro, 10.0f, 0.0f, 0.0f);
      GsEffectBus switched(old_config);
      GsEffectBus reference(old_config);
      switched.prepare(kSampleRate);
      reference.prepare(kSampleRate);

      constexpr int kFirstBlock = GsEffectBus::kBlockFrames;
      constexpr int kAfterSwitch = 2048;
      std::vector<float> switched_l(kAfterSwitch, 0.0f);
      std::vector<float> switched_r(kAfterSwitch, 0.0f);
      std::vector<float> reference_l(kAfterSwitch, 0.0f);
      std::vector<float> reference_r(kAfterSwitch, 0.0f);
      switched.begin_chunk();
      reference.begin_chunk();
      switched.reverb_in(0)[0] = 1.0f;
      reference.reverb_in(0)[0] = 1.0f;
      switched.render_returns(switched_l.data(), switched_r.data(), kFirstBlock);
      reference.render_returns(reference_l.data(), reference_r.data(), kFirstBlock);
      switched.set_config(new_config);
      for (int offset = kFirstBlock; offset < kAfterSwitch; offset += GsEffectBus::kBlockFrames) {
        const int n = std::min(GsEffectBus::kBlockFrames, kAfterSwitch - offset);
        switched.begin_chunk();
        reference.begin_chunk();
        switched.render_returns(switched_l.data() + offset, switched_r.data() + offset, n);
        reference.render_returns(reference_l.data() + offset, reference_r.data() + offset, n);
      }
      double wet_energy = 0.0;
      double max_difference = 0.0;
      for (int i = kFirstBlock; i < kAfterSwitch; ++i) {
        wet_energy += std::abs(reference_l[static_cast<size_t>(i)]);
        wet_energy += std::abs(reference_r[static_cast<size_t>(i)]);
        max_difference = std::max(
            max_difference, static_cast<double>(std::abs(switched_l[static_cast<size_t>(i)] -
                                                         reference_l[static_cast<size_t>(i)])));
        max_difference = std::max(
            max_difference, static_cast<double>(std::abs(switched_r[static_cast<size_t>(i)] -
                                                         reference_r[static_cast<size_t>(i)])));
      }
      INFO("macro " << static_cast<int>(macro));
      CHECK(max_difference < 1.0e-6);
      CHECK(wet_energy > 1.0e-5);
    }
  }
}

TEST_CASE("GS normal reverb preserves the previous PCM path", "[midi][synth][gs]") {
  using sonare::effects::reverb::DattorroReverb;
  using sonare::effects::reverb::DattorroReverbConfig;
  using sonare::midi::synth::gs_apply_reverb_macro;
  using sonare::midi::synth::gs_effects_config_from;
  using sonare::midi::synth::GsEffectBus;
  using sonare::midi::synth::GsEffectsConfig;
  using sonare::midi::synth::GsSystemEffects;

  constexpr double kSampleRate = 48000.0;
  constexpr int kSamples = 8192;
  for (int character = 0; character <= 5; ++character) {
    GsSystemEffects fx;
    gs_apply_reverb_macro(fx, static_cast<uint8_t>(character));
    GsEffectsConfig config = gs_effects_config_from(fx);
    config.enable_reverb = true;
    config.enable_chorus = false;
    config.enable_delay = false;
    config.reverb_level = 1.0f;

    GsEffectBus bus(config);
    bus.prepare(kSampleRate);

    DattorroReverbConfig baseline_config;
    baseline_config.decay = config.reverb_decay;
    baseline_config.damping = config.reverb_damping;
    baseline_config.dry_wet = 1.0f;
    DattorroReverb baseline(baseline_config);
    baseline.prepare(kSampleRate, GsEffectBus::kBlockFrames);

    std::vector<float> actual_l(kSamples, 0.0f);
    std::vector<float> actual_r(kSamples, 0.0f);
    std::vector<float> expected_l(kSamples, 0.0f);
    std::vector<float> expected_r(kSamples, 0.0f);
    for (int offset = 0; offset < kSamples; offset += GsEffectBus::kBlockFrames) {
      const int n = std::min(GsEffectBus::kBlockFrames, kSamples - offset);
      bus.begin_chunk();
      if (offset == 0) bus.reverb_in(0)[0] = 1.0f;
      bus.render_returns(actual_l.data() + offset, actual_r.data() + offset, n);

      std::array<float, GsEffectBus::kBlockFrames> baseline_l{};
      std::array<float, GsEffectBus::kBlockFrames> baseline_r{};
      if (offset == 0) baseline_l[0] = 1.0f;
      float* channels[2] = {baseline_l.data(), baseline_r.data()};
      baseline.process(channels, 2, n);
      std::copy_n(baseline_l.data(), n, expected_l.data() + offset);
      std::copy_n(baseline_r.data(), n, expected_r.data() + offset);
    }

    INFO("character " << character);
    for (int i = 0; i < kSamples; ++i) {
      INFO("sample " << i);
      CHECK(actual_l[static_cast<size_t>(i)] == expected_l[static_cast<size_t>(i)]);
      CHECK(actual_r[static_cast<size_t>(i)] == expected_r[static_cast<size_t>(i)]);
    }
    double expected_wet_energy = 0.0;
    for (int i = 0; i < kSamples; ++i) {
      expected_wet_energy += std::abs(expected_l[static_cast<size_t>(i)]);
      expected_wet_energy += std::abs(expected_r[static_cast<size_t>(i)]);
    }
    CHECK(expected_wet_energy > 1.0e-6);
  }
}

#endif  // SONARE_MIDI_WITH_FX
