#pragma once

/// @file gs_efx_convert.h
/// @brief GS insert-effect (EFX) byte-to-physical-unit conversions.
///
/// A value layer of pure functions: no allocation, no dependency on
/// gs_layer.h or the insert factory, so a conversion has a unit test that
/// needs neither. Each function takes the raw wire byte and, where the
/// archive's measurement records more than one table for the same byte
/// range, an enum selecting which table applies to that (type, slot).
///
/// The tables backing these conversions are measured, not derived from the
/// manual's printed curves — see gs_efx_tables.h for the generated data and
/// docs/gs.md for what "measured" means for this address space.

#include <array>
#include <cstddef>
#include <cstdint>

namespace sonare::midi::synth {

/// RATE byte range: narrow (LFO-rate parameters) or wide (rotary-speaker
/// rate). Two distinct printed ranges share one byte domain.
enum class GsRateRange { kNarrow, kWide };

/// TIME byte range: which of the five printed ladders a slot's archive record
/// selects. Ladders are 32000 Hz integer-sample rungs, cut to a whole sample of
/// the unit's own clock and returned as a physical millisecond value.
enum class GsTimeLadder { kLadder0, kLadder1, kLadder2, kLadder3, kLadder4 };

/// FREQ byte range: which of the three printed 1/3-octave columns a slot's
/// archive record selects.
enum class GsFreqColumn { kColumn0, kColumn1, kColumn2 };

/// Which of the equaliser's two shelves a corner byte sets.
enum class GsShelfSide { kLow, kHigh };

/// The five EFX modulation waveforms the archive's `wave` table enumerates.
enum class GsEfxWave { kSine, kTriangle, kSquare, kSawUp, kSawDown };

/// RATE byte -> LFO frequency in Hz, from the narrow or wide printed table.
float gs_efx_rate_hz(uint8_t value, GsRateRange range) noexcept;

/// TIME byte -> delay time in milliseconds, from one of the five ladders, each
/// entry cut back to a whole sample of the unit's own clock.
float gs_efx_delay_ms(uint8_t value, GsTimeLadder ladder) noexcept;

/// FREQ byte -> corner/centre frequency in Hz, from one of the three columns.
/// The byte's top four bits index the column, so eight settings share an entry.
/// Returns 0 Hz where a column's entry is the printed bypass.
float gs_efx_freq_hz(uint8_t value, GsFreqColumn column) noexcept;

/// GAIN byte -> dB, over the archive's measured window; clamps outside it.
float gs_efx_gain_db(uint8_t value) noexcept;

/// LEVEL byte -> linear multiplier, from the measured numerator table.
float gs_efx_level_mul(uint8_t value) noexcept;

/// WIDTH byte -> a filter Q, from the five measured entries. Settings past the
/// fifth return the first, which is what the archive measured them doing.
float gs_efx_width_q(uint8_t value) noexcept;

/// WAVE byte -> which of the five modulation shapes it selects. Settings past
/// the fifth return the first; the unit itself keeps the state it was already
/// in, which needs the caller's prior byte and so is not a conversion.
GsEfxWave gs_efx_wave(uint8_t value) noexcept;

/// PAN byte -> per-channel linear gain.
void gs_efx_pan(uint8_t value, float* left, float* right) noexcept;

/// BALANCE byte -> the direct/effect split, as two independent linear gains
/// (not a complementary pair -- the archive's two-ramp reflection means they
/// do not sum to a constant).
void gs_efx_balance(uint8_t value, float* direct, float* effect) noexcept;

/// PAN byte -> a balance position in [-1, 1]: the position whose constant-power
/// pair has the measured left/right ratio. A receiver applies its own pan law.
float gs_efx_pan_position(uint8_t value) noexcept;

/// BALANCE byte -> the effect's share effect / (direct + effect) of the measured
/// pair, the fraction a dry/wet control takes.
float gs_efx_balance_fraction(uint8_t value) noexcept;

/// AZIMUTH byte -> one of the 31 measured stereo positions, in degrees.
int gs_efx_azimuth_deg(uint8_t value) noexcept;

/// The upper 4 bits of an ACCEL byte -> a time constant in seconds. Held as a
/// physical quantity, never as a per-sample coefficient.
float gs_efx_accel_tau_s(uint8_t value) noexcept;

/// The upper 4 bits of an ACCEL byte -> the distance in Hz a rotor stops short
/// of the rate it was sent up to. Sent down it arrives exactly, so this applies
/// to the climb only. It reads the same divisor table as the time constant
/// above and takes its step rate from the same place, because the two are one
/// measurement: the loop's step rate over the divisor, against it times it.
float gs_efx_accel_undershoot_hz(uint8_t value) noexcept;

/// POST GAIN byte -> dB, in fixed steps. Settings past the fourth return the
/// first, the convention the measured small tables set.
float gs_efx_post_gain_db(uint8_t value) noexcept;

/// SHIFT MODE byte -> the splice window in milliseconds: how far the read-out
/// drifts between splices. Settings past the fifth return the first.
float gs_efx_window_ms(uint8_t value) noexcept;

/// CORNER byte -> a shelf's corner in Hz, at its half-gain point. Byte 0 selects
/// one state and byte 1 the other; which printed label names which state is not
/// measurable, so the byte is read and the label is not.
float gs_efx_corner_hz(uint8_t value, GsShelfSide side) noexcept;

/// A byte read through the printed endpoints of a slot no table was measured
/// for. Nothing here is fitted: the two endpoints admit exactly one step or
/// none, and where they admit none this returns false and writes nothing,
/// because a rounded conversion is indistinguishable downstream from a
/// measured one. Bytes outside the range clamp to their nearer endpoint.
bool gs_efx_ratio(uint8_t value, int lo_byte, int hi_byte, int lo_unit, int hi_unit,
                  float* out) noexcept;

/// A byte selecting one of @p count printed states. Past the list it returns
/// the first. On a slot printed as a list of states such a byte is never
/// taken (gs_efx_parameter_takes), so this is the answer only where no write
/// rule stands in front of it.
int gs_efx_enum_index(uint8_t value, int count) noexcept;

/// @name Row vocabulary
/// The generated binding rows carry these as plain integers. Measured classes
/// 0-13 are numbered by gs_efx_tables.h; the two below continue that run.
/// @{
inline constexpr uint8_t kGsEfxRowTranslated = 0;  ///< Row reads a measured table or ratio.
inline constexpr uint8_t kGsEfxRowDesigned = 1;    ///< Row reads a carried class or a designed law.
inline constexpr uint8_t kGsEfxRowClassRatio = 14;  ///< Printed endpoints, one unit step per byte.
inline constexpr uint8_t kGsEfxRowClassDrive =
    15;  ///< gs_efx_drive_db (0), gs_efx_drive_pedal_db (1).

inline constexpr uint8_t kGsEfxFormNone = 0;  ///< No designed law; the row reads a class.
inline constexpr uint8_t kGsEfxFormLinear = 1;
inline constexpr uint8_t kGsEfxFormLog = 2;
inline constexpr uint8_t kGsEfxFormDb = 3;
inline constexpr uint8_t kGsEfxFormBipolar = 4;
inline constexpr uint8_t kGsEfxFormEnum = 5;

inline constexpr uint8_t kGsEfxEnableStages = 0;  ///< Stages on at the bytes in on_mask.
inline constexpr uint8_t kGsEfxEnableSelect = 1;  ///< The byte's state picks one stage.
/// The most stages one enable row names; a select names at most four of them.
inline constexpr std::size_t kGsEfxEnableMaxStages = 8;
/// @}

/// A designed law, held by value in the row that uses it. @p form is one of the
/// kGsEfxForm* values; @p lo and @p hi are the output endpoints (dB for db, the
/// bipolar magnitude is @p hi); @p n_states is the state count of an enum law.
struct GsEfxDesignedLaw {
  uint8_t form;
  float lo, hi;
  uint8_t n_states;
};

/// Which quantity an accel-class row reads out of the shared divisor table.
/// Both time-constant outputs read the same table as a time: the rotary's
/// accelTauS and decelTauS keys are both time constants, and only the
/// drum/horn undershoot keys (suffix Hz) are frequencies.
enum class GsEfxOut : uint8_t { kValue, kAccelTau, kDecelTau, kUndershootHz };

/// One binding row: a wire (type, slot) and how its byte becomes a control value.
struct GsEfxBindingRow {
  uint16_t type;
  uint8_t slot;
  uint8_t kind;               ///< kGsEfxRowTranslated / kGsEfxRowDesigned.
  uint8_t conv_class, table;  ///< Measured or carried class, and which of its tables.
  GsEfxDesignedLaw law;       ///< Invented rows only (form != none).
  uint8_t byte_lo, byte_hi;   ///< Printed byte range: a designed law's domain, a ratio's endpoints.
  int16_t unit_lo, unit_hi;   ///< A ratio row's printed unit endpoints.
  GsEfxOut out;
  uint16_t stage;
  uint8_t ordinal;
  uint16_t key;          ///< Indices into the generated name tables.
  uint8_t printed_mark;  ///< 0, '+' or '#'.
};

/// A switch or selector row: which stages a byte turns on.
struct GsEfxEnable {
  uint16_t type;
  uint8_t slot;
  uint8_t mode;  ///< kGsEfxEnableStages / kGsEfxEnableSelect.
  uint16_t stages[kGsEfxEnableMaxStages];
  uint8_t ordinals[kGsEfxEnableMaxStages];
  uint8_t n_stages;
  uint32_t on_mask[4];  ///< Bit b of the 128-bit mask: byte b turns the stages on.
};

/// One stage index per stage an enable row names; 0xFF where none is mapped.
using GsEfxEnableStageIndices = std::array<uint8_t, kGsEfxEnableMaxStages>;

/// Every index unmapped.
inline constexpr GsEfxEnableStageIndices kGsEfxUnmappedStageIndices = [] {
  GsEfxEnableStageIndices out{};
  for (std::size_t i = 0; i < out.size(); ++i) out[i] = 0xFF;
  return out;
}();

/// The rows and enables one lookup runs over. No std::span in C++17.
struct GsEfxRowView {
  const GsEfxBindingRow* rows;
  std::size_t n_rows;
  const GsEfxEnable* enables;
  std::size_t n_enables;
};

/// DRIVE byte -> gain in dB in front of a fixed curve: 20 log10(v / 48), with
/// bytes 0 and 2 measured as one state.
float gs_efx_drive_db(uint8_t value) noexcept;

/// DRIVE byte -> a pedal's clip-path gain in dB: the gs_efx_drive_db curve with
/// its origin at byte 0, so the lowest byte is unity gain and 7F about +36 dB.
float gs_efx_drive_pedal_db(uint8_t value) noexcept;

/// A byte read through a designed law over the printed domain [byte_lo, byte_hi].
/// Equals law.lo at byte_lo and law.hi at byte_hi and is monotone between; bytes
/// outside the domain clamp. db returns the linear multiplier of the dB line,
/// bipolar runs -hi..+hi through the domain centre, enum returns the state index
/// clamped to n_states - 1. A linear or log law with n_states > 1 is stepped: the byte is
/// a state index, state i sits at i / (n_states - 1) between lo and hi.
float gs_efx_designed_value(const GsEfxDesignedLaw& law, uint8_t byte, uint8_t byte_lo,
                            uint8_t byte_hi) noexcept;

/// The control value a row gives a byte: the one place class, table, law and
/// out are dispatched. A row no generator emits (unknown class, table past its
/// class) asserts.
float gs_efx_binding_value(const GsEfxBindingRow& row, uint8_t byte) noexcept;

/// Whether an enable row turns the rule's @p stage_index_in_rule-th stage on at
/// @p byte. In select mode a byte past the stage count reads as state 0.
bool gs_efx_enable_on(const GsEfxEnable& enable, uint8_t byte,
                      uint8_t stage_index_in_rule) noexcept;

}  // namespace sonare::midi::synth
