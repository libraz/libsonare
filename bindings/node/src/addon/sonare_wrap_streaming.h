#ifndef SONARE_NODE_SONARE_WRAP_STREAMING_H_
#define SONARE_NODE_SONARE_WRAP_STREAMING_H_

#include <napi.h>

#include <memory>

#include "mastering/api/chain.h"
#include "mastering/eq/equalizer.h"

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
  Napi::Value LastAutoGainDb(const Napi::CallbackInfo& info);
  Napi::Value LatencySamples(const Napi::CallbackInfo& info);
  Napi::Value ProcessMono(const Napi::CallbackInfo& info);
  Napi::Value ProcessStereo(const Napi::CallbackInfo& info);
  Napi::Value Spectrum(const Napi::CallbackInfo& info);
  Napi::Value Match(const Napi::CallbackInfo& info);

  std::unique_ptr<sonare::mastering::eq::EqualizerProcessor> eq_;

  static Napi::FunctionReference constructor_;
};

}  // namespace sonare_node

#endif  // SONARE_NODE_SONARE_WRAP_STREAMING_H_
