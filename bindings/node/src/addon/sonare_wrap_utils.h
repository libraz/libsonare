#ifndef SONARE_NODE_SONARE_WRAP_UTILS_H_
#define SONARE_NODE_SONARE_WRAP_UTILS_H_

#include <napi.h>
#include <sonare/sonare_c.h>

#include <cmath>
#include <cstring>
#include <exception>
#include <new>
#include <optional>
#include <string>
#include <vector>

#include "mastering/api/named_processor.h"
#include "sonare_wrap_options.h"
#include "util/error_classification.h"
#include "util/exception.h"
namespace sonare_node {

/// @brief The calling thread's last C-ABI failure as a JS message: a recorded parameter refusal
///        rendered in request keys, else the flat detail without its C-entry prefix, else the
///        generic text for @p err.
std::string ErrorMessageForCode(SonareError err);

/// The exception text without a leading C-entry prefix, which the public entry replaces; a
/// parameter refusal is rendered in request keys from its subjects.
std::string ExceptionMessage(const std::exception& e);

/// Whether @p e refuses the request's shape (an unknown key or a wrong type), which surfaces as a
/// TypeError rather than the RangeError of a value outside its domain.
bool IsShapeRefusal(const std::exception& e);

/// @brief The last C-ABI parameter refusal as the caller reads it (in request keys), or none.
std::optional<sonare::SonareParameterRefusal> ShownLastRefusal();

/// @brief Carries @p shown, the refusal whose text @p error's message ends with, on @p error as
///        plain parts under Symbol.for('libsonare.refusal'), so a facade that converted an
///        argument can re-render it under the name and unit its caller wrote.
void AttachRefusal(Napi::Env env, Napi::Object error, const sonare::SonareParameterRefusal& shown);

/// @brief `renderRefusal(refusal, subjects, scales)`: the attached refusal with its subjects
///        replaced and its domain divided by `scales[0]`, as `{ message, refusal }`.
Napi::Value RenderRefusal(const Napi::CallbackInfo& info);

/// @brief Canonical name for a C-ABI error code ("InvalidParameter", ...).
///        Mirrors the JS-exposed ErrorCode enum; used as the error's codeName.
const char* ErrorCodeName(SonareError err);

/// @brief Map a C++ SonareException's ErrorCode onto the C-ABI SonareError so
///        JS errors expose the same numeric code as the C ABI / Python.
SonareError CErrorFromException(const sonare::SonareException& e);

/// @brief Inverse of CErrorFromException: a C-ABI error code as the C++ enum,
///        so a C-ABI failure can be re-raised as a code-carrying exception.
sonare::ErrorCode CodeFromCError(SonareError err);

/// @brief Build (without throwing) a JS Error for @p err with @p message.
///
/// The class follows the cause, not the layer that refused: INVALID_PARAMETER is
/// always a refused caller argument and becomes a plain RangeError, while every
/// other code is an Error carrying { name: 'SonareError', code, codeName }.
/// This is the single point a C code or SonareException becomes a JS error, so
/// every Throw* helper below inherits the rule.
/// A shape refusal (@ref IsShapeRefusal) under INVALID_PARAMETER is a TypeError instead; this
/// overload is for a binding-composed message, which is never one.
Napi::Error MakeSonareError(Napi::Env env, SonareError err, const std::string& message);

/// @brief MakeSonareError with the refusal's shape stated by the caller: @ref IsShapeRefusal for a
///        caught exception, @ref LastRefusalIsShape for a message read from the last C-ABI error.
Napi::Error MakeSonareError(Napi::Env env, SonareError err, const std::string& message,
                            bool shape_refusal);

/// @brief Throw MakeSonareError(@p err, ...) unless an exception is already pending.
///        The detail message is @p prefix + the thread-local / generic message.
void ThrowSonareError(Napi::Env env, SonareError err, const std::string& prefix = "");

/// @brief ThrowSonareError for an entry whose JS object flattens the C struct parameter @p c_root,
///        a root other than the `options` / `config` every refusal already drops: a refused
///        subject whose first path segment is @p c_root is named by the rest of its path.
void ThrowSonareErrorFlattening(Napi::Env env, SonareError err, const char* c_root);

/// @brief Like ThrowSonareError but uses an explicit detail message (e.g. a
///        caught SonareException's what()).
void ThrowSonareErrorMessage(Napi::Env env, SonareError err, const std::string& message);

/// @brief The one closed-handle refusal: a coded SonareError InvalidState
///        reading "<what> has been destroyed". Every released, destroyed or
///        disposed handle, whether the receiver or an argument, reports through here.
void ThrowHandleClosed(Napi::Env env, const char* what);

/// @brief The one catch-chain translation: reports the in-flight exception as a
///        JS error keyed on its cause (JS error as-is, SonareException by its own
///        code, other std::exception by the shared mapping, anything else Unknown).
///        Call only from inside a catch block.
void ThrowCurrentException(Napi::Env env);

/// @brief Reject a new Promise with the JS exception already pending, clearing it.
Napi::Promise RejectedFromPending(Napi::Env env);

/// @brief Throw the failure of the last C-ABI call that returned a null handle:
///        the code from sonare_last_error_code() with @p prefix + the recorded
///        detail as the message. @p fallback stands in when the code reads Ok.
void ThrowLastSonareError(Napi::Env env, const std::string& prefix,
                          SonareError fallback = SONARE_ERROR_INVALID_STATE);

/// @brief Splits a '\n'-joined name list a never-NULL C name getter returned
///        into a JS string[]. Those getters return "" on failure as well as for
///        an empty set, so an empty string with a recorded error throws it.
///        Call it directly after the getter, before any other C-ABI call.
Napi::Value JoinedNamesToArray(Napi::Env env, const char* joined, const std::string& what);

/// @brief Throw @p e classified through the shared std::exception classifier.
void ThrowStdException(Napi::Env env, const std::exception& e);

/// @brief Marks a pending exception a caller's callback threw, so no public entry renames it.
void MarkPendingForeign(Napi::Env env);

/// @brief Add the public SonareError shape to an existing JavaScript Error, or
/// re-class it as a RangeError for INVALID_PARAMETER (see MakeSonareError).
/// Used by AsyncWorker completion handlers before rejecting their Promise.
/// A shape refusal (@p shape_refusal) is re-classed as a TypeError instead, and a refusal the
/// worker captured (@p shown) is attached as @ref AttachRefusal does on the synchronous path.
void DecorateSonareError(Napi::Env env, Napi::Object error, SonareError err, bool shape_refusal,
                         const std::optional<sonare::SonareParameterRefusal>& shown);

/// @brief Whether the calling thread's last C-ABI failure was a shape refusal; an async worker
///        records it on its own thread beside the code.
bool LastRefusalIsShape();

/// @brief Read the optional JavaScript music-analysis settings into the C ABI
/// struct. Returns true only when @p value is an object supplied by the caller.
///
/// A false return has two meanings the caller must tell apart: no options were
/// supplied (analyse with the C defaults), or the options were rejected, in
/// which case a JS exception is already pending. Check env.IsExceptionPending()
/// and bail out before any further N-API call — the addon is built with
/// NAPI_DISABLE_CPP_EXCEPTIONS, where a second throw on a pending exception
/// aborts the process.
bool ReadMusicAnalyzeOptions(const Napi::Value& value, SonareMusicAnalyzeOptions* options);

/// @brief Read a meter candidate-numerator list from @p object's @p key into a C
/// options struct's fixed-capacity array, writing the entry count to @p count.
/// Shared by every entry point carrying such a list, so the capacity rule and
/// the rejection wording cannot drift apart between them.
///
/// An undefined or null value leaves @p numerators and @p count alone, which
/// keeps whatever default the caller staged, and a present non-array is refused
/// by name, as the *Property family does for every other key. An array replaces the set
/// wholesale, including an empty one: the core rejects a cleared list rather
/// than silently restoring the default, so the caller is told instead of
/// getting a set it did not ask for.
///
/// The two cases that cannot fall back throw exactly one catchable JS error and
/// return false: a list longer than the C-ABI capacity (truncating it would
/// analyse a different meter set than requested) and a non-numeric entry.
bool ReadMeterCandidateNumerators(const Napi::Object& object, const char* key,
                                  int (&numerators)[SONARE_MAX_METER_CANDIDATE_NUMERATORS],
                                  size_t* count);

/// @brief Read a required sample rate that must be a whole number of hertz in the supported range.
/// @return false with one pending JS exception: TypeError for a non-number, RangeError otherwise.
bool ReadWholeHertzSampleRate(Napi::Env env, const Napi::Value& value, int* out);

/// @brief The arguments both HPSS entry points take after the audio and its
///        sample rate: the two median-filter kernels, the STFT geometry, and
///        the mask mode.
struct HpssArguments {
  int kernel_harmonic;
  int kernel_percussive;
  int n_fft;
  int hop_length;
  bool hard_mask;
};

/// @brief Read the HPSS arguments at info[2..6] into @p out, applying the core's
///        own defaults where an argument was omitted.
///
/// One reader for both entry points, so neither the defaults nor the narrowing
/// can drift apart between them.
///
/// Every argument goes through the Optional*Arg family: an omitted, undefined or
/// null argument takes the core's default, a wrong-typed one is refused by name,
/// and a number the int cannot hold is refused rather than wrapped by ToInt32
/// (2^32 would arrive as 0 and 2^32 + 1 as 1, both of which the core's own guards
/// accept).
///
/// Nothing else is re-checked here. The core refuses a non-positive or
/// odd nFft, a non-positive hop, a hop past nFft / 2, and every kernel outside
/// the parity and ceiling rules, reporting each as a RangeError
/// naming the constraint it broke. An addon-side copy of any of those would be
/// redundant and would answer with a vaguer message.
///
/// @return false with exactly one pending JS exception on rejection; the caller
///         must return before any further N-API call.
bool ReadHpssArguments(Napi::Env env, const Napi::CallbackInfo& info, HpssArguments* out);

/// @brief Marshal one C-ABI MIDI event into the `{ ppq, word0, word1 }` object
///        every JS-facing event list on this surface is made of.
///
/// Shared rather than per-file: the flat event is the one shape the project,
/// the MIDI helpers and the transcription all hand back, and a second spelling
/// of it would be a second contract for `ProjectMidiEvent`.
Napi::Object MidiEventToObject(Napi::Env env, const SonareMidiEventPod& event);

/// @brief Input planes for a call that takes a `Float32Array[]` of channels.
struct ChannelInputs {
  std::vector<Napi::Float32Array> arrays;
  std::vector<const float*> ptrs;
  size_t length = 0;
};

/// @brief Reads a `Float32Array[]` into @p inputs, refusing what the C form
///        cannot express: a non-Float32Array element (TypeError) and a length
///        disagreement between channels (RangeError), which the single `length`
///        argument has no way to carry. An empty list goes through to the C
///        entry, which owns that rejection. Returns false with a JS exception
///        pending on refusal.
bool ReadChannelInputs(Napi::Env env, const Napi::Value& value, ChannelInputs* inputs);

/// @brief The messages of the diagnostics the most recent C-ABI call published, in order.
inline std::vector<std::string> LastDiagnosticMessages() {
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

/// @brief Throw a JS SonareError when @p err is not SONARE_OK; no-op otherwise.
///        Shared wrapper for the recurring
///        `if (err != SONARE_OK) ThrowSonareError(env, err);` guard.
///
/// The addon is built with NAPI_DISABLE_CPP_EXCEPTIONS, where a second N-API
/// throw raised while an exception is already pending is a fatal abort
/// (SIGABRT) rather than a JS-visible error. A caller that already left an
/// exception pending therefore gets no second throw here: the first error is
/// the one the caller can act on, and it stays the reported one. This is a
/// backstop, not a licence to call the C ABI with unread arguments — a caller
/// must still bail out before the native call.
inline void ThrowIfError(Napi::Env env, SonareError err) {
  if (env.IsExceptionPending()) return;
  if (err != SONARE_OK) {
    ThrowSonareError(env, err);
  }
}

/// @brief ThrowIfError's audio-thread counterpart: reports @p err from the code
///        alone, never from the thread-local detail message.
///
/// Audio-thread entry points — the ones carrying SONARE_C_RT_API_ENTRY, i.e. the
/// engine block-render calls and the realtime voice changer's process / latency
/// calls — neither clear nor record sonare_last_error_message, because
/// first-touch TLS setup would break their no-allocation contract
/// (sonare_c_types_functions.h states the slot must not be read as belonging to
/// the failed call). Routing them through ThrowIfError therefore reports
/// whatever an unrelated earlier control-thread call left behind: the message is
/// both wrong and intermittent, since it follows the call history rather than
/// the failure. Every addon call into that set must use this instead, which
/// mirrors the Python binding's _check_realtime.
inline void ThrowIfRealtimeError(Napi::Env env, SonareError err) {
  if (env.IsExceptionPending()) return;
  if (err != SONARE_OK) {
    ThrowSonareErrorMessage(env, err, sonare_error_message(err));
  }
}

/// @brief Throw a JS TypeError with @p message and return false when
///        info[index] is not a Float32Array; otherwise return true. The caller
///        passes its own detail message so existing error strings are preserved.
inline bool RequireFloat32Array(const Napi::CallbackInfo& info, size_t index, const char* message) {
  if (!IsFloat32Array(info[index])) {
    Napi::TypeError::New(info.Env(), message).ThrowAsJavaScriptException();
    return false;
  }
  return true;
}

/// @brief The (Float32Array samples, number sampleRate, ...) prologue every
///        mono entry point opens with; throws the TypeError naming the first
///        argument that does not fit and returns false.
inline bool RequireMonoArgs(const Napi::CallbackInfo& info) {
  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) return false;
  if (!info[1].IsNumber()) {
    Napi::TypeError::New(info.Env(), "sampleRate must be a number").ThrowAsJavaScriptException();
    return false;
  }
  return true;
}

/// @brief Sets @p key to @p value unless it is NaN: a scalar that was not measured is an absent
///        member, while -Infinity (no measurable level) is set as a number.
inline void SetMeasured(Napi::Object& out, const char* key, double value) {
  if (!std::isnan(value)) out.Set(key, Napi::Number::New(out.Env(), value));
}

/// @brief The single-number counterpart of SetMeasured: NaN (not measured) is undefined.
inline Napi::Value MeasuredNumber(Napi::Env env, double value) {
  return std::isnan(value) ? env.Undefined() : Napi::Number::New(env, value);
}

/// @brief Copies @p n floats from @p data into a fresh Float32Array with one memcpy;
///        a null @p data leaves the n zeros the array was created with.
inline Napi::Float32Array Float32ArrayFrom(Napi::Env env, const float* data, size_t n) {
  auto array = Napi::Float32Array::New(env, n);
  if (n > 0 && data != nullptr) std::memcpy(array.Data(), data, n * sizeof(float));
  return array;
}

inline Napi::Float32Array Float32ArrayFrom(Napi::Env env, const std::vector<float>& values) {
  return Float32ArrayFrom(env, values.data(), values.size());
}

/// @brief A JS array of @p n_channels Float32Arrays of @p length floats each, read from one
///        contiguous planar buffer (channel after channel).
inline Napi::Array Float32ArraysFromPlanar(Napi::Env env, const float* planar, size_t n_channels,
                                           size_t length) {
  Napi::Array out = Napi::Array::New(env, n_channels);
  for (size_t ch = 0; ch < n_channels; ++ch) {
    out.Set(static_cast<uint32_t>(ch), Float32ArrayFrom(env, planar + ch * length, length));
  }
  return out;
}

/// @brief A JS array of @p n_channels Float32Arrays of @p length floats each, one per pointer in
///        @p channels.
inline Napi::Array Float32ArraysFrom(Napi::Env env, const float* const* channels, size_t n_channels,
                                     size_t length) {
  Napi::Array out = Napi::Array::New(env, n_channels);
  for (size_t ch = 0; ch < n_channels; ++ch) {
    out.Set(static_cast<uint32_t>(ch), Float32ArrayFrom(env, channels[ch], length));
  }
  return out;
}

/// @brief A JS array holding one Float32Array per vector in @p channels, each at its own length.
inline Napi::Array Float32ArraysFrom(Napi::Env env,
                                     const std::vector<std::vector<float>>& channels) {
  Napi::Array out = Napi::Array::New(env, channels.size());
  for (size_t ch = 0; ch < channels.size(); ++ch) {
    out.Set(static_cast<uint32_t>(ch), Float32ArrayFrom(env, channels[ch]));
  }
  return out;
}

/// @brief Copies @p length floats of a C-owned heap array into a Float32Array and frees the C
///        copy; a null @p data leaves the zeros the array was created with.
inline Napi::Float32Array TakeCFloats(Napi::Env env, float* data, size_t length) {
  Napi::Float32Array out = Float32ArrayFrom(env, data, length);
  sonare_free_floats(data);
  return out;
}

/// @brief The Int32Array counterpart of TakeCFloats for a C-owned int array.
inline Napi::Int32Array TakeCInts(Napi::Env env, int* data, size_t length) {
  auto out = Napi::Int32Array::New(env, length);
  if (length > 0 && data != nullptr) std::memcpy(out.Data(), data, length * sizeof(int));
  sonare_free_ints(data);
  return out;
}

/// @brief Throws @p err as a coded SonareError and returns undefined, for the `return` of a
///        failed C-ABI call that has nothing to hand back.
inline Napi::Value CheckCResult(Napi::Env env, SonareError err) {
  ThrowIfError(env, err);
  return env.Undefined();
}

/// @brief Coerce a JS value into a std::vector<int>, accepting an Int32Array or
///        a plain number[]. Throws a JS TypeError otherwise.
/// @details A plain-array entry is read as a strict int named `@p name [i]`: a
///   fraction such as 1000.7 is refused rather than truncated onto a legal
///   index. The Int32Array path returns as-is because its elements are already
///   exact ints -- there is no fold left to refuse.
inline std::vector<int> IntVectorFromValue(const Napi::Value& value, const char* name) {
  if (IsInt32Array(value)) {
    auto arr = value.As<Napi::Int32Array>();
    return std::vector<int>(arr.Data(), arr.Data() + arr.ElementLength());
  }
  if (value.IsArray()) {
    auto arr = value.As<Napi::Array>();
    const uint32_t arr_count = node_list_length(arr, name);
    std::vector<int> out(arr_count);
    for (uint32_t i = 0; i < arr_count; ++i) {
      out[i] = node_narrow_int_element(value.Env(), arr.Get(i), name, i);
    }
    return out;
  }
  throw Napi::TypeError::New(value.Env(), std::string(name) + " must be an Int32Array or number[]");
}

/// @brief Validate that a flat row-major matrix's declared dims match its
///        backing typed-array length.
///
/// Throws a JS RangeError (and returns false) when either dimension is
/// negative or when rows*cols != length. The C ABI only null-checks pointers,
/// so this guard must live in the Node layer to prevent out-of-bounds reads
/// from caller-supplied dims that exceed the buffer.
///
/// @return true when the dims are consistent (no exception thrown).
bool ValidateMatrixDims(Napi::Env env, int rows, int cols, size_t length);
const char* PitchClassNameLocal(SonarePitchClass pc);
/// @brief Resolves a pitch class given as an ordinal in [0, 12) or as its name ("C", "C#", ...,
/// "B").
SonarePitchClass PitchClassFromValue(const Napi::Value& value, const char* what);
const char* ModeNameLocal(SonareMode mode);
const char* ChordQualityName(SonareChordQuality quality);
/// @brief Resolves a chord quality given as an ordinal or as the name a detected chord carries.
SonareChordQuality ChordQualityFromValue(const Napi::Value& value, const char* what);
Napi::Object KeyToObject(Napi::Env env, SonarePitchClass root, SonareMode mode, float confidence);
bool EnrichFullAnalysisObject(Napi::Env env, Napi::Object result, Napi::Error* error);

/// @brief Parse a heap-allocated C-ABI JSON string with the JS engine's own
/// JSON.parse and release it, whether or not the parse succeeded.
///
/// Throws a JS Error carrying @p failure_message when the string is missing or
/// does not parse into an object, so a caller only has to check
/// env.IsExceptionPending() before using the result.
///
/// @return The parsed object or env.Undefined() (exception already thrown).
Napi::Value ParseJsonObjectAndFree(Napi::Env env, char* json, const char* failure_message);

/// @brief Run sonare_analyze_json, parse the result with JSON.parse, add the
/// spelled chord and key names, and return the object. Frees the heap-allocated
/// JSON string. Throws a JS error on C-ABI failure or JSON parse failure.
///
/// @return The enriched JS object or env.Undefined() (exception already thrown).
Napi::Value FullAnalysisJsonToObject(Napi::Env env, const float* data, size_t length,
                                     int sample_rate,
                                     const SonareMusicAnalyzeOptions* options = nullptr);

/// @brief Convert a JS object of {name -> number|boolean} into mastering API params.
/// @param skip_keys Keys the caller consumes itself and that must not reach the
///        numeric conversion — a string-valued key would otherwise be rejected
///        as an unsupported type.
std::vector<sonare::mastering::api::Param> ParamsFromObject(
    const Napi::Object& object, const std::vector<std::string>& skip_keys = {});

/// @brief Build the C-ABI parameter array for `params`.
/// @note The returned keys borrow `params`' strings; `params` must outlive the result.
std::vector<SonareMasteringParam> MasteringCParams(
    const std::vector<sonare::mastering::api::Param>& params);

/// @brief The mixer meter record every strip, bus and master read returns: the
/// stereo fields plus the per-plane arrays over [0, numChannels).
Napi::Object MixMeterSnapshotToObject(Napi::Env env, const SonareMixMeterSnapshot& snapshot);

}  // namespace sonare_node

#define SONARE_NODE_TRY try {
#define SONARE_NODE_CATCH(env)               \
  }                                          \
  catch (...) {                              \
    sonare_node::ThrowCurrentException(env); \
    return env.Undefined();                  \
  }

// Promise-returning sibling of SONARE_NODE_CATCH: the failure arrives as a rejection, never as a
// synchronous throw.
#define SONARE_NODE_CATCH_REJECT(env)             \
  }                                               \
  catch (...) {                                   \
    sonare_node::ThrowCurrentException(env);      \
    return sonare_node::RejectedFromPending(env); \
  }

// Void-returning sibling of SONARE_NODE_CATCH, for the entry points registered
// as InstanceVoidMethod: the pending JS exception is what the caller sees.
#define SONARE_NODE_CATCH_VOID(env)          \
  }                                          \
  catch (...) {                              \
    sonare_node::ThrowCurrentException(env); \
  }

// Closed-handle guard for a wrapper's native pointer; `what` names the class.
#define SONARE_NODE_REQUIRE_OPEN(env, handle, what) \
  if ((handle) == nullptr) {                        \
    sonare_node::ThrowHandleClosed(env, what);      \
    return env.Undefined();                         \
  }
#define SONARE_NODE_REQUIRE_OPEN_VOID(env, handle, what) \
  if ((handle) == nullptr) {                             \
    sonare_node::ThrowHandleClosed(env, what);           \
    return;                                              \
  }

#endif  // SONARE_NODE_SONARE_WRAP_UTILS_H_
