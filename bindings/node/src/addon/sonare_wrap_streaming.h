#ifndef SONARE_NODE_SONARE_WRAP_STREAMING_H_
#define SONARE_NODE_SONARE_WRAP_STREAMING_H_

#include <napi.h>

#include <array>
#include <memory>
#include <vector>

#include "editing/voice_changer/realtime_voice_changer.h"
#include "mastering/api/chain.h"
#include "mastering/eq/equalizer.h"
#include "streaming/stream_analyzer.h"

namespace sonare_node {

/// @brief N-API wrapper around sonare::mastering::api::StreamingMasteringChain.
///
/// Block-by-block streaming variant of the mastering chain. Stages that
/// require whole-signal buffering (repair.denoise, loudness) are rejected at
/// construction. JS surface:
///   const chain = new sonare.StreamingMasteringChain(config);
///   chain.prepare(sampleRate, maxBlockSize, numChannels);
///   const out = chain.processMono(samples);              // mono
///   const { left, right } = chain.processStereo(l, r);   // stereo
///   chain.reset();
///   chain.latencySamples();  // number
///   chain.stageNames();      // string[]
class StreamingMasteringChainWrap : public Napi::ObjectWrap<StreamingMasteringChainWrap> {
 public:
  static Napi::Object Init(Napi::Env env, Napi::Object exports);
  explicit StreamingMasteringChainWrap(const Napi::CallbackInfo& info);
  ~StreamingMasteringChainWrap();

  StreamingMasteringChainWrap(const StreamingMasteringChainWrap&) = delete;
  StreamingMasteringChainWrap& operator=(const StreamingMasteringChainWrap&) = delete;
  StreamingMasteringChainWrap(StreamingMasteringChainWrap&&) = delete;
  StreamingMasteringChainWrap& operator=(StreamingMasteringChainWrap&&) = delete;

 private:
  Napi::Value Prepare(const Napi::CallbackInfo& info);
  Napi::Value ProcessMono(const Napi::CallbackInfo& info);
  Napi::Value ProcessStereo(const Napi::CallbackInfo& info);
  Napi::Value Reset(const Napi::CallbackInfo& info);
  Napi::Value LatencySamples(const Napi::CallbackInfo& info);
  Napi::Value StageNames(const Napi::CallbackInfo& info);

  std::unique_ptr<sonare::mastering::api::StreamingMasteringChain> chain_;

  static Napi::FunctionReference constructor_;
};

/// @brief N-API wrapper around sonare::mastering::eq::EqualizerProcessor.
///
/// Block-by-block streaming equalizer. JS surface:
///   const eq = new sonare.StreamingEqualizer({ sampleRate, maxBlockSize });
///   eq.setBand(index, { type: 'HighShelf', frequencyHz: 8000, gainDb: 6 });
///   const out = eq.processMono(samples);
///   const { left, right } = eq.processStereo(l, r);
///   const snap = eq.spectrum();
///   eq.match(source, reference, { sampleRate, maxBands });
class StreamingEqualizerWrap : public Napi::ObjectWrap<StreamingEqualizerWrap> {
 public:
  static Napi::Object Init(Napi::Env env, Napi::Object exports);
  explicit StreamingEqualizerWrap(const Napi::CallbackInfo& info);
  ~StreamingEqualizerWrap();

  StreamingEqualizerWrap(const StreamingEqualizerWrap&) = delete;
  StreamingEqualizerWrap& operator=(const StreamingEqualizerWrap&) = delete;
  StreamingEqualizerWrap(StreamingEqualizerWrap&&) = delete;
  StreamingEqualizerWrap& operator=(StreamingEqualizerWrap&&) = delete;

 private:
  Napi::Value SetBand(const Napi::CallbackInfo& info);
  Napi::Value Clear(const Napi::CallbackInfo& info);
  Napi::Value SetPhaseMode(const Napi::CallbackInfo& info);
  Napi::Value SetAutoGain(const Napi::CallbackInfo& info);
  Napi::Value SetGainScale(const Napi::CallbackInfo& info);
  Napi::Value SetOutputGainDb(const Napi::CallbackInfo& info);
  Napi::Value SetOutputPan(const Napi::CallbackInfo& info);
  Napi::Value SetSidechainMono(const Napi::CallbackInfo& info);
  Napi::Value SetSidechainStereo(const Napi::CallbackInfo& info);
  Napi::Value ClearSidechain(const Napi::CallbackInfo& info);
  Napi::Value LastAutoGainDb(const Napi::CallbackInfo& info);
  Napi::Value LatencySamples(const Napi::CallbackInfo& info);
  Napi::Value ProcessMono(const Napi::CallbackInfo& info);
  Napi::Value ProcessStereo(const Napi::CallbackInfo& info);
  Napi::Value Spectrum(const Napi::CallbackInfo& info);
  Napi::Value Match(const Napi::CallbackInfo& info);

  std::unique_ptr<sonare::mastering::eq::EqualizerProcessor> eq_;
  Napi::Reference<Napi::Float32Array> sidechain_left_;
  Napi::Reference<Napi::Float32Array> sidechain_right_;
  std::array<const float*, 2> sidechain_channels_{};

  static Napi::FunctionReference constructor_;
};

class RealtimeVoiceChangerWrap : public Napi::ObjectWrap<RealtimeVoiceChangerWrap> {
 public:
  static Napi::Object Init(Napi::Env env, Napi::Object exports);
  explicit RealtimeVoiceChangerWrap(const Napi::CallbackInfo& info);
  ~RealtimeVoiceChangerWrap();

  RealtimeVoiceChangerWrap(const RealtimeVoiceChangerWrap&) = delete;
  RealtimeVoiceChangerWrap& operator=(const RealtimeVoiceChangerWrap&) = delete;
  RealtimeVoiceChangerWrap(RealtimeVoiceChangerWrap&&) = delete;
  RealtimeVoiceChangerWrap& operator=(RealtimeVoiceChangerWrap&&) = delete;

 private:
  Napi::Value Prepare(const Napi::CallbackInfo& info);
  Napi::Value Reset(const Napi::CallbackInfo& info);
  Napi::Value SetConfig(const Napi::CallbackInfo& info);
  Napi::Value ConfigJson(const Napi::CallbackInfo& info);
  Napi::Value LatencySamples(const Napi::CallbackInfo& info);
  Napi::Value ProcessMono(const Napi::CallbackInfo& info);
  Napi::Value ProcessMonoInto(const Napi::CallbackInfo& info);
  Napi::Value ProcessInterleaved(const Napi::CallbackInfo& info);
  Napi::Value ProcessInterleavedInto(const Napi::CallbackInfo& info);

  sonare::editing::voice_changer::RealtimeVoiceChanger changer_;
  bool prepared_ = false;
  int max_block_size_ = 0;
  int channels_ = 1;
  // RT-safe scratch buffers allocated once in prepare(). planar_scratch_ holds
  // channels_ * max_block_size_ floats and planar_ptrs_ exposes a pointer
  // table into it for the planar process_block API.
  std::vector<float> planar_scratch_;
  std::vector<float*> planar_ptrs_;

  static Napi::FunctionReference constructor_;
};

Napi::Value RealtimeVoiceChangerPresetNames(const Napi::CallbackInfo& info);
Napi::Value RealtimeVoiceChangerPresetJson(const Napi::CallbackInfo& info);
Napi::Value ValidateRealtimeVoiceChangerPresetJson(const Napi::CallbackInfo& info);

/// @brief N-API wrapper around sonare::StreamAnalyzer.
///
/// Stateful, causal real-time analyzer. Feed audio chunks, then drain
/// per-frame features (mel / chroma / onset / spectral / chord) and read the
/// progressive BPM / key / chord estimate. Mirrors the WASM StreamAnalyzer
/// class (see src/wasm/bindings.cpp). JS surface:
///   const a = new sonare.StreamAnalyzer(config);
///   a.process(samples);                  // internal offset tracking
///   a.processWithOffset(samples, off);   // external offset sync
///   a.availableFrames();                 // number
///   a.readFramesSoa(maxFrames);          // Float32 SOA frame buffer
///   a.readFramesU8(maxFrames);           // quantized 8-bit frame buffer
///   a.readFramesI16(maxFrames);          // quantized 16-bit frame buffer
///   a.reset(baseSampleOffset?);
///   a.stats();                           // progressive estimate
///   a.frameCount(); a.currentTime(); a.sampleRate();
///   a.setExpectedDuration(seconds);
///   a.setNormalizationGain(gain);
///   a.setTuningRefHz(hz);
class StreamAnalyzerWrap : public Napi::ObjectWrap<StreamAnalyzerWrap> {
 public:
  static Napi::Object Init(Napi::Env env, Napi::Object exports);
  explicit StreamAnalyzerWrap(const Napi::CallbackInfo& info);
  ~StreamAnalyzerWrap();

  StreamAnalyzerWrap(const StreamAnalyzerWrap&) = delete;
  StreamAnalyzerWrap& operator=(const StreamAnalyzerWrap&) = delete;
  StreamAnalyzerWrap(StreamAnalyzerWrap&&) = delete;
  StreamAnalyzerWrap& operator=(StreamAnalyzerWrap&&) = delete;

 private:
  Napi::Value Process(const Napi::CallbackInfo& info);
  Napi::Value ProcessWithOffset(const Napi::CallbackInfo& info);
  Napi::Value AvailableFrames(const Napi::CallbackInfo& info);
  Napi::Value ReadFramesSoa(const Napi::CallbackInfo& info);
  Napi::Value ReadFramesU8(const Napi::CallbackInfo& info);
  Napi::Value ReadFramesI16(const Napi::CallbackInfo& info);
  Napi::Value Reset(const Napi::CallbackInfo& info);
  Napi::Value Stats(const Napi::CallbackInfo& info);
  Napi::Value FrameCount(const Napi::CallbackInfo& info);
  Napi::Value CurrentTime(const Napi::CallbackInfo& info);
  Napi::Value SampleRate(const Napi::CallbackInfo& info);
  Napi::Value SetExpectedDuration(const Napi::CallbackInfo& info);
  Napi::Value SetNormalizationGain(const Napi::CallbackInfo& info);
  Napi::Value SetTuningRefHz(const Napi::CallbackInfo& info);

  sonare::StreamConfig config_;
  std::unique_ptr<sonare::StreamAnalyzer> analyzer_;

  static Napi::FunctionReference constructor_;
};

}  // namespace sonare_node

#endif  // SONARE_NODE_SONARE_WRAP_STREAMING_H_
