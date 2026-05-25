#ifndef SONARE_NODE_SONARE_WRAP_MIXER_H_
#define SONARE_NODE_SONARE_WRAP_MIXER_H_

#include <napi.h>

#include "sonare_c.h"

namespace sonare_node {

/// @brief N-API wrapper around the scene-based C mixer API (sonare_mixer_*).
///
/// Persistent stereo mixer built from a scene JSON string. Routes per-strip
/// stereo blocks through the compiled routing graph and sums to a stereo
/// master. The raw SonareStrip* handles are never exposed to JS; strips are
/// addressed by 0-based index or by their string id (see ResolveStrip).
/// Beyond compile/process/automation, the wrapper exposes per-strip solo,
/// polarity, pan-law, channel-delay, VCA, dual-pan, sends, metering, and
/// goniometer reads. JS surface (abbreviated):
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

  // Strip-targeted state setters.
  Napi::Value SetSoloed(const Napi::CallbackInfo& info);
  Napi::Value SetSoloSafe(const Napi::CallbackInfo& info);
  Napi::Value SetPolarityInvert(const Napi::CallbackInfo& info);
  Napi::Value SetPanLaw(const Napi::CallbackInfo& info);
  Napi::Value SetChannelDelaySamples(const Napi::CallbackInfo& info);
  Napi::Value SetVcaOffsetDb(const Napi::CallbackInfo& info);
  Napi::Value SetDualPan(const Napi::CallbackInfo& info);

  // Sends.
  Napi::Value AddSend(const Napi::CallbackInfo& info);
  Napi::Value SetSendDb(const Napi::CallbackInfo& info);

  // Metering.
  Napi::Value StripMeter(const Napi::CallbackInfo& info);
  Napi::Value MeterTap(const Napi::CallbackInfo& info);
  Napi::Value ReadGoniometerLatest(const Napi::CallbackInfo& info);

  // Strip lookup.
  Napi::Value StripById(const Napi::CallbackInfo& info);

  // Strip-targeted automation.
  Napi::Value ScheduleFaderAutomation(const Napi::CallbackInfo& info);
  Napi::Value SchedulePanAutomation(const Napi::CallbackInfo& info);
  Napi::Value ScheduleWidthAutomation(const Napi::CallbackInfo& info);
  Napi::Value ScheduleSendAutomation(const Napi::CallbackInfo& info);

  // Resolves a strip from a JS reference: a number (index) or a string (id).
  // Throws a JS exception and returns nullptr on failure.
  SonareStrip* ResolveStrip(const Napi::CallbackInfo& info, const Napi::Value& ref);

  SonareMixer* mixer_ = nullptr;
  int sample_rate_ = 48000;
  int block_size_ = 0;

  static Napi::FunctionReference constructor_;
};

}  // namespace sonare_node

#endif  // SONARE_NODE_SONARE_WRAP_MIXER_H_
