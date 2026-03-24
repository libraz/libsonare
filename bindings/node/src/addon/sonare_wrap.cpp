#include "sonare_wrap.h"

#include <cstring>
#include <string>
#include <vector>

#include "core/audio.h"
#include "core/convert.h"
#include "core/resample.h"
#include "core/spectrum.h"
#include "effects/hpss.h"
#include "effects/normalize.h"
#include "effects/pitch_shift.h"
#include "effects/time_stretch.h"
#include "feature/chroma.h"
#include "feature/mel_spectrogram.h"
#include "feature/pitch.h"
#include "feature/spectral.h"

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

Napi::Float32Array SonareWrap::VecToFloat32(Napi::Env env, const std::vector<float>& vec) {
  auto result = Napi::Float32Array::New(env, vec.size());
  if (!vec.empty()) {
    std::memcpy(result.Data(), vec.data(), vec.size() * sizeof(float));
  }
  return result;
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

  // Effects
  exports.Set("hpss", Napi::Function::New(env, &SonareWrap::Hpss, "hpss"));
  exports.Set("harmonic", Napi::Function::New(env, &SonareWrap::Harmonic, "harmonic"));
  exports.Set("percussive", Napi::Function::New(env, &SonareWrap::Percussive, "percussive"));
  exports.Set("timeStretch", Napi::Function::New(env, &SonareWrap::TimeStretch, "timeStretch"));
  exports.Set("pitchShift", Napi::Function::New(env, &SonareWrap::PitchShift, "pitchShift"));
  exports.Set("normalize", Napi::Function::New(env, &SonareWrap::Normalize, "normalize"));
  exports.Set("trim", Napi::Function::New(env, &SonareWrap::Trim, "trim"));

  // Features - Spectrogram
  exports.Set("stft", Napi::Function::New(env, &SonareWrap::Stft, "stft"));
  exports.Set("stftDb", Napi::Function::New(env, &SonareWrap::StftDb, "stftDb"));

  // Features - Mel
  exports.Set("melSpectrogram",
              Napi::Function::New(env, &SonareWrap::MelSpectrogramFn, "melSpectrogram"));
  exports.Set("mfcc", Napi::Function::New(env, &SonareWrap::Mfcc, "mfcc"));

  // Features - Chroma
  exports.Set("chroma", Napi::Function::New(env, &SonareWrap::ChromaFn, "chroma"));

  // Features - Spectral
  exports.Set("spectralCentroid",
              Napi::Function::New(env, &SonareWrap::SpectralCentroid, "spectralCentroid"));
  exports.Set("spectralBandwidth",
              Napi::Function::New(env, &SonareWrap::SpectralBandwidth, "spectralBandwidth"));
  exports.Set("spectralRolloff",
              Napi::Function::New(env, &SonareWrap::SpectralRolloff, "spectralRolloff"));
  exports.Set("spectralFlatness",
              Napi::Function::New(env, &SonareWrap::SpectralFlatness, "spectralFlatness"));
  exports.Set("zeroCrossingRate",
              Napi::Function::New(env, &SonareWrap::ZeroCrossingRate, "zeroCrossingRate"));
  exports.Set("rmsEnergy", Napi::Function::New(env, &SonareWrap::RmsEnergy, "rmsEnergy"));

  // Features - Pitch
  exports.Set("pitchYin", Napi::Function::New(env, &SonareWrap::PitchYin, "pitchYin"));
  exports.Set("pitchPyin", Napi::Function::New(env, &SonareWrap::PitchPyin, "pitchPyin"));

  // Core - Conversion
  exports.Set("hzToMel", Napi::Function::New(env, &SonareWrap::HzToMel, "hzToMel"));
  exports.Set("melToHz", Napi::Function::New(env, &SonareWrap::MelToHz, "melToHz"));
  exports.Set("hzToMidi", Napi::Function::New(env, &SonareWrap::HzToMidi, "hzToMidi"));
  exports.Set("midiToHz", Napi::Function::New(env, &SonareWrap::MidiToHz, "midiToHz"));
  exports.Set("hzToNote", Napi::Function::New(env, &SonareWrap::HzToNote, "hzToNote"));
  exports.Set("noteToHz", Napi::Function::New(env, &SonareWrap::NoteToHz, "noteToHz"));
  exports.Set("framesToTime",
              Napi::Function::New(env, &SonareWrap::FramesToTime, "framesToTime"));
  exports.Set("timeToFrames",
              Napi::Function::New(env, &SonareWrap::TimeToFrames, "timeToFrames"));

  // Core - Resample
  exports.Set("resample", Napi::Function::New(env, &SonareWrap::Resample, "resample"));

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

void SonareWrap::Destroy(const Napi::CallbackInfo& /*info*/) {
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

// ============================================================================
// Effects
// ============================================================================

Napi::Value SonareWrap::Hpss(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 2 || !info[0].IsTypedArray() || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, ...)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = info[1].As<Napi::Number>().Int32Value();
  int kernel_harmonic = info.Length() >= 3 && info[2].IsNumber()
                            ? info[2].As<Napi::Number>().Int32Value()
                            : 31;
  int kernel_percussive = info.Length() >= 4 && info[3].IsNumber()
                              ? info[3].As<Napi::Number>().Int32Value()
                              : 31;

  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);

  sonare::HpssConfig config;
  config.kernel_size_harmonic = kernel_harmonic;
  config.kernel_size_percussive = kernel_percussive;

  sonare::HpssAudioResult result = sonare::hpss(audio, config);

  Napi::Object out = Napi::Object::New(env);

  std::vector<float> harmonic_vec(result.harmonic.data(),
                                  result.harmonic.data() + result.harmonic.size());
  out.Set("harmonic", VecToFloat32(env, harmonic_vec));

  std::vector<float> percussive_vec(result.percussive.data(),
                                    result.percussive.data() + result.percussive.size());
  out.Set("percussive", VecToFloat32(env, percussive_vec));

  out.Set("sampleRate", Napi::Number::New(env, result.harmonic.sample_rate()));

  return out;
}

Napi::Value SonareWrap::Harmonic(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 2 || !info[0].IsTypedArray() || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = info[1].As<Napi::Number>().Int32Value();

  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::Audio result = sonare::harmonic(audio);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return VecToFloat32(env, out_vec);
}

Napi::Value SonareWrap::Percussive(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 2 || !info[0].IsTypedArray() || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = info[1].As<Napi::Number>().Int32Value();

  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::Audio result = sonare::percussive(audio);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return VecToFloat32(env, out_vec);
}

Napi::Value SonareWrap::TimeStretch(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 3 || !info[0].IsTypedArray() || !info[1].IsNumber() ||
      !info[2].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, rate)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = info[1].As<Napi::Number>().Int32Value();
  float rate = info[2].As<Napi::Number>().FloatValue();

  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::Audio result = sonare::time_stretch(audio, rate);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return VecToFloat32(env, out_vec);
}

Napi::Value SonareWrap::PitchShift(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 3 || !info[0].IsTypedArray() || !info[1].IsNumber() ||
      !info[2].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, semitones)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = info[1].As<Napi::Number>().Int32Value();
  float semitones = info[2].As<Napi::Number>().FloatValue();

  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::Audio result = sonare::pitch_shift(audio, semitones);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return VecToFloat32(env, out_vec);
}

Napi::Value SonareWrap::Normalize(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 2 || !info[0].IsTypedArray() || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, ...)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = info[1].As<Napi::Number>().Int32Value();
  float target_db = info.Length() >= 3 && info[2].IsNumber()
                        ? info[2].As<Napi::Number>().FloatValue()
                        : 0.0f;

  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::Audio result = sonare::normalize(audio, target_db);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return VecToFloat32(env, out_vec);
}

Napi::Value SonareWrap::Trim(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 2 || !info[0].IsTypedArray() || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, ...)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = info[1].As<Napi::Number>().Int32Value();
  float threshold_db = info.Length() >= 3 && info[2].IsNumber()
                           ? info[2].As<Napi::Number>().FloatValue()
                           : -60.0f;

  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::Audio result = sonare::trim(audio, threshold_db);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return VecToFloat32(env, out_vec);
}

// ============================================================================
// Features - Spectrogram
// ============================================================================

Napi::Value SonareWrap::Stft(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !info[0].IsTypedArray()) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = info.Length() >= 2 && info[1].IsNumber()
               ? info[1].As<Napi::Number>().Int32Value()
               : 22050;
  int n_fft = info.Length() >= 3 && info[2].IsNumber()
                  ? info[2].As<Napi::Number>().Int32Value()
                  : 2048;
  int hop_length = info.Length() >= 4 && info[3].IsNumber()
                       ? info[3].As<Napi::Number>().Int32Value()
                       : 512;

  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::StftConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;

  sonare::Spectrogram spec = sonare::Spectrogram::compute(audio, config);

  Napi::Object out = Napi::Object::New(env);
  out.Set("nBins", Napi::Number::New(env, spec.n_bins()));
  out.Set("nFrames", Napi::Number::New(env, spec.n_frames()));
  out.Set("nFft", Napi::Number::New(env, spec.n_fft()));
  out.Set("hopLength", Napi::Number::New(env, spec.hop_length()));
  out.Set("sampleRate", Napi::Number::New(env, spec.sample_rate()));
  out.Set("magnitude", VecToFloat32(env, spec.magnitude()));
  out.Set("power", VecToFloat32(env, spec.power()));

  return out;
}

Napi::Value SonareWrap::StftDb(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !info[0].IsTypedArray()) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = info.Length() >= 2 && info[1].IsNumber()
               ? info[1].As<Napi::Number>().Int32Value()
               : 22050;
  int n_fft = info.Length() >= 3 && info[2].IsNumber()
                  ? info[2].As<Napi::Number>().Int32Value()
                  : 2048;
  int hop_length = info.Length() >= 4 && info[3].IsNumber()
                       ? info[3].As<Napi::Number>().Int32Value()
                       : 512;

  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::StftConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;

  sonare::Spectrogram spec = sonare::Spectrogram::compute(audio, config);

  Napi::Object out = Napi::Object::New(env);
  out.Set("nBins", Napi::Number::New(env, spec.n_bins()));
  out.Set("nFrames", Napi::Number::New(env, spec.n_frames()));
  out.Set("db", VecToFloat32(env, spec.to_db()));

  return out;
}

// ============================================================================
// Features - Mel
// ============================================================================

Napi::Value SonareWrap::MelSpectrogramFn(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !info[0].IsTypedArray()) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = info.Length() >= 2 && info[1].IsNumber()
               ? info[1].As<Napi::Number>().Int32Value()
               : 22050;
  int n_fft = info.Length() >= 3 && info[2].IsNumber()
                  ? info[2].As<Napi::Number>().Int32Value()
                  : 2048;
  int hop_length = info.Length() >= 4 && info[3].IsNumber()
                       ? info[3].As<Napi::Number>().Int32Value()
                       : 512;
  int n_mels = info.Length() >= 5 && info[4].IsNumber()
                   ? info[4].As<Napi::Number>().Int32Value()
                   : 128;

  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::MelConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  config.n_mels = n_mels;

  sonare::MelSpectrogram mel = sonare::MelSpectrogram::compute(audio, config);

  Napi::Object out = Napi::Object::New(env);
  out.Set("nMels", Napi::Number::New(env, mel.n_mels()));
  out.Set("nFrames", Napi::Number::New(env, mel.n_frames()));
  out.Set("sampleRate", Napi::Number::New(env, mel.sample_rate()));
  out.Set("hopLength", Napi::Number::New(env, mel.hop_length()));

  // Power values
  std::vector<float> power_vec(mel.power_data(),
                               mel.power_data() + mel.n_mels() * mel.n_frames());
  out.Set("power", VecToFloat32(env, power_vec));

  // dB values
  out.Set("db", VecToFloat32(env, mel.to_db()));

  return out;
}

Napi::Value SonareWrap::Mfcc(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !info[0].IsTypedArray()) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = info.Length() >= 2 && info[1].IsNumber()
               ? info[1].As<Napi::Number>().Int32Value()
               : 22050;
  int n_fft = info.Length() >= 3 && info[2].IsNumber()
                  ? info[2].As<Napi::Number>().Int32Value()
                  : 2048;
  int hop_length = info.Length() >= 4 && info[3].IsNumber()
                       ? info[3].As<Napi::Number>().Int32Value()
                       : 512;
  int n_mels = info.Length() >= 5 && info[4].IsNumber()
                   ? info[4].As<Napi::Number>().Int32Value()
                   : 128;
  int n_mfcc = info.Length() >= 6 && info[5].IsNumber()
                   ? info[5].As<Napi::Number>().Int32Value()
                   : 13;

  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::MelConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  config.n_mels = n_mels;

  sonare::MelSpectrogram mel = sonare::MelSpectrogram::compute(audio, config);
  std::vector<float> mfcc_coeffs = mel.mfcc(n_mfcc);

  Napi::Object out = Napi::Object::New(env);
  out.Set("nMfcc", Napi::Number::New(env, n_mfcc));
  out.Set("nFrames", Napi::Number::New(env, mel.n_frames()));
  out.Set("coefficients", VecToFloat32(env, mfcc_coeffs));

  return out;
}

// ============================================================================
// Features - Chroma
// ============================================================================

Napi::Value SonareWrap::ChromaFn(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !info[0].IsTypedArray()) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = info.Length() >= 2 && info[1].IsNumber()
               ? info[1].As<Napi::Number>().Int32Value()
               : 22050;
  int n_fft = info.Length() >= 3 && info[2].IsNumber()
                  ? info[2].As<Napi::Number>().Int32Value()
                  : 2048;
  int hop_length = info.Length() >= 4 && info[3].IsNumber()
                       ? info[3].As<Napi::Number>().Int32Value()
                       : 512;

  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::ChromaConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;

  sonare::Chroma chroma = sonare::Chroma::compute(audio, config);

  Napi::Object out = Napi::Object::New(env);
  out.Set("nChroma", Napi::Number::New(env, chroma.n_chroma()));
  out.Set("nFrames", Napi::Number::New(env, chroma.n_frames()));
  out.Set("sampleRate", Napi::Number::New(env, chroma.sample_rate()));
  out.Set("hopLength", Napi::Number::New(env, chroma.hop_length()));

  std::vector<float> features_vec(chroma.data(),
                                  chroma.data() + chroma.n_chroma() * chroma.n_frames());
  out.Set("features", VecToFloat32(env, features_vec));

  // Mean energy per pitch class
  auto mean = chroma.mean_energy();
  Napi::Array mean_arr = Napi::Array::New(env, 12);
  for (int i = 0; i < 12; ++i) {
    mean_arr.Set(static_cast<uint32_t>(i), Napi::Number::New(env, mean[i]));
  }
  out.Set("meanEnergy", mean_arr);

  return out;
}

// ============================================================================
// Features - Spectral
// ============================================================================

Napi::Value SonareWrap::SpectralCentroid(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !info[0].IsTypedArray()) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = info.Length() >= 2 && info[1].IsNumber()
               ? info[1].As<Napi::Number>().Int32Value()
               : 22050;
  int n_fft = info.Length() >= 3 && info[2].IsNumber()
                  ? info[2].As<Napi::Number>().Int32Value()
                  : 2048;
  int hop_length = info.Length() >= 4 && info[3].IsNumber()
                       ? info[3].As<Napi::Number>().Int32Value()
                       : 512;

  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::StftConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;

  sonare::Spectrogram spec = sonare::Spectrogram::compute(audio, config);
  std::vector<float> centroid = sonare::spectral_centroid(spec, sr);

  return VecToFloat32(env, centroid);
}

Napi::Value SonareWrap::SpectralBandwidth(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !info[0].IsTypedArray()) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = info.Length() >= 2 && info[1].IsNumber()
               ? info[1].As<Napi::Number>().Int32Value()
               : 22050;
  int n_fft = info.Length() >= 3 && info[2].IsNumber()
                  ? info[2].As<Napi::Number>().Int32Value()
                  : 2048;
  int hop_length = info.Length() >= 4 && info[3].IsNumber()
                       ? info[3].As<Napi::Number>().Int32Value()
                       : 512;

  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::StftConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;

  sonare::Spectrogram spec = sonare::Spectrogram::compute(audio, config);
  std::vector<float> bandwidth = sonare::spectral_bandwidth(spec, sr);

  return VecToFloat32(env, bandwidth);
}

Napi::Value SonareWrap::SpectralRolloff(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !info[0].IsTypedArray()) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = info.Length() >= 2 && info[1].IsNumber()
               ? info[1].As<Napi::Number>().Int32Value()
               : 22050;
  int n_fft = info.Length() >= 3 && info[2].IsNumber()
                  ? info[2].As<Napi::Number>().Int32Value()
                  : 2048;
  int hop_length = info.Length() >= 4 && info[3].IsNumber()
                       ? info[3].As<Napi::Number>().Int32Value()
                       : 512;
  float roll_percent = info.Length() >= 5 && info[4].IsNumber()
                           ? info[4].As<Napi::Number>().FloatValue()
                           : 0.85f;

  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::StftConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;

  sonare::Spectrogram spec = sonare::Spectrogram::compute(audio, config);
  std::vector<float> rolloff = sonare::spectral_rolloff(spec, sr, roll_percent);

  return VecToFloat32(env, rolloff);
}

Napi::Value SonareWrap::SpectralFlatness(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !info[0].IsTypedArray()) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = info.Length() >= 2 && info[1].IsNumber()
               ? info[1].As<Napi::Number>().Int32Value()
               : 22050;
  int n_fft = info.Length() >= 3 && info[2].IsNumber()
                  ? info[2].As<Napi::Number>().Int32Value()
                  : 2048;
  int hop_length = info.Length() >= 4 && info[3].IsNumber()
                       ? info[3].As<Napi::Number>().Int32Value()
                       : 512;

  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::StftConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;

  sonare::Spectrogram spec = sonare::Spectrogram::compute(audio, config);
  std::vector<float> flatness = sonare::spectral_flatness(spec);

  return VecToFloat32(env, flatness);
}

Napi::Value SonareWrap::ZeroCrossingRate(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !info[0].IsTypedArray()) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = info.Length() >= 2 && info[1].IsNumber()
               ? info[1].As<Napi::Number>().Int32Value()
               : 22050;
  int frame_length = info.Length() >= 3 && info[2].IsNumber()
                         ? info[2].As<Napi::Number>().Int32Value()
                         : 2048;
  int hop_length = info.Length() >= 4 && info[3].IsNumber()
                       ? info[3].As<Napi::Number>().Int32Value()
                       : 512;

  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  std::vector<float> zcr = sonare::zero_crossing_rate(audio, frame_length, hop_length);

  return VecToFloat32(env, zcr);
}

Napi::Value SonareWrap::RmsEnergy(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !info[0].IsTypedArray()) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = info.Length() >= 2 && info[1].IsNumber()
               ? info[1].As<Napi::Number>().Int32Value()
               : 22050;
  int frame_length = info.Length() >= 3 && info[2].IsNumber()
                         ? info[2].As<Napi::Number>().Int32Value()
                         : 2048;
  int hop_length = info.Length() >= 4 && info[3].IsNumber()
                       ? info[3].As<Napi::Number>().Int32Value()
                       : 512;

  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  std::vector<float> rms = sonare::rms_energy(audio, frame_length, hop_length);

  return VecToFloat32(env, rms);
}

// ============================================================================
// Features - Pitch
// ============================================================================

Napi::Value SonareWrap::PitchYin(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !info[0].IsTypedArray()) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = info.Length() >= 2 && info[1].IsNumber()
               ? info[1].As<Napi::Number>().Int32Value()
               : 22050;
  int frame_length = info.Length() >= 3 && info[2].IsNumber()
                         ? info[2].As<Napi::Number>().Int32Value()
                         : 2048;
  int hop_length = info.Length() >= 4 && info[3].IsNumber()
                       ? info[3].As<Napi::Number>().Int32Value()
                       : 512;
  float fmin = info.Length() >= 5 && info[4].IsNumber()
                   ? info[4].As<Napi::Number>().FloatValue()
                   : 65.0f;
  float fmax = info.Length() >= 6 && info[5].IsNumber()
                   ? info[5].As<Napi::Number>().FloatValue()
                   : 2093.0f;
  float threshold = info.Length() >= 7 && info[6].IsNumber()
                        ? info[6].As<Napi::Number>().FloatValue()
                        : 0.3f;

  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::PitchConfig config;
  config.frame_length = frame_length;
  config.hop_length = hop_length;
  config.fmin = fmin;
  config.fmax = fmax;
  config.threshold = threshold;

  sonare::PitchResult result = sonare::yin_track(audio, config);

  Napi::Object out = Napi::Object::New(env);
  out.Set("f0", VecToFloat32(env, result.f0));
  out.Set("voicedProb", VecToFloat32(env, result.voiced_prob));

  // Convert voiced_flag to array of bools
  Napi::Array voiced_arr = Napi::Array::New(env, result.voiced_flag.size());
  for (size_t i = 0; i < result.voiced_flag.size(); ++i) {
    voiced_arr.Set(static_cast<uint32_t>(i),
                   Napi::Boolean::New(env, static_cast<bool>(result.voiced_flag[i])));
  }
  out.Set("voicedFlag", voiced_arr);

  out.Set("nFrames", Napi::Number::New(env, result.n_frames()));
  out.Set("medianF0", Napi::Number::New(env, static_cast<double>(result.median_f0())));
  out.Set("meanF0", Napi::Number::New(env, static_cast<double>(result.mean_f0())));

  return out;
}

Napi::Value SonareWrap::PitchPyin(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !info[0].IsTypedArray()) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = info.Length() >= 2 && info[1].IsNumber()
               ? info[1].As<Napi::Number>().Int32Value()
               : 22050;
  int frame_length = info.Length() >= 3 && info[2].IsNumber()
                         ? info[2].As<Napi::Number>().Int32Value()
                         : 2048;
  int hop_length = info.Length() >= 4 && info[3].IsNumber()
                       ? info[3].As<Napi::Number>().Int32Value()
                       : 512;
  float fmin = info.Length() >= 5 && info[4].IsNumber()
                   ? info[4].As<Napi::Number>().FloatValue()
                   : 65.0f;
  float fmax = info.Length() >= 6 && info[5].IsNumber()
                   ? info[5].As<Napi::Number>().FloatValue()
                   : 2093.0f;
  float threshold = info.Length() >= 7 && info[6].IsNumber()
                        ? info[6].As<Napi::Number>().FloatValue()
                        : 0.3f;

  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::PitchConfig config;
  config.frame_length = frame_length;
  config.hop_length = hop_length;
  config.fmin = fmin;
  config.fmax = fmax;
  config.threshold = threshold;

  sonare::PitchResult result = sonare::pyin(audio, config);

  Napi::Object out = Napi::Object::New(env);
  out.Set("f0", VecToFloat32(env, result.f0));
  out.Set("voicedProb", VecToFloat32(env, result.voiced_prob));

  // Convert voiced_flag to array of bools
  Napi::Array voiced_arr = Napi::Array::New(env, result.voiced_flag.size());
  for (size_t i = 0; i < result.voiced_flag.size(); ++i) {
    voiced_arr.Set(static_cast<uint32_t>(i),
                   Napi::Boolean::New(env, static_cast<bool>(result.voiced_flag[i])));
  }
  out.Set("voicedFlag", voiced_arr);

  out.Set("nFrames", Napi::Number::New(env, result.n_frames()));
  out.Set("medianF0", Napi::Number::New(env, static_cast<double>(result.median_f0())));
  out.Set("meanF0", Napi::Number::New(env, static_cast<double>(result.mean_f0())));

  return out;
}

// ============================================================================
// Core - Conversion
// ============================================================================

Napi::Value SonareWrap::HzToMel(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "Expected number argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  float hz = info[0].As<Napi::Number>().FloatValue();
  return Napi::Number::New(env, static_cast<double>(sonare::hz_to_mel(hz)));
}

Napi::Value SonareWrap::MelToHz(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "Expected number argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  float mel = info[0].As<Napi::Number>().FloatValue();
  return Napi::Number::New(env, static_cast<double>(sonare::mel_to_hz(mel)));
}

Napi::Value SonareWrap::HzToMidi(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "Expected number argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  float hz = info[0].As<Napi::Number>().FloatValue();
  return Napi::Number::New(env, static_cast<double>(sonare::hz_to_midi(hz)));
}

Napi::Value SonareWrap::MidiToHz(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "Expected number argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  float midi = info[0].As<Napi::Number>().FloatValue();
  return Napi::Number::New(env, static_cast<double>(sonare::midi_to_hz(midi)));
}

Napi::Value SonareWrap::HzToNote(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "Expected number argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  float hz = info[0].As<Napi::Number>().FloatValue();
  return Napi::String::New(env, sonare::hz_to_note(hz));
}

Napi::Value SonareWrap::NoteToHz(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "Expected string argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  std::string note = info[0].As<Napi::String>().Utf8Value();
  return Napi::Number::New(env, static_cast<double>(sonare::note_to_hz(note)));
}

Napi::Value SonareWrap::FramesToTime(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 3 || !info[0].IsNumber() || !info[1].IsNumber() || !info[2].IsNumber()) {
    Napi::TypeError::New(env, "Expected (frames, sr, hopLength)").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  int frames = info[0].As<Napi::Number>().Int32Value();
  int sr = info[1].As<Napi::Number>().Int32Value();
  int hop_length = info[2].As<Napi::Number>().Int32Value();

  return Napi::Number::New(env,
                           static_cast<double>(sonare::frames_to_time(frames, sr, hop_length)));
}

Napi::Value SonareWrap::TimeToFrames(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 3 || !info[0].IsNumber() || !info[1].IsNumber() || !info[2].IsNumber()) {
    Napi::TypeError::New(env, "Expected (time, sr, hopLength)").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  float time = info[0].As<Napi::Number>().FloatValue();
  int sr = info[1].As<Napi::Number>().Int32Value();
  int hop_length = info[2].As<Napi::Number>().Int32Value();

  return Napi::Number::New(env, sonare::time_to_frames(time, sr, hop_length));
}

// ============================================================================
// Core - Resample
// ============================================================================

Napi::Value SonareWrap::Resample(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 3 || !info[0].IsTypedArray() || !info[1].IsNumber() ||
      !info[2].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, srcSr, targetSr)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int src_sr = info[1].As<Napi::Number>().Int32Value();
  int target_sr = info[2].As<Napi::Number>().Int32Value();

  std::vector<float> result = sonare::resample(data, length, src_sr, target_sr);
  return VecToFloat32(env, result);
}
