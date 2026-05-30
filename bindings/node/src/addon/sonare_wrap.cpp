#include "sonare_wrap.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "sonare_wrap_mixer.h"
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
  switch (mode) {
    case SONARE_MODE_MAJOR:
      return "major";
    case SONARE_MODE_MINOR:
      return "minor";
    case SONARE_MODE_DORIAN:
      return "dorian";
    case SONARE_MODE_PHRYGIAN:
      return "phrygian";
    case SONARE_MODE_LYDIAN:
      return "lydian";
    case SONARE_MODE_MIXOLYDIAN:
      return "mixolydian";
    case SONARE_MODE_LOCRIAN:
      return "locrian";
    default:
      return "unknown";
  }
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
          InstanceMethod<&SonareWrap::DetectKeyCandidatesInstance>("detectKeyCandidates"),
          InstanceMethod<&SonareWrap::DetectBeatsInstance>("detectBeats"),
          InstanceMethod<&SonareWrap::DetectDownbeatsInstance>("detectDownbeats"),
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
  exports.Set("detectKeyCandidates",
              Napi::Function::New(env, &SonareWrap::DetectKeyCandidates, "detectKeyCandidates"));
  exports.Set("detectBeats", Napi::Function::New(env, &SonareWrap::DetectBeats, "detectBeats"));
  exports.Set("detectDownbeats",
              Napi::Function::New(env, &SonareWrap::DetectDownbeats, "detectDownbeats"));
  exports.Set("detectOnsets", Napi::Function::New(env, &SonareWrap::DetectOnsets, "detectOnsets"));
  exports.Set("analyze", Napi::Function::New(env, &SonareWrap::Analyze, "analyze"));
  exports.Set("analyzeAsync", Napi::Function::New(env, &SonareWrap::AnalyzeAsync, "analyzeAsync"));
  exports.Set("analyzeWithProgress",
              Napi::Function::New(env, &SonareWrap::AnalyzeWithProgress, "analyzeWithProgress"));
  exports.Set("analyzeSections",
              Napi::Function::New(env, &SonareWrap::AnalyzeSections, "analyzeSections"));
  exports.Set("analyzeMelody",
              Napi::Function::New(env, &SonareWrap::AnalyzeMelody, "analyzeMelody"));
  exports.Set("analyzeBpm", Napi::Function::New(env, &SonareWrap::AnalyzeBpm, "analyzeBpm"));
  exports.Set(
      "analyzeImpulseResponse",
      Napi::Function::New(env, &SonareWrap::AnalyzeImpulseResponse, "analyzeImpulseResponse"));
  exports.Set("detectAcoustic",
              Napi::Function::New(env, &SonareWrap::DetectAcoustic, "detectAcoustic"));
  exports.Set("analyzeRhythm",
              Napi::Function::New(env, &SonareWrap::AnalyzeRhythm, "analyzeRhythm"));
  exports.Set("analyzeDynamics",
              Napi::Function::New(env, &SonareWrap::AnalyzeDynamics, "analyzeDynamics"));
  exports.Set("analyzeTimbre",
              Napi::Function::New(env, &SonareWrap::AnalyzeTimbre, "analyzeTimbre"));
  exports.Set("detectChords", Napi::Function::New(env, &SonareWrap::DetectChords, "detectChords"));
  exports.Set("lufs", Napi::Function::New(env, &SonareWrap::Lufs, "lufs"));
  exports.Set("momentaryLufs",
              Napi::Function::New(env, &SonareWrap::MomentaryLufs, "momentaryLufs"));
  exports.Set("shortTermLufs",
              Napi::Function::New(env, &SonareWrap::ShortTermLufs, "shortTermLufs"));
  exports.Set("meteringPeakDb",
              Napi::Function::New(env, &SonareWrap::MeteringPeakDb, "meteringPeakDb"));
  exports.Set("meteringRmsDb",
              Napi::Function::New(env, &SonareWrap::MeteringRmsDb, "meteringRmsDb"));
  exports.Set("meteringCrestFactorDb", Napi::Function::New(env, &SonareWrap::MeteringCrestFactorDb,
                                                           "meteringCrestFactorDb"));
  exports.Set("meteringDcOffset",
              Napi::Function::New(env, &SonareWrap::MeteringDcOffset, "meteringDcOffset"));
  exports.Set("meteringTruePeakDb",
              Napi::Function::New(env, &SonareWrap::MeteringTruePeakDb, "meteringTruePeakDb"));
  exports.Set(
      "meteringDetectClipping",
      Napi::Function::New(env, &SonareWrap::MeteringDetectClipping, "meteringDetectClipping"));
  exports.Set("meteringDynamicRange",
              Napi::Function::New(env, &SonareWrap::MeteringDynamicRange, "meteringDynamicRange"));
  exports.Set("meteringStereoCorrelation",
              Napi::Function::New(env, &SonareWrap::MeteringStereoCorrelation,
                                  "meteringStereoCorrelation"));
  exports.Set("meteringStereoWidth",
              Napi::Function::New(env, &SonareWrap::MeteringStereoWidth, "meteringStereoWidth"));
  exports.Set("meteringVectorscope",
              Napi::Function::New(env, &SonareWrap::MeteringVectorscope, "meteringVectorscope"));
  exports.Set("meteringPhaseScope",
              Napi::Function::New(env, &SonareWrap::MeteringPhaseScope, "meteringPhaseScope"));
  exports.Set("meteringSpectrum",
              Napi::Function::New(env, &SonareWrap::MeteringSpectrum, "meteringSpectrum"));
  exports.Set("scaleQuantizeMidi",
              Napi::Function::New(env, &SonareWrap::ScaleQuantizeMidi, "scaleQuantizeMidi"));
  exports.Set(
      "scaleCorrectionSemitones",
      Napi::Function::New(env, &SonareWrap::ScaleCorrectionSemitones, "scaleCorrectionSemitones"));
  exports.Set(
      "scalePitchClassEnabled",
      Napi::Function::New(env, &SonareWrap::ScalePitchClassEnabled, "scalePitchClassEnabled"));
  exports.Set("version", Napi::Function::New(env, &SonareWrap::Version, "version"));
  exports.Set("hasFfmpegSupport",
              Napi::Function::New(env, &SonareWrap::HasFfmpegSupport, "hasFfmpegSupport"));

  // Effects
  exports.Set("hpss", Napi::Function::New(env, &SonareWrap::Hpss, "hpss"));
  exports.Set("harmonic", Napi::Function::New(env, &SonareWrap::Harmonic, "harmonic"));
  exports.Set("percussive", Napi::Function::New(env, &SonareWrap::Percussive, "percussive"));
  exports.Set("timeStretch", Napi::Function::New(env, &SonareWrap::TimeStretch, "timeStretch"));
  exports.Set("pitchShift", Napi::Function::New(env, &SonareWrap::PitchShift, "pitchShift"));
  exports.Set("pitchCorrectToMidi",
              Napi::Function::New(env, &SonareWrap::PitchCorrectToMidi, "pitchCorrectToMidi"));
  exports.Set("noteStretch", Napi::Function::New(env, &SonareWrap::NoteStretch, "noteStretch"));
  exports.Set("voiceChange", Napi::Function::New(env, &SonareWrap::VoiceChange, "voiceChange"));
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
  exports.Set(
      "mixingScenePresetNames",
      Napi::Function::New(env, &SonareWrap::MixingScenePresetNames, "mixingScenePresetNames"));
  exports.Set("mixingScenePresetJson", Napi::Function::New(env, &SonareWrap::MixingScenePresetJson,
                                                           "mixingScenePresetJson"));
  exports.Set("mixStereo", Napi::Function::New(env, &SonareWrap::MixStereo, "mixStereo"));
  exports.Set("masterAudio", Napi::Function::New(env, &SonareWrap::MasterAudio, "masterAudio"));
  exports.Set("masterAudioAsync",
              Napi::Function::New(env, &SonareWrap::MasterAudioAsync, "masterAudioAsync"));
  exports.Set("masterAudioStereo",
              Napi::Function::New(env, &SonareWrap::MasterAudioStereo, "masterAudioStereo"));
  exports.Set(
      "masterAudioStereoAsync",
      Napi::Function::New(env, &SonareWrap::MasterAudioStereoAsync, "masterAudioStereoAsync"));
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
  exports.Set("masteringAssistantSuggest",
              Napi::Function::New(env, &SonareWrap::MasteringAssistantSuggest,
                                  "masteringAssistantSuggest"));
  exports.Set("masteringAudioProfile", Napi::Function::New(env, &SonareWrap::MasteringAudioProfile,
                                                           "masteringAudioProfile"));
  exports.Set("masteringStreamingPreview",
              Napi::Function::New(env, &SonareWrap::MasteringStreamingPreview,
                                  "masteringStreamingPreview"));
  exports.Set(
      "masteringRepairDeclick",
      Napi::Function::New(env, &SonareWrap::MasteringRepairDeclick, "masteringRepairDeclick"));
  exports.Set("masteringRepairDenoiseClassical",
              Napi::Function::New(env, &SonareWrap::MasteringRepairDenoiseClassical,
                                  "masteringRepairDenoiseClassical"));
  exports.Set("masteringRepairDeclip", Napi::Function::New(env, &SonareWrap::MasteringRepairDeclip,
                                                           "masteringRepairDeclip"));
  exports.Set(
      "masteringRepairDecrackle",
      Napi::Function::New(env, &SonareWrap::MasteringRepairDecrackle, "masteringRepairDecrackle"));
  exports.Set("masteringRepairDehum",
              Napi::Function::New(env, &SonareWrap::MasteringRepairDehum, "masteringRepairDehum"));
  exports.Set("masteringRepairDereverbClassical",
              Napi::Function::New(env, &SonareWrap::MasteringRepairDereverbClassical,
                                  "masteringRepairDereverbClassical"));
  exports.Set("masteringRepairTrimSilence",
              Napi::Function::New(env, &SonareWrap::MasteringRepairTrimSilence,
                                  "masteringRepairTrimSilence"));
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

  // Features - Constant-Q / Variable-Q transforms
  exports.Set("cqt", Napi::Function::New(env, &SonareWrap::Cqt, "cqt"));
  exports.Set("vqt", Napi::Function::New(env, &SonareWrap::Vqt, "vqt"));

  // Features - Inverse reconstruction (Mel/MFCC -> spectrogram -> audio)
  exports.Set("melToStft", Napi::Function::New(env, &SonareWrap::MelToStft, "melToStft"));
  exports.Set("melToAudio", Napi::Function::New(env, &SonareWrap::MelToAudio, "melToAudio"));
  exports.Set("mfccToMel", Napi::Function::New(env, &SonareWrap::MfccToMel, "mfccToMel"));
  exports.Set("mfccToAudio", Napi::Function::New(env, &SonareWrap::MfccToAudio, "mfccToAudio"));

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
  exports.Set("cyclicTempogram",
              Napi::Function::New(env, &SonareWrap::CyclicTempogram, "cyclicTempogram"));
  exports.Set("plp", Napi::Function::New(env, &SonareWrap::Plp, "plp"));
  exports.Set("onsetEnvelope",
              Napi::Function::New(env, &SonareWrap::OnsetEnvelope, "onsetEnvelope"));
  exports.Set("fourierTempogram",
              Napi::Function::New(env, &SonareWrap::FourierTempogram, "fourierTempogram"));
  exports.Set("tempogramRatio",
              Napi::Function::New(env, &SonareWrap::TempogramRatio, "tempogramRatio"));
  exports.Set("nnlsChroma", Napi::Function::New(env, &SonareWrap::NnlsChroma, "nnlsChroma"));

  // Core - Resample
  exports.Set("resample", Napi::Function::New(env, &SonareWrap::Resample, "resample"));

  // Streaming - StreamingMasteringChain
  StreamingMasteringChainWrap::Init(env, exports);

  // Streaming - StreamingEqualizer
  StreamingEqualizerWrap::Init(env, exports);

  // Streaming - RealtimeVoiceChanger
  RealtimeVoiceChangerWrap::Init(env, exports);
  exports.Set("realtimeVoiceChangerPresetNames",
              Napi::Function::New(env, RealtimeVoiceChangerPresetNames));
  exports.Set("realtimeVoiceChangerPresetJson",
              Napi::Function::New(env, RealtimeVoiceChangerPresetJson));
  exports.Set("validateRealtimeVoiceChangerPresetJson",
              Napi::Function::New(env, ValidateRealtimeVoiceChangerPresetJson));

  // Streaming - StreamAnalyzer (real-time frame analyzer)
  StreamAnalyzerWrap::Init(env, exports);

  // Mixing - scene-based Mixer
  MixerWrap::Init(env, exports);

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

  int n_fft = 4096;
  int hop_length = 512;
  bool use_hpss = false;
  bool loudness_weighted = false;
  float high_pass_hz = 0.0f;
  if (info.Length() >= 1 && info[0].IsObject()) {
    Napi::Object options = info[0].As<Napi::Object>();
    Napi::Value n_fft_value = options.Get("nFft");
    Napi::Value hop_value = options.Get("hopLength");
    Napi::Value hpss_value = options.Get("useHpss");
    Napi::Value loudness_value = options.Get("loudnessWeighted");
    Napi::Value high_pass_value = options.Get("highPassHz");
    if (n_fft_value.IsNumber()) n_fft = n_fft_value.As<Napi::Number>().Int32Value();
    if (hop_value.IsNumber()) hop_length = hop_value.As<Napi::Number>().Int32Value();
    if (hpss_value.IsBoolean()) use_hpss = hpss_value.As<Napi::Boolean>().Value();
    if (loudness_value.IsBoolean()) {
      loudness_weighted = loudness_value.As<Napi::Boolean>().Value();
    }
    if (high_pass_value.IsNumber()) {
      high_pass_hz = high_pass_value.As<Napi::Number>().FloatValue();
    }
  }

  SonareKey key{};
  SonareError err = sonare_detect_key_with_options(
      sonare_audio_data(audio_), sonare_audio_length(audio_), sonare_audio_sample_rate(audio_),
      n_fft, hop_length, use_hpss ? 1 : 0, loudness_weighted ? 1 : 0, high_pass_hz, &key);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return KeyToObject(env, key.root, key.mode, key.confidence);
}

Napi::Value SonareWrap::DetectKeyCandidatesInstance(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!audio_) {
    Napi::Error::New(env, "Audio has been destroyed").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  int n_fft = 4096;
  int hop_length = 512;
  bool use_hpss = false;
  bool loudness_weighted = false;
  float high_pass_hz = 0.0f;
  if (info.Length() >= 1 && info[0].IsObject()) {
    Napi::Object options = info[0].As<Napi::Object>();
    Napi::Value n_fft_value = options.Get("nFft");
    Napi::Value hop_value = options.Get("hopLength");
    Napi::Value hpss_value = options.Get("useHpss");
    Napi::Value loudness_value = options.Get("loudnessWeighted");
    Napi::Value high_pass_value = options.Get("highPassHz");
    if (n_fft_value.IsNumber()) n_fft = n_fft_value.As<Napi::Number>().Int32Value();
    if (hop_value.IsNumber()) hop_length = hop_value.As<Napi::Number>().Int32Value();
    if (hpss_value.IsBoolean()) use_hpss = hpss_value.As<Napi::Boolean>().Value();
    if (loudness_value.IsBoolean()) loudness_weighted = loudness_value.As<Napi::Boolean>().Value();
    if (high_pass_value.IsNumber()) high_pass_hz = high_pass_value.As<Napi::Number>().FloatValue();
  }

  SonareKeyCandidate* candidates = nullptr;
  size_t count = 0;
  SonareError err = sonare_detect_key_candidates(
      sonare_audio_data(audio_), sonare_audio_length(audio_), sonare_audio_sample_rate(audio_),
      n_fft, hop_length, use_hpss ? 1 : 0, loudness_weighted ? 1 : 0, high_pass_hz, &candidates,
      &count);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }

  Napi::Array out = Napi::Array::New(env, count);
  for (size_t i = 0; i < count; ++i) {
    Napi::Object candidate = Napi::Object::New(env);
    candidate.Set("key", KeyToObject(env, candidates[i].key.root, candidates[i].key.mode,
                                     candidates[i].key.confidence));
    candidate.Set("correlation",
                  Napi::Number::New(env, static_cast<double>(candidates[i].correlation)));
    out.Set(i, candidate);
  }
  sonare_free_key_candidates(candidates);
  return out;
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

Napi::Value SonareWrap::DetectDownbeatsInstance(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!audio_) {
    Napi::Error::New(env, "Audio has been destroyed").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  float* times = nullptr;
  size_t count = 0;
  SonareError err = sonare_detect_downbeats(sonare_audio_data(audio_), sonare_audio_length(audio_),
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
