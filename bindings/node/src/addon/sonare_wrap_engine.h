#ifndef SONARE_NODE_SONARE_WRAP_ENGINE_H_
#define SONARE_NODE_SONARE_WRAP_ENGINE_H_

#include <napi.h>

#include <vector>

#include "sonare_c.h"

class RealtimeEngineWrap : public Napi::ObjectWrap<RealtimeEngineWrap> {
 public:
  static Napi::Object Init(Napi::Env env, Napi::Object exports);
  explicit RealtimeEngineWrap(const Napi::CallbackInfo& info);
  ~RealtimeEngineWrap();

 private:
  Napi::Value Prepare(const Napi::CallbackInfo& info);
  Napi::Value Play(const Napi::CallbackInfo& info);
  Napi::Value Stop(const Napi::CallbackInfo& info);
  Napi::Value SeekSample(const Napi::CallbackInfo& info);
  Napi::Value SeekPpq(const Napi::CallbackInfo& info);
  Napi::Value SetTempo(const Napi::CallbackInfo& info);
  Napi::Value SetTimeSignature(const Napi::CallbackInfo& info);
  Napi::Value SetLoop(const Napi::CallbackInfo& info);
  Napi::Value AddParameter(const Napi::CallbackInfo& info);
  Napi::Value ParameterCount(const Napi::CallbackInfo& info);
  Napi::Value ParameterInfoByIndex(const Napi::CallbackInfo& info);
  Napi::Value ParameterInfo(const Napi::CallbackInfo& info);
  Napi::Value SetAutomationLane(const Napi::CallbackInfo& info);
  Napi::Value AutomationLaneCount(const Napi::CallbackInfo& info);
  Napi::Value SetMarkers(const Napi::CallbackInfo& info);
  Napi::Value MarkerCount(const Napi::CallbackInfo& info);
  Napi::Value MarkerByIndex(const Napi::CallbackInfo& info);
  Napi::Value Marker(const Napi::CallbackInfo& info);
  Napi::Value SeekMarker(const Napi::CallbackInfo& info);
  Napi::Value SetLoopFromMarkers(const Napi::CallbackInfo& info);
  Napi::Value SetMetronome(const Napi::CallbackInfo& info);
  Napi::Value Metronome(const Napi::CallbackInfo& info);
  Napi::Value CountInEndSample(const Napi::CallbackInfo& info);
  Napi::Value SetClips(const Napi::CallbackInfo& info);
  Napi::Value ClipCount(const Napi::CallbackInfo& info);
  Napi::Value SetCaptureBuffer(const Napi::CallbackInfo& info);
  Napi::Value ArmCapture(const Napi::CallbackInfo& info);
  Napi::Value SetCapturePunch(const Napi::CallbackInfo& info);
  Napi::Value ResetCapture(const Napi::CallbackInfo& info);
  Napi::Value CaptureStatus(const Napi::CallbackInfo& info);
  Napi::Value CapturedAudio(const Napi::CallbackInfo& info);
  Napi::Value SetGraph(const Napi::CallbackInfo& info);
  Napi::Value GraphNodeCount(const Napi::CallbackInfo& info);
  Napi::Value GraphConnectionCount(const Napi::CallbackInfo& info);
  Napi::Value Process(const Napi::CallbackInfo& info);
  Napi::Value RenderOffline(const Napi::CallbackInfo& info);
  Napi::Value BounceOffline(const Napi::CallbackInfo& info);
  Napi::Value FreezeOffline(const Napi::CallbackInfo& info);
  Napi::Value DrainTelemetry(const Napi::CallbackInfo& info);
  Napi::Value DrainMeterTelemetry(const Napi::CallbackInfo& info);
  Napi::Value SetParameter(const Napi::CallbackInfo& info);
  Napi::Value SetParameterSmoothed(const Napi::CallbackInfo& info);
  Napi::Value GetTransportState(const Napi::CallbackInfo& info);
  void Destroy(const Napi::CallbackInfo& info);

  SonareRealtimeEngine* engine_ = nullptr;
  std::vector<Napi::Reference<Napi::Float32Array>> capture_refs_;
  std::vector<float*> capture_ptrs_;
  int64_t capture_capacity_frames_ = 0;
};

#endif  // SONARE_NODE_SONARE_WRAP_ENGINE_H_
