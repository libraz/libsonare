#ifndef SONARE_NODE_SONARE_WRAP_H_
#define SONARE_NODE_SONARE_WRAP_H_

#include <napi.h>

#include "sonare_c.h"

class SonareWrap : public Napi::ObjectWrap<SonareWrap> {
 public:
  static Napi::Object Init(Napi::Env env, Napi::Object exports);
  explicit SonareWrap(const Napi::CallbackInfo& info);
  ~SonareWrap();

 private:
  // Instance methods (Audio object)
  Napi::Value GetData(const Napi::CallbackInfo& info);
  Napi::Value GetLength(const Napi::CallbackInfo& info);
  Napi::Value GetSampleRate(const Napi::CallbackInfo& info);
  Napi::Value GetDuration(const Napi::CallbackInfo& info);
  void Destroy(const Napi::CallbackInfo& info);

  // Static factory methods (return new SonareWrap instances)
  static Napi::Value FromFile(const Napi::CallbackInfo& info);
  static Napi::Value FromBuffer(const Napi::CallbackInfo& info);
  static Napi::Value FromMemory(const Napi::CallbackInfo& info);

  // Static analysis functions (standalone, exported on module)
  static Napi::Value DetectBpm(const Napi::CallbackInfo& info);
  static Napi::Value DetectKey(const Napi::CallbackInfo& info);
  static Napi::Value DetectBeats(const Napi::CallbackInfo& info);
  static Napi::Value DetectOnsets(const Napi::CallbackInfo& info);
  static Napi::Value Analyze(const Napi::CallbackInfo& info);
  static Napi::Value Version(const Napi::CallbackInfo& info);

  // Helper to get pitch class name string
  static const char* PitchClassName(SonarePitchClass pc);
  // Helper to get mode name string
  static const char* ModeName(SonareMode mode);

  SonareAudio* audio_;

  // Reference to the constructor function for creating instances from static methods
  static Napi::FunctionReference constructor_;
};

#endif  // SONARE_NODE_SONARE_WRAP_H_
