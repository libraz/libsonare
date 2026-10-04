#pragma once

/// @file session.h
/// @brief Control-thread ownership, drafts and immutable vocal snapshots.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "editing/vocal_edit/analysis.h"
#include "editing/vocal_edit/pitch_plan.h"
#include "editing/vocal_edit/render_snapshot.h"

namespace sonare::editing::vocal_edit {

struct VocalSessionCreateOptions {
  std::optional<VocalAnalysisData> analysis;
  VocalAnalysisOptions analysis_options{};
  int64_t output_length_samples = 0;
  RenderSettings render_settings{};
  VocalSessionLimits limits{};
};

class VocalEditDraft;

/// Call-scoped result preparation, used by facades before publication.
/// Callbacks may throw, are never retained, and must not reenter the session.
using StateResultPreparation = std::function<void(const StateChangeResult&)>;
using DraftResultPreparation = std::function<void(const DraftApplyResult&)>;

/// @brief Mutable, single-control-thread vocal edit session.
class VocalEditSession {
 public:
  VocalEditSession() = default;
  ~VocalEditSession();
  VocalEditSession(VocalEditSession&&) noexcept;
  VocalEditSession& operator=(VocalEditSession&&) noexcept;
  VocalEditSession(const VocalEditSession&) = delete;
  VocalEditSession& operator=(const VocalEditSession&) = delete;

  static VocalEditSession create(Audio source, const VocalSessionCreateOptions& options = {});
  static VocalEditSession restore(Audio source, const std::vector<uint8_t>& sve1,
                                  const VocalSessionLimits& runtime_limits = {});

  bool valid() const noexcept { return static_cast<bool>(impl_); }
  std::vector<VocalNote> notes() const;
  VocalAnalysisData analysis() const;
  SourceDescriptor source_descriptor() const;
  VocalCapabilities capabilities() const noexcept;
  std::vector<PitchTransition> transitions() const;
  VocalStateToken token() const;
  int64_t output_length_samples() const;
  const RenderSettings& render_settings() const;

  std::unique_ptr<VocalEditDraft> begin_edit(VocalRevision expected_revision);
  std::shared_ptr<const VocalRenderSnapshot> capture_render_snapshot() const;
  std::vector<CompiledPitchPlan> evaluate_pitch(
      const std::vector<VocalNoteId>& note_ids = {}) const;
  double source_sample_to_destination_sample(VocalNoteId note_id, double source_sample) const;
  double destination_sample_to_source_sample(VocalNoteId note_id, double destination_sample) const;

  StateChangeResult undo(VocalRevision expected_revision,
                         const StateResultPreparation& before_publish = {});
  StateChangeResult redo(VocalRevision expected_revision,
                         const StateResultPreparation& before_publish = {});
  bool can_undo() const noexcept;
  bool can_redo() const noexcept;

  /// Encodes committed edits; an active draft must be committed or cancelled.
  std::vector<uint8_t> export_state() const;

 private:
  struct Impl;
  explicit VocalEditSession(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

  std::shared_ptr<Impl> impl_;
  friend class VocalEditDraft;
};

/// @brief Transactional draft for one drag or grouped edit operation.
class VocalEditDraft {
 public:
  ~VocalEditDraft();
  VocalEditDraft(VocalEditDraft&&) noexcept;
  VocalEditDraft& operator=(VocalEditDraft&&) noexcept;
  VocalEditDraft(const VocalEditDraft&) = delete;
  VocalEditDraft& operator=(const VocalEditDraft&) = delete;

  std::vector<VocalNote> notes() const;
  VocalAnalysisData analysis() const;
  std::vector<PitchTransition> transitions() const;
  VocalStateToken token() const;
  DraftApplyResult apply(VocalGeneration expected_generation,
                         const std::vector<Operation>& operations,
                         const DraftResultPreparation& before_publish = {});
  std::vector<CompiledPitchPlan> evaluate_pitch(
      const std::vector<VocalNoteId>& note_ids = {}) const;
  std::shared_ptr<const VocalRenderSnapshot> capture_render_snapshot() const;
  StateChangeResult commit(VocalRevision expected_revision,
                           const StateResultPreparation& before_publish = {});
  void cancel() noexcept;
  bool active() const noexcept { return active_; }
  double source_sample_to_destination_sample(VocalNoteId note_id, double source_sample) const;
  double destination_sample_to_source_sample(VocalNoteId note_id, double destination_sample) const;

 private:
  explicit VocalEditDraft(std::shared_ptr<VocalEditSession::Impl> impl);
  std::shared_ptr<VocalEditSession::Impl> impl_;
  std::shared_ptr<VocalEditState> candidate_;
  VocalStateToken draft_token_{};
  uint64_t local_next_note_id_ = 1;
  bool active_ = false;

  friend class VocalEditSession;
};

}  // namespace sonare::editing::vocal_edit
