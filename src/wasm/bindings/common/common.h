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
///   caller against whatever it indexes; this only guarantees it is a real,
///   non-negative, exactly representable integer.
/// @throws SonareException(InvalidParameter) for a non-finite, negative,
///   fractional, or unsafe value.
std::size_t wasmCountArg(double value, const char* subject);
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
/// @brief Loads a JS Float32Array into an Audio after the shared offline-input
/// validation (rejects null/empty, an out-of-range sampleRate, an oversized
/// buffer, and any non-finite sample). Mirrors the C ABI validate_audio_params
/// so the WASM surface rejects the same inputs even though it bypasses the
/// C-ABI translation unit. @throws SonareException(InvalidParameter).
Audio loadValidatedAudio(val samples, int sample_rate);
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
float floatProperty(val object, const char* key, float default_value);
int intProperty(val object, const char* key, int default_value);
bool boolProperty(val object, const char* key, bool default_value);
std::string stringProperty(val object, const char* key, const std::string& default_value);
/// @brief Type-checked optional reader: returns the numeric value only when @p v
/// is present (not undefined/null) and is a JS number, otherwise std::nullopt.
/// Unlike floatProperty (presence-checked with fallback), a present-but-wrong-type
/// value yields nullopt so the caller skips the assignment instead of coercing.
std::optional<float> optionalNumber(const val& v);
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
/// fill_key_profile / fill_key_modes, features_streaming.cpp's valid_window),
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
template <typename T>
T requireProperty(const val& object, const char* key, const char* subject) {
  if (!hasProperty(object, key)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          std::string(subject) + "." + key + " is required");
  }
  const val value = object[key];
  const std::string type = value.typeOf().as<std::string>();
  if constexpr (std::is_same_v<T, bool>) {
    if (type != "boolean") {
      throw SonareException(ErrorCode::InvalidParameter,
                            std::string(subject) + "." + key + " must be a boolean");
    }
  } else {
    if (type != "number") {
      throw SonareException(ErrorCode::InvalidParameter,
                            std::string(subject) + "." + key + " must be a number");
    }
    if (!std::isfinite(value.as<double>())) {
      throw SonareException(ErrorCode::InvalidParameter,
                            std::string(subject) + "." + key + " must be finite");
    }
  }
  return value.as<T>();
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
void registerMasteringChainBindings();
void registerMasteringApiBindings();
void registerQuickAnalysisBindings();
void registerQuickDetailedAnalysisBindings();
void registerAnalysisFeatureBindings();

#endif  // __EMSCRIPTEN__
