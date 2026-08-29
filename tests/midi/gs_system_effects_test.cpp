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

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#if defined(SONARE_MIDI_WITH_FX)
#include "midi/synth/gs_effects.h"
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
    CHECK(gs_reverb_time_seconds(127) == Approx(12.0).epsilon(1e-4));
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
  using sonare::midi::synth::gs_effects_config_from;
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
  }

  SECTION("a longer reverb time gives a longer tank decay") {
    GsSystemEffects shorter;
    shorter.reverb_time = 16;
    GsSystemEffects longer;
    longer.reverb_time = 112;
    CHECK(gs_effects_config_from(shorter).reverb_decay <
          gs_effects_config_from(longer).reverb_decay);
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

#endif  // SONARE_MIDI_WITH_FX
