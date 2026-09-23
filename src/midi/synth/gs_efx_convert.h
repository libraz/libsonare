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
/// one state and every other byte the other; which printed label names which
/// state is not measurable, so the byte is read and the label is not.
float gs_efx_corner_hz(uint8_t value, GsShelfSide side) noexcept;

/// A byte read through the printed endpoints of a slot no table was measured
/// for. Nothing here is fitted: the two endpoints admit exactly one step or
/// none, and where they admit none this returns false and writes nothing,
/// because a rounded conversion is indistinguishable downstream from a
/// measured one. Bytes outside the range clamp to their nearer endpoint.
bool gs_efx_ratio(uint8_t value, int lo_byte, int hi_byte, int lo_unit, int hi_unit,
                  float* out) noexcept;

/// A byte selecting one of @p count printed states. Past the list it returns
/// the first, which is what the measured small tables were read doing.
int gs_efx_enum_index(uint8_t value, int count) noexcept;

}  // namespace sonare::midi::synth
