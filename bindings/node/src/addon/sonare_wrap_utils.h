#ifndef SONARE_NODE_SONARE_WRAP_UTILS_H_
#define SONARE_NODE_SONARE_WRAP_UTILS_H_

#include <napi.h>

#include <exception>
#include <new>
#include <vector>

#include "mastering/api/named_processor.h"
#include "sonare_c.h"
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

bool IsFloat32Array(const Napi::Value& value);
bool IsUint8Array(const Napi::Value& value);
bool IsInt32Array(const Napi::Value& value);

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

/// @brief Run sonare_analyze_json, parse the result with JSON.parse, inject a
/// legacy `beatTimes` Float32Array derived from `beats[].time`, and return the
/// enriched object. Frees the heap-allocated JSON string. Throws a JS error on
/// C-ABI failure or JSON parse failure.
///
/// @return The enriched JS object or env.Undefined() (exception already thrown).
Napi::Value FullAnalysisJsonToObject(Napi::Env env, const float* data, size_t length,
                                     int sample_rate);

/// @brief Convert a JS object of {name -> number|boolean} into mastering API params.
std::vector<sonare::mastering::api::Param> ParamsFromObject(const Napi::Object& object);

}  // namespace sonare_node

#define SONARE_NODE_TRY try {
#define SONARE_NODE_CATCH(env)                                                                \
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
