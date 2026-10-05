#include "sonare_wrap_project_timeline.h"

#include "sonare_wrap_utils.h"

namespace {

// The class constructor, kept so a JS value can be checked with InstanceOf
// before it is unwrapped and so Wrap can construct instances. Persistent and
// deliberately never released: the addon is loaded once per process.
Napi::FunctionReference* g_project_timeline_constructor = nullptr;

}  // namespace

Napi::Object ProjectTimelineWrap::Init(Napi::Env env, Napi::Object exports) {
  Napi::Function func = DefineClass(env, "ProjectTimeline",
                                    {
                                        InstanceMethod<&ProjectTimelineWrap::Destroy>("destroy"),
                                    });
  g_project_timeline_constructor = new Napi::FunctionReference();
  *g_project_timeline_constructor = Napi::Persistent(func);
  g_project_timeline_constructor->SuppressDestruct();
  exports.Set("ProjectTimeline", func);
  return exports;
}

Napi::Object ProjectTimelineWrap::Wrap(Napi::Env env, SonareProjectTimeline* handle) {
  Napi::External<SonareProjectTimeline> external =
      Napi::External<SonareProjectTimeline>::New(env, handle);
  return g_project_timeline_constructor->New({external});
}

ProjectTimelineWrap::ProjectTimelineWrap(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<ProjectTimelineWrap>(info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 1 || !info[0].IsExternal()) {
    Napi::TypeError::New(env, "ProjectTimeline is created by Project.compileTimeline()")
        .ThrowAsJavaScriptException();
    return;
  }
  timeline_ = info[0].As<Napi::External<SonareProjectTimeline>>().Data();
  SONARE_NODE_CATCH_VOID(env)
}

ProjectTimelineWrap::~ProjectTimelineWrap() {
  if (timeline_ != nullptr) {
    sonare_project_timeline_destroy(timeline_);
    timeline_ = nullptr;
  }
}

bool ProjectTimelineWrap::ReadHandle(Napi::Env env, const Napi::Value& value,
                                     const SonareProjectTimeline** out) {
  if (env.IsExceptionPending() || out == nullptr) return false;
  *out = nullptr;
  if (g_project_timeline_constructor == nullptr || !value.IsObject() ||
      !value.As<Napi::Object>().InstanceOf(g_project_timeline_constructor->Value())) {
    Napi::TypeError::New(env, "timeline must be a ProjectTimeline instance")
        .ThrowAsJavaScriptException();
    return false;
  }
  ProjectTimelineWrap* wrap = ProjectTimelineWrap::Unwrap(value.As<Napi::Object>());
  if (wrap == nullptr || wrap->timeline_ == nullptr) {
    Napi::TypeError::New(env, "timeline is disposed").ThrowAsJavaScriptException();
    return false;
  }
  *out = wrap->timeline_;
  return true;
}

void ProjectTimelineWrap::Destroy(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY(void) info;
  if (timeline_ != nullptr) {
    sonare_project_timeline_destroy(timeline_);
    timeline_ = nullptr;
  }
  SONARE_NODE_CATCH_VOID(env)
}
