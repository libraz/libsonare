#pragma once

/// @file edit_history.h
/// @brief Deterministic undo/redo stack for arrangement edit commands.
///
/// @ref sonare::arrangement::EditHistory owns the @ref Project and its
/// associated @ref MidiContentStore and is the single public mutation entry for
/// the arrangement subsystem: callers construct an @ref EditCommand and apply it
/// through the history so that undo/redo and deterministic replay stay uniform.
///
/// CONTROL-THREAD-ONLY: no internal locks, no I/O, no clock/random. All state is
/// value-oriented. Replaying the same command sequence on a fresh history yields
/// an identical Project (deterministic ids via the model's monotonic counters).

#include <cstddef>
#include <deque>
#include <vector>

#include "arrangement/edit_command.h"
#include "arrangement/edit_model.h"

namespace sonare::arrangement {

/// Optional rollback seam for commands which mutate control-thread state that
/// is not owned by EditHistory (for example decoded PCM in an AudioContentStore
/// owned by a C API wrapper). EditHistory calls this when apply() returned
/// false or threw after a sidecar may have changed, and when a later commit
/// step fails. Implementations MUST be allocation-free and noexcept so they
/// can restore that sidecar while the value-owned Project snapshot is swapped
/// back. The operation must be safe after a command has already rolled itself
/// back.
class EditCommandRollback {
 public:
  virtual ~EditCommandRollback() = default;
  virtual void rollback_apply(Project& project, MidiContentStore& store) noexcept = 0;
};

/// Undo/redo manager around a Project + MidiContentStore.
class EditHistory {
 public:
  EditHistory() = default;
  explicit EditHistory(Project project) : project_(std::move(project)) {}

  // ---- Access --------------------------------------------------------------

  const Project& project() const noexcept { return project_; }
  Project& project() noexcept { return project_; }

  const MidiContentStore& midi_content() const noexcept { return midi_content_; }
  MidiContentStore& midi_content() noexcept { return midi_content_; }

  // ---- Apply / undo / redo -------------------------------------------------

  /// Applies `command`, pushing it (with its inverse) onto the undo stack and
  /// clearing the redo stack. Returns true on success. On failure the project is
  /// left unchanged and nothing is pushed.
  ///
  /// The inverse is captured AFTER apply (so Add* commands have allocated their
  /// id) using a snapshot of the project taken BEFORE apply.
  bool apply(EditCommandPtr command);

  /// Applies a sequence as one undoable transaction. The sequence is committed
  /// only if every command applies and every inverse can be captured.
  bool apply_transaction(std::vector<EditCommandPtr> commands);

  /// Undoes the most recent applied command. Returns false when the undo stack
  /// is empty.
  bool undo();

  /// Redoes the most recently undone command. Returns false when the redo stack
  /// is empty.
  bool redo();

  bool can_undo() const noexcept { return !undo_stack_.empty(); }
  bool can_redo() const noexcept { return !redo_stack_.empty(); }
  size_t undo_depth() const noexcept { return undo_stack_.size(); }
  size_t redo_depth() const noexcept { return redo_stack_.size(); }

  /// The maximum number of undo entries retained before the oldest are evicted.
  size_t max_undo_depth() const noexcept { return max_undo_depth_; }

  /// Maximum combined retained bytes across undo and redo. A value of zero is
  /// valid and disables history retention while keeping successful edits
  /// successful.
  size_t max_history_bytes() const noexcept { return max_history_bytes_; }

  /// Dynamically recomputed conservative retained bytes across both stacks.
  size_t retained_history_bytes() const noexcept;

  /// Alias kept concise for diagnostics and tests.
  size_t history_bytes() const noexcept { return retained_history_bytes(); }

  /// Sets the maximum retained undo depth. A value of 0 is treated as 1 (at
  /// least the most recent edit is always kept). If the current undo stack is
  /// deeper than the new bound, the oldest entries are evicted immediately so
  /// resident memory drops right away rather than only on the next edit.
  void set_max_undo_depth(size_t depth);

  /// Clears both stacks (does not touch the project state).
  void clear_history();

  /// Sets the combined undo+redo retained-byte cap. Zero disables retention;
  /// all values represent bytes and are accepted, including SIZE_MAX. The
  /// current stacks are trimmed immediately.
  void set_max_history_bytes(size_t bytes);

 private:
  // A do/undo pair: `forward` re-applies on redo, `inverse` applies on undo.
  struct Entry {
    EditCommandPtr forward;
    EditCommandPtr inverse;
    // Measured live rather than cached: a command may own a sidecar whose size
    // changes after the entry was committed, and the cap has to follow it.
    size_t retained_bytes() const noexcept {
      size_t total = 0;
      if (forward != nullptr) total = retained::saturating_add(total, forward->retained_bytes());
      if (inverse != nullptr) total = retained::saturating_add(total, inverse->retained_bytes());
      return total;
    }
  };

  // Upper bound on retained undo entries. Each entry deep-copies the affected
  // clip events / SysEx payloads, so an unbounded stack grows resident memory
  // without limit across a long editing session. When the bound is exceeded the
  // OLDEST entries are evicted (a ring over a deque) so a generous, practical
  // undo window is preserved while memory stays bounded. The redo stack is
  // bounded implicitly: it never holds more entries than have been undone.
  static constexpr std::size_t kDefaultMaxUndoDepth = 512;
  static constexpr std::size_t kDefaultMaxHistoryBytes = 256u * 1024u * 1024u;

  // Reserves a destination entry before a command is applied.  The returned
  // slot is filled only after apply + invert have succeeded; no allocation is
  // permitted on the post-apply commit path.
  Entry& reserve_undo_entry();

  // Evicts oldest entries so the undo stack never exceeds max_undo_depth_.
  void trim_undo_stack() noexcept;

  // Enforces the combined byte cap. The caller supplies the replay-specific
  // newest-entry policy; all ordinary overage evicts redo-oldest first, then
  // undo-oldest. Each call measures command sizes afresh so no stale cache
  // exists; within one call nothing mutates them, so the running total is
  // carried across evictions rather than re-summed per eviction.
  void trim_history_bytes() noexcept;
  void trim_after_apply() noexcept;
  void trim_after_undo() noexcept;
  void trim_after_redo() noexcept;

  Project project_;
  MidiContentStore midi_content_;
  std::deque<Entry> undo_stack_;
  std::deque<Entry> redo_stack_;
  std::size_t max_undo_depth_ = kDefaultMaxUndoDepth;
  std::size_t max_history_bytes_ = kDefaultMaxHistoryBytes;
};

}  // namespace sonare::arrangement
