#ifndef SONARE_NODE_SONARE_WRAP_UTILS_H_
#define SONARE_NODE_SONARE_WRAP_UTILS_H_

#include <napi.h>
#include <sonare/sonare_c.h>

#include <exception>
#include <new>
#include <vector>

#include "mastering/api/named_processor.h"
#include "util/exception.h"

namespace sonare_node {

const char* ErrorMessageForCode(SonareError err);

/// @brief Canonical name for a C-ABI error code ("InvalidParameter", ...).
///        Mirrors the JS-exposed ErrorCode enum; used as the error's codeName.
const char* ErrorCodeName(SonareError err);

/// @brief Map a C++ SonareException's ErrorCode onto the C-ABI SonareError so
///        JS errors expose the same numeric code as the C ABI / Python.
SonareError CErrorFromException(const sonare::SonareException& e);

/// @brief Inverse of CErrorFromException: a C-ABI error code as the C++ enum,
///        so a C-ABI failure can be re-raised as a code-carrying exception.
sonare::ErrorCode CodeFromCError(SonareError err);

/// @brief Throw a JS Error carrying { name: 'SonareError', code, codeName }.
///        The detail message is @p prefix + the thread-local / generic message.
void ThrowSonareError(Napi::Env env, SonareError err, const std::string& prefix = "");

/// @brief Like ThrowSonareError but uses an explicit detail message (e.g. a
///        caught SonareException's what()).
void ThrowSonareErrorMessage(Napi::Env env, SonareError err, const std::string& message);

/// @brief Add the public SonareError shape to an existing JavaScript Error.
/// Used by AsyncWorker completion handlers before rejecting their Promise.
void DecorateSonareError(Napi::Env env, Napi::Object error, SonareError err);

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
/// An absent or non-array value leaves @p numerators and @p count alone, which
/// keeps whatever default the caller staged — the type-checked fallback the
/// node_*_option family gives every other key. An array replaces the set
/// wholesale, including an empty one: the core rejects a cleared list rather
/// than silently restoring the default, so the caller is told instead of
/// getting a set it did not ask for.
///
/// The two cases that cannot fall back throw exactly one catchable JS error and
/// return false: a list longer than the C-ABI capacity (truncating it would
/// analyse a different meter set than requested) and a non-numeric entry.
bool ReadMeterCandidateNumerators(const Napi::Object& object, const char* key,
                                  int (&numerators)[SONARE_MAX_METER_CANDIDATE_NUMERATORS],
                                  int* count);

bool IsFloat32Array(const Napi::Value& value);
bool IsUint8Array(const Napi::Value& value);
bool IsInt32Array(const Napi::Value& value);

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

/// @brief Coerce a JS value into a std::vector<int>, accepting an Int32Array or
///        a plain number[]. Throws a JS TypeError otherwise.
inline std::vector<int> IntVectorFromValue(const Napi::Value& value) {
  if (value.IsTypedArray() && value.As<Napi::TypedArray>().TypedArrayType() == napi_int32_array) {
    auto arr = value.As<Napi::Int32Array>();
    return std::vector<int>(arr.Data(), arr.Data() + arr.ElementLength());
  }
  if (value.IsArray()) {
    auto arr = value.As<Napi::Array>();
    std::vector<int> out(arr.Length());
    for (uint32_t i = 0; i < arr.Length(); ++i) {
      out[i] = arr.Get(i).As<Napi::Number>().Int32Value();
    }
    return out;
  }
  throw Napi::TypeError::New(value.Env(), "Expected Int32Array or number[]");
}

/// @brief Coerce a JS value into a std::vector<float>, accepting a Float32Array
///        or a plain number[]. Throws a JS TypeError otherwise.
inline std::vector<float> FloatVectorFromValue(const Napi::Value& value) {
  if (value.IsTypedArray() && value.As<Napi::TypedArray>().TypedArrayType() == napi_float32_array) {
    auto arr = value.As<Napi::Float32Array>();
    return std::vector<float>(arr.Data(), arr.Data() + arr.ElementLength());
  }
  if (value.IsArray()) {
    auto arr = value.As<Napi::Array>();
    std::vector<float> out(arr.Length());
    for (uint32_t i = 0; i < arr.Length(); ++i) {
      out[i] = arr.Get(i).As<Napi::Number>().FloatValue();
    }
    return out;
  }
  throw Napi::TypeError::New(value.Env(), "Expected Float32Array or number[]");
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
bool ValidateMatrixDims(Napi::Env env, const char* fn_name, int rows, int cols, size_t length);
const char* PitchClassNameLocal(SonarePitchClass pc);
const char* ModeNameLocal(SonareMode mode);
const char* ChordQualityName(SonareChordQuality quality);
Napi::Object KeyToObject(Napi::Env env, SonarePitchClass root, SonareMode mode, float confidence);
Napi::Object AnalysisToObject(Napi::Env env, const SonareAnalysisResult& analysis);
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

/// @brief Run sonare_analyze_json, parse the result with JSON.parse, inject a
/// legacy `beatTimes` Float32Array derived from `beats[].time`, and return the
/// enriched object. Frees the heap-allocated JSON string. Throws a JS error on
/// C-ABI failure or JSON parse failure.
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

}  // namespace sonare_node

#define SONARE_NODE_TRY try {
#define SONARE_NODE_CATCH(env)                                                                \
  }                                                                                           \
  catch (const Napi::Error& e) {                                                              \
    e.ThrowAsJavaScriptException();                                                           \
    return env.Undefined();                                                                   \
  }                                                                                           \
  catch (const sonare::SonareException& e) {                                                  \
    sonare_node::ThrowSonareErrorMessage(env, sonare_node::CErrorFromException(e), e.what()); \
    return env.Undefined();                                                                   \
  }                                                                                           \
  catch (const std::bad_alloc&) {                                                             \
    sonare_node::ThrowSonareError(env, SONARE_ERROR_OUT_OF_MEMORY);                           \
    return env.Undefined();                                                                   \
  }                                                                                           \
  catch (const std::exception& e) {                                                           \
    Napi::Error::New(env, e.what()).ThrowAsJavaScriptException();                             \
    return env.Undefined();                                                                   \
  }                                                                                           \
  catch (...) {                                                                               \
    Napi::Error::New(env, "Unknown error").ThrowAsJavaScriptException();                      \
    return env.Undefined();                                                                   \
  }

#endif  // SONARE_NODE_SONARE_WRAP_UTILS_H_
