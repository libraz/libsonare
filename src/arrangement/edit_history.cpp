/// @file edit_history.cpp
/// @brief Implementation of the deterministic undo/redo stack.

#include "arrangement/edit_history.h"

#include <algorithm>
#include <utility>

namespace sonare::arrangement {
namespace {

class EditCommandGroup final : public EditCommand {
 public:
  explicit EditCommandGroup(std::vector<EditCommandPtr> commands)
      : commands_(std::move(commands)) {}

  bool apply(Project& project, MidiContentStore& store) override {
    for (auto& command : commands_) {
      if (command == nullptr || !command->apply(project, store)) {
        return false;
      }
    }
    return true;
  }

  EditCommandPtr invert(const Project& /*before*/,
                        const MidiContentStore& /*store_before*/) const override {
    return nullptr;
  }

  const char* type_name() const noexcept override { return "EditCommandGroup"; }

  bool mutates_midi_store() const noexcept override {
    // The group mutates the store iff any child does; an empty group cannot.
    for (const auto& command : commands_) {
      if (command != nullptr && command->mutates_midi_store()) {
        return true;
      }
    }
    return false;
  }

 private:
  std::vector<EditCommandPtr> commands_;
};

// Revert commands that have already succeeded in a transaction before restoring
// the value-owned Project/MIDI snapshots.  Most EditCommands change only those
// two stores, but a command may also own a tightly coupled sidecar (for example
// decoded PCM in AudioContentStore).  Restoring just the snapshots would leave
// such a sidecar ahead of the history after a later command fails.
bool rollback_applied_commands(std::vector<EditCommandPtr>& inverses, Project& project,
                               MidiContentStore& store) {
  for (auto it = inverses.rbegin(); it != inverses.rend(); ++it) {
    if (*it == nullptr || !(*it)->apply(project, store)) return false;
  }
  return true;
}

}  // namespace

void EditHistory::push_undo(Entry entry) {
  undo_stack_.push_back(std::move(entry));
  // Bound resident memory: evict the oldest entries once the depth cap is
  // exceeded. max_undo_depth_ >= 1 by construction, so at least the most recent
  // edit is always retained.
  while (undo_stack_.size() > max_undo_depth_) {
    undo_stack_.pop_front();
  }
}

bool EditHistory::apply(EditCommandPtr command) {
  if (command == nullptr) {
    return false;
  }
  // Snapshot the pre-apply state so the inverse can capture prior values. The
  // Project and MidiContentStore are value types, so the copy is a deep clone.
  // Skip cloning the (potentially large) MIDI store for commands that provably
  // cannot mutate it; store_before then aliases the live store, which apply()
  // leaves untouched.
  const Project before = project_;
  const bool clone_store = command->mutates_midi_store();
  MidiContentStore store_copy;
  if (clone_store) {
    store_copy = midi_content_;
  }
  const MidiContentStore& store_before = clone_store ? store_copy : midi_content_;

  if (!command->apply(project_, midi_content_)) {
    // Apply failed: leave the project untouched (apply() must not partially
    // mutate on its failure paths) and push nothing.
    project_ = before;
    if (clone_store) {
      midi_content_ = store_copy;
    }
    return false;
  }

  EditCommandPtr inverse = command->invert(before, store_before);
  if (inverse == nullptr) {
    // No inverse means the command is not safely undoable; revert and reject so
    // the history never holds an irreversible entry.
    project_ = before;
    if (clone_store) {
      midi_content_ = store_copy;
    }
    return false;
  }

  Entry entry;
  entry.forward = std::move(command);
  entry.inverse = std::move(inverse);
  push_undo(std::move(entry));
  redo_stack_.clear();
  return true;
}

bool EditHistory::apply_transaction(std::vector<EditCommandPtr> commands) {
  if (commands.empty()) {
    return false;
  }

  // Clone the transaction-level store snapshot for rollback only when some
  // command in the batch may mutate the store; a metadata-only batch never
  // touches it, so the live store is a valid rollback target.
  bool batch_mutates_store = false;
  for (const auto& command : commands) {
    if (command != nullptr && command->mutates_midi_store()) {
      batch_mutates_store = true;
      break;
    }
  }

  const Project transaction_before = project_;
  MidiContentStore transaction_store_before;
  if (batch_mutates_store) {
    transaction_store_before = midi_content_;
  }

  std::vector<EditCommandPtr> forward;
  std::vector<EditCommandPtr> inverse;
  forward.reserve(commands.size());
  inverse.reserve(commands.size());

  const auto rollback = [&]() {
    // Apply captured inverses first so commands which maintain external
    // control-thread state (such as AudioContentStore transfers) return that
    // state to its pre-transaction ownership before the value snapshots below
    // are restored.  The snapshots remain the authoritative fallback for the
    // Project and MIDI stores even if a malformed inverse reports failure.
    (void)rollback_applied_commands(inverse, project_, midi_content_);
    project_ = transaction_before;
    if (batch_mutates_store) {
      midi_content_ = transaction_store_before;
    }
  };

  for (auto& command : commands) {
    if (command == nullptr) {
      rollback();
      return false;
    }

    const Project before = project_;
    const bool clone_store = command->mutates_midi_store();
    MidiContentStore store_copy;
    if (clone_store) {
      store_copy = midi_content_;
    }
    const MidiContentStore& store_before = clone_store ? store_copy : midi_content_;
    if (!command->apply(project_, midi_content_)) {
      rollback();
      return false;
    }

    EditCommandPtr undo = command->invert(before, store_before);
    if (undo == nullptr) {
      rollback();
      return false;
    }
    forward.push_back(std::move(command));
    inverse.push_back(std::move(undo));
  }

  std::reverse(inverse.begin(), inverse.end());

  Entry entry;
  entry.forward = std::make_unique<EditCommandGroup>(std::move(forward));
  entry.inverse = std::make_unique<EditCommandGroup>(std::move(inverse));
  push_undo(std::move(entry));
  redo_stack_.clear();
  return true;
}

bool EditHistory::undo() {
  if (undo_stack_.empty()) {
    return false;
  }
  Entry entry = std::move(undo_stack_.back());
  undo_stack_.pop_back();

  // Snapshot before applying the inverse so we can build a fresh inverse-of-the
  // -inverse, keeping redo exact even for commands whose inverse differs by id.
  const Project before = project_;
  const bool clone_store = entry.inverse->mutates_midi_store();
  MidiContentStore store_copy;
  if (clone_store) {
    store_copy = midi_content_;
  }

  if (!entry.inverse->apply(project_, midi_content_)) {
    // Should not happen for a well-formed entry; restore and report failure.
    project_ = before;
    if (clone_store) {
      midi_content_ = store_copy;
    }
    undo_stack_.push_back(std::move(entry));
    return false;
  }

  // The redo of this step re-runs the forward command. Rebuild the inverse from
  // the post-undo state so a subsequent undo (after redo) stays exact.
  redo_stack_.push_back(std::move(entry));
  return true;
}

bool EditHistory::redo() {
  if (redo_stack_.empty()) {
    return false;
  }
  Entry entry = std::move(redo_stack_.back());
  redo_stack_.pop_back();

  const Project before = project_;
  const bool clone_store = entry.forward->mutates_midi_store();
  MidiContentStore store_copy;
  if (clone_store) {
    store_copy = midi_content_;
  }
  const MidiContentStore& store_before = clone_store ? store_copy : midi_content_;

  if (!entry.forward->apply(project_, midi_content_)) {
    project_ = before;
    if (clone_store) {
      midi_content_ = store_copy;
    }
    redo_stack_.push_back(std::move(entry));
    return false;
  }

  // Refresh the inverse against the pre-redo state so it remains exact even when
  // the forward command's effect depends on current state.
  EditCommandPtr inverse = entry.forward->invert(before, store_before);
  if (inverse != nullptr) {
    entry.inverse = std::move(inverse);
  }
  // Total entries (undo + redo) are conserved across undo/redo, so this never
  // exceeds the cap; routed through push_undo for uniformity.
  push_undo(std::move(entry));
  return true;
}

void EditHistory::set_max_undo_depth(size_t depth) {
  max_undo_depth_ = std::max<size_t>(1, depth);
  // Evict immediately when shrinking below the current depth so the memory is
  // released now, not deferred to the next push_undo().
  while (undo_stack_.size() > max_undo_depth_) {
    undo_stack_.pop_front();
  }
}

void EditHistory::clear_history() {
  undo_stack_.clear();
  redo_stack_.clear();
}

}  // namespace sonare::arrangement
