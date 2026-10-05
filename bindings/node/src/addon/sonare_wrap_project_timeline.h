#ifndef SONARE_NODE_SONARE_WRAP_PROJECT_TIMELINE_H_
#define SONARE_NODE_SONARE_WRAP_PROJECT_TIMELINE_H_

#include <napi.h>
#include <sonare/sonare_c.h>

/// @brief N-API ObjectWrap over the opaque compiled-timeline handle
///        (@ref SonareProjectTimeline).
///
/// Only @c Project.compileTimeline() creates one (through @ref Wrap). The C
/// handle is not GC-aware: @ref Destroy is the deterministic release the facade
/// exposes as `dispose()`, and the destructor is the finalizer backstop. An
/// engine the timeline was applied to keeps its own reference, so disposing
/// right after the apply is safe.
class ProjectTimelineWrap : public Napi::ObjectWrap<ProjectTimelineWrap> {
 public:
  static Napi::Object Init(Napi::Env env, Napi::Object exports);
  explicit ProjectTimelineWrap(const Napi::CallbackInfo& info);
  ~ProjectTimelineWrap();

  /// @brief Wrap a freshly compiled handle, taking ownership of it.
  static Napi::Object Wrap(Napi::Env env, SonareProjectTimeline* handle);

  /// @brief Read the native timeline a JS value carries into @p out.
  ///
  /// A value that is not a ProjectTimeline instance, and an already disposed
  /// timeline, are each exactly one catchable TypeError and a false return, so
  /// the caller bails out before its C-ABI call.
  static bool ReadHandle(Napi::Env env, const Napi::Value& value,
                         const SonareProjectTimeline** out);

 private:
  void Destroy(const Napi::CallbackInfo& info);

  SonareProjectTimeline* timeline_ = nullptr;
};

#endif  // SONARE_NODE_SONARE_WRAP_PROJECT_TIMELINE_H_
