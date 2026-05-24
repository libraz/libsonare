#ifndef SONARE_NODE_SONARE_WRAP_MIXER_H_
#define SONARE_NODE_SONARE_WRAP_MIXER_H_

#include <napi.h>

#include "sonare_c.h"

namespace sonare_node {

/// @brief N-API wrapper around the scene-based C mixer API (sonare_mixer_*).
///
/// Persistent stereo mixer built from a scene JSON string. Routes per-strip
/// stereo blocks through the compiled routing graph and sums to a stereo
/// master. The raw SonareStrip* handles are never exposed to JS; insert
/// automation is scheduled by strip index. JS surface:
///   const mixer = new sonare.Mixer(sceneJson, sampleRate, blockSize);
///   mixer.compile();
///   mixer.stripCount();  // number
///   mixer.scheduleInsertAutomation(stripIndex, insertIndex, paramId,
///                                  samplePos, value, curve);  // curve 0/1
///   const { left, right, sampleRate } = mixer.processStereo(l, r);
///   const json = mixer.toSceneJson();
///   mixer.destroy();
class MixerWrap : public Napi::ObjectWrap<MixerWrap> {
 public:
  static Napi::Object Init(Napi::Env env, Napi::Object exports);
  explicit MixerWrap(const Napi::CallbackInfo& info);
  ~MixerWrap();

  MixerWrap(const MixerWrap&) = delete;
  MixerWrap& operator=(const MixerWrap&) = delete;
  MixerWrap(MixerWrap&&) = delete;
  MixerWrap& operator=(MixerWrap&&) = delete;

 private:
  Napi::Value Compile(const Napi::CallbackInfo& info);
  Napi::Value ProcessStereo(const Napi::CallbackInfo& info);
  Napi::Value StripCount(const Napi::CallbackInfo& info);
  Napi::Value ScheduleInsertAutomation(const Napi::CallbackInfo& info);
  Napi::Value ToSceneJson(const Napi::CallbackInfo& info);
  void Destroy(const Napi::CallbackInfo& info);

  SonareMixer* mixer_ = nullptr;
  int sample_rate_ = 48000;
  int block_size_ = 0;

  static Napi::FunctionReference constructor_;
};

}  // namespace sonare_node

#endif  // SONARE_NODE_SONARE_WRAP_MIXER_H_
