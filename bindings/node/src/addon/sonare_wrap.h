#ifndef SONARE_NODE_SONARE_WRAP_H_
#define SONARE_NODE_SONARE_WRAP_H_

#include <napi.h>

#include "sonare_c.h"

class SonareWrap : public Napi::ObjectWrap<SonareWrap> {
 public:
  static Napi::Object Init(Napi::Env env, Napi::Object exports);
  explicit SonareWrap(const Napi::CallbackInfo& info);
  ~SonareWrap();

  // Non-copyable, non-movable: audio_ has unique ownership semantics
  SonareWrap(const SonareWrap&) = delete;
  SonareWrap& operator=(const SonareWrap&) = delete;
  SonareWrap(SonareWrap&&) = delete;
  SonareWrap& operator=(SonareWrap&&) = delete;

 private:
  // Instance methods (Audio object)
  Napi::Value GetData(const Napi::CallbackInfo& info);
  Napi::Value GetLength(const Napi::CallbackInfo& info);
  Napi::Value GetSampleRate(const Napi::CallbackInfo& info);
  Napi::Value GetDuration(const Napi::CallbackInfo& info);
  Napi::Value DetectBpmInstance(const Napi::CallbackInfo& info);
  Napi::Value DetectKeyInstance(const Napi::CallbackInfo& info);
  Napi::Value DetectBeatsInstance(const Napi::CallbackInfo& info);
  Napi::Value DetectOnsetsInstance(const Napi::CallbackInfo& info);
  Napi::Value AnalyzeInstance(const Napi::CallbackInfo& info);
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
  static Napi::Value AnalyzeBpm(const Napi::CallbackInfo& info);
  static Napi::Value AnalyzeRhythm(const Napi::CallbackInfo& info);
  static Napi::Value AnalyzeDynamics(const Napi::CallbackInfo& info);
  static Napi::Value AnalyzeTimbre(const Napi::CallbackInfo& info);
  static Napi::Value DetectChords(const Napi::CallbackInfo& info);
  static Napi::Value Version(const Napi::CallbackInfo& info);
  static Napi::Value HasFfmpegSupport(const Napi::CallbackInfo& info);

  // Effects
  static Napi::Value Hpss(const Napi::CallbackInfo& info);
  static Napi::Value Harmonic(const Napi::CallbackInfo& info);
  static Napi::Value Percussive(const Napi::CallbackInfo& info);
  static Napi::Value TimeStretch(const Napi::CallbackInfo& info);
  static Napi::Value PitchShift(const Napi::CallbackInfo& info);
  static Napi::Value Normalize(const Napi::CallbackInfo& info);
  static Napi::Value Mastering(const Napi::CallbackInfo& info);
  static Napi::Value MasteringProcess(const Napi::CallbackInfo& info);
  static Napi::Value MasteringProcessStereo(const Napi::CallbackInfo& info);
  static Napi::Value MasteringChain(const Napi::CallbackInfo& info);
  static Napi::Value MasteringChainStereo(const Napi::CallbackInfo& info);
  static Napi::Value MasteringPresetNames(const Napi::CallbackInfo& info);
  static Napi::Value MasterAudio(const Napi::CallbackInfo& info);
  static Napi::Value MasterAudioStereo(const Napi::CallbackInfo& info);
  static Napi::Value MasteringProcessorNames(const Napi::CallbackInfo& info);
  static Napi::Value MasteringPairProcessorNames(const Napi::CallbackInfo& info);
  static Napi::Value MasteringPairAnalysisNames(const Napi::CallbackInfo& info);
  static Napi::Value MasteringStereoAnalysisNames(const Napi::CallbackInfo& info);
  static Napi::Value MasteringPairProcess(const Napi::CallbackInfo& info);
  static Napi::Value MasteringPairAnalyze(const Napi::CallbackInfo& info);
  static Napi::Value MasteringStereoAnalyze(const Napi::CallbackInfo& info);
  static Napi::Value Trim(const Napi::CallbackInfo& info);

  // Features - Spectrogram
  static Napi::Value Stft(const Napi::CallbackInfo& info);
  static Napi::Value StftDb(const Napi::CallbackInfo& info);

  // Features - Mel
  static Napi::Value MelSpectrogramFn(const Napi::CallbackInfo& info);
  static Napi::Value Mfcc(const Napi::CallbackInfo& info);

  // Features - Chroma
  static Napi::Value ChromaFn(const Napi::CallbackInfo& info);

  // Features - Spectral
  static Napi::Value SpectralCentroid(const Napi::CallbackInfo& info);
  static Napi::Value SpectralBandwidth(const Napi::CallbackInfo& info);
  static Napi::Value SpectralRolloff(const Napi::CallbackInfo& info);
  static Napi::Value SpectralFlatness(const Napi::CallbackInfo& info);
  static Napi::Value ZeroCrossingRate(const Napi::CallbackInfo& info);
  static Napi::Value RmsEnergy(const Napi::CallbackInfo& info);

  // Features - Pitch
  static Napi::Value PitchYin(const Napi::CallbackInfo& info);
  static Napi::Value PitchPyin(const Napi::CallbackInfo& info);

  // Core - Conversion
  static Napi::Value HzToMel(const Napi::CallbackInfo& info);
  static Napi::Value MelToHz(const Napi::CallbackInfo& info);
  static Napi::Value HzToMidi(const Napi::CallbackInfo& info);
  static Napi::Value MidiToHz(const Napi::CallbackInfo& info);
  static Napi::Value HzToNote(const Napi::CallbackInfo& info);
  static Napi::Value NoteToHz(const Napi::CallbackInfo& info);
  static Napi::Value FramesToTime(const Napi::CallbackInfo& info);
  static Napi::Value TimeToFrames(const Napi::CallbackInfo& info);
  static Napi::Value FramesToSamples(const Napi::CallbackInfo& info);
  static Napi::Value SamplesToFrames(const Napi::CallbackInfo& info);
  static Napi::Value PowerToDb(const Napi::CallbackInfo& info);
  static Napi::Value AmplitudeToDb(const Napi::CallbackInfo& info);
  static Napi::Value DbToPower(const Napi::CallbackInfo& info);
  static Napi::Value DbToAmplitude(const Napi::CallbackInfo& info);
  static Napi::Value Preemphasis(const Napi::CallbackInfo& info);
  static Napi::Value Deemphasis(const Napi::CallbackInfo& info);
  static Napi::Value TrimSilence(const Napi::CallbackInfo& info);
  static Napi::Value SplitSilence(const Napi::CallbackInfo& info);
  static Napi::Value FrameSignal(const Napi::CallbackInfo& info);
  static Napi::Value PadCenter(const Napi::CallbackInfo& info);
  static Napi::Value FixLength(const Napi::CallbackInfo& info);
  static Napi::Value FixFrames(const Napi::CallbackInfo& info);
  static Napi::Value PeakPick(const Napi::CallbackInfo& info);
  static Napi::Value VectorNormalize(const Napi::CallbackInfo& info);
  static Napi::Value Pcen(const Napi::CallbackInfo& info);
  static Napi::Value Tonnetz(const Napi::CallbackInfo& info);
  static Napi::Value Tempogram(const Napi::CallbackInfo& info);
  static Napi::Value Plp(const Napi::CallbackInfo& info);

  // Core - Resample
  static Napi::Value Resample(const Napi::CallbackInfo& info);

  // Helper to get pitch class name string
  static const char* PitchClassName(SonarePitchClass pc);
  // Helper to get mode name string
  static const char* ModeName(SonareMode mode);
  // Helper to create Float32Array from vector
  static Napi::Float32Array VecToFloat32(Napi::Env env, const std::vector<float>& vec);

  SonareAudio* audio_;

  // Reference to the constructor function for creating instances from static methods
  static Napi::FunctionReference constructor_;
};

#endif  // SONARE_NODE_SONARE_WRAP_H_
