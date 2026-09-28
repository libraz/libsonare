#ifndef SONARE_NODE_SONARE_WRAP_PLAYBACK_H_
#define SONARE_NODE_SONARE_WRAP_PLAYBACK_H_

#include <napi.h>
#include <sonare/sonare_c.h>

/// @brief N-API ObjectWrap over an opaque HRTF set (@ref SonareHrtfSet).
///
/// Built through one of the two static factories, never through `new` directly.
/// A renderer that adopts one keeps its own copy (see @ref
/// sonare_playback_renderer_create_json), so disposing this handle after
/// construction is safe. JS surface:
///   const hrtf = HrtfSet.default();             // native only
///   const hrtf = HrtfSet.fromBytes(shrfBytes);   // SHRF v1
///   hrtf.destroy();
class HrtfSetWrap : public Napi::ObjectWrap<HrtfSetWrap> {
 public:
  static Napi::Object Init(Napi::Env env, Napi::Object exports);
  explicit HrtfSetWrap(const Napi::CallbackInfo& info);
  ~HrtfSetWrap();

  HrtfSetWrap(const HrtfSetWrap&) = delete;
  HrtfSetWrap& operator=(const HrtfSetWrap&) = delete;
  HrtfSetWrap(HrtfSetWrap&&) = delete;
  HrtfSetWrap& operator=(HrtfSetWrap&&) = delete;

  /// @brief Reads @p value as an optional HrtfSet instance.
  /// @details undefined/null is a legal "no HRTF" and reports `*out = nullptr`
  ///          with a true return. Any other value must be a live HrtfSet
  ///          instance; a wrong type or a destroyed one is refused by name.
  /// @return false with exactly one pending JS exception on rejection.
  static bool ReadHandle(Napi::Env env, const Napi::Value& value, const SonareHrtfSet** out);

 private:
  static Napi::Value Default(const Napi::CallbackInfo& info);
  static Napi::Value FromBytes(const Napi::CallbackInfo& info);
  Napi::Value Destroy(const Napi::CallbackInfo& info);

  // Adopts an already-created native handle (used by the static factories).
  static Napi::Object Wrap(const Napi::CallbackInfo& info, SonareHrtfSet* handle);

  SonareHrtfSet* set_ = nullptr;
};

/// @brief N-API ObjectWrap over the opaque playback renderer handle
///        (@ref SonarePlaybackRenderer). Every method routes through
///        `sonare_playback_renderer_*` (include/sonare/sonare_c_playback.h),
///        the oracle this binding does not reinterpret.
///
/// The constructor and every JS-facing method take positional arguments; the
/// request-object shape (`{ config, hrtf, sampleRate, maxBlockSize }`) is
/// normalized to this positional call by the TypeScript facade
/// (src/playback.ts), matching this binding's WASM sibling.
///
/// `processPlanar` / `processInterleaved` / `setHeadOrientation` are the three
/// realtime-safe C-ABI entries (`SONARE_C_RT_API_ENTRY`): their failures never
/// touch `sonare_last_error_message`, so every call site here reports through
/// `ThrowIfRealtimeError`, never `ThrowIfError`.
class PlaybackRendererWrap : public Napi::ObjectWrap<PlaybackRendererWrap> {
 public:
  static Napi::Object Init(Napi::Env env, Napi::Object exports);
  explicit PlaybackRendererWrap(const Napi::CallbackInfo& info);
  ~PlaybackRendererWrap();

  PlaybackRendererWrap(const PlaybackRendererWrap&) = delete;
  PlaybackRendererWrap& operator=(const PlaybackRendererWrap&) = delete;
  PlaybackRendererWrap(PlaybackRendererWrap&&) = delete;
  PlaybackRendererWrap& operator=(PlaybackRendererWrap&&) = delete;

 private:
  Napi::Value ProcessPlanar(const Napi::CallbackInfo& info);
  Napi::Value ProcessInterleaved(const Napi::CallbackInfo& info);
  Napi::Value SetConfig(const Napi::CallbackInfo& info);
  Napi::Value Config(const Napi::CallbackInfo& info);
  Napi::Value SetHeadOrientation(const Napi::CallbackInfo& info);
  Napi::Value Reset(const Napi::CallbackInfo& info);
  Napi::Value LatencySamples(const Napi::CallbackInfo& info);
  Napi::Value InputChannels(const Napi::CallbackInfo& info);
  Napi::Value OutputChannels(const Napi::CallbackInfo& info);
  Napi::Value Diagnostics(const Napi::CallbackInfo& info);
  Napi::Value NonFiniteDiscardCount(const Napi::CallbackInfo& info);
  Napi::Value Destroy(const Napi::CallbackInfo& info);
  bool EnsureAlive(Napi::Env env) const;

  SonarePlaybackRenderer* renderer_ = nullptr;
};

/// @brief N-API ObjectWrap over the opaque integrated-loudness meter
///        (@ref SonarePlaybackLoudnessMeter). JS surface:
///   const meter = new PlaybackLoudnessMeter(channels, sampleRate);
///   meter.pushInterleaved(samples);
///   meter.integratedLufs();
///   meter.destroy();
class PlaybackLoudnessMeterWrap : public Napi::ObjectWrap<PlaybackLoudnessMeterWrap> {
 public:
  static Napi::Object Init(Napi::Env env, Napi::Object exports);
  explicit PlaybackLoudnessMeterWrap(const Napi::CallbackInfo& info);
  ~PlaybackLoudnessMeterWrap();

  PlaybackLoudnessMeterWrap(const PlaybackLoudnessMeterWrap&) = delete;
  PlaybackLoudnessMeterWrap& operator=(const PlaybackLoudnessMeterWrap&) = delete;
  PlaybackLoudnessMeterWrap(PlaybackLoudnessMeterWrap&&) = delete;
  PlaybackLoudnessMeterWrap& operator=(PlaybackLoudnessMeterWrap&&) = delete;

 private:
  Napi::Value PushInterleaved(const Napi::CallbackInfo& info);
  Napi::Value IntegratedLufs(const Napi::CallbackInfo& info);
  Napi::Value Destroy(const Napi::CallbackInfo& info);
  bool EnsureAlive(Napi::Env env) const;

  SonarePlaybackLoudnessMeter* meter_ = nullptr;
};

namespace sonare_node {

/// @brief `renderPlayback(samples, inChannels, sampleRate, configJson, hrtf?)`
///        — the offline one-shot render (@ref
///        sonare_playback_render_interleaved). Positional like the other
///        handle-free bridges on this surface; the facade's request object is
///        folded before the call. Defined in playback/render_offline.cpp.
Napi::Value RenderPlayback(const Napi::CallbackInfo& info);

}  // namespace sonare_node

#endif  // SONARE_NODE_SONARE_WRAP_PLAYBACK_H_
