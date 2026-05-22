#ifndef SONARE_NODE_SONARE_WRAP_UTILS_H_
#define SONARE_NODE_SONARE_WRAP_UTILS_H_

#include <napi.h>

#include <exception>
#include <new>

#include "sonare_c.h"
#include "util/exception.h"

namespace sonare_node {

const char* ErrorMessageForCode(SonareError err);
bool IsFloat32Array(const Napi::Value& value);
bool IsUint8Array(const Napi::Value& value);
const char* PitchClassNameLocal(SonarePitchClass pc);
const char* ModeNameLocal(SonareMode mode);
const char* ChordQualityName(SonareChordQuality quality);
Napi::Object KeyToObject(Napi::Env env, SonarePitchClass root, SonareMode mode, float confidence);
Napi::Object AnalysisToObject(Napi::Env env, const SonareAnalysisResult& analysis);

}  // namespace sonare_node

#define SONARE_NODE_TRY try {
#define SONARE_NODE_CATCH(env)                                           \
  }                                                                      \
  catch (const sonare::SonareException& e) {                             \
    Napi::Error::New(env, e.what()).ThrowAsJavaScriptException();        \
    return env.Undefined();                                              \
  }                                                                      \
  catch (const std::bad_alloc&) {                                        \
    Napi::Error::New(env, "Out of memory").ThrowAsJavaScriptException(); \
    return env.Undefined();                                              \
  }                                                                      \
  catch (const std::exception& e) {                                      \
    Napi::Error::New(env, e.what()).ThrowAsJavaScriptException();        \
    return env.Undefined();                                              \
  }                                                                      \
  catch (...) {                                                          \
    Napi::Error::New(env, "Unknown error").ThrowAsJavaScriptException(); \
    return env.Undefined();                                              \
  }

#endif  // SONARE_NODE_SONARE_WRAP_UTILS_H_
