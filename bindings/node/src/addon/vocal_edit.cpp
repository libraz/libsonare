#include "vocal_edit.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "c_api/editing_input_domains.h"
#include "core/audio.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_utils.h"
#include "vocal_edit_convert.h"

namespace sonare_node {

using namespace vocal_edit_detail;

Napi::FunctionReference VocalEditSessionWrap::constructor_;
Napi::FunctionReference VocalEditDraftWrap::constructor_;
Napi::FunctionReference VocalRenderSnapshotWrap::constructor_;
Napi::FunctionReference VocalRenderJobWrap::constructor_;

namespace {

Napi::Value RequireSessionValue(Napi::Env env, SonareVocalSession* session) {
  if (session != nullptr) return env.Undefined();
  CheckVocalError(env, SONARE_ERROR_INVALID_STATE);
  return env.Undefined();
}

// Reads the (range, requestId?) leading arguments of the render entry points;
// false leaves one pending exception.
bool ReadRenderRange(Napi::Env env, const Napi::CallbackInfo& info, SonareVocalRange* range,
                     uint64_t* request_id) {
  if (info.Length() == 0) {
    Napi::TypeError::New(env, "range must be an object").ThrowAsJavaScriptException();
    return false;
  }
  if (!ReadRange(env, info[0], range)) return false;
  if (info.Length() > 1 && !info[1].IsUndefined() && !info[1].IsNull() &&
      !RequiredUint64Value(env, info[1], "requestId", request_id)) {
    return false;
  }
  return true;
}

class VocalRenderAsyncWorker final : public Napi::AsyncWorker {
 public:
  VocalRenderAsyncWorker(Napi::Env env, SonareVocalRenderJob* job, const Napi::Object& snapshot,
                         std::shared_ptr<std::atomic_bool> cancelled, Napi::Object signal,
                         Napi::Function listener)
      : Napi::AsyncWorker(env),
        deferred_(Napi::Promise::Deferred::New(env)),
        job_(job),
        snapshot_(Napi::Persistent(snapshot)),
        cancelled_(std::move(cancelled)),
        signal_(signal.IsEmpty() ? Napi::ObjectReference{} : Napi::Persistent(signal)),
        listener_(listener.IsEmpty() ? Napi::FunctionReference{} : Napi::Persistent(listener)) {}

  ~VocalRenderAsyncWorker() override {
    if (result_initialized_) {
      sonare_free_vocal_render_result(&result_);
      result_initialized_ = false;
    }
    if (job_ != nullptr) {
      sonare_vocal_render_job_destroy(job_);
      job_ = nullptr;
    }
  }

  Napi::Promise Promise() { return deferred_.Promise(); }

  void Execute() override {
    int complete = 0;
    while (!complete) {
      const SonareError error = sonare_vocal_render_job_next(
          job_, &VocalRenderAsyncWorker::CancelCallback, cancelled_.get(), &complete);
      if (error != SONARE_OK) {
        CaptureError(error);
        return;
      }
    }
    sonare_vocal_render_result_init(&result_);
    result_initialized_ = true;
    const SonareError error = sonare_vocal_render_job_finalize(
        job_, &VocalRenderAsyncWorker::CancelCallback, cancelled_.get(), &result_);
    if (error != SONARE_OK) {
      // Capture first: the message is this worker thread's last error.
      CaptureError(error);
      sonare_free_vocal_render_result(&result_);
      result_initialized_ = false;
      return;
    }
  }

  void OnOK() override {
    Napi::HandleScope scope(Env());
    DetachSignal();
    deferred_.Resolve(RenderResultToObject(Env(), result_));
    sonare_free_vocal_render_result(&result_);
    result_initialized_ = false;
    if (job_ != nullptr) {
      sonare_vocal_render_job_destroy(job_);
      job_ = nullptr;
    }
  }

  void OnError(const Napi::Error& error) override {
    Napi::HandleScope scope(Env());
    DetachSignal();
    Napi::Object value = error.Value();
    sonare_node::DecorateSonareError(Env(), value, error_code_, shape_refusal_, refusal_,
                                     refusal_entry_);
    value.Set("reason", detail_.reason);
    value.Set("field", detail_.field);
    value.Set("expected", detail_.expected_text[0] == '\0' ? Uint64String(detail_.expected)
                                                           : detail_.expected_text);
    value.Set("actual",
              detail_.actual_text[0] == '\0' ? Uint64String(detail_.actual) : detail_.actual_text);
    value.Set("expectedText", detail_.expected_text);
    value.Set("actualText", detail_.actual_text);
    deferred_.Reject(value);
    if (result_initialized_) {
      sonare_free_vocal_render_result(&result_);
      result_initialized_ = false;
    }
    if (job_ != nullptr) {
      sonare_vocal_render_job_destroy(job_);
      job_ = nullptr;
    }
  }

 private:
  static int CancelCallback(void* user_data) {
    return static_cast<std::atomic_bool*>(user_data)->load(std::memory_order_relaxed) ? 1 : 0;
  }

  void CaptureError(SonareError error) {
    error_code_ = error;
    shape_refusal_ = sonare_node::LastRefusalIsShape();
    refusal_ = sonare_node::ShownLastRefusal();
    refusal_entry_ = sonare_node::LastRefusalEntry();
    sonare_vocal_error_detail_init(&detail_);
    sonare_vocal_last_error_detail(&detail_);
    // Runs on the render worker thread, whose thread-local holds the refusal's detail.
    SetError(sonare_node::ErrorMessageForCode(error));
  }

  void DetachSignal() {
    const Napi::Env env = Env();
    const bool had_pending_exception = env.IsExceptionPending();
    if (!had_pending_exception && !signal_.IsEmpty() && !listener_.IsEmpty()) {
      const Napi::Value remove = signal_.Value().Get("removeEventListener");
      if (!env.IsExceptionPending() && remove.IsFunction()) {
        remove.As<Napi::Function>().Call(signal_.Value(),
                                         {Napi::String::New(env, "abort"), listener_.Value()});
      }
      // Swallow detach errors so a throwing AbortSignal cannot poison the env before cleanup.
      if (env.IsExceptionPending()) {
        env.GetAndClearPendingException();
      }
    }
    listener_.Reset();
    signal_.Reset();
    snapshot_.Reset();
  }

  Napi::Promise::Deferred deferred_;
  SonareVocalRenderJob* job_ = nullptr;
  Napi::ObjectReference snapshot_;
  std::shared_ptr<std::atomic_bool> cancelled_;
  Napi::ObjectReference signal_;
  Napi::FunctionReference listener_;
  SonareVocalRenderResult result_{};
  bool result_initialized_ = false;
  SonareError error_code_ = SONARE_ERROR_UNKNOWN;
  bool shape_refusal_ = false;
  std::optional<sonare::SonareParameterRefusal> refusal_;
  std::string refusal_entry_;
  SonareVocalErrorDetail detail_{};
};

struct VocalRenderJobDeleter {
  void operator()(SonareVocalRenderJob* job) const noexcept {
    if (job != nullptr) sonare_vocal_render_job_destroy(job);
  }
};

bool PrepareAbortSignal(Napi::Env env, const Napi::Value& value,
                        const std::shared_ptr<std::atomic_bool>& cancelled,
                        Napi::Object* signal_out, Napi::Function* listener_out) {
  if (value.IsUndefined() || value.IsNull()) return true;
  if (!value.IsObject()) {
    Napi::TypeError::New(env, "signal must be an AbortSignal-like object")
        .ThrowAsJavaScriptException();
    return false;
  }
  const Napi::Object signal = value.As<Napi::Object>();
  const Napi::Value aborted = signal.Get("aborted");
  if (!aborted.IsUndefined() && !aborted.IsBoolean()) {
    Napi::TypeError::New(env, "signal.aborted must be a boolean").ThrowAsJavaScriptException();
    return false;
  }
  if (aborted.IsBoolean() && aborted.As<Napi::Boolean>().Value()) {
    cancelled->store(true, std::memory_order_relaxed);
  }
  const Napi::Value add = signal.Get("addEventListener");
  if (!add.IsFunction()) {
    Napi::TypeError::New(env, "signal must provide addEventListener").ThrowAsJavaScriptException();
    return false;
  }
  const Napi::Function listener = Napi::Function::New(env, [cancelled](const Napi::CallbackInfo&) {
    cancelled->store(true, std::memory_order_relaxed);
  });
  add.As<Napi::Function>().Call(signal, {Napi::String::New(env, "abort"), listener});
  sonare_node::MarkPendingForeign(env);
  if (env.IsExceptionPending()) return false;
  *signal_out = signal;
  *listener_out = listener;
  return true;
}

}  // namespace

Napi::Object VocalEditSessionWrap::Init(Napi::Env env, Napi::Object exports) {
  const Napi::Function function = DefineClass(
      env, "VocalEditSession",
      {
          InstanceMethod<&VocalEditSessionWrap::Notes>("notes", napi_default_method),
          InstanceMethod<&VocalEditSessionWrap::Analysis>("analysis", napi_default_method),
          InstanceMethod<&VocalEditSessionWrap::Capabilities>("capabilities", napi_default_method),
          InstanceMethod<&VocalEditSessionWrap::Token>("token", napi_default_method),
          InstanceMethod<&VocalEditSessionWrap::Revision>("revision", napi_default_method),
          InstanceMethod<&VocalEditSessionWrap::OutputLengthSamples>("outputLengthSamples",
                                                                     napi_default_method),
          InstanceMethod<&VocalEditSessionWrap::History>("history", napi_default_method),
          InstanceMethod<&VocalEditSessionWrap::BeginEdit>("beginEdit", napi_default_method),
          InstanceMethod<&VocalEditSessionWrap::Undo>("undo", napi_default_method),
          InstanceMethod<&VocalEditSessionWrap::Redo>("redo", napi_default_method),
          InstanceMethod<&VocalEditSessionWrap::EvaluatePitch>("evaluatePitch",
                                                               napi_default_method),
          InstanceMethod<&VocalEditSessionWrap::SourceSampleToDestinationSample>(
              "sourceSampleToDestinationSample", napi_default_method),
          InstanceMethod<&VocalEditSessionWrap::DestinationSampleToSourceSample>(
              "destinationSampleToSourceSample", napi_default_method),
          InstanceMethod<&VocalEditSessionWrap::CaptureRenderSnapshot>("captureRenderSnapshot",
                                                                       napi_default_method),
          InstanceMethod<&VocalEditSessionWrap::ExportState>("exportState", napi_default_method),
          InstanceMethod<&VocalEditSessionWrap::Destroy>("destroy", napi_default_method),
      });
  constructor_ = Napi::Persistent(function);
  constructor_.SuppressDestruct();
  exports.Set("VocalEditSession", function);
  return exports;
}

Napi::Object VocalEditSessionWrap::NewInstance(Napi::Env env, SonareVocalSession* session) {
  (void)env;
  Napi::Object object = constructor_.New({});
  VocalEditSessionWrap::Unwrap(object)->session_ = session;
  return object;
}

VocalEditSessionWrap::VocalEditSessionWrap(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<VocalEditSessionWrap>(info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_CATCH_VOID(env)
}

VocalEditSessionWrap::~VocalEditSessionWrap() {
  // Env teardown finalizes in any order; a surviving draft must not reach back into this wrapper.
  for (VocalEditDraftWrap* draft : drafts_) draft->DetachOwner();
  if (session_ != nullptr) sonare_vocal_session_destroy(session_);
  session_ = nullptr;
}

Napi::Value VocalEditSessionWrap::Notes(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (session_ == nullptr) return RequireSessionValue(env, session_);
  NotesGuard notes;
  const SonareError error = sonare_vocal_session_notes(session_, &notes.value);
  if (!CheckVocalError(env, error)) return env.Undefined();
  return NotesResultToObject(env, notes.value);
  SONARE_NODE_CATCH(env)
}

Napi::Value VocalEditSessionWrap::Analysis(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (session_ == nullptr) return RequireSessionValue(env, session_);
  AnalysisGuard analysis;
  const SonareError error = sonare_vocal_session_analysis(session_, &analysis.value);
  if (!CheckVocalError(env, error)) return env.Undefined();
  return AnalysisResultToObject(env, analysis.value);
  SONARE_NODE_CATCH(env)
}

Napi::Value VocalEditSessionWrap::Capabilities(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (session_ == nullptr) return RequireSessionValue(env, session_);
  SonareVocalCapabilities capabilities{};
  sonare_vocal_capabilities_init(&capabilities);
  const SonareError error = sonare_vocal_session_capabilities(session_, &capabilities);
  if (!CheckVocalError(env, error)) return env.Undefined();
  return CapabilitiesToObject(env, capabilities);
  SONARE_NODE_CATCH(env)
}

Napi::Value VocalEditSessionWrap::Token(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (session_ == nullptr) return RequireSessionValue(env, session_);
  SonareVocalStateToken token{};
  const SonareError error = sonare_vocal_session_token(session_, &token);
  if (!CheckVocalError(env, error)) return env.Undefined();
  return TokenToObject(env, token);
  SONARE_NODE_CATCH(env)
}

Napi::Value VocalEditSessionWrap::Revision(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (session_ == nullptr) return RequireSessionValue(env, session_);
  uint64_t revision = 0;
  const SonareError error = sonare_vocal_session_revision(session_, &revision);
  if (!CheckVocalError(env, error)) return env.Undefined();
  return Napi::String::New(env, Uint64String(revision));
  SONARE_NODE_CATCH(env)
}

Napi::Value VocalEditSessionWrap::OutputLengthSamples(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (session_ == nullptr) return RequireSessionValue(env, session_);
  int64_t samples = 0;
  const SonareError error = sonare_vocal_session_output_length(session_, &samples);
  if (!CheckVocalError(env, error)) return env.Undefined();
  if (samples < 0 || static_cast<double>(samples) > kMaxSafeInteger) {
    Napi::RangeError::New(env, "output length cannot be represented as a JavaScript number")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return Napi::Number::New(env, static_cast<double>(samples));
  SONARE_NODE_CATCH(env)
}

Napi::Value VocalEditSessionWrap::History(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (session_ == nullptr) return RequireSessionValue(env, session_);
  int can_undo = 0;
  int can_redo = 0;
  const SonareError error = sonare_vocal_session_history(session_, &can_undo, &can_redo);
  if (!CheckVocalError(env, error)) return env.Undefined();
  Napi::Object result = Napi::Object::New(env);
  result.Set("canUndo", Napi::Boolean::New(env, can_undo != 0));
  result.Set("canRedo", Napi::Boolean::New(env, can_redo != 0));
  return result;
  SONARE_NODE_CATCH(env)
}

Napi::Value VocalEditSessionWrap::BeginEdit(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (session_ == nullptr) return RequireSessionValue(env, session_);
  uint64_t revision = 0;
  if (info.Length() != 0 && !info[0].IsUndefined() && !info[0].IsNull() &&
      !RequiredUint64Value(env, info[0], "expectedRevision", &revision)) {
    return env.Undefined();
  }
  if (info.Length() == 0 || info[0].IsUndefined() || info[0].IsNull()) {
    if (!CheckVocalError(env, sonare_vocal_session_revision(session_, &revision))) {
      return env.Undefined();
    }
  }
  SonareVocalDraft* draft = nullptr;
  const SonareError error = sonare_vocal_session_begin_edit(session_, revision, &draft);
  if (!CheckVocalError(env, error)) return env.Undefined();
  return VocalEditDraftWrap::NewInstance(env, draft, this, info.This().As<Napi::Object>());
  SONARE_NODE_CATCH(env)
}

Napi::Value VocalEditSessionWrap::Undo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (session_ == nullptr) return RequireSessionValue(env, session_);
  uint64_t revision = 0;
  if (info.Length() != 0 && !info[0].IsUndefined() && !info[0].IsNull() &&
      !RequiredUint64Value(env, info[0], "expectedRevision", &revision)) {
    return env.Undefined();
  }
  if ((info.Length() == 0 || info[0].IsUndefined() || info[0].IsNull()) &&
      !CheckVocalError(env, sonare_vocal_session_revision(session_, &revision))) {
    return env.Undefined();
  }
  EditGuard result;
  const SonareError error = sonare_vocal_session_undo(session_, revision, &result.value);
  if (!CheckVocalError(env, error)) return env.Undefined();
  return EditResultToObject(env, result.value);
  SONARE_NODE_CATCH(env)
}

Napi::Value VocalEditSessionWrap::Redo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (session_ == nullptr) return RequireSessionValue(env, session_);
  uint64_t revision = 0;
  if (info.Length() != 0 && !info[0].IsUndefined() && !info[0].IsNull() &&
      !RequiredUint64Value(env, info[0], "expectedRevision", &revision)) {
    return env.Undefined();
  }
  if ((info.Length() == 0 || info[0].IsUndefined() || info[0].IsNull()) &&
      !CheckVocalError(env, sonare_vocal_session_revision(session_, &revision))) {
    return env.Undefined();
  }
  EditGuard result;
  const SonareError error = sonare_vocal_session_redo(session_, revision, &result.value);
  if (!CheckVocalError(env, error)) return env.Undefined();
  return EditResultToObject(env, result.value);
  SONARE_NODE_CATCH(env)
}

namespace {

template <typename Handle>
Napi::Value EvaluateNotePitch(const Napi::CallbackInfo& info, const Handle* handle,
                              SonareError (*evaluate)(const Handle*, uint32_t,
                                                      SonareVocalPitchResult*)) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (handle == nullptr) return RequireSessionValue(env, nullptr);
  uint32_t note_id = 0;
  if (info.Length() == 0 || !ReadNoteId(env, info[0], "noteId", &note_id)) return env.Undefined();
  PitchGuard result;
  const SonareError error = evaluate(handle, note_id, &result.value);
  if (!CheckVocalError(env, error)) return env.Undefined();
  return PitchResultToObject(env, result.value);
  SONARE_NODE_CATCH(env)
}

template <typename Handle>
Napi::Value MapNoteSample(const Napi::CallbackInfo& info, const Handle* handle,
                          const char* sample_name,
                          SonareError (*map)(const Handle*, uint32_t, double, double*)) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (handle == nullptr) return RequireSessionValue(env, nullptr);
  uint32_t note_id = 0;
  double input_sample = 0.0;
  if (info.Length() < 2 || !ReadNoteId(env, info[0], "noteId", &note_id) ||
      !RequiredFiniteDoubleValue(env, info[1], sample_name, &input_sample))
    return env.Undefined();
  double output_sample = 0.0;
  const SonareError error = map(handle, note_id, input_sample, &output_sample);
  if (!CheckVocalError(env, error)) return env.Undefined();
  return Napi::Number::New(env, output_sample);
  SONARE_NODE_CATCH(env)
}

}  // namespace

Napi::Value VocalEditSessionWrap::EvaluatePitch(const Napi::CallbackInfo& info) {
  return EvaluateNotePitch(info, session_, &sonare_vocal_session_evaluate_pitch);
}

Napi::Value VocalEditSessionWrap::SourceSampleToDestinationSample(const Napi::CallbackInfo& info) {
  return MapNoteSample(info, session_, "sourceSample", &sonare_vocal_session_source_to_destination);
}

Napi::Value VocalEditSessionWrap::DestinationSampleToSourceSample(const Napi::CallbackInfo& info) {
  return MapNoteSample(info, session_, "destinationSample",
                       &sonare_vocal_session_destination_to_source);
}

Napi::Value VocalEditSessionWrap::CaptureRenderSnapshot(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (session_ == nullptr) return RequireSessionValue(env, session_);
  SonareVocalSnapshot* snapshot = nullptr;
  const SonareError error = sonare_vocal_session_capture_snapshot(session_, &snapshot);
  if (!CheckVocalError(env, error)) return env.Undefined();
  return VocalRenderSnapshotWrap::NewInstance(env, snapshot);
  SONARE_NODE_CATCH(env)
}

Napi::Value VocalEditSessionWrap::ExportState(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (session_ == nullptr) return RequireSessionValue(env, session_);
  StateGuard state;
  const SonareError error = sonare_vocal_session_export_state(session_, &state.value);
  if (!CheckVocalError(env, error)) return env.Undefined();
  Napi::Uint8Array result = Napi::Uint8Array::New(env, state.value.size);
  if (state.value.size != 0) std::memcpy(result.Data(), state.value.data, state.value.size);
  return result;
  SONARE_NODE_CATCH(env)
}

void VocalEditSessionWrap::Destroy(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (session_ == nullptr) return;
  // Drafts reference the session, so they are disposed first.
  const std::vector<VocalEditDraftWrap*> drafts = drafts_;
  for (VocalEditDraftWrap* draft : drafts) draft->DisposeForOwner();
  drafts_.clear();
  sonare_vocal_session_destroy(session_);
  session_ = nullptr;
  destroyed_ = true;
  SONARE_NODE_CATCH_VOID(env)
}

Napi::Object VocalEditDraftWrap::Init(Napi::Env env, Napi::Object exports) {
  const Napi::Function function = DefineClass(
      env, "VocalEditDraft",
      {
          InstanceMethod<&VocalEditDraftWrap::Notes>("notes", napi_default_method),
          InstanceMethod<&VocalEditDraftWrap::Token>("token", napi_default_method),
          InstanceMethod<&VocalEditDraftWrap::Apply>("apply", napi_default_method),
          InstanceMethod<&VocalEditDraftWrap::Commit>("commit", napi_default_method),
          InstanceMethod<&VocalEditDraftWrap::Cancel>("cancel", napi_default_method),
          InstanceMethod<&VocalEditDraftWrap::EvaluatePitch>("evaluatePitch", napi_default_method),
          InstanceMethod<&VocalEditDraftWrap::SourceSampleToDestinationSample>(
              "sourceSampleToDestinationSample", napi_default_method),
          InstanceMethod<&VocalEditDraftWrap::DestinationSampleToSourceSample>(
              "destinationSampleToSourceSample", napi_default_method),
          InstanceMethod<&VocalEditDraftWrap::CaptureRenderSnapshot>("captureRenderSnapshot",
                                                                     napi_default_method),
          InstanceMethod<&VocalEditDraftWrap::Destroy>("destroy", napi_default_method),
      });
  constructor_ = Napi::Persistent(function);
  constructor_.SuppressDestruct();
  exports.Set("VocalEditDraft", function);
  return exports;
}

Napi::Object VocalEditDraftWrap::NewInstance(Napi::Env env, SonareVocalDraft* draft,
                                             VocalEditSessionWrap* owner,
                                             const Napi::Object& owner_object) {
  (void)env;
  Napi::Object object = constructor_.New({});
  auto* wrapper = VocalEditDraftWrap::Unwrap(object);
  wrapper->draft_ = draft;
  wrapper->owner_ = owner;
  wrapper->owner_object_ = Napi::Persistent(owner_object);
  owner->RetainDraft(wrapper);
  return object;
}

VocalEditDraftWrap::VocalEditDraftWrap(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<VocalEditDraftWrap>(info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_CATCH_VOID(env)
}

VocalEditDraftWrap::~VocalEditDraftWrap() {
  if (draft_ != nullptr) {
    sonare_vocal_draft_destroy(draft_);
    draft_ = nullptr;
  }
  if (owner_ != nullptr && !released_owner_) owner_->ReleaseDraft(this);
  owner_ = nullptr;
  owner_object_.Reset();
}

void VocalEditDraftWrap::DetachOwner() noexcept {
  released_owner_ = true;
  owner_ = nullptr;
}

void VocalEditDraftWrap::DisposeForOwner() noexcept {
  if (draft_ != nullptr) {
    sonare_vocal_draft_destroy(draft_);
    draft_ = nullptr;
  }
  released_owner_ = true;
  owner_ = nullptr;
  owner_object_.Reset();
}

Napi::Value VocalEditDraftWrap::Notes(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (draft_ == nullptr) return RequireSessionValue(env, nullptr);
  NotesGuard notes;
  const SonareError error = sonare_vocal_draft_notes(draft_, &notes.value);
  if (!CheckVocalError(env, error)) return env.Undefined();
  return NotesResultToObject(env, notes.value);
  SONARE_NODE_CATCH(env)
}

Napi::Value VocalEditDraftWrap::Token(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (draft_ == nullptr) return RequireSessionValue(env, nullptr);
  SonareVocalStateToken token{};
  const SonareError error = sonare_vocal_draft_token(draft_, &token);
  if (!CheckVocalError(env, error)) return env.Undefined();
  return TokenToObject(env, token);
  SONARE_NODE_CATCH(env)
}

Napi::Value VocalEditDraftWrap::Apply(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (draft_ == nullptr) return RequireSessionValue(env, nullptr);
  uint64_t generation = 0;
  if (!RequiredUint64Value(env, info[0], "expectedGeneration", &generation)) return env.Undefined();
  if (info.Length() < 2) {
    Napi::TypeError::New(env, "operations must be an array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  std::vector<ParsedOperation> parsed;
  if (!ReadOperations(env, info[1], &parsed)) return env.Undefined();
  std::vector<SonareVocalOperation> operations;
  operations.reserve(parsed.size());
  for (const ParsedOperation& item : parsed) operations.push_back(item.operation);
  EditGuard result;
  const SonareError error =
      sonare_vocal_draft_apply(draft_, generation, operations.empty() ? nullptr : operations.data(),
                               operations.size(), &result.value);
  if (!CheckVocalError(env, error)) return env.Undefined();
  return EditResultToObject(env, result.value);
  SONARE_NODE_CATCH(env)
}

Napi::Value VocalEditDraftWrap::Commit(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (draft_ == nullptr) return RequireSessionValue(env, nullptr);
  uint64_t revision = 0;
  if (info.Length() != 0 && !info[0].IsUndefined() && !info[0].IsNull() &&
      !RequiredUint64Value(env, info[0], "expectedRevision", &revision)) {
    return env.Undefined();
  }
  if ((info.Length() == 0 || info[0].IsUndefined() || info[0].IsNull()) && owner_ != nullptr &&
      owner_->native() != nullptr &&
      !CheckVocalError(env, sonare_vocal_session_revision(owner_->native(), &revision))) {
    return env.Undefined();
  }
  EditGuard result;
  const SonareError error = sonare_vocal_draft_commit(draft_, revision, &result.value);
  if (!CheckVocalError(env, error)) return env.Undefined();
  sonare_vocal_draft_destroy(draft_);
  draft_ = nullptr;
  if (owner_ != nullptr && !released_owner_) {
    owner_->ReleaseDraft(this);
    released_owner_ = true;
  }
  owner_object_.Reset();
  return EditResultToObject(env, result.value);
  SONARE_NODE_CATCH(env)
}

Napi::Value VocalEditDraftWrap::Cancel(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (draft_ == nullptr) return env.Undefined();
  const SonareError error = sonare_vocal_draft_cancel(draft_);
  if (!CheckVocalError(env, error)) return env.Undefined();
  sonare_vocal_draft_destroy(draft_);
  draft_ = nullptr;
  if (owner_ != nullptr && !released_owner_) {
    owner_->ReleaseDraft(this);
    released_owner_ = true;
  }
  owner_object_.Reset();
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value VocalEditDraftWrap::EvaluatePitch(const Napi::CallbackInfo& info) {
  return EvaluateNotePitch(info, draft_, &sonare_vocal_draft_evaluate_pitch);
}

Napi::Value VocalEditDraftWrap::SourceSampleToDestinationSample(const Napi::CallbackInfo& info) {
  return MapNoteSample(info, draft_, "sourceSample", &sonare_vocal_draft_source_to_destination);
}

Napi::Value VocalEditDraftWrap::DestinationSampleToSourceSample(const Napi::CallbackInfo& info) {
  return MapNoteSample(info, draft_, "destinationSample",
                       &sonare_vocal_draft_destination_to_source);
}

Napi::Value VocalEditDraftWrap::CaptureRenderSnapshot(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (draft_ == nullptr) return RequireSessionValue(env, nullptr);
  SonareVocalSnapshot* snapshot = nullptr;
  const SonareError error = sonare_vocal_draft_capture_snapshot(draft_, &snapshot);
  if (!CheckVocalError(env, error)) return env.Undefined();
  return VocalRenderSnapshotWrap::NewInstance(env, snapshot);
  SONARE_NODE_CATCH(env)
}

void VocalEditDraftWrap::Destroy(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (draft_ != nullptr) {
    sonare_vocal_draft_destroy(draft_);
    draft_ = nullptr;
  }
  if (owner_ != nullptr && !released_owner_) {
    owner_->ReleaseDraft(this);
    released_owner_ = true;
  }
  owner_object_.Reset();
  SONARE_NODE_CATCH_VOID(env)
}

Napi::Object VocalRenderSnapshotWrap::Init(Napi::Env env, Napi::Object exports) {
  const Napi::Function function = DefineClass(
      env, "VocalRenderSnapshot",
      {
          InstanceMethod<&VocalRenderSnapshotWrap::Render>("render", napi_default_method),
          InstanceMethod<&VocalRenderSnapshotWrap::RenderAsync>("renderAsync", napi_default_method),
          InstanceMethod<&VocalRenderSnapshotWrap::BeginRenderJob>("beginRenderJob",
                                                                   napi_default_method),
          InstanceMethod<&VocalRenderSnapshotWrap::OutputLengthSamples>("outputLengthSamples",
                                                                        napi_default_method),
          InstanceMethod<&VocalRenderSnapshotWrap::Destroy>("destroy", napi_default_method),
      });
  constructor_ = Napi::Persistent(function);
  constructor_.SuppressDestruct();
  exports.Set("VocalRenderSnapshot", function);
  return exports;
}

Napi::Object VocalRenderSnapshotWrap::NewInstance(Napi::Env env, SonareVocalSnapshot* snapshot) {
  (void)env;
  Napi::Object object = constructor_.New({});
  VocalRenderSnapshotWrap::Unwrap(object)->snapshot_ = snapshot;
  return object;
}

VocalRenderSnapshotWrap::VocalRenderSnapshotWrap(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<VocalRenderSnapshotWrap>(info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_CATCH_VOID(env)
}

VocalRenderSnapshotWrap::~VocalRenderSnapshotWrap() {
  if (snapshot_ != nullptr) sonare_vocal_snapshot_destroy(snapshot_);
  snapshot_ = nullptr;
}

Napi::Value VocalRenderSnapshotWrap::OutputLengthSamples(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (snapshot_ == nullptr) return RequireSessionValue(env, nullptr);
  int64_t samples = 0;
  const SonareError error = sonare_vocal_snapshot_output_length(snapshot_, &samples);
  if (!CheckVocalError(env, error)) return env.Undefined();
  if (samples < 0 || static_cast<double>(samples) > kMaxSafeInteger) {
    Napi::RangeError::New(env, "output length cannot be represented as a JavaScript number")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return Napi::Number::New(env, static_cast<double>(samples));
  SONARE_NODE_CATCH(env)
}

Napi::Value VocalRenderSnapshotWrap::Render(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (snapshot_ == nullptr) return RequireSessionValue(env, nullptr);
  SonareVocalRange range{};
  uint64_t request_id = 0;
  if (!ReadRenderRange(env, info, &range, &request_id)) {
    return env.Undefined();
  }
  RenderGuard result;
  const SonareError error =
      sonare_vocal_snapshot_render(snapshot_, range, request_id, nullptr, nullptr, &result.value);
  if (!CheckVocalError(env, error)) return env.Undefined();
  return RenderResultToObject(env, result.value);
  SONARE_NODE_CATCH(env)
}

Napi::Value VocalRenderSnapshotWrap::RenderAsync(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (snapshot_ == nullptr) return RejectedVocalPromise(env, SONARE_ERROR_INVALID_STATE);
  SonareVocalRange range{};
  uint64_t request_id = 0;
  if (!ReadRenderRange(env, info, &range, &request_id)) {
    return sonare_node::RejectedFromPending(env);
  }
  SonareVocalRenderJob* job = nullptr;
  const SonareError begin_error = sonare_vocal_render_job_begin(snapshot_, range, request_id, &job);
  if (begin_error != SONARE_OK) return RejectedVocalPromise(env, begin_error);
  std::unique_ptr<SonareVocalRenderJob, VocalRenderJobDeleter> job_owner(job);

  const auto cancelled = std::make_shared<std::atomic_bool>(false);
  Napi::Object signal;
  Napi::Function listener;
  if (info.Length() > 2 && !info[2].IsUndefined() && !info[2].IsNull() &&
      !PrepareAbortSignal(env, info[2], cancelled, &signal, &listener)) {
    return sonare_node::RejectedFromPending(env);
  }
  auto worker = std::make_unique<VocalRenderAsyncWorker>(
      env, job_owner.get(), info.This().As<Napi::Object>(), cancelled, signal, listener);
  job_owner.release();
  Napi::Promise promise = worker->Promise();
  worker->Queue();
  if (env.IsExceptionPending()) return sonare_node::RejectedFromPending(env);
  worker.release();
  return promise;
  SONARE_NODE_CATCH_REJECT(env)
}

Napi::Value VocalRenderSnapshotWrap::BeginRenderJob(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (snapshot_ == nullptr) return RequireSessionValue(env, nullptr);
  SonareVocalRange range{};
  uint64_t request_id = 0;
  if (!ReadRenderRange(env, info, &range, &request_id)) {
    return env.Undefined();
  }
  SonareVocalRenderJob* job = nullptr;
  const SonareError error = sonare_vocal_render_job_begin(snapshot_, range, request_id, &job);
  if (!CheckVocalError(env, error)) return env.Undefined();
  return VocalRenderJobWrap::NewInstance(env, job, this, info.This().As<Napi::Object>());
  SONARE_NODE_CATCH(env)
}

void VocalRenderSnapshotWrap::Destroy(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (snapshot_ != nullptr) {
    sonare_vocal_snapshot_destroy(snapshot_);
    snapshot_ = nullptr;
  }
  destroyed_ = true;
  SONARE_NODE_CATCH_VOID(env)
}

Napi::Object VocalRenderJobWrap::Init(Napi::Env env, Napi::Object exports) {
  const Napi::Function function =
      DefineClass(env, "VocalRenderJob",
                  {
                      InstanceMethod<&VocalRenderJobWrap::Next>("next", napi_default_method),
                      InstanceMethod<&VocalRenderJobWrap::Finish>("finalize", napi_default_method),
                      InstanceMethod<&VocalRenderJobWrap::Abort>("abort", napi_default_method),
                      InstanceMethod<&VocalRenderJobWrap::Destroy>("destroy", napi_default_method),
                  });
  constructor_ = Napi::Persistent(function);
  constructor_.SuppressDestruct();
  exports.Set("VocalRenderJob", function);
  return exports;
}

Napi::Object VocalRenderJobWrap::NewInstance(Napi::Env env, SonareVocalRenderJob* job,
                                             VocalRenderSnapshotWrap* owner,
                                             const Napi::Object& owner_object) {
  (void)env;
  Napi::Object object = constructor_.New({});
  auto* wrapper = VocalRenderJobWrap::Unwrap(object);
  wrapper->job_ = job;
  wrapper->owner_ = owner;
  wrapper->owner_object_ = Napi::Persistent(owner_object);
  return object;
}

VocalRenderJobWrap::VocalRenderJobWrap(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<VocalRenderJobWrap>(info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_CATCH_VOID(env)
}

VocalRenderJobWrap::~VocalRenderJobWrap() {
  if (job_ != nullptr) sonare_vocal_render_job_destroy(job_);
  job_ = nullptr;
  owner_ = nullptr;
  owner_object_.Reset();
}

Napi::Value VocalRenderJobWrap::Next(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (job_ == nullptr) return RequireSessionValue(env, nullptr);
  int complete = 0;
  const SonareError error = sonare_vocal_render_job_next(job_, nullptr, nullptr, &complete);
  if (!CheckVocalError(env, error)) return env.Undefined();
  Napi::Object result = Napi::Object::New(env);
  result.Set("complete", Napi::Boolean::New(env, complete != 0));
  return result;
  SONARE_NODE_CATCH(env)
}

Napi::Value VocalRenderJobWrap::Finish(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (job_ == nullptr) return RequireSessionValue(env, nullptr);
  RenderGuard result;
  const SonareError error = sonare_vocal_render_job_finalize(job_, nullptr, nullptr, &result.value);
  if (!CheckVocalError(env, error)) return env.Undefined();
  return RenderResultToObject(env, result.value);
  SONARE_NODE_CATCH(env)
}

void VocalRenderJobWrap::Abort(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (job_ != nullptr) sonare_vocal_render_job_abort(job_);
  SONARE_NODE_CATCH_VOID(env)
}

void VocalRenderJobWrap::Destroy(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (job_ != nullptr) {
    sonare_vocal_render_job_destroy(job_);
    job_ = nullptr;
  }
  owner_ = nullptr;
  owner_object_.Reset();
  SONARE_NODE_CATCH_VOID(env)
}

namespace {

Napi::Value CreateVocalEditSession(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() == 0 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "options must be an object").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const Napi::Object request = info[0].As<Napi::Object>();
  Napi::Float32Array samples;
  if (!RequiredFloat32ArrayValue(env, request.Get("samples"), "samples", &samples))
    return env.Undefined();
  int sample_rate = 0;
  if (!ReadSampleRate(env, request.Get("sampleRate"), &sample_rate)) return env.Undefined();
  SonareVocalCreateOptions options{};
  SonareVocalAnalysis analysis{};
  std::vector<float> f0_storage;
  std::vector<uint8_t> voiced_storage;
  std::string algorithm_storage;
  if (!ReadCreateOptions(env, request, &options, &analysis, &f0_storage, &voiced_storage,
                         &algorithm_storage)) {
    return env.Undefined();
  }
  SonareVocalSession* session = nullptr;
  const SonareError error =
      sonare_vocal_session_create(samples.Data(), static_cast<int64_t>(samples.ElementLength()), 1,
                                  sample_rate, &options, &session);
  if (!CheckVocalError(env, error)) return env.Undefined();
  return VocalEditSessionWrap::NewInstance(env, session);
  SONARE_NODE_CATCH(env)
}

Napi::Value RestoreVocalEditSession(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() == 0 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "options must be an object").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const Napi::Object request = info[0].As<Napi::Object>();
  Napi::Float32Array samples;
  Napi::Uint8Array state;
  if (!RequiredFloat32ArrayValue(env, request.Get("samples"), "samples", &samples) ||
      !RequiredUint8ArrayValue(env, request.Get("state"), "state", &state))
    return env.Undefined();
  int sample_rate = 0;
  if (!ReadSampleRate(env, request.Get("sampleRate"), &sample_rate)) return env.Undefined();
  SonareVocalRestoreOptions options{};
  sonare_vocal_restore_options_init(&options);
  if (!ReadSessionLimits(env, request, &options.max_history_bytes, &options.max_cache_bytes,
                         &options.max_undo_depth, &options.max_render_jobs)) {
    return env.Undefined();
  }
  SonareVocalSession* session = nullptr;
  const SonareError error = sonare_vocal_session_restore(
      samples.Data(), static_cast<int64_t>(samples.ElementLength()), 1, sample_rate, state.Data(),
      state.ElementLength(), &options, &session);
  if (!CheckVocalError(env, error)) return env.Undefined();
  return VocalEditSessionWrap::NewInstance(env, session);
  SONARE_NODE_CATCH(env)
}

Napi::Value VocalEditAbiVersion(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  return Napi::Number::New(env, sonare_vocal_edit_abi_version());
  SONARE_NODE_CATCH(env)
}

}  // namespace

Napi::Object InitVocalEdit(Napi::Env env, Napi::Object exports) {
  VocalEditSessionWrap::Init(env, exports);
  VocalEditDraftWrap::Init(env, exports);
  VocalRenderSnapshotWrap::Init(env, exports);
  VocalRenderJobWrap::Init(env, exports);
  exports.Set("vocalEditAbiVersion",
              Napi::Function::New(env, VocalEditAbiVersion, "vocalEditAbiVersion"));
  exports.Set("createVocalEditSession",
              Napi::Function::New(env, CreateVocalEditSession, "createVocalEditSession"));
  exports.Set("restoreVocalEditSession",
              Napi::Function::New(env, RestoreVocalEditSession, "restoreVocalEditSession"));
  return exports;
}

}  // namespace sonare_node
