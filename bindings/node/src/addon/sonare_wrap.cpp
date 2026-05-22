#include "sonare_wrap.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "sonare_wrap_streaming.h"
#include "sonare_wrap_utils.h"

using namespace sonare_node;

Napi::FunctionReference SonareWrap::constructor_;

const char* SonareWrap::PitchClassName(SonarePitchClass pc) {
  static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
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
  Napi::Function func =
      DefineClass(env, "Audio",
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
  exports.Set("detectOnsets", Napi::Function::New(env, &SonareWrap::DetectOnsets, "detectOnsets"));
  exports.Set("analyze", Napi::Function::New(env, &SonareWrap::Analyze, "analyze"));
  exports.Set("analyzeBpm", Napi::Function::New(env, &SonareWrap::AnalyzeBpm, "analyzeBpm"));
  exports.Set("analyzeRhythm",
              Napi::Function::New(env, &SonareWrap::AnalyzeRhythm, "analyzeRhythm"));
  exports.Set("analyzeDynamics",
              Napi::Function::New(env, &SonareWrap::AnalyzeDynamics, "analyzeDynamics"));
  exports.Set("analyzeTimbre",
              Napi::Function::New(env, &SonareWrap::AnalyzeTimbre, "analyzeTimbre"));
  exports.Set("detectChords", Napi::Function::New(env, &SonareWrap::DetectChords, "detectChords"));
  exports.Set("version", Napi::Function::New(env, &SonareWrap::Version, "version"));
  exports.Set("hasFfmpegSupport",
              Napi::Function::New(env, &SonareWrap::HasFfmpegSupport, "hasFfmpegSupport"));

  // Effects
  exports.Set("hpss", Napi::Function::New(env, &SonareWrap::Hpss, "hpss"));
  exports.Set("harmonic", Napi::Function::New(env, &SonareWrap::Harmonic, "harmonic"));
  exports.Set("percussive", Napi::Function::New(env, &SonareWrap::Percussive, "percussive"));
  exports.Set("timeStretch", Napi::Function::New(env, &SonareWrap::TimeStretch, "timeStretch"));
  exports.Set("pitchShift", Napi::Function::New(env, &SonareWrap::PitchShift, "pitchShift"));
  exports.Set("normalize", Napi::Function::New(env, &SonareWrap::Normalize, "normalize"));
  exports.Set("mastering", Napi::Function::New(env, &SonareWrap::Mastering, "mastering"));
  exports.Set("masteringProcess",
              Napi::Function::New(env, &SonareWrap::MasteringProcess, "masteringProcess"));
  exports.Set(
      "masteringProcessStereo",
      Napi::Function::New(env, &SonareWrap::MasteringProcessStereo, "masteringProcessStereo"));
  exports.Set("masteringChain",
              Napi::Function::New(env, &SonareWrap::MasteringChain, "masteringChain"));
  exports.Set("masteringChainStereo",
              Napi::Function::New(env, &SonareWrap::MasteringChainStereo, "masteringChainStereo"));
  exports.Set("masteringChainWithProgress",
              Napi::Function::New(env, &SonareWrap::MasteringChainWithProgress,
                                  "masteringChainWithProgress"));
  exports.Set("masteringChainStereoWithProgress",
              Napi::Function::New(env, &SonareWrap::MasteringChainStereoWithProgress,
                                  "masteringChainStereoWithProgress"));
  exports.Set("masteringPresetNames",
              Napi::Function::New(env, &SonareWrap::MasteringPresetNames, "masteringPresetNames"));
  exports.Set("masterAudio", Napi::Function::New(env, &SonareWrap::MasterAudio, "masterAudio"));
  exports.Set("masterAudioStereo",
              Napi::Function::New(env, &SonareWrap::MasterAudioStereo, "masterAudioStereo"));
  exports.Set(
      "masterAudioWithProgress",
      Napi::Function::New(env, &SonareWrap::MasterAudioWithProgress, "masterAudioWithProgress"));
  exports.Set("masterAudioStereoWithProgress",
              Napi::Function::New(env, &SonareWrap::MasterAudioStereoWithProgress,
                                  "masterAudioStereoWithProgress"));
  exports.Set(
      "masteringProcessorNames",
      Napi::Function::New(env, &SonareWrap::MasteringProcessorNames, "masteringProcessorNames"));
  exports.Set("masteringPairProcessorNames",
              Napi::Function::New(env, &SonareWrap::MasteringPairProcessorNames,
                                  "masteringPairProcessorNames"));
  exports.Set("masteringPairAnalysisNames",
              Napi::Function::New(env, &SonareWrap::MasteringPairAnalysisNames,
                                  "masteringPairAnalysisNames"));
  exports.Set("masteringStereoAnalysisNames",
              Napi::Function::New(env, &SonareWrap::MasteringStereoAnalysisNames,
                                  "masteringStereoAnalysisNames"));
  exports.Set("masteringPairProcess",
              Napi::Function::New(env, &SonareWrap::MasteringPairProcess, "masteringPairProcess"));
  exports.Set("masteringPairAnalyze",
              Napi::Function::New(env, &SonareWrap::MasteringPairAnalyze, "masteringPairAnalyze"));
  exports.Set(
      "masteringStereoAnalyze",
      Napi::Function::New(env, &SonareWrap::MasteringStereoAnalyze, "masteringStereoAnalyze"));
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
  exports.Set("framesToTime", Napi::Function::New(env, &SonareWrap::FramesToTime, "framesToTime"));
  exports.Set("timeToFrames", Napi::Function::New(env, &SonareWrap::TimeToFrames, "timeToFrames"));
  exports.Set("framesToSamples",
              Napi::Function::New(env, &SonareWrap::FramesToSamples, "framesToSamples"));
  exports.Set("samplesToFrames",
              Napi::Function::New(env, &SonareWrap::SamplesToFrames, "samplesToFrames"));
  exports.Set("powerToDb", Napi::Function::New(env, &SonareWrap::PowerToDb, "powerToDb"));
  exports.Set("amplitudeToDb",
              Napi::Function::New(env, &SonareWrap::AmplitudeToDb, "amplitudeToDb"));
  exports.Set("dbToPower", Napi::Function::New(env, &SonareWrap::DbToPower, "dbToPower"));
  exports.Set("dbToAmplitude",
              Napi::Function::New(env, &SonareWrap::DbToAmplitude, "dbToAmplitude"));
  exports.Set("preemphasis", Napi::Function::New(env, &SonareWrap::Preemphasis, "preemphasis"));
  exports.Set("deemphasis", Napi::Function::New(env, &SonareWrap::Deemphasis, "deemphasis"));
  exports.Set("trimSilence", Napi::Function::New(env, &SonareWrap::TrimSilence, "trimSilence"));
  exports.Set("splitSilence", Napi::Function::New(env, &SonareWrap::SplitSilence, "splitSilence"));
  exports.Set("frameSignal", Napi::Function::New(env, &SonareWrap::FrameSignal, "frameSignal"));
  exports.Set("padCenter", Napi::Function::New(env, &SonareWrap::PadCenter, "padCenter"));
  exports.Set("fixLength", Napi::Function::New(env, &SonareWrap::FixLength, "fixLength"));
  exports.Set("fixFrames", Napi::Function::New(env, &SonareWrap::FixFrames, "fixFrames"));
  exports.Set("peakPick", Napi::Function::New(env, &SonareWrap::PeakPick, "peakPick"));
  exports.Set("vectorNormalize",
              Napi::Function::New(env, &SonareWrap::VectorNormalize, "vectorNormalize"));
  exports.Set("pcen", Napi::Function::New(env, &SonareWrap::Pcen, "pcen"));
  exports.Set("tonnetz", Napi::Function::New(env, &SonareWrap::Tonnetz, "tonnetz"));
  exports.Set("tempogram", Napi::Function::New(env, &SonareWrap::Tempogram, "tempogram"));
  exports.Set("plp", Napi::Function::New(env, &SonareWrap::Plp, "plp"));

  // Core - Resample
  exports.Set("resample", Napi::Function::New(env, &SonareWrap::Resample, "resample"));

  // Streaming - StreamingMasteringChain
  StreamingMasteringChainWrap::Init(env, exports);

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

  SonareAudio* audio_raw = nullptr;
  SonareError err = sonare_audio_from_file(path.c_str(), &audio_raw);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }
  std::unique_ptr<SonareAudio, decltype(&sonare_audio_free)> audio_guard(audio_raw,
                                                                         sonare_audio_free);

  Napi::External<SonareAudio> external = Napi::External<SonareAudio>::New(env, audio_raw);
  auto result = constructor_.New({external});
  audio_guard.release();
  return result;
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

  SonareAudio* audio_raw = nullptr;
  SonareError err = sonare_audio_from_buffer(data, length, sample_rate, &audio_raw);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }
  std::unique_ptr<SonareAudio, decltype(&sonare_audio_free)> audio_guard(audio_raw,
                                                                         sonare_audio_free);

  Napi::External<SonareAudio> external = Napi::External<SonareAudio>::New(env, audio_raw);
  auto result = constructor_.New({external});
  audio_guard.release();
  return result;
}

Napi::Value SonareWrap::FromMemory(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  const uint8_t* data = nullptr;
  size_t len = 0;

  if (info.Length() < 1) {
    Napi::TypeError::New(env, "Expected Buffer or Uint8Array argument")
        .ThrowAsJavaScriptException();
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
    Napi::TypeError::New(env, "Expected Buffer or Uint8Array argument")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SonareAudio* audio_raw = nullptr;
  SonareError err = sonare_audio_from_memory(data, len, &audio_raw);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }
  std::unique_ptr<SonareAudio, decltype(&sonare_audio_free)> audio_guard(audio_raw,
                                                                         sonare_audio_free);

  Napi::External<SonareAudio> external = Napi::External<SonareAudio>::New(env, audio_raw);
  auto result = constructor_.New({external});
  audio_guard.release();
  return result;
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
