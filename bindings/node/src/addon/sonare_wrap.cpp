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
#include "effects/tts.h"
#include "feature/chroma.h"
#include "feature/mel_spectrogram.h"
#include "feature/pitch.h"
#include "feature/spectral.h"
#include "util/exception.h"

Napi::FunctionReference SonareWrap::constructor_;

namespace {

/// @brief Returns the most informative error string available for a C API failure.
/// @details Prefers the thread-local detailed message from @c sonare_last_error_message
///          (which carries the original @c SonareException::what() text, e.g. the
///          "Unsupported audio format: '.m4a'..." hint), falling back to the
///          generic code label if the detail string is empty. Never returns null.
const char* ErrorMessageForCode(SonareError err) {
  const char* detail = sonare_last_error_message();
  if (detail != nullptr && detail[0] != '\0') {
    return detail;
  }
  return sonare_error_message(err);
}

#define SONARE_NODE_TRY try {
#define SONARE_NODE_CATCH(env)                                                     \
  }                                                                                \
  catch (const sonare::SonareException& e) {                                       \
    Napi::Error::New(env, e.what()).ThrowAsJavaScriptException();                  \
    return env.Undefined();                                                        \
  }                                                                                \
  catch (const std::bad_alloc&) {                                                  \
    Napi::Error::New(env, "Out of memory").ThrowAsJavaScriptException();           \
    return env.Undefined();                                                        \
  }                                                                                \
  catch (const std::exception& e) {                                                \
    Napi::Error::New(env, e.what()).ThrowAsJavaScriptException();                  \
    return env.Undefined();                                                        \
  }                                                                                \
  catch (...) {                                                                    \
    Napi::Error::New(env, "Unknown error").ThrowAsJavaScriptException();           \
    return env.Undefined();                                                        \
  }

bool IsFloat32Array(const Napi::Value& value) {
  return value.IsTypedArray() &&
         value.As<Napi::TypedArray>().TypedArrayType() == napi_float32_array;
}

bool IsUint8Array(const Napi::Value& value) {
  return value.IsTypedArray() && value.As<Napi::TypedArray>().TypedArrayType() == napi_uint8_array;
}

const char* PitchClassNameLocal(SonarePitchClass pc) {
  static const char* names[] = {
      "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  int idx = static_cast<int>(pc);
  if (idx < 0 || idx > 11) return "C";
  return names[idx];
}

const char* ModeNameLocal(SonareMode mode) {
  return mode == SONARE_MODE_MAJOR ? "major" : "minor";
}

const char* ChordQualityName(SonareChordQuality quality) {
  switch (quality) {
    case SONARE_CHORD_MAJOR:
      return "major";
    case SONARE_CHORD_MINOR:
      return "minor";
    case SONARE_CHORD_DIMINISHED:
      return "diminished";
    case SONARE_CHORD_AUGMENTED:
      return "augmented";
    case SONARE_CHORD_DOMINANT7:
      return "dominant7";
    case SONARE_CHORD_MAJOR7:
      return "major7";
    case SONARE_CHORD_MINOR7:
      return "minor7";
    case SONARE_CHORD_SUS2:
      return "sus2";
    case SONARE_CHORD_SUS4:
      return "sus4";
    case SONARE_CHORD_UNKNOWN:
      return "unknown";
  }
  return "unknown";
}

}  // namespace

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

Napi::Object KeyToObject(Napi::Env env, SonarePitchClass root, SonareMode mode, float confidence) {
  Napi::Object key = Napi::Object::New(env);
  std::string root_name = PitchClassNameLocal(root);
  std::string mode_name = ModeNameLocal(mode);
  key.Set("root", Napi::String::New(env, root_name));
  key.Set("mode", Napi::String::New(env, mode_name));
  key.Set("confidence", Napi::Number::New(env, static_cast<double>(confidence)));
  key.Set("name", Napi::String::New(env, root_name + " " + mode_name));
  key.Set("shortName", Napi::String::New(env, root_name + (mode == SONARE_MODE_MAJOR ? "" : "m")));
  return key;
}

Napi::Object AnalysisToObject(Napi::Env env, const SonareAnalysisResult& analysis) {
  Napi::Object result = Napi::Object::New(env);
  result.Set("bpm", Napi::Number::New(env, static_cast<double>(analysis.bpm)));
  result.Set("bpmConfidence",
             Napi::Number::New(env, static_cast<double>(analysis.bpm_confidence)));
  result.Set("key", KeyToObject(env, analysis.key.root, analysis.key.mode, analysis.key.confidence));

  Napi::Object ts = Napi::Object::New(env);
  ts.Set("numerator", Napi::Number::New(env, analysis.time_signature.numerator));
  ts.Set("denominator", Napi::Number::New(env, analysis.time_signature.denominator));
  ts.Set("confidence",
         Napi::Number::New(env, static_cast<double>(analysis.time_signature.confidence)));
  result.Set("timeSignature", ts);

  auto beat_times = Napi::Float32Array::New(env, analysis.beat_count);
  Napi::Array beats = Napi::Array::New(env, analysis.beat_count);
  if (analysis.beat_count > 0 && analysis.beat_times != nullptr) {
    std::memcpy(beat_times.Data(), analysis.beat_times, analysis.beat_count * sizeof(float));
    for (size_t i = 0; i < analysis.beat_count; ++i) {
      Napi::Object beat = Napi::Object::New(env);
      beat.Set("time", Napi::Number::New(env, static_cast<double>(analysis.beat_times[i])));
      beat.Set("strength", env.Undefined());
      beats.Set(static_cast<uint32_t>(i), beat);
    }
  }
  result.Set("beatTimes", beat_times);
  result.Set("beats", beats);
  return result;
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
          InstanceMethod<&SonareWrap::DetectBpmInstance>("detectBpm"),
          InstanceMethod<&SonareWrap::DetectKeyInstance>("detectKey"),
          InstanceMethod<&SonareWrap::DetectBeatsInstance>("detectBeats"),
          InstanceMethod<&SonareWrap::DetectOnsetsInstance>("detectOnsets"),
          InstanceMethod<&SonareWrap::AnalyzeInstance>("analyze"),
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
  exports.Set("analyzeBpm", Napi::Function::New(env, &SonareWrap::AnalyzeBpm, "analyzeBpm"));
  exports.Set("analyzeRhythm",
              Napi::Function::New(env, &SonareWrap::AnalyzeRhythm, "analyzeRhythm"));
  exports.Set("analyzeDynamics",
              Napi::Function::New(env, &SonareWrap::AnalyzeDynamics, "analyzeDynamics"));
  exports.Set("analyzeTimbre",
              Napi::Function::New(env, &SonareWrap::AnalyzeTimbre, "analyzeTimbre"));
  exports.Set("detectChords",
              Napi::Function::New(env, &SonareWrap::DetectChords, "detectChords"));
  exports.Set("version", Napi::Function::New(env, &SonareWrap::Version, "version"));
  exports.Set(
      "hasFfmpegSupport",
      Napi::Function::New(env, &SonareWrap::HasFfmpegSupport, "hasFfmpegSupport"));

  // Effects
  exports.Set("hpss", Napi::Function::New(env, &SonareWrap::Hpss, "hpss"));
  exports.Set("harmonic", Napi::Function::New(env, &SonareWrap::Harmonic, "harmonic"));
  exports.Set("percussive", Napi::Function::New(env, &SonareWrap::Percussive, "percussive"));
  exports.Set("timeStretch", Napi::Function::New(env, &SonareWrap::TimeStretch, "timeStretch"));
  exports.Set("pitchShift", Napi::Function::New(env, &SonareWrap::PitchShift, "pitchShift"));
  exports.Set("normalize", Napi::Function::New(env, &SonareWrap::Normalize, "normalize"));
  exports.Set("trim", Napi::Function::New(env, &SonareWrap::Trim, "trim"));
  exports.Set("analyzeTtsQuality",
              Napi::Function::New(env, &SonareWrap::AnalyzeTtsQuality, "analyzeTtsQuality"));
  exports.Set("prepareTts", Napi::Function::New(env, &SonareWrap::PrepareTts, "prepareTts"));
  exports.Set("compressPauses",
              Napi::Function::New(env, &SonareWrap::CompressPauses, "compressPauses"));

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
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }

  Napi::External<SonareAudio> external = Napi::External<SonareAudio>::New(env, audio);
  return constructor_.New({external});
}

Napi::Value SonareWrap::FromBuffer(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
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
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
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
  } else if (IsUint8Array(info[0])) {
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
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
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

Napi::Value SonareWrap::DetectBpmInstance(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!audio_) {
    Napi::Error::New(env, "Audio has been destroyed").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  float bpm = 0.0f;
  SonareError err = sonare_detect_bpm(sonare_audio_data(audio_), sonare_audio_length(audio_),
                                      sonare_audio_sample_rate(audio_), &bpm);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return Napi::Number::New(env, static_cast<double>(bpm));
}

Napi::Value SonareWrap::DetectKeyInstance(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!audio_) {
    Napi::Error::New(env, "Audio has been destroyed").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SonareKey key{};
  SonareError err = sonare_detect_key(sonare_audio_data(audio_), sonare_audio_length(audio_),
                                      sonare_audio_sample_rate(audio_), &key);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return KeyToObject(env, key.root, key.mode, key.confidence);
}

Napi::Value SonareWrap::DetectBeatsInstance(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!audio_) {
    Napi::Error::New(env, "Audio has been destroyed").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  float* times = nullptr;
  size_t count = 0;
  SonareError err = sonare_detect_beats(sonare_audio_data(audio_), sonare_audio_length(audio_),
                                        sonare_audio_sample_rate(audio_), &times, &count);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto result = Napi::Float32Array::New(env, count);
  if (count > 0 && times != nullptr) {
    std::memcpy(result.Data(), times, count * sizeof(float));
    sonare_free_floats(times);
  }
  return result;
}

Napi::Value SonareWrap::DetectOnsetsInstance(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!audio_) {
    Napi::Error::New(env, "Audio has been destroyed").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  float* times = nullptr;
  size_t count = 0;
  SonareError err = sonare_detect_onsets(sonare_audio_data(audio_), sonare_audio_length(audio_),
                                         sonare_audio_sample_rate(audio_), &times, &count);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto result = Napi::Float32Array::New(env, count);
  if (count > 0 && times != nullptr) {
    std::memcpy(result.Data(), times, count * sizeof(float));
    sonare_free_floats(times);
  }
  return result;
}

Napi::Value SonareWrap::AnalyzeInstance(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!audio_) {
    Napi::Error::New(env, "Audio has been destroyed").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SonareAnalysisResult analysis{};
  SonareError err = sonare_analyze(sonare_audio_data(audio_), sonare_audio_length(audio_),
                                   sonare_audio_sample_rate(audio_), &analysis);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }

  Napi::Object result = AnalysisToObject(env, analysis);
  sonare_free_result(&analysis);
  return result;
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

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
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
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }

  return Napi::Number::New(env, static_cast<double>(bpm));
}

Napi::Value SonareWrap::DetectKey(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
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
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return KeyToObject(env, key.root, key.mode, key.confidence);
}

Napi::Value SonareWrap::DetectBeats(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
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
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
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

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
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
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
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

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
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
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Object result = AnalysisToObject(env, analysis);
  sonare_free_result(&analysis);

  return result;
}

Napi::Value SonareWrap::AnalyzeBpm(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sample_rate = info.Length() >= 2 && info[1].IsNumber()
                        ? info[1].As<Napi::Number>().Int32Value()
                        : 22050;
  float bpm_min = info.Length() >= 3 && info[2].IsNumber()
                      ? info[2].As<Napi::Number>().FloatValue()
                      : 30.0f;
  float bpm_max = info.Length() >= 4 && info[3].IsNumber()
                      ? info[3].As<Napi::Number>().FloatValue()
                      : 300.0f;
  float start_bpm = info.Length() >= 5 && info[4].IsNumber()
                        ? info[4].As<Napi::Number>().FloatValue()
                        : 120.0f;
  int n_fft = info.Length() >= 6 && info[5].IsNumber() ? info[5].As<Napi::Number>().Int32Value()
                                                       : 2048;
  int hop_length = info.Length() >= 7 && info[6].IsNumber()
                       ? info[6].As<Napi::Number>().Int32Value()
                       : 512;
  int max_candidates = info.Length() >= 8 && info[7].IsNumber()
                           ? info[7].As<Napi::Number>().Int32Value()
                           : 5;

  SonareBpmAnalysisResult analysis{};
  SonareError err = sonare_analyze_bpm(data, length, sample_rate, bpm_min, bpm_max, start_bpm,
                                       n_fft, hop_length, max_candidates, &analysis);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }

  Napi::Object result = Napi::Object::New(env);
  result.Set("bpm", Napi::Number::New(env, analysis.bpm));
  result.Set("confidence", Napi::Number::New(env, analysis.confidence));

  Napi::Array candidates = Napi::Array::New(env, analysis.candidate_count);
  for (size_t i = 0; i < analysis.candidate_count; ++i) {
    Napi::Object candidate = Napi::Object::New(env);
    candidate.Set("bpm", Napi::Number::New(env, analysis.candidates[i].bpm));
    candidate.Set("confidence", Napi::Number::New(env, analysis.candidates[i].confidence));
    candidates.Set(static_cast<uint32_t>(i), candidate);
  }
  result.Set("candidates", candidates);

  auto autocorrelation = Napi::Float32Array::New(env, analysis.autocorrelation_count);
  if (analysis.autocorrelation_count > 0 && analysis.autocorrelation != nullptr) {
    std::memcpy(autocorrelation.Data(), analysis.autocorrelation,
                analysis.autocorrelation_count * sizeof(float));
  }
  result.Set("autocorrelation", autocorrelation);

  auto tempogram = Napi::Float32Array::New(env, analysis.tempogram_count);
  if (analysis.tempogram_count > 0 && analysis.tempogram != nullptr) {
    std::memcpy(tempogram.Data(), analysis.tempogram, analysis.tempogram_count * sizeof(float));
  }
  result.Set("tempogram", tempogram);

  sonare_free_bpm_analysis_result(&analysis);
  return result;
}

Napi::Value SonareWrap::AnalyzeRhythm(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sample_rate = info.Length() >= 2 && info[1].IsNumber()
                        ? info[1].As<Napi::Number>().Int32Value()
                        : 22050;
  float bpm_min = info.Length() >= 3 && info[2].IsNumber()
                      ? info[2].As<Napi::Number>().FloatValue()
                      : 60.0f;
  float bpm_max = info.Length() >= 4 && info[3].IsNumber()
                      ? info[3].As<Napi::Number>().FloatValue()
                      : 200.0f;
  float start_bpm = info.Length() >= 5 && info[4].IsNumber()
                        ? info[4].As<Napi::Number>().FloatValue()
                        : 120.0f;
  int n_fft = info.Length() >= 6 && info[5].IsNumber() ? info[5].As<Napi::Number>().Int32Value()
                                                       : 2048;
  int hop_length = info.Length() >= 7 && info[6].IsNumber()
                       ? info[6].As<Napi::Number>().Int32Value()
                       : 512;

  SonareRhythmResult rhythm{};
  SonareError err = sonare_analyze_rhythm(data, length, sample_rate, bpm_min, bpm_max, start_bpm,
                                          n_fft, hop_length, &rhythm);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }

  const char* groove = "straight";
  if (rhythm.groove_type == SONARE_GROOVE_SHUFFLE) groove = "shuffle";
  if (rhythm.groove_type == SONARE_GROOVE_SWING) groove = "swing";

  Napi::Object result = Napi::Object::New(env);
  result.Set("bpm", Napi::Number::New(env, rhythm.bpm));
  Napi::Object time_signature = Napi::Object::New(env);
  time_signature.Set("numerator", Napi::Number::New(env, rhythm.time_signature.numerator));
  time_signature.Set("denominator", Napi::Number::New(env, rhythm.time_signature.denominator));
  time_signature.Set("confidence", Napi::Number::New(env, rhythm.time_signature.confidence));
  result.Set("timeSignature", time_signature);
  result.Set("grooveType", Napi::String::New(env, groove));
  result.Set("syncopation", Napi::Number::New(env, rhythm.syncopation));
  result.Set("patternRegularity", Napi::Number::New(env, rhythm.pattern_regularity));
  result.Set("tempoStability", Napi::Number::New(env, rhythm.tempo_stability));

  auto intervals = Napi::Float32Array::New(env, rhythm.beat_interval_count);
  if (rhythm.beat_interval_count > 0 && rhythm.beat_intervals != nullptr) {
    std::memcpy(intervals.Data(), rhythm.beat_intervals,
                rhythm.beat_interval_count * sizeof(float));
  }
  result.Set("beatIntervals", intervals);

  sonare_free_rhythm_result(&rhythm);
  return result;
}

Napi::Value SonareWrap::AnalyzeDynamics(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sample_rate = info.Length() >= 2 && info[1].IsNumber()
                        ? info[1].As<Napi::Number>().Int32Value()
                        : 22050;
  float window_sec = info.Length() >= 3 && info[2].IsNumber()
                         ? info[2].As<Napi::Number>().FloatValue()
                         : 0.4f;
  int hop_length = info.Length() >= 4 && info[3].IsNumber()
                       ? info[3].As<Napi::Number>().Int32Value()
                       : 512;
  float compression_threshold = info.Length() >= 5 && info[4].IsNumber()
                                    ? info[4].As<Napi::Number>().FloatValue()
                                    : 6.0f;

  SonareDynamicsResult dynamics{};
  SonareError err = sonare_analyze_dynamics(data, length, sample_rate, window_sec, hop_length,
                                            compression_threshold, &dynamics);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }

  Napi::Object result = Napi::Object::New(env);
  result.Set("dynamicRangeDb", Napi::Number::New(env, dynamics.dynamic_range_db));
  result.Set("peakDb", Napi::Number::New(env, dynamics.peak_db));
  result.Set("rmsDb", Napi::Number::New(env, dynamics.rms_db));
  result.Set("crestFactor", Napi::Number::New(env, dynamics.crest_factor));
  result.Set("loudnessRangeDb", Napi::Number::New(env, dynamics.loudness_range_db));
  result.Set("isCompressed", Napi::Boolean::New(env, dynamics.is_compressed != 0));

  auto times = Napi::Float32Array::New(env, dynamics.loudness_count);
  auto rms_db = Napi::Float32Array::New(env, dynamics.loudness_count);
  if (dynamics.loudness_count > 0) {
    std::memcpy(times.Data(), dynamics.loudness_times, dynamics.loudness_count * sizeof(float));
    std::memcpy(rms_db.Data(), dynamics.loudness_rms_db, dynamics.loudness_count * sizeof(float));
  }
  result.Set("loudnessTimes", times);
  result.Set("loudnessRmsDb", rms_db);

  sonare_free_dynamics_result(&dynamics);
  return result;
}

Napi::Value SonareWrap::AnalyzeTimbre(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sample_rate = info.Length() >= 2 && info[1].IsNumber()
                        ? info[1].As<Napi::Number>().Int32Value()
                        : 22050;
  int n_fft = info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value()
                                                       : 2048;
  int hop_length = info.Length() >= 4 && info[3].IsNumber()
                       ? info[3].As<Napi::Number>().Int32Value()
                       : 512;
  int n_mels = info.Length() >= 5 && info[4].IsNumber() ? info[4].As<Napi::Number>().Int32Value()
                                                        : 128;
  int n_mfcc = info.Length() >= 6 && info[5].IsNumber() ? info[5].As<Napi::Number>().Int32Value()
                                                        : 13;
  float window_sec = info.Length() >= 7 && info[6].IsNumber()
                         ? info[6].As<Napi::Number>().FloatValue()
                         : 0.5f;

  SonareTimbreResult timbre{};
  SonareError err = sonare_analyze_timbre(data, length, sample_rate, n_fft, hop_length, n_mels,
                                          n_mfcc, window_sec, &timbre);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }

  Napi::Object result = Napi::Object::New(env);
  result.Set("brightness", Napi::Number::New(env, timbre.brightness));
  result.Set("warmth", Napi::Number::New(env, timbre.warmth));
  result.Set("density", Napi::Number::New(env, timbre.density));
  result.Set("roughness", Napi::Number::New(env, timbre.roughness));
  result.Set("complexity", Napi::Number::New(env, timbre.complexity));

  auto centroid = Napi::Float32Array::New(env, timbre.spectral_centroid_count);
  if (timbre.spectral_centroid_count > 0 && timbre.spectral_centroid != nullptr) {
    std::memcpy(centroid.Data(), timbre.spectral_centroid,
                timbre.spectral_centroid_count * sizeof(float));
  }
  result.Set("spectralCentroid", centroid);

  auto flatness = Napi::Float32Array::New(env, timbre.spectral_flatness_count);
  if (timbre.spectral_flatness_count > 0 && timbre.spectral_flatness != nullptr) {
    std::memcpy(flatness.Data(), timbre.spectral_flatness,
                timbre.spectral_flatness_count * sizeof(float));
  }
  result.Set("spectralFlatness", flatness);

  auto rolloff = Napi::Float32Array::New(env, timbre.spectral_rolloff_count);
  if (timbre.spectral_rolloff_count > 0 && timbre.spectral_rolloff != nullptr) {
    std::memcpy(rolloff.Data(), timbre.spectral_rolloff,
                timbre.spectral_rolloff_count * sizeof(float));
  }
  result.Set("spectralRolloff", rolloff);

  sonare_free_timbre_result(&timbre);
  return result;
}

Napi::Value SonareWrap::DetectChords(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sample_rate = info.Length() >= 2 && info[1].IsNumber()
                        ? info[1].As<Napi::Number>().Int32Value()
                        : 22050;
  float min_duration = info.Length() >= 3 && info[2].IsNumber()
                           ? info[2].As<Napi::Number>().FloatValue()
                           : 0.3f;
  float smoothing_window = info.Length() >= 4 && info[3].IsNumber()
                               ? info[3].As<Napi::Number>().FloatValue()
                               : 2.0f;
  float threshold = info.Length() >= 5 && info[4].IsNumber()
                        ? info[4].As<Napi::Number>().FloatValue()
                        : 0.5f;
  bool use_triads_only = info.Length() >= 6 && info[5].IsBoolean()
                             ? info[5].As<Napi::Boolean>().Value()
                             : false;
  int n_fft = info.Length() >= 7 && info[6].IsNumber() ? info[6].As<Napi::Number>().Int32Value()
                                                       : 2048;
  int hop_length = info.Length() >= 8 && info[7].IsNumber()
                       ? info[7].As<Napi::Number>().Int32Value()
                       : 512;
  bool use_beat_sync = info.Length() >= 9 && info[8].IsBoolean()
                           ? info[8].As<Napi::Boolean>().Value()
                           : true;

  SonareChordAnalysisResult analysis{};
  SonareError err = sonare_detect_chords(data, length, sample_rate, min_duration,
                                         smoothing_window, threshold, use_triads_only ? 1 : 0,
                                         n_fft, hop_length, use_beat_sync ? 1 : 0, &analysis);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }

  Napi::Array chords = Napi::Array::New(env, analysis.chord_count);
  for (size_t i = 0; i < analysis.chord_count; ++i) {
    Napi::Object chord = Napi::Object::New(env);
    std::string root = PitchClassNameLocal(analysis.chords[i].root);
    std::string quality = ChordQualityName(analysis.chords[i].quality);
    chord.Set("root", Napi::String::New(env, root));
    chord.Set("quality", Napi::String::New(env, quality));
    chord.Set("start", Napi::Number::New(env, analysis.chords[i].start));
    chord.Set("end", Napi::Number::New(env, analysis.chords[i].end));
    chord.Set("duration", Napi::Number::New(env, analysis.chords[i].end - analysis.chords[i].start));
    chord.Set("confidence", Napi::Number::New(env, analysis.chords[i].confidence));
    chords.Set(static_cast<uint32_t>(i), chord);
  }

  Napi::Object result = Napi::Object::New(env);
  result.Set("chords", chords);
  sonare_free_chord_analysis_result(&analysis);
  return result;
}

Napi::Value SonareWrap::Version(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  return Napi::String::New(env, sonare_version());
}

Napi::Value SonareWrap::HasFfmpegSupport(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  return Napi::Boolean::New(env, sonare_has_ffmpeg_support() != 0);
}

// ============================================================================
// Effects
// ============================================================================

Napi::Value SonareWrap::Hpss(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, ...)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
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
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::Harmonic(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = info[1].As<Napi::Number>().Int32Value();

  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::Audio result = sonare::harmonic(audio);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return VecToFloat32(env, out_vec);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::Percussive(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = info[1].As<Napi::Number>().Int32Value();

  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::Audio result = sonare::percussive(audio);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return VecToFloat32(env, out_vec);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::TimeStretch(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !info[1].IsNumber() ||
      !info[2].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, rate)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = info[1].As<Napi::Number>().Int32Value();
  float rate = info[2].As<Napi::Number>().FloatValue();

  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::Audio result = sonare::time_stretch(audio, rate);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return VecToFloat32(env, out_vec);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::PitchShift(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !info[1].IsNumber() ||
      !info[2].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, semitones)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = info[1].As<Napi::Number>().Int32Value();
  float semitones = info[2].As<Napi::Number>().FloatValue();

  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::Audio result = sonare::pitch_shift(audio, semitones);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return VecToFloat32(env, out_vec);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::Normalize(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, ...)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
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
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::Trim(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, ...)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
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
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::AnalyzeTtsQuality(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, ...)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = info[1].As<Napi::Number>().Int32Value();
  float silence_threshold_db = info.Length() >= 3 && info[2].IsNumber()
                                   ? info[2].As<Napi::Number>().FloatValue()
                                   : -45.0f;

  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::TtsQualityResult result = sonare::analyze_tts_quality(audio, silence_threshold_db);

  Napi::Object out = Napi::Object::New(env);
  out.Set("durationSec", Napi::Number::New(env, result.duration_sec));
  out.Set("peakDb", Napi::Number::New(env, result.peak_db));
  out.Set("rmsDb", Napi::Number::New(env, result.rms_db));
  out.Set("silenceRatio", Napi::Number::New(env, result.silence_ratio));
  out.Set("clippingRatio", Napi::Number::New(env, result.clipping_ratio));
  out.Set("leadingSilenceSec", Napi::Number::New(env, result.leading_silence_sec));
  out.Set("trailingSilenceSec", Napi::Number::New(env, result.trailing_silence_sec));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::PrepareTts(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, ...)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = info[1].As<Napi::Number>().Int32Value();
  float target_rms_db = info.Length() >= 3 && info[2].IsNumber()
                            ? info[2].As<Napi::Number>().FloatValue()
                            : -20.0f;
  float silence_threshold_db = info.Length() >= 4 && info[3].IsNumber()
                                   ? info[3].As<Napi::Number>().FloatValue()
                                   : -45.0f;
  float peak_limit_db = info.Length() >= 5 && info[4].IsNumber()
                            ? info[4].As<Napi::Number>().FloatValue()
                            : -1.0f;
  float fade_sec = info.Length() >= 6 && info[5].IsNumber()
                       ? info[5].As<Napi::Number>().FloatValue()
                       : 0.005f;

  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::Audio result =
      sonare::prepare_tts(audio, target_rms_db, silence_threshold_db, peak_limit_db, fade_sec);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return VecToFloat32(env, out_vec);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::CompressPauses(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, ...)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = info[1].As<Napi::Number>().Int32Value();
  float max_pause_sec = info.Length() >= 3 && info[2].IsNumber()
                            ? info[2].As<Napi::Number>().FloatValue()
                            : 0.6f;
  float silence_threshold_db = info.Length() >= 4 && info[3].IsNumber()
                                   ? info[3].As<Napi::Number>().FloatValue()
                                   : -45.0f;

  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::Audio result = sonare::compress_pauses(audio, max_pause_sec, silence_threshold_db);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return VecToFloat32(env, out_vec);
  SONARE_NODE_CATCH(env)
}

// ============================================================================
// Features - Spectrogram
// ============================================================================

Napi::Value SonareWrap::Stft(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
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
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::StftDb(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
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
  SONARE_NODE_CATCH(env)
}

// ============================================================================
// Features - Mel
// ============================================================================

Napi::Value SonareWrap::MelSpectrogramFn(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
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
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::Mfcc(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
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
  SONARE_NODE_CATCH(env)
}

// ============================================================================
// Features - Chroma
// ============================================================================

Napi::Value SonareWrap::ChromaFn(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
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
  SONARE_NODE_CATCH(env)
}

// ============================================================================
// Features - Spectral
// ============================================================================

Napi::Value SonareWrap::SpectralCentroid(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
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
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::SpectralBandwidth(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
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
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::SpectralRolloff(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
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
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::SpectralFlatness(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
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
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::ZeroCrossingRate(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
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
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::RmsEnergy(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
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
  SONARE_NODE_CATCH(env)
}

// ============================================================================
// Features - Pitch
// ============================================================================

Napi::Value SonareWrap::PitchYin(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
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
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::PitchPyin(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
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
  SONARE_NODE_CATCH(env)
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

  SONARE_NODE_TRY
  float hz = info[0].As<Napi::Number>().FloatValue();
  return Napi::Number::New(env, static_cast<double>(sonare::hz_to_mel(hz)));
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MelToHz(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "Expected number argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  float mel = info[0].As<Napi::Number>().FloatValue();
  return Napi::Number::New(env, static_cast<double>(sonare::mel_to_hz(mel)));
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::HzToMidi(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "Expected number argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  float hz = info[0].As<Napi::Number>().FloatValue();
  return Napi::Number::New(env, static_cast<double>(sonare::hz_to_midi(hz)));
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MidiToHz(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "Expected number argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  float midi = info[0].As<Napi::Number>().FloatValue();
  return Napi::Number::New(env, static_cast<double>(sonare::midi_to_hz(midi)));
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::HzToNote(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "Expected number argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  float hz = info[0].As<Napi::Number>().FloatValue();
  return Napi::String::New(env, sonare::hz_to_note(hz));
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::NoteToHz(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "Expected string argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  std::string note = info[0].As<Napi::String>().Utf8Value();
  return Napi::Number::New(env, static_cast<double>(sonare::note_to_hz(note)));
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::FramesToTime(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 3 || !info[0].IsNumber() || !info[1].IsNumber() || !info[2].IsNumber()) {
    Napi::TypeError::New(env, "Expected (frames, sr, hopLength)").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  int frames = info[0].As<Napi::Number>().Int32Value();
  int sr = info[1].As<Napi::Number>().Int32Value();
  int hop_length = info[2].As<Napi::Number>().Int32Value();

  return Napi::Number::New(env,
                           static_cast<double>(sonare::frames_to_time(frames, sr, hop_length)));
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::TimeToFrames(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 3 || !info[0].IsNumber() || !info[1].IsNumber() || !info[2].IsNumber()) {
    Napi::TypeError::New(env, "Expected (time, sr, hopLength)").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  float time = info[0].As<Napi::Number>().FloatValue();
  int sr = info[1].As<Napi::Number>().Int32Value();
  int hop_length = info[2].As<Napi::Number>().Int32Value();

  return Napi::Number::New(env, sonare::time_to_frames(time, sr, hop_length));
  SONARE_NODE_CATCH(env)
}

// ============================================================================
// Core - Resample
// ============================================================================

Napi::Value SonareWrap::Resample(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !info[1].IsNumber() ||
      !info[2].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, srcSr, targetSr)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int src_sr = info[1].As<Napi::Number>().Int32Value();
  int target_sr = info[2].As<Napi::Number>().Int32Value();

  std::vector<float> result = sonare::resample(data, length, src_sr, target_sr);
  return VecToFloat32(env, result);
  SONARE_NODE_CATCH(env)
}
