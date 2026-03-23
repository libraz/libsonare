#include "sonare_wrap.h"

#include <cstring>
#include <string>

Napi::FunctionReference SonareWrap::constructor_;

const char* SonareWrap::PitchClassName(SonarePitchClass pc) {
  static const char* names[] = {
      "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  int idx = static_cast<int>(pc);
  if (idx < 0 || idx > 11) return "C";
  return names[idx];
}

const char* SonareWrap::ModeName(SonareMode mode) {
  return mode == SONARE_MODE_MAJOR ? "major" : "minor";
}

Napi::Object SonareWrap::Init(Napi::Env env, Napi::Object exports) {
  Napi::Function func = DefineClass(
      env, "Audio",
      {
          InstanceMethod<&SonareWrap::GetData>("getData"),
          InstanceMethod<&SonareWrap::GetLength>("getLength"),
          InstanceMethod<&SonareWrap::GetSampleRate>("getSampleRate"),
          InstanceMethod<&SonareWrap::GetDuration>("getDuration"),
          InstanceMethod<&SonareWrap::Destroy>("destroy"),
          StaticMethod<&SonareWrap::FromFile>("fromFile"),
          StaticMethod<&SonareWrap::FromBuffer>("fromBuffer"),
          StaticMethod<&SonareWrap::FromMemory>("fromMemory"),
      });

  constructor_ = Napi::Persistent(func);
  constructor_.SuppressDestruct();

  exports.Set("Audio", func);

  // Standalone analysis functions
  exports.Set("detectBpm", Napi::Function::New(env, &SonareWrap::DetectBpm, "detectBpm"));
  exports.Set("detectKey", Napi::Function::New(env, &SonareWrap::DetectKey, "detectKey"));
  exports.Set("detectBeats", Napi::Function::New(env, &SonareWrap::DetectBeats, "detectBeats"));
  exports.Set("detectOnsets",
              Napi::Function::New(env, &SonareWrap::DetectOnsets, "detectOnsets"));
  exports.Set("analyze", Napi::Function::New(env, &SonareWrap::Analyze, "analyze"));
  exports.Set("version", Napi::Function::New(env, &SonareWrap::Version, "version"));

  return exports;
}

SonareWrap::SonareWrap(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<SonareWrap>(info), audio_(nullptr) {
  // If called with an External, extract the audio pointer (factory pattern)
  if (info.Length() >= 1 && info[0].IsExternal()) {
    audio_ = info[0].As<Napi::External<SonareAudio>>().Data();
  }
}

SonareWrap::~SonareWrap() {
  if (audio_) {
    sonare_audio_free(audio_);
    audio_ = nullptr;
  }
}

// --- Static factory methods ---

Napi::Value SonareWrap::FromFile(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "Expected string path argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  std::string path = info[0].As<Napi::String>().Utf8Value();

  SonareAudio* audio = nullptr;
  SonareError err = sonare_audio_from_file(path.c_str(), &audio);
  if (err != SONARE_OK) {
    Napi::Error::New(env, sonare_error_message(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }

  Napi::External<SonareAudio> external = Napi::External<SonareAudio>::New(env, audio);
  return constructor_.New({external});
}

Napi::Value SonareWrap::FromBuffer(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !info[0].IsTypedArray()) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();

  int sample_rate = 22050;
  if (info.Length() >= 2 && info[1].IsNumber()) {
    sample_rate = info[1].As<Napi::Number>().Int32Value();
  }

  SonareAudio* audio = nullptr;
  SonareError err = sonare_audio_from_buffer(data, length, sample_rate, &audio);
  if (err != SONARE_OK) {
    Napi::Error::New(env, sonare_error_message(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }

  Napi::External<SonareAudio> external = Napi::External<SonareAudio>::New(env, audio);
  return constructor_.New({external});
}

Napi::Value SonareWrap::FromMemory(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  const uint8_t* data = nullptr;
  size_t len = 0;

  if (info.Length() < 1) {
    Napi::TypeError::New(env, "Expected Buffer or Uint8Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  if (info[0].IsBuffer()) {
    auto buf = info[0].As<Napi::Buffer<uint8_t>>();
    data = buf.Data();
    len = buf.Length();
  } else if (info[0].IsTypedArray()) {
    auto arr = info[0].As<Napi::Uint8Array>();
    data = arr.Data();
    len = arr.ByteLength();
  } else {
    Napi::TypeError::New(env, "Expected Buffer or Uint8Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SonareAudio* audio = nullptr;
  SonareError err = sonare_audio_from_memory(data, len, &audio);
  if (err != SONARE_OK) {
    Napi::Error::New(env, sonare_error_message(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }

  Napi::External<SonareAudio> external = Napi::External<SonareAudio>::New(env, audio);
  return constructor_.New({external});
}

// --- Instance methods ---

Napi::Value SonareWrap::GetData(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!audio_) {
    Napi::Error::New(env, "Audio has been destroyed").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  const float* data = sonare_audio_data(audio_);
  size_t length = sonare_audio_length(audio_);

  auto result = Napi::Float32Array::New(env, length);
  if (length > 0 && data != nullptr) {
    std::memcpy(result.Data(), data, length * sizeof(float));
  }
  return result;
}

Napi::Value SonareWrap::GetLength(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!audio_) {
    Napi::Error::New(env, "Audio has been destroyed").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  return Napi::Number::New(env, static_cast<double>(sonare_audio_length(audio_)));
}

Napi::Value SonareWrap::GetSampleRate(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!audio_) {
    Napi::Error::New(env, "Audio has been destroyed").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  return Napi::Number::New(env, sonare_audio_sample_rate(audio_));
}

Napi::Value SonareWrap::GetDuration(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!audio_) {
    Napi::Error::New(env, "Audio has been destroyed").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  return Napi::Number::New(env, static_cast<double>(sonare_audio_duration(audio_)));
}

void SonareWrap::Destroy(const Napi::CallbackInfo& info) {
  if (audio_) {
    sonare_audio_free(audio_);
    audio_ = nullptr;
  }
}

// --- Static analysis functions ---

Napi::Value SonareWrap::DetectBpm(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !info[0].IsTypedArray()) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();

  int sample_rate = 22050;
  if (info.Length() >= 2 && info[1].IsNumber()) {
    sample_rate = info[1].As<Napi::Number>().Int32Value();
  }

  float bpm = 0.0f;
  SonareError err = sonare_detect_bpm(data, length, sample_rate, &bpm);
  if (err != SONARE_OK) {
    Napi::Error::New(env, sonare_error_message(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }

  return Napi::Number::New(env, static_cast<double>(bpm));
}

Napi::Value SonareWrap::DetectKey(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !info[0].IsTypedArray()) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();

  int sample_rate = 22050;
  if (info.Length() >= 2 && info[1].IsNumber()) {
    sample_rate = info[1].As<Napi::Number>().Int32Value();
  }

  SonareKey key{};
  SonareError err = sonare_detect_key(data, length, sample_rate, &key);
  if (err != SONARE_OK) {
    Napi::Error::New(env, sonare_error_message(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }

  Napi::Object result = Napi::Object::New(env);
  result.Set("root", Napi::String::New(env, PitchClassName(key.root)));
  result.Set("mode", Napi::String::New(env, ModeName(key.mode)));
  result.Set("confidence", Napi::Number::New(env, static_cast<double>(key.confidence)));
  return result;
}

Napi::Value SonareWrap::DetectBeats(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !info[0].IsTypedArray()) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();

  int sample_rate = 22050;
  if (info.Length() >= 2 && info[1].IsNumber()) {
    sample_rate = info[1].As<Napi::Number>().Int32Value();
  }

  float* times = nullptr;
  size_t count = 0;
  SonareError err = sonare_detect_beats(data, length, sample_rate, &times, &count);
  if (err != SONARE_OK) {
    Napi::Error::New(env, sonare_error_message(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto result = Napi::Float32Array::New(env, count);
  if (count > 0 && times != nullptr) {
    std::memcpy(result.Data(), times, count * sizeof(float));
    sonare_free_floats(times);
  }
  return result;
}

Napi::Value SonareWrap::DetectOnsets(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !info[0].IsTypedArray()) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();

  int sample_rate = 22050;
  if (info.Length() >= 2 && info[1].IsNumber()) {
    sample_rate = info[1].As<Napi::Number>().Int32Value();
  }

  float* times = nullptr;
  size_t count = 0;
  SonareError err = sonare_detect_onsets(data, length, sample_rate, &times, &count);
  if (err != SONARE_OK) {
    Napi::Error::New(env, sonare_error_message(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto result = Napi::Float32Array::New(env, count);
  if (count > 0 && times != nullptr) {
    std::memcpy(result.Data(), times, count * sizeof(float));
    sonare_free_floats(times);
  }
  return result;
}

Napi::Value SonareWrap::Analyze(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !info[0].IsTypedArray()) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();

  int sample_rate = 22050;
  if (info.Length() >= 2 && info[1].IsNumber()) {
    sample_rate = info[1].As<Napi::Number>().Int32Value();
  }

  SonareAnalysisResult analysis{};
  SonareError err = sonare_analyze(data, length, sample_rate, &analysis);
  if (err != SONARE_OK) {
    Napi::Error::New(env, sonare_error_message(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }

  Napi::Object result = Napi::Object::New(env);
  result.Set("bpm", Napi::Number::New(env, static_cast<double>(analysis.bpm)));
  result.Set("bpmConfidence",
             Napi::Number::New(env, static_cast<double>(analysis.bpm_confidence)));

  // Key
  Napi::Object key = Napi::Object::New(env);
  key.Set("root", Napi::String::New(env, PitchClassName(analysis.key.root)));
  key.Set("mode", Napi::String::New(env, ModeName(analysis.key.mode)));
  key.Set("confidence",
          Napi::Number::New(env, static_cast<double>(analysis.key.confidence)));
  result.Set("key", key);

  // Time signature
  Napi::Object ts = Napi::Object::New(env);
  ts.Set("numerator", Napi::Number::New(env, analysis.time_signature.numerator));
  ts.Set("denominator", Napi::Number::New(env, analysis.time_signature.denominator));
  ts.Set("confidence",
         Napi::Number::New(env, static_cast<double>(analysis.time_signature.confidence)));
  result.Set("timeSignature", ts);

  // Beat times
  auto beats = Napi::Float32Array::New(env, analysis.beat_count);
  if (analysis.beat_count > 0 && analysis.beat_times != nullptr) {
    std::memcpy(beats.Data(), analysis.beat_times,
                analysis.beat_count * sizeof(float));
  }
  result.Set("beatTimes", beats);

  sonare_free_result(&analysis);

  return result;
}

Napi::Value SonareWrap::Version(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  return Napi::String::New(env, sonare_version());
}
