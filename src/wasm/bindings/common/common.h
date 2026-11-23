#pragma once

#ifdef __EMSCRIPTEN__

#include <emscripten/bind.h>
#include <emscripten/val.h>
#include <sonare/sonare_c.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <deque>
#include <initializer_list>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
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
#include "c_api/sonare_c_error_mapping.h"
#include "c_api/sonare_c_last_refusal.h"
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
#include "util/json_schema.h"
#include "util/padding.h"
#include "util/parameter_checks.h"
#include "util/peak.h"
#include "util/types.h"
#include "util/vector_normalize.h"
#include "wasm/field_spelling.h"

using namespace emscripten;
using namespace sonare;

/// @brief Copies @p names into a JS array of strings.
/// @details One owner for a loop nine binding entry points had written out for
///          themselves; each copy carried its own inlined embind push sequence.
val stringVectorToVal(const std::vector<std::string>& names);
/// @brief A detected key as the `{ root, mode, confidence, name, shortName }` object every key
/// result carries.
val keyToVal(const sonare::Key& key);
/// @brief Copies @p n elements starting at @p data into a fresh JS
///        Float32Array/Int32Array/Uint8Array via one bulk memcpy.
/// @details The pointer+length overload is the one every call site that
///          already owns a contiguous buffer (an Audio, a result struct's own
///          storage) should reach for; the `std::vector` overload exists for a
///          caller that built one anyway and forwards to this one, so there is
///          exactly one copy across the boundary rather than a throwaway
///          std::vector construction ahead of it.
val vectorToFloat32Array(const float* data, std::size_t n);
val vectorToFloat32Array(const std::vector<float>& vec);
val vectorToInt32Array(const int* data, std::size_t n);
val vectorToInt32Array(const std::vector<int>& vec);

/// Sets @p key to @p value unless it is NaN: a scalar that was not measured is an absent member,
/// while -Infinity (no measurable level) is set as a number.
inline void setMeasured(val& out, const char* key, double value) {
  if (!std::isnan(value)) out.set(key, value);
}
/// Sets @p key to @p value only when a count was produced: a stage that did not run leaves the
/// member absent, while a run that counted nothing sets 0.
inline void setOptionalCount(val& out, const char* key, const std::optional<std::size_t>& value) {
  if (value) out.set(key, static_cast<double>(*value));
}
/// Sets @p key to a Float32Array of @p values, taken by a measurement of @p count elements, unless
/// the measurement was not taken (empty @p values): an array not measured as a whole is an absent
/// member, while a measured one keeps its length, with NaN for each element that was not measured,
/// even when that is every element. A measurement of zero elements is an empty array.
inline void setMeasuredArray(val& out, const char* key, const std::vector<float>& values,
                             std::size_t count) {
  if (count > 0 && values.empty()) return;
  out.set(key, vectorToFloat32Array(values));
}
val vectorToUint8Array(const uint8_t* data, std::size_t n);
val vectorToUint8Array(const std::vector<uint8_t>& vec);
/// @brief An argument the binding layer refused itself, before any library call.
/// @details The refusal contract of the JS surface is by cause, not by which layer
///   refused: a wrong-typed argument is a JS `TypeError` and an out-of-domain one a
///   `RangeError`, whether this layer or the library refused it. Every
///   SonareException carrying InvalidParameter is such a refusal and
///   @c sonareExceptionInfo surfaces it as a `RangeError`; this base only chooses
///   the class (@ref kind), so a `TypeError` is possible. Failures that are not
///   an argument (state, unsupported, allocation, decode, cancellation) stay a
///   coded `SonareError`. It derives from SonareException(InvalidParameter) so a
///   C++ handler that catches the library exception keeps catching it.
class WasmArgumentError : public sonare::SonareException {
 public:
  /// @brief The JS error class a refusal surfaces as.
  enum class Kind { Type, Range };

  Kind kind() const { return kind_; }

 protected:
  WasmArgumentError(Kind kind, const std::string& message)
      : sonare::SonareException(sonare::ErrorCode::InvalidParameter, message), kind_(kind) {}

 private:
  Kind kind_;
};

/// @brief A wrong-typed argument; surfaces as a JS `TypeError`.
class WasmTypeError final : public WasmArgumentError {
 public:
  explicit WasmTypeError(const std::string& message) : WasmArgumentError(Kind::Type, message) {}
};

/// @brief An out-of-domain argument; surfaces as a JS `RangeError`.
class WasmRangeError final : public WasmArgumentError {
 public:
  explicit WasmRangeError(const std::string& message) : WasmArgumentError(Kind::Range, message) {}
};

/// @brief The C string for a JS-supplied name, key, JSON text or other string
///        that is about to reach a NUL-terminated API.
/// @details The C ABI and the internal `const char*` functions end a string at
///   its first NUL, so a value carrying one would be cut there and succeed as
///   its prefix, indistinguishable from a value the caller chose. Every
///   `.c_str()` handoff of a string that came from JS goes through here; the
///   returned pointer is @p text's own storage and lives as long as it does.
/// @param subject The JS field or argument name the refusal reports.
/// @throws WasmRangeError when @p text contains a NUL.
const char* wasmCString(const std::string& text, const char* subject);
/// @brief The NUL check of @ref wasmCString for a string that is read from JS
///        and stays a @c std::string, so the refusal lands at the read.
/// @throws WasmRangeError when @p text contains a NUL.
void wasmRefuseEmbeddedNul(const std::string& text, const char* subject);

/// @brief The C detail, or the code's generic text.
inline std::string cErrorMessage(SonareError err) {
  const char* detail = sonare_last_error_message();
  const char* fallback = sonare_error_message(err);
  return detail != nullptr && detail[0] != '\0' ? detail : fallback;
}

/// @brief A refusal a C-ABI call recorded, carrying the entry key of the call that refused
///        (`cEntry` on the JS refusal record; a direct core refusal has none).
class CEntryRefusal : public sonare::SonareParameterRefusal {
 public:
  CEntryRefusal(const sonare::SonareParameterRefusal& refusal, std::string c_entry)
      : sonare::SonareParameterRefusal(refusal), c_entry_(std::move(c_entry)) {}
  const std::string& c_entry() const noexcept { return c_entry_; }

 private:
  std::string c_entry_;
};

/// @brief Throws @p shown as the refusal the last C-ABI call recorded.
[[noreturn]] inline void throwCEntryRefusal(const sonare::SonareParameterRefusal& shown) {
  const char* entry = sonare_last_refusal_entry();
  throw CEntryRefusal(shown, entry != nullptr ? entry : "");
}

/// @brief Rethrows the parameter refusal the last C-ABI call recorded, spelled under the refused
///        entry's request keys, for the JS translation point to classify; returns when none was.
inline void rethrowLastCRefusal() {
  if (std::optional<sonare::SonareParameterRefusal> shown =
          sonare_field_spelling::wasm::SpelledLastRefusal()) {
    throwCEntryRefusal(*shown);
  }
}

/// @brief Throws the returned @p err as its recorded refusal, else as its SonareException with
///        cErrorMessage(@p err).
[[noreturn]] inline void throwCodedCError(SonareError err) {
  rethrowLastCRefusal();
  throw sonare::SonareException(sonare_c_detail::error_code_from_c_error(err), cErrorMessage(err));
}

/// @brief throwCodedCError for a call whose struct's refusal subjects carry a "<c_root>." prefix
///        beyond the `options` / `config` every refusal already drops; it is stripped too.
[[noreturn]] inline void throwCodedCErrorFlattening(SonareError err, const char* c_root) {
  if (std::optional<sonare::SonareParameterRefusal> shown =
          sonare_field_spelling::wasm::SpelledLastRefusal(c_root)) {
    throwCEntryRefusal(*shown);
  }
  throwCodedCError(err);
}

/// @brief The messages of the diagnostics the most recent C-ABI call published, in order.
inline std::vector<std::string> lastDiagnosticMessages() {
  std::vector<std::string> messages;
  const size_t count = sonare_last_diagnostic_count();
  for (size_t index = 0; index < count; ++index) {
    SonareDiagnostic diagnostic;
    sonare_diagnostic_init(&diagnostic);
    if (sonare_last_diagnostic(index, &diagnostic) == SONARE_OK) {
      messages.emplace_back(diagnostic.message);
    }
  }
  return messages;
}

/// @brief Throws the failure recorded by a C-ABI call that returned NULL
///        (a handle, or a descriptor / JSON string): the thread-local code with
///        the recorded detail as the message.
/// @param fallback Code used when the library recorded none (the code reads Ok).
[[noreturn]] inline void throwLastCError(SonareError fallback = SONARE_ERROR_INVALID_STATE) {
  rethrowLastCRefusal();
  SonareError code = sonare_last_error_code();
  if (code == SONARE_OK) code = fallback;
  const char* detail = sonare_last_error_message();
  const char* text = detail != nullptr && detail[0] != '\0' ? detail : sonare_error_message(code);
  throw sonare::SonareException(sonare_c_detail::error_code_from_c_error(code), text);
}

/// @brief Throws NotSupported for an arrangement/MIDI entry point in a build without the engine.
[[noreturn]] inline void throwMidiEngineUnavailable() {
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "arrangement/MIDI engine is not available in this build");
}

/// Element budget for a plain JS array, whose elements are converted one at a
/// time. A typed array is copied in bulk and is bounded by the heap instead.
inline constexpr std::size_t kMaxWasmPlainArrayElements = 64u * 1024u * 1024u;
/// Elements of @p element_bytes each the heap could hold at its maximum size.
std::size_t wasmHeapElementCapacity(std::size_t element_bytes);
/// Upper bound on the elements a caller-supplied length may pre-reserve for a
/// JS array of objects. Larger arrays still work — the destination vector grows
/// as elements are read and validated — so a fabricated `.length` cannot turn
/// into an allocation the actual data does not back.
inline constexpr std::size_t kMaxWasmObjectArrayReserve = 1u * 1024u * 1024u;
/// The CQT-family core names for the lowest frequency and the rate, as its requests spell them.
inline constexpr std::array<sonare::util::check::MemberKey, 2> kCqtRequestKeys{
    {{"fmin", "fminHz"}, {"sr", "sampleRate"}}};
/// The synthesis core names for the rate, frequencies and durations, as their requests spell them.
inline constexpr std::array<sonare::util::check::MemberKey, 2> kToneRequestKeys{
    {{"sr", "sampleRate"}, {"duration", "durationSec"}}};
inline constexpr std::array<sonare::util::check::MemberKey, 4> kChirpRequestKeys{
    {{"fmin", "fminHz"}, {"fmax", "fmaxHz"}, {"sr", "sampleRate"}, {"duration", "durationSec"}}};
inline constexpr std::array<sonare::util::check::MemberKey, 3> kClicksRequestKeys{
    {{"sr", "sampleRate"}, {"frequency", "frequencyHz"}, {"click_duration", "clickDurationSec"}}};
/// The HPSS core names for the two median kernels, as hpss() spells them.
inline constexpr std::array<sonare::util::check::MemberKey, 2> kHpssRequestKeys{
    {{"kernel_size_harmonic", "kernelHarmonic"}, {"kernel_size_percussive", "kernelPercussive"}}};
/// The time-stretch core names the speed ratio after its own parameter.
inline constexpr std::array<sonare::util::check::MemberKey, 1> kTimeStretchRequestKeys{
    {{"rate", "speedRatio"}}};
/// The same kernels as the percussive-event requests spell them.
inline constexpr std::array<sonare::util::check::MemberKey, 2> kPercussiveSeparationKeys{
    {{"kernel_size_harmonic", "hpssKernelHarmonic"},
     {"kernel_size_percussive", "hpssKernelPercussive"}}};
/// The core names for the mel range, as the mel requests spell them.
inline constexpr std::array<sonare::util::check::MemberKey, 2> kMelRangeKeys{
    {{"fmin", "fminHz"}, {"fmax", "fmaxHz"}}};
/// The core names the mel and MFCC inverse transforms refuse under, as their requests spell them.
inline constexpr std::array<sonare::util::check::MemberKey, 5> kInverseRequestKeys{
    {{"sr", "sampleRate"},
     {"M", "power"},
     {"mfcc", "coefficients"},
     {"fmin", "fminHz"},
     {"fmax", "fmaxHz"}}};
/// @brief The element pointer of @p copy, never null. A copy of a zero-length array is a given
///        argument of length 0, so a C entry answers with its own emptiness rule instead of
///        reading the null an empty container may carry as an omitted argument. Every pointer
///        into a copy the bindings hand the C ABI goes through here.
template <typename Container>
auto elementData(Container& copy) -> decltype(copy.data()) {
  using Element = std::remove_pointer_t<decltype(copy.data())>;
  static Element empty{};
  return copy.empty() ? &empty : copy.data();
}
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
/// @throws WasmArgumentError for a non-finite, negative,
///   fractional, or unsafe value.
std::size_t wasmCountArg(double value, const char* subject);
/// Validates a caller-supplied INDEX or OFFSET into a buffer: wasmCountArg plus
/// a refusal above the addressable range, so a request past it is reported
/// rather than silently landing on the last address. Use this wherever the
/// number names a position; use wasmCountArg where it names a quantity the
/// callee will cap against something real.
/// @throws WasmArgumentError for everything wasmCountArg
///   refuses, and for a value the build cannot address.
std::size_t wasmIndexArg(double value, const char* subject);
/// Reads the element count of a JS array or typed array, refusing anything else
/// (an object with a `length`, a string, a DataView) as "<subject> must be an array".
/// @p length_key names the count property ("length", or "byteLength" for a Uint8Array blob).
std::size_t wasmArrayLength(const val& arr, const char* subject, const char* length_key = "length");
/// Reads the count of a list of audio planes, refusing a non-array as
/// "<subject> must be an array of Float32Array"; each plane is read with
/// wasmFloat32ArrayLength under "<subject>[i]".
std::size_t wasmPlaneListLength(const val& list, const char* subject);
/// Reads the length of an input sample buffer: a Float32Array or a plain array.
std::size_t wasmFloat32ArrayLength(const val& arr, const char* subject);
/// Reads the length of an output buffer the binding writes into with `set`, so
/// only a Float32Array is taken.
std::size_t wasmFloat32OutputLength(const val& arr, const char* subject);
/// One of the length readers above, for helpers that compare two arguments.
using WasmLengthReader = std::size_t (*)(const val&, const char*);
/// Reads one array length and, for a plain array, adds it to a caller-maintained
/// cumulative plain-element count, so entry points taking several buffers
/// budget every plain one before the first copy.
std::size_t accumulateWasmFloat32ArrayLength(const val& arr, const char* array_subject,
                                             std::size_t* cumulative_count);
/// Budgets the plain arrays among several buffers together, before any is copied.
void validatePlainElementBudget(std::initializer_list<std::pair<const val*, const char*>> buffers);
/// Preflights both members of an offline pair before either is copied. When
/// @p require_matching_lengths is true, mismatched stereo planes are rejected
/// at the same pre-allocation boundary.
void validateWasmFloat32ArrayPair(const val& first, const char* first_subject, const val& second,
                                  const char* second_subject, bool require_matching_lengths);
std::vector<float> float32ArrayToVector(val arr, const char* subject);
/// @brief float32ArrayToVector for one span of the source, so a windowed entry
/// point costs its window rather than the buffer it is polling. @p start and
/// @p count must already be clamped to the source's length.
std::vector<float> float32ArrayWindowToVector(val arr, const char* subject, std::size_t start,
                                              std::size_t count);
/// @brief Loads a JS Float32Array into an Audio after the shared offline-input
/// validation (rejects null/empty, an out-of-range sampleRate, an oversized
/// buffer, and any non-finite sample). Mirrors the C ABI validate_audio_params
/// so the WASM surface rejects the same inputs even though it bypasses the
/// C-ABI translation unit. @p name is the argument the refusal names.
/// @throws WasmArgumentError.
Audio loadValidatedAudio(const char* name, val samples, int sample_rate);
/// @brief Loads a JS array of equal-length Float32Array channels, running
/// loadValidatedAudio over EVERY channel. A linked core entry guards channels[0]
/// alone and takes one shared length, so this is the whole non-finite and length
/// guard on the set.
/// @throws WasmArgumentError.
std::vector<Audio> loadValidatedChannelSet(const char* name, const val& channels, int sample_rate);

enum class ChannelSetValidation { Offline, StructureOnly };
std::vector<Audio> loadChannelSet(const char* name, const val& channels, int sample_rate,
                                  ChannelSetValidation validation);

/// @brief loadValidatedAudio for an entry point that reads one window of the
/// buffer. The null/empty, sampleRate and buffer-size rules still cover the
/// whole buffer; only the non-finite scan narrows, to
/// [@p scan_offset, @p scan_offset + @p scan_count) clamped to the end. A
/// sample outside that window is never read, so its value is not a precondition
/// -- the contract the C ABI states for its windowed calls.
/// @throws WasmArgumentError.
Audio loadValidatedAudioWindow(const char* name, val samples, int sample_rate,
                               std::size_t scan_offset, std::size_t scan_count);
/// @brief Interleaved sibling for the (samples, channels, sampleRate) facade.
/// Validates channels > 0, the shared offline-input rules over the whole buffer,
/// and that the length is a whole number of frames (no silent truncation of a
/// trailing partial frame). Writes the per-channel frame count to @p frames and
/// returns the copied samples. @throws WasmArgumentError.
std::vector<float> loadValidatedInterleaved(const char* name, val samples, int channels,
                                            int sample_rate, size_t* frames);
std::vector<int32_t> int32ArrayToVector(val arr, const char* subject);
std::vector<uint8_t> uint8ArrayToVector(val arr, const char* subject);
bool hasProperty(val object, const char* key);
val objectProperty(val object, const char* key);

/// @brief The name a field of a list element is refused by: @p key under @p at (`bindings[2]`),
///        or @p key alone when @p at is empty. The property readers taking @p at name by it.
inline std::string elementField(const std::string& at, const char* key) {
  return at.empty() ? std::string(key) : at + "." + key;
}

/// @brief The element at @p index of the list @p list_name, which must be an object:
///        "<list_name>[<index>] must be an object" otherwise. Name its fields with
///        @ref elementAt.
/// @throws WasmTypeError naming the element.
val requiredElementObject(const val& list, std::size_t index, const char* list_name);
/// @brief The name a list element is refused by: "<list_name>[<index>]".
inline std::string elementAt(const char* list_name, std::size_t index) {
  return std::string(list_name) + "[" + std::to_string(index) + "]";
}
/// @brief A required number field of an object, checked for type only so the owning check
///        states the domain: absent, null or not a number is a WasmTypeError
///        "<at>.<key> must be a number"; NaN and the infinities pass to that check.
double requiredDomainDoubleProperty(const val& object, const char* key, const std::string& at);
/// @brief A required number field of a list element, type-only: absent, null or not a number is
///        a WasmTypeError "<at>.<key> must be a number". The caller narrows the returned value
///        with the checked reader that owns the field's domain, named by the same subject.
val requiredNumberElementProperty(const val& object, const char* key, const std::string& at);
/// @brief @ref requiredNumberElementProperty for a field whose domain is an integer range:
///        the type refusal says "must be an integer", as the domain's own text does.
val requiredDomainIntProperty(const val& object, const char* key, const std::string& at);
/// @brief A required string field of a list element: absent, null or not a string is a
///        WasmTypeError "<at>.<key> must be a string".
std::string requiredStringElementProperty(const val& object, const char* key,
                                          const std::string& at);
/// @brief Optional sibling of @ref requiredDomainDoubleProperty: absent or null takes
///        @p fallback.
double optionalDomainDoubleProperty(const val& object, const char* key, double fallback,
                                    const std::string& at);

/// @brief Presence- AND type-checked float reader: an absent field -- omitted,
///        `undefined` or `null` -- takes @p default_value, a present one must be
///        a JS number validated by @ref checkedFloatFromVal.
/// @details Presence and validity are separate questions, and only the first one
///          has a default. A present NaN, numeric string or boolean is a caller
///          error rather than a request to fall back: val::as<double>() would
///          coerce the last two into a number nobody wrote, and a NaN lands on
///          the permissive arm of the `x > lo` guards most config structs use.
///          Use @ref floatOption for the fields whose owner documents
///          "non-finite means unspecified". Every property reader below shares
///          this contract: absent is the only way to ask for the default.
/// @throws WasmArgumentError naming @p key.
float floatProperty(val object, const char* key, float default_value,
                    const std::string& at = std::string());

/// @brief Type-only sibling of @ref floatProperty, for a field whose core check states its domain.
/// @details NaN and the infinities pass, and a finite value past the float range saturates to an
///          infinity, so the core refuses all of them in its own domain text.
/// @throws WasmArgumentError naming @p key for a non-number.
float domainFloatProperty(val object, const char* key, float default_value);
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
///          The choice against @ref floatProperty: a float the caller sets is refused when
///          non-finite; a float carrying an engine result back in, whose owner documents NaN as
///          "unmeasurable", is read here, and the read site names the result field it mirrors.
/// @throws WasmArgumentError naming @p key, for a wrong-typed
///         value and for a finite value wider than a 32-bit float.
float floatOption(val object, const char* key, float default_value);
/// @brief Refuses a fractional number, the integrality half of the narrowings
///        below.
/// @details Declared rather than left file-local because a site whose range
///          check is already someone else's -- a C-ABI bound, a core validator --
///          otherwise has nothing to reach for and hand-writes the trunc
///          comparison. Truncation is the same silent value change as a wrap
///          from the other end.
/// @throws WasmArgumentError naming @p key.
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
/// @throws WasmArgumentError naming @p key.
int checkedIntFromVal(const val& value, const char* key);
/// @brief Reads a sample rate that must be an integer in the supported audio range.
/// @throws WasmTypeError for a non-number, WasmRangeError otherwise.
int wholeHertzSampleRateFromVal(const val& value);
/// @brief Reads the hop of a resynthesizing call for the core: 0 (which the facades
///        send for an omitted hop) is the automatic hop, a quarter of n_fft; a
///        negative value is refused in the C ABI's words.
/// @throws WasmArgumentError naming hopLength.
int resynthesisHopFromVal(const val& value);
/// @brief Reads an integer MIDI note in the 7-bit domain [0, 127].
/// @throws WasmArgumentError for a wrong type or note-domain value.
int checkedMidiNoteFromVal(const val& note);

/// @brief Resolves a musical mode given as its ordinal (0..6) or its lowercase
///        church-mode name ("major", "minor", "dorian", ...).
/// @throws WasmArgumentError naming @p what for any other value.
Mode modeFromVal(const val& value, const char* what);
/// @brief Presence- AND type-checked int reader: an absent field -- omitted,
///        `undefined` or `null` -- takes @p default_value, a present one must be
///        a JS number.
/// @details @ref floatProperty's integer sibling, for a COUNT or a SIZE. A
///          present number still goes through @ref checkedIntFromVal, so
///          out-of-range and fractional stay refused.
/// @throws WasmArgumentError naming @p key, for a wrong-typed
///         value and for one @ref checkedIntFromVal refuses.
int intProperty(val object, const char* key, int default_value);
/// @brief Integer in [@p lo, @p hi] whose absence the C ABI spells as @p absent: absent or
///        null returns @p absent, and any other value outside the range, @p absent
///        included, is a WasmRangeError naming @p key.
int intInRangeOrAbsentProperty(val object, const char* key, int lo, int hi, int absent,
                               const std::string& at = std::string());
/// @brief Unsigned sibling of @ref checkedIntFromVal.
/// @details val::as<uint32_t>() saturates at the top and clamps a negative to 0,
///          so -1 -- the sentinel several fields here spell "none" with -- lands
///          on the first real id or the first enum member instead.
/// @throws WasmArgumentError naming @p key.
uint32_t checkedUintFromVal(const val& value, const char* key);
uint32_t uintProperty(val object, const char* key, uint32_t default_value);
/// @brief Reads a size_t count or position.
/// @throws WasmTypeError unless a number; WasmRangeError unless a finite non-negative integer
///         within both Number.MAX_SAFE_INTEGER and the native size_t maximum, both naming @p key.
std::size_t checkedSizeTFromVal(const val& value, const char* key);
/// @brief The domain half of @ref checkedSizeTFromVal, for a value embind already read as a number.
std::size_t sizeTFromNumber(double value, const char* key);
/// @brief Reads a raw 32-bit word (a UMP word, a packed MIDI 1.0 message).
/// @details Deliberately NOT @ref uintProperty: the whole 32-bit range is legal
///          here, and the idiomatic JS spelling `(0x4 << 28) | …` is a SIGNED
///          int once bit 31 is set, so a negative is reinterpreted as its
///          two's-complement word rather than refused. Only a value outside
///          [-2^31, 2^32) or a fractional one is a caller error.
/// @throws WasmArgumentError naming @p key.
uint32_t checkedWordFromVal(const val& value, const char* key);
uint32_t wordProperty(val object, const char* key, uint32_t default_value);
/// @brief MIDI-byte sibling of @ref checkedIntFromVal, narrowing only: the
///        member's own domain stays with whoever owns it.
/// @details val::as<uint8_t>() WRAPS, and a wrapped byte is always inside the
///          byte domain, so no downstream range check can see it: 256 reads as
///          controller 0, 300 as controller 44, and 511 as the any-channel
///          wildcard.
/// @throws WasmArgumentError naming @p key.
uint8_t checkedByteFromVal(const val& value, const char* key);
uint8_t byteProperty(val object, const char* key, uint8_t default_value,
                     const std::string& at = std::string());
/// @brief 16-bit sibling of @ref checkedByteFromVal: 65536 would wrap to 0, inside the type.
/// @throws WasmArgumentError naming @p key.
uint16_t checkedUint16FromVal(const val& value, const char* key);
/// @brief 64-bit sibling of @ref checkedIntFromVal.
/// @details Reading through double instead of the BigInt conversion turns NaN
///          into 0 and truncates a fractional frame position, neither of which
///          any downstream non-negative check can tell from a real request.
/// @throws WasmArgumentError naming @p key.
int64_t checkedInt64FromVal(const val& value, const char* key);
int64_t int64Property(val object, const char* key, int64_t default_value);
/// @brief Reads a 64-bit unsigned value carried as a canonical-length decimal
///        string, the only spelling a JS number cannot corrupt above 2^53.
/// @details Refuses a non-string, an empty string, a sign, whitespace, trailing
///          characters and a value past 2^64 - 1; nothing is coerced.
/// @throws WasmArgumentError naming @p key.
uint64_t checkedDecimalUint64FromVal(const val& value, const char* key);
/// @brief Presence-checked sibling of @ref checkedDecimalUint64FromVal: an absent
///        field -- omitted, `undefined` or `null` -- takes @p default_value.
uint64_t decimalUint64Property(val object, const char* key, uint64_t default_value);
/// @brief Reads a trailing render-frame argument; undefined is -1 ("now").
/// @throws WasmArgumentError for anything checkedInt64FromVal refuses.
/// @throws WasmRangeError for a negative frame.
int64_t renderFrameFromVal(const val& value);
/// @brief Narrows a JS number to float, rejecting what float cannot hold.
/// @details The integer readers' counterpart for the other overflow: a value
///          past FLT_MAX becomes +inf, and a config validator that rejects NaN
///          need not reject inf, so distinct absurd requests collapse into one
///          accepted result.
/// @throws WasmArgumentError naming @p key.
float checkedFloatFromVal(const val& value, const char* key);
/// @brief Reads a JS number for a float field whose consumer states the domain: type-checked only.
/// @details NaN and the infinities travel on, and a finite value past the float range saturates
///          to an infinity, so the consumer's "<key> must be finite and in [lo, hi]" refuses all.
/// @throws WasmArgumentError naming @p key when @p value is not a number.
float domainFloatFromVal(const val& value, const char* key);
/// @brief Refuses a non-finite double. No narrowing to do -- a JS number is a
///        double already -- so finiteness is the whole check.
/// @details Every field reading through this today is also checked by the site
///          that reads it (the tempo and time-signature segment validators, and
///          an explicit isfinite on the marker id and the MIDI clip start), so
///          this is the family's guarantee rather than the only guard. It is
///          what a field added to one of those bags inherits.
/// @throws WasmArgumentError naming @p key.
double checkedDoubleFromVal(const val& value, const char* key);
/// @brief Reads a JS number as a double, refusing every other JS type.
/// @details val::as<double>() accepts a boolean as 0 or 1, so a caller error would
///          arrive as a value; every checked narrowing above starts here.
/// @throws WasmArgumentError naming @p key.
double numberFromVal(const val& value, const char* key);
/// @brief Presence- AND type-checked double reader: an absent field -- omitted,
///        `undefined` or `null` -- takes @p default_value, a present one must be
///        a JS number.
/// @details @ref floatProperty's full-width sibling, for a field the C ABI
///          declares as a double. It carries no range check because a double IS
///          what a JS number is, which is why @ref floatProperty's 32-bit bound
///          has no counterpart here.
/// @throws WasmArgumentError naming @p key, for a wrong-typed
///         value and for a non-finite one.
double doubleProperty(val object, const char* key, double default_value);
/// @brief Resolves a built-in oscillator waveform given as a JS string or a JS
///        number to its @ref SonareSynthWaveform ordinal.
/// @details Both spellings reach the same rejection naming the accepted set.
///          Validating only the string form leaves the numeric form -- the one a
///          generated binding produces -- silently falling back to sine, and the
///          first value past the enum is 4, not some implausible number. Not
///          reachable through @ref checkedIntFromVal alone: 4 and -1 are in range
///          for an int, so this is a domain check, not a narrowing check.
/// @throws WasmArgumentError for any value outside the set.
int builtinWaveformFromVal(const val& value);
/// @brief Presence- AND type-checked bool reader: an absent field -- omitted,
///        `undefined` or `null` -- takes @p default_value, a present one must be
///        a JS boolean.
/// @details For a FLAG, whose two values are the whole domain. val::as<bool>()
///          applies JS truthiness, so `'false'`, `[]` and `0` would each arrive
///          as a flag the caller never wrote; the type test is the whole check.
/// @throws WasmArgumentError naming @p key.
bool boolProperty(val object, const char* key, bool default_value,
                  const std::string& at = std::string());
/// @brief Reads the GS insertion-effect realisation ("modern" or "classic") as
///        its C ABI ordinal (0 or 1): absent, `undefined` or `null` is modern.
/// @throws WasmArgumentError naming @p key for a non-string or
///         any other name.
int gsEfxRealizationProperty(val object, const char* key);
/// @brief String reader: absent, `undefined` or `null` takes @p default_value, a
///        present one must be a JS string.
/// @throws WasmArgumentError naming @p key.
std::string stringProperty(val object, const char* key, const std::string& default_value);
/// @brief String reader for a value already in hand: it must be a JS string without a NUL.
/// @throws WasmTypeError naming @p key for a non-string; WasmRangeError for an embedded NUL.
std::string stringFromVal(const val& value, const char* key);
/// @brief Optional number reader: std::nullopt only when @p v is undefined or
///        null, so the caller skips the assignment.
/// @details A present number narrows through @ref checkedFloatFromVal.
/// @throws WasmArgumentError naming @p key when @p v is not a
///         number, or is one the 32-bit float cannot hold.
std::optional<float> optionalNumber(const val& v, const char* key);
/// @brief Optional sibling of @ref domainFloatFromVal: std::nullopt only when @p v is undefined
///        or null.
/// @throws WasmArgumentError naming @p key when @p v is present and not a number.
std::optional<float> optionalDomainNumber(const val& v, const char* key);
/// @brief Reads a JS number and checks it against the core's statement of its domain, so a
///        fraction, a NaN, an infinity or a value out of bounds is refused in the domain's own
///        text rather than in a reader text or a facade copy of the bound.
/// @details Defined out of line so the refusal's catch stays in a TU compiled with exceptions;
///          the refusal is the core's, with @p key taken as already in the caller's spelling.
/// @throws WasmTypeError naming @p key when @p value is not a number.
/// @param float_target When true, a value inside @p domain that a 32-bit float cannot hold is
///        refused in the float range's text.
double domainCheckedNumber(const val& value, const char* key,
                           const sonare::util::check::NumericDomain& domain, bool float_target);
/// @brief Narrows a JS number onto @p T after @ref domainCheckedNumber.
/// @details @p domain must lie inside @p T.
template <typename T>
T domainNumberFromVal(const val& value, const char* key,
                      const sonare::util::check::NumericDomain& domain) {
  return static_cast<T>(domainCheckedNumber(value, key, domain, std::is_same_v<T, float>));
}
/// @brief Optional sibling of domainNumberFromVal: omitted, `undefined` or `null` is
///        std::nullopt.
template <typename T>
std::optional<T> optionalDomainValue(const val& v, const char* key,
                                     const sonare::util::check::NumericDomain& domain) {
  if (v.isUndefined() || v.isNull()) return std::nullopt;
  return domainNumberFromVal<T>(v, key, domain);
}
/// @brief Boolean sibling of optionalNumber.
/// @throws WasmArgumentError naming @p key when @p v is present
///         and not a JS boolean.
std::optional<bool> optionalBool(const val& v, const char* key);
/// Invokes a JS cancellation callback and returns true only when it returns the
/// literal boolean true. Undefined and all other values leave the operation running.
bool cancelCallbackRequested(const val& callback);
/// @brief True when @p callback is a JS function; false for `undefined` or `null`.
/// @throws WasmTypeError naming @p key when it is present and not a function.
bool callbackGiven(const val& callback, const char* key);
/// @brief Reads @p first and @p second with @p read_length and requires they match.
/// @p first_subject and @p second_subject name each argument (e.g. "leftChannels"), so a
/// refusal of one names that one. When @p require_non_zero is true (the default — this is
/// the historically documented contract), an empty pair is also rejected;
/// call sites that intentionally tolerate a zero-length pair (e.g. a
/// zero-strip mixer configuration, which already handles `count == 0` as a
/// no-op downstream) pass false explicitly with a comment explaining why.
/// Returns the common length.
/// @throws WasmArgumentError on mismatch, or (when required)
/// zero length.
int requireMatchedLength(const val& first, const char* first_subject, const val& second,
                         const char* second_subject, WasmLengthReader read_length,
                         bool require_non_zero = true);
/// @brief Rejects an enum ordinal outside [0, @p count) instead of letting a switch or
/// ternary's default/else arm silently substitute whichever member it happens to return.
/// Mirrors the C ABI's range-checked enum converters (e.g. core_common.cpp's
/// fill_key_profile / fill_key_modes, sonare_c_internal.h's valid_window), which reject an
/// unmapped ordinal rather than defaulting it. @p subject is the caller's spelling of the field.
/// @throws WasmRangeError "<subject> must be a name or an ordinal in [0, <count>)".
void requireEnumOrdinal(int value, int count, const char* subject);
/// @brief Rejects a plain integer index outside [@p lo, @p hi].
/// @throws WasmRangeError "<subject> must be an integer in [<lo>, <hi>]".
void requireIntegerInRange(int value, int lo, int hi, const char* subject);
/// @brief Returns @p object[@p key], refusing it by name when absent or when its
///        JS `typeof` is not @p js_type (@p article_type is the noun the message uses).
val requireTypedProperty(const val& object, const char* key, const char* subject,
                         const char* js_type, const char* article_type);

/// @brief The numeric half of @ref requireProperty, shared by every arithmetic
///        instantiation so the validation is compiled once rather than per type.
inline double requireNumberProperty(const val& object, const char* key, const char* subject) {
  const val value = requireTypedProperty(object, key, subject, "number", "a number");
  const double number = value.as<double>();
  if (!std::isfinite(number)) {
    throw WasmRangeError(sonare::util::json::schema::child_path(subject, key) + " must be finite");
  }
  return number;
}

/// @brief The int64 sibling of @ref requireNumberProperty: required, refused by
///        name for a non-number type (not just coerced through it, the way
///        @ref checkedInt64FromVal's val::as<double>() would), and refused by
///        name again for a fraction or a value outside the 64-bit integer
///        range rather than truncated or cast into undefined behavior.
inline int64_t requireInt64Property(const val& object, const char* key, const char* subject) {
  const val value = requireTypedProperty(object, key, subject, "number", "a number");
  return checkedInt64FromVal(value, sonare::util::json::schema::child_path(subject, key).c_str());
}

/// @brief Reads a required integer-valued property from an object.
/// @details Unlike @ref intProperty, a missing, undefined, or null property is
///          an error. A present value must be a JS number and pass
///          @ref checkedIntFromVal without truncation or saturation.
/// @throws WasmArgumentError naming @p subject and @p key.
inline int requireIntProperty(const val& object, const char* key, const char* subject) {
  const val value = requireTypedProperty(object, key, subject, "number", "a number");
  return checkedIntFromVal(value, sonare::util::json::schema::child_path(subject, key).c_str());
}

inline bool requireBoolProperty(const val& object, const char* key, const char* subject) {
  return requireTypedProperty(object, key, subject, "boolean", "a boolean").as<bool>();
}

/// @brief Reads a REQUIRED field of exactly type T: throws a WasmArgumentError
/// naming @p subject and @p key if the field is absent, null, or not the
/// expected JS type (a non-bool T additionally requires a finite number).
/// Unlike floatProperty/intProperty/boolProperty (which take a default for an
/// absent field), this never substitutes a value for a missing or
/// malformed field. Use for POD-shaped inputs where every field is mandatory
/// (e.g. the AudioWorklet's flat voice-changer config), so a partial object
/// is rejected instead of zero-filled — a missing boolean silently read as
/// `false` has turned off a safety-critical DSP stage (the ISP limiter) in
/// the past.
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
/// @brief Reads a chain configuration document (`{"version"?, "params"}` text) as an offline
///        chain config, or as a streaming one when @p streaming is set.
/// @details Absent, `undefined` or `null` selects the defaults of that chain kind.
/// @throws WasmTypeError for a non-string; a refused key or value as a parameter refusal under
///         the request key @p root ("" when the request object is the config itself).
mastering::api::MasteringChainConfig masteringChainConfigFromDocument(
    const val& document, bool streaming = false, std::string_view root = "config");

void registerProjectBindings();
void registerStreamAnalyzerBindings();
void registerRealtimeEngineBindings();
void registerStreamingMasteringChainBindings();
void registerStreamingEqualizerBindings();
void registerStreamingRetuneBindings();
void registerRealtimeVoiceChangerStreamingBindings();
void registerPlaybackBindings();
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
void registerEffectsNoteBindings();
void registerEffectsPercussiveEventBindings();
void registerEffectsDecompositionBindings();
void registerEffectsSpectralEditBindings();
void registerPolyphonyBindings();
void registerVocalEditBindings();
void registerMasteringChainBindings();
void registerMasteringApiBindings();
void registerQuickAnalysisBindings();
void registerQuickDetailedAnalysisBindings();
void registerRoomAcousticsBindings();
void registerAnalysisFeatureBindings();

#endif  // __EMSCRIPTEN__
