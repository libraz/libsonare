#pragma once

/// @file gs_system_effects.h
/// @brief The GS system-effect block at `40 01 30`-`5A`: reverb, chorus and
///        delay as they arrive on the wire, their power-on defaults, and the
///        conversions from a wire byte to a physical unit.
///
/// A value layer — POD state and pure functions, no allocation and no FX
/// dependency, so it compiles with BUILD_FX=OFF: a parameter with no
/// counterpart still has to arrive (docs/gs.md). The bridge onto the effect
/// units is gs_effects.h, behind SONARE_MIDI_WITH_FX.
///
/// Everything here is total and clamps to its domain; refusing an out-of-range
/// byte belongs to the decode layer, which checks the address table's per-row
/// lo/hi first. A macro is a one-shot write of the whole parameter block rather
/// than a mode, so an individual parameter arriving after it wins.
///
/// Ranges, defaults and the DELAY TIME CENTER table are the Roland SC-8850
/// Owner's Manual, "Parameter Address Map". Where it gives a range but no
/// curve the conversion is a documented approximation chosen to match the
/// documented end-to-end range; tests assert direction and monotonicity, not
/// absolute Roland-hardware values.

#include <array>
#include <cstddef>
#include <cstdint>

namespace sonare::midi::synth {

/// The system-effect block, one entry per address: X(field, power-on default).
/// The address and range of each field are in its comment; the address table in
/// gs_address_table.h is what a decoder walks.
#define SONARE_GS_SYSTEM_EFFECT_FIELDS(X)                                               \
  X(reverb_macro, 4)              /* 40 01 30, 0-7. Hall 2 — the default is NOT 0. */ \
  X(reverb_character, 4)          /* 40 01 31, 0-7. Same numbering as the macro. */     \
  X(reverb_pre_lpf, 0)            /* 40 01 32, 0-7. 0 = THRU. */                        \
  X(reverb_level, 64)             /* 40 01 33, 0-127. Return gain. */                   \
  X(reverb_time, 64)              /* 40 01 34, 0-127. */                                \
  X(reverb_delay_feedback, 0)     /* 40 01 35, 0-127. Characters 6/7 only. */           \
  X(reverb_predelay, 0)           /* 40 01 37, 0-127 ms. */                             \
  X(chorus_macro, 2)              /* 40 01 38, 0-7. Chorus 3. */                        \
  X(chorus_pre_lpf, 0)            /* 40 01 39, 0-7. */                                  \
  X(chorus_level, 64)             /* 40 01 3A, 0-127. Return gain. */                   \
  X(chorus_feedback, 8)           /* 40 01 3B, 0-127. */                                \
  X(chorus_delay, 80)             /* 40 01 3C, 0-127. Centre delay. */                  \
  X(chorus_rate, 3)               /* 40 01 3D, 0-127. LFO frequency. */                 \
  X(chorus_depth, 19)             /* 40 01 3E, 0-127. LFO depth. */                     \
  X(chorus_send_to_reverb, 0)     /* 40 01 3F, 0-127. */                                \
  X(chorus_send_to_delay, 0)      /* 40 01 40, 0-127. */                                \
  X(delay_macro, 0)               /* 40 01 50, 0-9. Delay 1. */                         \
  X(delay_pre_lpf, 0)             /* 40 01 51, 0-7. */                                  \
  X(delay_time_center, 0x61)      /* 40 01 52, 01-73. 340 ms. Non-linear. */            \
  X(delay_time_ratio_left, 0x01)  /* 40 01 53, 01-78. Ratio of the centre time. */      \
  X(delay_time_ratio_right, 0x01) /* 40 01 54, 01-78. */                                \
  X(delay_level_center, 0x7F)     /* 40 01 55, 0-127. Centre tap. */                    \
  X(delay_level_left, 0)          /* 40 01 56, 0-127. Left tap. */                      \
  X(delay_level_right, 0)         /* 40 01 57, 0-127. Right tap. */                     \
  X(delay_level, 0x40)            /* 40 01 58, 0-127. Return gain. */                   \
  X(delay_feedback, 0x50)         /* 40 01 59, 00-7F = -64..+63. +16, NOT centre. */    \
  X(delay_send_to_reverb, 0)      /* 40 01 5A, 0-127. */

/// Raw wire state of the system-effect block, at the GS power-on defaults.
struct GsSystemEffects {
#define SONARE_GS_DECLARE_FIELD(name, value) uint8_t name = value;
  SONARE_GS_SYSTEM_EFFECT_FIELDS(SONARE_GS_DECLARE_FIELD)
#undef SONARE_GS_DECLARE_FIELD
};

/// Number of addresses the block holds.
#define SONARE_GS_COUNT_FIELD(name, value) +1
inline constexpr size_t kGsSystemEffectFieldCount =
    0 SONARE_GS_SYSTEM_EFFECT_FIELDS(SONARE_GS_COUNT_FIELD);
#undef SONARE_GS_COUNT_FIELD

// Every field is one byte, so the size is the field count: a field added
// outside SONARE_GS_SYSTEM_EFFECT_FIELDS — and therefore without a documented
// default — fails here rather than silently defaulting to zero.
static_assert(sizeof(GsSystemEffects) == kGsSystemEffectFieldCount,
              "GsSystemEffects fields must come from SONARE_GS_SYSTEM_EFFECT_FIELDS");

// ---------------------------------------------------------------------------
// Value conversions. Byte in, physical quantity out; each clamps its domain.
// ---------------------------------------------------------------------------

/// A DELAY TIME CENTER segment endpoint. The manual gives nine segments as
/// shared endpoints (`01`-`14`, `14`-`23`, …); a boundary value is the end of
/// the lower segment and the start of the upper, and both readings give the
/// same time, so the table holds the ten breakpoints and a lookup takes the
/// first segment whose end covers the value.
struct GsDelayTimeBreakpoint {
  uint8_t value;
  float ms;
};

inline constexpr std::array<GsDelayTimeBreakpoint, 10> kGsDelayTimeBreakpoints{{
    {0x01, 0.1f},
    {0x14, 2.0f},
    {0x23, 5.0f},
    {0x2D, 10.0f},
    {0x37, 20.0f},
    {0x46, 50.0f},
    {0x50, 100.0f},
    {0x5A, 200.0f},
    {0x69, 500.0f},
    {0x73, 1000.0f},
}};

/// DELAY TIME CENTER `01`-`73` → milliseconds, linear inside a segment.
float gs_delay_time_ms(uint8_t value) noexcept;

/// DELAY TIME RATIO LEFT/RIGHT `01`-`78` → percent of the centre time.
float gs_delay_time_ratio_percent(uint8_t value) noexcept;

/// DELAY FEEDBACK `00`-`7F` → -64..+63. Negative inverts the polarity.
int gs_delay_feedback_signed(uint8_t value) noexcept;

/// DELAY FEEDBACK as a signed feedback coefficient, |g| <= 0.9.
float gs_delay_feedback_coefficient(uint8_t value) noexcept;

/// REVERB TIME 0-127 → RT60 seconds.
float gs_reverb_time_seconds(uint8_t value) noexcept;

/// REVERB PREDELAY TIME 0-127 → milliseconds (the manual's unit is ms).
float gs_reverb_predelay_ms(uint8_t value) noexcept;

/// REVERB DELAY FEEDBACK 0-127 → an unsigned feedback coefficient, [0, 0.95].
float gs_reverb_delay_feedback_coefficient(uint8_t value) noexcept;

/// CHORUS RATE 0-127 → LFO frequency in Hz.
float gs_chorus_rate_hz(uint8_t value) noexcept;

/// CHORUS DEPTH 0-127 → LFO depth in milliseconds.
float gs_chorus_depth_ms(uint8_t value) noexcept;

/// CHORUS DELAY 0-127 → centre delay in milliseconds.
float gs_chorus_delay_ms(uint8_t value) noexcept;

/// CHORUS FEEDBACK 0-127 → a feedback coefficient, [0, 0.95].
float gs_chorus_feedback_coefficient(uint8_t value) noexcept;

/// A LEVEL / SEND byte 0-127 → linear gain, [0, 1].
float gs_effect_level(uint8_t value) noexcept;

/// A RETURN level byte 0-127 -> linear gain with the power-on 64 at unity, so
/// full scale is about +6 dB.
///
/// A send is a fraction of a part and 127 is all of it, but a return has no
/// such anchor: the manual gives 0-127 and no unit, and the reset value is 64.
/// Reading 127 as unity would make the state a file never writes sit 6 dB below
/// the bus this synth was voiced and fitted at, which is a claim about our own
/// nominal that no source makes. Anchoring on the reset value instead leaves an
/// untouched file exactly where it was and still spends the whole range.
float gs_return_level(uint8_t value) noexcept;

/// Cutoff a PRE-LPF of 0 (THRU) reports: above the audio band, so a filter set
/// to it is transparent and callers need no special case.
inline constexpr float kGsPreLpfThruHz = 20000.0f;

/// PRE-LPF 0-7 → cutoff in Hz, monotonically decreasing from THRU.
float gs_pre_lpf_cutoff_hz(uint8_t value) noexcept;

// ---------------------------------------------------------------------------
// Macros. Each is a one-shot write of the individual parameters it covers.
// ---------------------------------------------------------------------------

/// The six reverb parameters a REVERB MACRO writes (`40 01 31`-`37`; `36` has
/// no row in the map).
struct GsReverbMacroParams {
  uint8_t character;
  uint8_t pre_lpf;
  uint8_t level;
  uint8_t time;
  uint8_t delay_feedback;
  uint8_t predelay;
};

/// The eight chorus parameters a CHORUS MACRO writes (`40 01 39`-`40`).
struct GsChorusMacroParams {
  uint8_t pre_lpf;
  uint8_t level;
  uint8_t feedback;
  uint8_t delay;
  uint8_t rate;
  uint8_t depth;
  uint8_t send_to_reverb;
  uint8_t send_to_delay;
};

/// The ten delay parameters a DELAY MACRO writes (`40 01 51`-`5A`).
struct GsDelayMacroParams {
  uint8_t pre_lpf;
  uint8_t time_center;
  uint8_t time_ratio_left;
  uint8_t time_ratio_right;
  uint8_t level_center;
  uint8_t level_left;
  uint8_t level_right;
  uint8_t level;
  uint8_t feedback;
  uint8_t send_to_reverb;
};

/// Room 1 / Room 2 / Room 3 / Hall 1 / Hall 2 / Plate / Delay / Panning Delay.
/// Decay follows the hardware's order (Room 1 < … < Hall 2); entry 4 is the
/// power-on state, so the reset defaults and "Hall 2 selected" agree.
inline constexpr std::array<GsReverbMacroParams, 8> kGsReverbMacros{{
    {0, 0, 64, 32, 0, 0},   // Room 1
    {1, 0, 64, 40, 0, 0},   // Room 2
    {2, 0, 64, 48, 0, 4},   // Room 3
    {3, 0, 64, 56, 0, 16},  // Hall 1
    {4, 0, 64, 64, 0, 0},   // Hall 2 — the power-on defaults
    {5, 0, 64, 52, 0, 4},   // Plate
    {6, 0, 64, 64, 48, 0},  // Delay
    {7, 0, 64, 64, 48, 0},  // Panning Delay
}};

/// What a REVERB CHARACTER selects beyond the addressed parameters. Characters
/// 6-7 sound the delay unit instead of the reverb; GS has no damping address,
/// so the character carries it.
struct GsReverbCharacterInfo {
  bool delay_type;
  float damping;
};

inline constexpr std::array<GsReverbCharacterInfo, 8> kGsReverbCharacters{{
    {false, 0.75f},  // Room 1
    {false, 0.70f},  // Room 2
    {false, 0.60f},  // Room 3
    {false, 0.45f},  // Hall 1
    {false, 0.40f},  // Hall 2
    {false, 0.35f},  // Plate
    {true, 0.0f},    // Delay
    {true, 0.0f},    // Panning Delay
}};

/// Chorus 1-4 / Feedback Chorus / Flanger / Short Delay / Short Delay (FB).
/// Feedback rises monotonically across Chorus 1-4; entry 2 is the power-on
/// state.
inline constexpr std::array<GsChorusMacroParams, 8> kGsChorusMacros{{
    {0, 64, 0, 112, 3, 5, 0, 0},    // Chorus 1
    {0, 64, 5, 80, 9, 19, 0, 0},    // Chorus 2
    {0, 64, 8, 80, 3, 19, 0, 0},    // Chorus 3 — the power-on defaults
    {0, 64, 16, 64, 9, 16, 0, 0},   // Chorus 4
    {0, 64, 64, 127, 2, 24, 0, 0},  // Feedback Chorus
    {0, 64, 112, 8, 1, 5, 0, 0},    // Flanger
    {0, 64, 0, 127, 0, 0, 0, 0},    // Short Delay — LFO stopped
    {0, 64, 96, 127, 0, 0, 0, 0},   // Short Delay (FB)
}};

/// Delay 1-4 / Pan Delay 1-4 / Delay to Reverb / Pan Repeat. Entry 0 is the
/// power-on state; entry 8 is the only macro that writes DELAY SEND TO REVERB.
inline constexpr std::array<GsDelayMacroParams, 10> kGsDelayMacros{{
    {0, 0x61, 0x01, 0x01, 0x7F, 0x00, 0x00, 0x40, 0x50, 0x00},  // Delay 1 — power-on
    {0, 0x69, 0x01, 0x01, 0x7F, 0x00, 0x00, 0x40, 0x50, 0x00},  // Delay 2
    {0, 0x6E, 0x01, 0x01, 0x7F, 0x00, 0x00, 0x40, 0x50, 0x00},  // Delay 3
    {0, 0x73, 0x01, 0x01, 0x7F, 0x00, 0x00, 0x40, 0x50, 0x00},  // Delay 4
    {0, 0x61, 0x18, 0x0C, 0x00, 0x7F, 0x7F, 0x40, 0x50, 0x00},  // Pan Delay 1
    {0, 0x69, 0x18, 0x0C, 0x00, 0x7F, 0x7F, 0x40, 0x50, 0x00},  // Pan Delay 2
    {0, 0x6E, 0x18, 0x0C, 0x00, 0x7F, 0x7F, 0x40, 0x50, 0x00},  // Pan Delay 3
    {0, 0x73, 0x18, 0x0C, 0x00, 0x7F, 0x7F, 0x40, 0x50, 0x00},  // Pan Delay 4
    {0, 0x61, 0x01, 0x01, 0x7F, 0x00, 0x00, 0x40, 0x50, 0x7F},  // Delay to Reverb
    {0, 0x61, 0x18, 0x0C, 0x00, 0x7F, 0x7F, 0x40, 0x68, 0x00},  // Pan Repeat
}};

/// The parameter block a macro index loads, clamped to the table.
GsReverbMacroParams gs_reverb_macro_params(uint8_t macro) noexcept;
GsChorusMacroParams gs_chorus_macro_params(uint8_t macro) noexcept;
GsDelayMacroParams gs_delay_macro_params(uint8_t macro) noexcept;

/// Select a macro: store the index and overwrite the parameters it covers.
void gs_apply_reverb_macro(GsSystemEffects& fx, uint8_t macro) noexcept;
void gs_apply_chorus_macro(GsSystemEffects& fx, uint8_t macro) noexcept;
void gs_apply_delay_macro(GsSystemEffects& fx, uint8_t macro) noexcept;

/// Whether a REVERB CHARACTER sounds the delay unit rather than the reverb.
bool gs_reverb_character_is_delay(uint8_t character) noexcept;

/// Tank damping a REVERB CHARACTER carries, [0, 1].
float gs_reverb_character_damping(uint8_t character) noexcept;

}  // namespace sonare::midi::synth
