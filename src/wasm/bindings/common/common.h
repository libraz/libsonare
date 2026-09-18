#pragma once

#ifdef __EMSCRIPTEN__

#include <emscripten/bind.h>
#include <emscripten/val.h>
#include <sonare/sonare_c.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <deque>
#include <initializer_list>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include "acoustic/material.h"
#include "acoustic/rir_synthesizer.h"
#include "acoustic/room_model.h"
#include "analysis/acoustic_analyzer.h"
#include "analysis/beat_analyzer.h"
#include "analysis/bpm_analyzer.h"
#include "analysis/chord_analyzer.h"
#include "analysis/dynamics_analyzer.h"
#include "analysis/key_analyzer.h"
#include "analysis/melody_analyzer.h"
#include "analysis/music_analyzer.h"
#include "analysis/onset_analyzer.h"
#include "analysis/rhythm_analyzer.h"
#include "analysis/room_estimator.h"
#include "analysis/section_analyzer.h"
#include "analysis/timbre_analyzer.h"
#include "automation/parameter.h"
#include "core/audio.h"
#include "core/convert.h"
#include "core/db_convert.h"
#include "core/pcen.h"
#include "core/resample.h"
#include "core/spectrum.h"
#include "editing/pitch_editor/note_editor.h"
#include "editing/pitch_editor/pitch_corrector.h"
#include "editing/pitch_editor/scale_quantizer.h"
#include "editing/voice_changer/realtime.h"
#include "editing/voice_changer/streaming_retune.h"
#include "editing/voice_changer/voice_changer.h"
#include "effects/acoustic/room_morph.h"
#include "effects/decompose.h"
#include "effects/hpss.h"
#include "effects/normalize.h"
#include "effects/phase_vocoder.h"
#include "effects/pitch_shift.h"
#include "effects/preemphasis.h"
#include "effects/remix.h"
#include "effects/silence.h"
#include "effects/spectral_edit.h"
#include "effects/time_stretch.h"
#include "engine/realtime_engine.h"
#include "feature/chroma.h"
#include "feature/cqt.h"
#include "feature/inverse.h"
#include "feature/mel_spectrogram.h"
#include "feature/nnls_chroma.h"
#include "feature/onset.h"
#include "feature/pitch.h"
#include "feature/rhythm.h"
#include "feature/segment.h"
#include "feature/spectral.h"
#include "feature/tonnetz.h"
#include "feature/vqt.h"
#include "graph/graph.h"
#include "mastering/api/chain.h"
#include "mastering/api/internal_processor_runner.h"
#include "mastering/api/named_processor.h"
#include "mastering/api/presets.h"
#include "mastering/assistant/config_from_params.h"
#include "mastering/assistant/suggester.h"
#include "mastering/dynamics/compressor.h"
#include "mastering/dynamics/gate.h"
#include "mastering/dynamics/transient_shaper.h"
#include "mastering/eq/equalizer.h"
#include "mastering/eq/tilt.h"
#include "mastering/final/dither.h"
#include "mastering/match/match_eq.h"
#include "mastering/match/reference_spectrum.h"
#include "mastering/maximizer/loudness_optimize.h"
#include "mastering/maximizer/streaming_preview.h"
#include "mastering/maximizer/true_peak_limiter.h"
#include "mastering/repair/declick.h"
#include "mastering/repair/declip.h"
#include "mastering/repair/decrackle.h"
#include "mastering/repair/dehum.h"
#include "mastering/repair/denoise_classical.h"
#include "mastering/repair/dereverb_classical.h"
#include "mastering/repair/trim_silence.h"
#include "mastering/saturation/exciter.h"
#include "mastering/saturation/tape.h"
#include "mastering/spectral/air_band.h"
#include "mastering/stereo/imager.h"
#include "mastering/stereo/mono_maker.h"
#include "metering/basic.h"
#include "metering/clipping.h"
#include "metering/dynamic_range.h"
#include "metering/lufs.h"
#include "metering/normalize.h"
#include "metering/phase_scope.h"
#include "metering/spectrum.h"
#include "metering/stereo.h"
#include "metering/true_peak.h"
#include "metering/waveform.h"
#include "midi/builtin_synth.h"
#include "mixing/api/presets.h"
#include "mixing/channel_strip.h"
#include "quick.h"
#include "rt/command.h"
#include "rt/gain_processor.h"
#include "rt/processor_base.h"
#include "sonare.h"
#include "streaming/stream_analyzer.h"
#include "util/db.h"
#include "util/exception.h"
#include "util/frame.h"
#include "util/padding.h"
#include "util/peak.h"
#include "util/types.h"
#include "util/vector_normalize.h"

using namespace emscripten;
using namespace sonare;

/// @brief Copies @p names into a JS array of strings.
/// @details One owner for a loop nine binding entry points had written out for
///          themselves; each copy carried its own inlined embind push sequence.
val stringVectorToVal(const std::vector<std::string>& names);
val vectorToFloat32Array(const std::vector<float>& vec);
val vectorToInt32Array(const std::vector<int>& vec);
val vectorToUint8Array(const std::vector<uint8_t>& vec);
/// Conservative wasm32 budget for caller-owned Float32Array data copied into
/// the linear-memory heap. Keeping this below the native offline ceiling leaves
/// room for the input copy, DSP work buffers, and output arrays.
inline constexpr std::size_t kMaxWasmFloat32Elements = 64u * 1024u * 1024u;
/// Upper bound on the elements a caller-supplied length may pre-reserve for a
/// JS array of objects. Larger arrays still work — the destination vector grows
/// as elements are read and validated — so a fabricated `.length` cannot turn
/// into an allocation the actual data does not back.
inline constexpr std::size_t kMaxWasmObjectArrayReserve = 1u * 1024u * 1024u;
/// Validates a caller-supplied COUNT that arrived as a bare JS number, before
/// it is allowed anywhere near an allocation.
/// @details The array-like readers below cannot help here: there is no object
///   to read a `.length` from, just a scalar argument. Take it as a @c double
///   rather than a @c size_t — embind converts a JS number to @c size_t by a
///   plain cast, so by the time a @c size_t parameter is in hand a negative
///   value has already wrapped to a huge one and a NaN is undefined behaviour,
///   with nothing left to detect. The count must still be bounded by the
///   caller against whatever it indexes. What it guarantees is narrower than it
///   looks: the accepted range is the JS safe-integer range, which is wider than
///   `std::size_t` on wasm32, and the narrowing SATURATES rather than trapping.
///   A value at or above 2^32 therefore comes back as the largest address rather
///   than as itself, which is harmless for a request a real container caps and
///   is not harmless for anything that addresses memory.
/// @throws SonareException(InvalidParameter) for a non-finite, negative,
///   fractional, or unsafe value.
std::size_t wasmCountArg(double value, const char* subject);
/// Validates a caller-supplied INDEX or OFFSET into a buffer: wasmCountArg plus
/// a refusal above the addressable range, so a request past it is reported
/// rather than silently landing on the last address. Use this wherever the
/// number names a position; use wasmCountArg where it names a quantity the
/// callee will cap against something real.
/// @throws SonareException(InvalidParameter) for everything wasmCountArg
///   refuses, and for a value the build cannot address.
std::size_t wasmIndexArg(double value, const char* subject);
/// Reads an array-like object's element count after rejecting null, undefined,
/// non-numeric, fractional, unsafe, and over-budget values. Use this before
/// indexing arbitrary JS arrays so embind never leaks a raw JS TypeError.
/// @p length_key names the count property ("length" for arrays and Int32Array,
/// "byteLength" for a Uint8Array byte blob).
std::size_t wasmArrayLikeLength(const val& arr, const char* subject = "array",
                                const char* length_key = "length");
/// Reads and validates an array-like object's JS `.length` without narrowing a
/// non-finite, fractional, or unsafe Number to wasm32 size_t.
std::size_t wasmFloat32ArrayLength(const val& arr, const char* subject = "Float32Array");
/// Checks a cumulative caller-controlled Float32 element count before any
/// associated vectors are allocated.
void validateWasmFloat32ElementBudget(std::initializer_list<std::size_t> counts,
                                      const char* subject);
/// Reads one array length and adds it to a caller-maintained cumulative input
/// count. Use this for entry points with optional or variable numbers of input
/// buffers so every length is budgeted before the first vector copy.
std::size_t accumulateWasmFloat32ArrayLength(const val& arr, const char* array_subject,
                                             const char* budget_subject,
                                             std::size_t* cumulative_count);
/// Preflights both members of an offline pair before either is copied. When
/// @p require_matching_lengths is true, mismatched stereo planes are rejected
/// at the same pre-allocation boundary.
void validateWasmFloat32ArrayPair(const val& first, const char* first_subject, const val& second,
                                  const char* second_subject, const char* budget_subject,
                                  bool require_matching_lengths);
std::vector<float> float32ArrayToVector(val arr);
/// @brief float32ArrayToVector for one span of the source, so a windowed entry
/// point costs its window rather than the buffer it is polling. @p start and
/// @p count must already be clamped to the source's length.
std::vector<float> float32ArrayWindowToVector(val arr, std::size_t start, std::size_t count);
/// @brief Loads a JS Float32Array into an Audio after the shared offline-input
/// validation (rejects null/empty, an out-of-range sampleRate, an oversized
/// buffer, and any non-finite sample). Mirrors the C ABI validate_audio_params
/// so the WASM surface rejects the same inputs even though it bypasses the
/// C-ABI translation unit. @throws SonareException(InvalidParameter).
Audio loadValidatedAudio(val samples, int sample_rate);
/// @brief loadValidatedAudio for an entry point that reads one window of the
/// buffer. The null/empty, sampleRate and buffer-size rules still cover the
/// whole buffer; only the non-finite scan narrows, to
/// [@p scan_offset, @p scan_offset + @p scan_count) clamped to the end. A
/// sample outside that window is never read, so its value is not a precondition
/// -- the contract the C ABI states for its windowed calls.
/// @throws SonareException(InvalidParameter).
Audio loadValidatedAudioWindow(val samples, int sample_rate, std::size_t scan_offset,
                               std::size_t scan_count);
/// @brief Interleaved sibling for the (samples, channels, sampleRate) facade.
/// Validates channels > 0, the shared offline-input rules over the whole buffer,
/// and that the length is a whole number of frames (no silent truncation of a
/// trailing partial frame). Writes the per-channel frame count to @p frames and
/// returns the copied samples. @throws SonareException(InvalidParameter).
std::vector<float> loadValidatedInterleaved(val samples, int channels, int sample_rate,
                                            size_t* frames);
std::vector<int32_t> int32ArrayToVector(val arr);
std::vector<uint8_t> uint8ArrayToVector(val arr);
bool hasProperty(val object, const char* key);
val objectProperty(val object, const char* key);
/// @brief Presence-checked float reader: an absent field takes @p default_value,
///        a present one is validated by @ref checkedFloatFromVal.
/// @details Presence and validity are separate questions, and only the first one
///          has a default. A present NaN is a caller error rather than a request
///          to fall back: most options bags here are read straight into a config
///          struct whose guards are written as `x > lo` or `isfinite(a) && a > b`,
///          so a NaN lands on the permissive arm and the call returns a plausible
///          finite result. Use @ref floatOption for the fields whose owner has a
///          documented "non-finite means unspecified" convention.
/// @throws SonareException(InvalidParameter) naming @p key.
float floatProperty(val object, const char* key, float default_value);
/// @brief Presence- AND type-checked float reader: an absent field takes @p
///        default_value, a present one must be a JS number.
/// @details The addon's FloatProperty spelled for this surface, for a field that
///          is a QUANTITY -- it has no value meaning "unspecified", so omitting
///          the key is the only way to ask for the default. @ref floatProperty
///          reaches the same field through val::as<double>(), which COERCES: a
///          numeric string and a one-element array arrive as the number they
///          spell and a boolean as 0 or 1, so a caller error becomes a result
///          indistinguishable from a value the caller chose, and the addon
///          refuses the very same input. Which of the two a field takes is a
///          contract decision; that the surfaces answer it differently is not.
/// @throws SonareException(InvalidParameter) naming @p key, for a wrong-typed
///         value and for one @ref checkedFloatFromVal refuses.
float typedFloatProperty(val object, const char* key, float default_value);
/// @brief Fallback float reader: a field that is absent, or present but not a
///        finite number, takes @p default_value. A wrong-typed value, and a
///        finite value outside the 32-bit float range, are refused rather than
///        substituted.
/// @details The sibling of @ref floatProperty for a field whose owner documents
///          a non-finite value as "unspecified". Reserved for that case: a
///          reader that silently eats a bad number reports success with a
///          plausible result, so every call site must be able to point at the
///          convention it is honouring. The substitution covers a value that is
///          non-finite in the caller's own arithmetic, never one the narrowing
///          made non-finite on the way in -- a caller who wrote 1e300 chose a
///          number, and answering it with the default reports success carrying a
///          value they did not choose. A wrong TYPE is outside the convention
///          entirely: nothing documents a string as meaning "unspecified".
/// @throws SonareException(InvalidParameter) naming @p key, for a wrong-typed
///         value and for a finite value wider than a 32-bit float.
float floatOption(val object, const char* key, float default_value);
/// @brief Refuses a fractional number, the integrality half of the narrowings
///        below.
/// @details Declared rather than left file-local because a site whose range
///          check is already someone else's -- a C-ABI bound, a core validator --
///          otherwise has nothing to reach for and hand-writes the trunc
///          comparison. Truncation is the same silent value change as a wrap
///          from the other end.
/// @throws SonareException(InvalidParameter) naming @p key.
void requireIntegral(double number, const char* key);
/// @brief Narrows a JS number to int, rejecting anything out of range or
///        fractional.
/// @details The one place that knows how to do this safely. A reader that needs
///          an int from a val must call this rather than val::as<int>(), which
///          saturates: a file-local copy in repair.cpp accepted 2^31 and
///          4294967295 as the same INT_MAX for 15 fields. Truncation is the same
///          silent value change from the other end, so both are refused here
///          rather than per facade -- a JS-side check cannot see a caller
///          driving the embind classes directly.
/// @throws SonareException(InvalidParameter) naming @p key.
int checkedIntFromVal(const val& value, const char* key);
int intProperty(val object, const char* key, int default_value);
/// @brief Presence- AND type-checked int reader: an absent field takes @p
///        default_value, a present one must be a JS number.
/// @details The addon's IntProperty spelled for this surface, and @ref
///          typedFloatProperty's integer sibling: for a COUNT or a SIZE, which
///          has no value meaning "unspecified", so omitting the key is the only
///          way to ask for the default. @ref intProperty reaches the same field
///          through val::as<double>(), which COERCES, so `'1024'` and `[1024]`
///          arrive as 1024 and `true` as 1 -- a caller error that lands inside
///          the field's own domain, where no later range check can see it. The
///          narrowing is unchanged: a present number still goes through @ref
///          checkedIntFromVal, so out-of-range and fractional stay refused.
/// @throws SonareException(InvalidParameter) naming @p key, for a wrong-typed
///         value and for one @ref checkedIntFromVal refuses.
int typedIntProperty(val object, const char* key, int default_value);
/// @brief Unsigned sibling of @ref checkedIntFromVal.
/// @details val::as<uint32_t>() saturates at the top and clamps a negative to 0,
///          so -1 -- the sentinel several fields here spell "none" with -- lands
///          on the first real id or the first enum member instead.
/// @throws SonareException(InvalidParameter) naming @p key.
uint32_t checkedUintFromVal(const val& value, const char* key);
uint32_t uintProperty(val object, const char* key, uint32_t default_value);
/// @brief Reads a raw 32-bit word (a UMP word, a packed MIDI 1.0 message).
/// @details Deliberately NOT @ref uintProperty: the whole 32-bit range is legal
///          here, and the idiomatic JS spelling `(0x4 << 28) | …` is a SIGNED
///          int once bit 31 is set, so a negative is reinterpreted as its
///          two's-complement word rather than refused. Only a value outside
///          [-2^31, 2^32) or a fractional one is a caller error.
/// @throws SonareException(InvalidParameter) naming @p key.
uint32_t checkedWordFromVal(const val& value, const char* key);
uint32_t wordProperty(val object, const char* key, uint32_t default_value);
/// @brief MIDI-byte sibling of @ref checkedIntFromVal, narrowing only: the
///        member's own domain stays with whoever owns it.
/// @details val::as<uint8_t>() WRAPS, and a wrapped byte is always inside the
///          byte domain, so no downstream range check can see it: 256 reads as
///          controller 0, 300 as controller 44, and 511 as the any-channel
///          wildcard.
/// @throws SonareException(InvalidParameter) naming @p key.
uint8_t checkedByteFromVal(const val& value, const char* key);
uint8_t byteProperty(val object, const char* key, uint8_t default_value);
/// @brief 64-bit sibling of @ref checkedIntFromVal.
/// @details Reading through double instead of the BigInt conversion turns NaN
///          into 0 and truncates a fractional frame position, neither of which
///          any downstream non-negative check can tell from a real request.
/// @throws SonareException(InvalidParameter) naming @p key.
int64_t checkedInt64FromVal(const val& value, const char* key);
int64_t int64Property(val object, const char* key, int64_t default_value);
/// @brief Narrows a JS number to float, rejecting what float cannot hold.
/// @details The integer readers' counterpart for the other overflow: a value
///          past FLT_MAX becomes +inf, and a config validator that rejects NaN
///          need not reject inf, so distinct absurd requests collapse into one
///          accepted result.
/// @throws SonareException(InvalidParameter) naming @p key.
float checkedFloatFromVal(const val& value, const char* key);
/// @brief Refuses a non-finite double. No narrowing to do -- a JS number is a
///        double already -- so finiteness is the whole check.
/// @details Every field reading through this today is also checked by the site
///          that reads it (the tempo and time-signature segment validators, and
///          an explicit isfinite on the marker id and the MIDI clip start), so
///          this is the family's guarantee rather than the only guard. It is
///          what a field added to one of those bags inherits.
/// @throws SonareException(InvalidParameter) naming @p key.
double checkedDoubleFromVal(const val& value, const char* key);
/// @brief Presence-checked double reader: an absent field takes @p default_value,
///        a present one is validated by @ref checkedDoubleFromVal.
double doubleProperty(val object, const char* key, double default_value);
/// @brief Resolves a built-in oscillator waveform given as a JS string or a JS
///        number to its @ref SonareSynthWaveform ordinal.
/// @details Both spellings reach the same rejection naming the accepted set.
///          Validating only the string form leaves the numeric form -- the one a
///          generated binding produces -- silently falling back to sine, and the
///          first value past the enum is 4, not some implausible number. Not
///          reachable through @ref checkedIntFromVal alone: 4 and -1 are in range
///          for an int, so this is a domain check, not a narrowing check.
/// @throws SonareException(InvalidParameter) for any value outside the set.
int builtinWaveformFromVal(const val& value);
bool boolProperty(val object, const char* key, bool default_value);
/// @brief Presence- AND type-checked bool reader: an absent field takes @p
///        default_value, a present one must be a JS boolean.
/// @details The addon's BoolProperty spelled for this surface, for a FLAG, whose
///          two values are the whole domain -- there is no third one meaning
///          "unspecified". @ref boolProperty reaches the same field through
///          val::as<bool>(), which applies JS truthiness, so `'false'`, `[]` and
///          `0` all arrive as a flag the caller never wrote and every one of them
///          is a legal flag downstream. There is no narrowing to keep: a JS
///          boolean is already the value, so the type test is the whole check.
/// @throws SonareException(InvalidParameter) naming @p key.
bool typedBoolProperty(val object, const char* key, bool default_value);
std::string stringProperty(val object, const char* key, const std::string& default_value);
/// @brief Type-checked optional reader: returns the numeric value only when @p v
/// is present (not undefined/null) and is a JS number, otherwise std::nullopt.
/// Unlike floatProperty (presence-checked with fallback), a present-but-wrong-type
/// value yields nullopt so the caller skips the assignment instead of coercing.
/// @details A present number narrows through @ref checkedFloatFromVal. A wrong
///          type is absent; a number float cannot hold is wrong, and reading it
///          raw made it an infinity indistinguishable from a requested one.
/// @throws SonareException(InvalidParameter) naming @p key when @p v is a number
///         outside the 32-bit float range.
std::optional<float> optionalNumber(const val& v, const char* key);
/// @brief Boolean sibling of optionalNumber: returns the value only when @p v is
/// present and a JS boolean, otherwise std::nullopt.
std::optional<bool> optionalBool(const val& v);
/// Invokes a JS cancellation callback and returns true only when it returns the
/// literal boolean true. Undefined and all other values leave the operation running.
bool cancelCallbackRequested(const val& callback);
/// @brief Reads the JS `.length` of @p a and @p b and requires they match.
/// @p subject names the arrays in the error message (e.g. "leftChannels and
/// rightChannels"). When @p require_non_zero is true (the default — this is
/// the historically documented contract), an empty pair is also rejected;
/// call sites that intentionally tolerate a zero-length pair (e.g. a
/// zero-strip mixer configuration, which already handles `count == 0` as a
/// no-op downstream) pass false explicitly with a comment explaining why.
/// Returns the common length.
/// @throws SonareException(InvalidParameter) on mismatch, or (when required)
/// zero length.
int requireMatchedLength(const val& a, const val& b, const char* subject,
                         bool require_non_zero = true);
/// @brief Rejects an out-of-range enum ordinal instead of letting a switch or
/// ternary's default/else arm silently substitute whichever member it
/// happens to return. @p subject names the field in the error message.
/// Mirrors the C ABI's range-checked enum converters (e.g. core_common.cpp's
/// fill_key_profile / fill_key_modes, sonare_c_internal.h's valid_window),
/// which reject an unmapped ordinal rather than defaulting it.
/// @throws SonareException(InvalidParameter) when @p value is outside
/// [@p min, @p max].
void requireOrdinalInRange(int value, int min, int max, const char* subject);
/// @brief Reads a REQUIRED field of exactly type T: throws InvalidParameter
/// naming @p subject and @p key if the field is absent, null, or not the
/// expected JS type (a non-bool T additionally requires a finite number).
/// Unlike floatProperty/intProperty/boolProperty (presence-checked with a
/// silent default), this never substitutes a value for a missing or
/// malformed field. Use for POD-shaped inputs where every field is mandatory
/// (e.g. the AudioWorklet's flat voice-changer config), so a partial object
/// is rejected instead of zero-filled — a missing boolean silently read as
/// `false` has turned off a safety-critical DSP stage (the ISP limiter) in
/// the past.
/// @brief The numeric half of @ref requireProperty, shared by every arithmetic
///        instantiation so the validation is compiled once rather than per type.
inline double requireNumberProperty(const val& object, const char* key, const char* subject) {
  if (!hasProperty(object, key)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          std::string(subject) + "." + key + " is required");
  }
  const val value = object[key];
  if (value.typeOf().as<std::string>() != "number") {
    throw SonareException(ErrorCode::InvalidParameter,
                          std::string(subject) + "." + key + " must be a number");
  }
  const double number = value.as<double>();
  if (!std::isfinite(number)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          std::string(subject) + "." + key + " must be finite");
  }
  return number;
}

inline bool requireBoolProperty(const val& object, const char* key, const char* subject) {
  if (!hasProperty(object, key)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          std::string(subject) + "." + key + " is required");
  }
  const val value = object[key];
  if (value.typeOf().as<std::string>() != "boolean") {
    throw SonareException(ErrorCode::InvalidParameter,
                          std::string(subject) + "." + key + " must be a boolean");
  }
  return value.as<bool>();
}

template <typename T>
T requireProperty(const val& object, const char* key, const char* subject) {
  if constexpr (std::is_same_v<T, bool>) {
    return requireBoolProperty(object, key, subject);
  } else {
    return static_cast<T>(requireNumberProperty(object, key, subject));
  }
}
/// @brief Converts a JS object of {name -> number|boolean} into mastering params.
/// @param skip_keys Keys the caller consumes itself and that must not reach the
///        numeric conversion — a string-valued key would otherwise be rejected
///        as an unsupported type.
std::vector<mastering::api::Param> masteringParamsFromObject(
    val object, const std::vector<std::string>& skip_keys = {});
mastering::api::MasteringChainConfig masteringChainConfigFromVal(val config);

void registerProjectBindings();
void registerStreamAnalyzerBindings();
void registerRealtimeEngineBindings();
void registerStreamingMasteringChainBindings();
void registerStreamingEqualizerBindings();
void registerStreamingRetuneBindings();
void registerRealtimeVoiceChangerStreamingBindings();
void registerMixingBindings();
void registerOfflineBindings();
void registerOfflineDynamicsEditingBindings();
void registerRepairBindings();
void registerMeteringBindings();
void registerFeatureSpectrogramBindings();
void registerFeatureMusicBindings();
void registerFeatureSpectralBindings();
void registerFeaturePitchBindings();
void registerFeatureCoreBindings();
void registerEffectsAudioBindings();
void registerPolyphonyBindings();
void registerMasteringChainBindings();
void registerMasteringApiBindings();
void registerQuickAnalysisBindings();
void registerQuickDetailedAnalysisBindings();
void registerAnalysisFeatureBindings();

#endif  // __EMSCRIPTEN__
