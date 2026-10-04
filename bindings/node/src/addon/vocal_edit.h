#ifndef SONARE_NODE_VOCAL_EDIT_H_
#define SONARE_NODE_VOCAL_EDIT_H_

#include <napi.h>
#include <sonare/sonare_c_vocal_edit.h>

#include <cstddef>
#include <cstdint>

namespace sonare_node {

class VocalEditSessionWrap;
class VocalEditDraftWrap;
class VocalRenderSnapshotWrap;

/// Registers the native vocal-edit handles and factory functions on the addon.
/// The registration is intentionally separate from addon.cpp so the optional
/// feature has one ownership point and can be omitted from analysis-only builds.
Napi::Object InitVocalEdit(Napi::Env env, Napi::Object exports);

class VocalEditSessionWrap final : public Napi::ObjectWrap<VocalEditSessionWrap> {
 public:
  static Napi::Object Init(Napi::Env env, Napi::Object exports);
  static Napi::Object NewInstance(Napi::Env env, SonareVocalEditSession* session);

  explicit VocalEditSessionWrap(const Napi::CallbackInfo& info);
  ~VocalEditSessionWrap() override;

  SonareVocalEditSession* native() const noexcept { return session_; }
  void RetainDraft() noexcept { ++draft_count_; }
  void ReleaseDraft() noexcept {
    if (draft_count_ != 0) --draft_count_;
  }

 private:
  Napi::Value Notes(const Napi::CallbackInfo& info);
  Napi::Value Analysis(const Napi::CallbackInfo& info);
  Napi::Value Capabilities(const Napi::CallbackInfo& info);
  Napi::Value Token(const Napi::CallbackInfo& info);
  Napi::Value Revision(const Napi::CallbackInfo& info);
  Napi::Value OutputLengthSamples(const Napi::CallbackInfo& info);
  Napi::Value History(const Napi::CallbackInfo& info);
  Napi::Value BeginEdit(const Napi::CallbackInfo& info);
  Napi::Value Undo(const Napi::CallbackInfo& info);
  Napi::Value Redo(const Napi::CallbackInfo& info);
  Napi::Value EvaluatePitch(const Napi::CallbackInfo& info);
  Napi::Value SourceSampleToDestinationSample(const Napi::CallbackInfo& info);
  Napi::Value DestinationSampleToSourceSample(const Napi::CallbackInfo& info);
  Napi::Value CaptureRenderSnapshot(const Napi::CallbackInfo& info);
  Napi::Value ExportState(const Napi::CallbackInfo& info);
  void Destroy(const Napi::CallbackInfo& info);

  SonareVocalEditSession* session_ = nullptr;
  size_t draft_count_ = 0;
  bool destroyed_ = false;
  static Napi::FunctionReference constructor_;
};

class VocalEditDraftWrap final : public Napi::ObjectWrap<VocalEditDraftWrap> {
 public:
  static Napi::Object Init(Napi::Env env, Napi::Object exports);
  static Napi::Object NewInstance(Napi::Env env, SonareVocalEditDraft* draft,
                                  VocalEditSessionWrap* owner, const Napi::Object& owner_object);

  explicit VocalEditDraftWrap(const Napi::CallbackInfo& info);
  ~VocalEditDraftWrap() override;

  SonareVocalEditDraft* native() const noexcept { return draft_; }

 private:
  Napi::Value Notes(const Napi::CallbackInfo& info);
  Napi::Value Token(const Napi::CallbackInfo& info);
  Napi::Value Apply(const Napi::CallbackInfo& info);
  Napi::Value Commit(const Napi::CallbackInfo& info);
  Napi::Value Cancel(const Napi::CallbackInfo& info);
  Napi::Value EvaluatePitch(const Napi::CallbackInfo& info);
  Napi::Value SourceSampleToDestinationSample(const Napi::CallbackInfo& info);
  Napi::Value DestinationSampleToSourceSample(const Napi::CallbackInfo& info);
  Napi::Value CaptureRenderSnapshot(const Napi::CallbackInfo& info);
  void Destroy(const Napi::CallbackInfo& info);

  SonareVocalEditDraft* draft_ = nullptr;
  VocalEditSessionWrap* owner_ = nullptr;
  Napi::ObjectReference owner_object_;
  bool released_owner_ = false;
  static Napi::FunctionReference constructor_;
};

class VocalRenderSnapshotWrap final : public Napi::ObjectWrap<VocalRenderSnapshotWrap> {
 public:
  static Napi::Object Init(Napi::Env env, Napi::Object exports);
  static Napi::Object NewInstance(Napi::Env env, SonareVocalRenderSnapshot* snapshot);

  explicit VocalRenderSnapshotWrap(const Napi::CallbackInfo& info);
  ~VocalRenderSnapshotWrap() override;

  SonareVocalRenderSnapshot* native() const noexcept { return snapshot_; }

 private:
  Napi::Value Render(const Napi::CallbackInfo& info);
  Napi::Value RenderAsync(const Napi::CallbackInfo& info);
  Napi::Value BeginRenderJob(const Napi::CallbackInfo& info);
  Napi::Value OutputLengthSamples(const Napi::CallbackInfo& info);
  void Destroy(const Napi::CallbackInfo& info);

  SonareVocalRenderSnapshot* snapshot_ = nullptr;
  bool destroyed_ = false;
  static Napi::FunctionReference constructor_;
};

class VocalRenderJobWrap final : public Napi::ObjectWrap<VocalRenderJobWrap> {
 public:
  static Napi::Object Init(Napi::Env env, Napi::Object exports);
  static Napi::Object NewInstance(Napi::Env env, SonareVocalRenderJob* job,
                                  VocalRenderSnapshotWrap* owner, const Napi::Object& owner_object);

  explicit VocalRenderJobWrap(const Napi::CallbackInfo& info);
  ~VocalRenderJobWrap() override;

  SonareVocalRenderJob* native() const noexcept { return job_; }

 private:
  Napi::Value Next(const Napi::CallbackInfo& info);
  Napi::Value Finish(const Napi::CallbackInfo& info);
  void Abort(const Napi::CallbackInfo& info);
  void Destroy(const Napi::CallbackInfo& info);

  SonareVocalRenderJob* job_ = nullptr;
  VocalRenderSnapshotWrap* owner_ = nullptr;
  Napi::ObjectReference owner_object_;
  static Napi::FunctionReference constructor_;
};

}  // namespace sonare_node

#endif  // SONARE_NODE_VOCAL_EDIT_H_
