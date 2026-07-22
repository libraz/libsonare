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

  /// Sets the maximum retained undo depth. A value of 0 is treated as 1 (at
  /// least the most recent edit is always kept). If the current undo stack is
  /// deeper than the new bound, the oldest entries are evicted immediately so
  /// resident memory drops right away rather than only on the next edit.
  void set_max_undo_depth(size_t depth);

  /// Clears both stacks (does not touch the project state).
  void clear_history();

 private:
  // A do/undo pair: `forward` re-applies on redo, `inverse` applies on undo.
  struct Entry {
    EditCommandPtr forward;
    EditCommandPtr inverse;
  };

  // Upper bound on retained undo entries. Each entry deep-copies the affected
  // clip events / SysEx payloads, so an unbounded stack grows resident memory
  // without limit across a long editing session. When the bound is exceeded the
  // OLDEST entries are evicted (a ring over a deque) so a generous, practical
  // undo window is preserved while memory stays bounded. The redo stack is
  // bounded implicitly: it never holds more entries than have been undone.
  static constexpr std::size_t kDefaultMaxUndoDepth = 512;

  // Pushes an entry onto the undo stack, evicting the oldest entries so the
  // stack never exceeds max_undo_depth_.
  void push_undo(Entry entry);

  Project project_;
  MidiContentStore midi_content_;
  std::deque<Entry> undo_stack_;
  std::deque<Entry> redo_stack_;
  std::size_t max_undo_depth_ = kDefaultMaxUndoDepth;
};

}  // namespace sonare::arrangement
