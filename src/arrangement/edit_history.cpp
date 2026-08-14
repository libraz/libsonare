/// @file edit_history.cpp
/// @brief Implementation of the deterministic undo/redo stack.

#include "arrangement/edit_history.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace sonare::arrangement {
namespace {

void rollback_command(EditCommand* command, Project& project, MidiContentStore& store) noexcept;

class EditCommandGroup final : public EditCommand, public EditCommandRollback {
 public:
  EditCommandGroup() = default;

  explicit EditCommandGroup(std::vector<EditCommandPtr> commands)
      : commands_(std::move(commands)) {}

  // The two transaction groups are allocated before the first child applies.
  // Moving a vector with the default allocator is non-allocating, so adopting
  // the captured commands cannot create a post-mutation bad_alloc seam.
  void adopt(std::vector<EditCommandPtr>&& commands) noexcept { commands_.swap(commands); }

  bool apply(Project& project, MidiContentStore& store) override {
    applied_count_ = 0;
    applying_index_ = kNoApplyingIndex;
    for (size_t index = 0; index < commands_.size(); ++index) {
      auto& command = commands_[index];
      applying_index_ = index;
      if (command == nullptr || !command->apply(project, store)) {
        // A false return is not caught by EditHistory, so the group must undo
        // any child-sidecar mutations before reporting failure itself.
        rollback_apply(project, store);
        return false;
      }
      ++applied_count_;
      applying_index_ = kNoApplyingIndex;
    }
    return true;
  }

  void rollback_apply(Project& project, MidiContentStore& store) noexcept override {
    // The child currently being applied is not part of applied_count_ yet. It
    // can nevertheless have mutated a sidecar before throwing, so roll it back
    // first, followed by completed children in reverse order.
    if (applying_index_ != kNoApplyingIndex && applying_index_ < commands_.size()) {
      rollback_command(commands_[applying_index_].get(), project, store);
    }
    for (size_t count = applied_count_; count > 0; --count) {
      rollback_command(commands_[count - 1].get(), project, store);
    }
    applying_index_ = kNoApplyingIndex;
    applied_count_ = 0;
  }

  EditCommandPtr invert(const Project& /*before*/,
                        const MidiContentStore& /*store_before*/) const override {
    return nullptr;
  }

  const char* type_name() const noexcept override { return "EditCommandGroup"; }

  size_t retained_bytes() const noexcept override {
    size_t total = retained::saturating_add(
        sizeof(*this), retained::saturating_multiply(commands_.capacity(), sizeof(EditCommandPtr)));
    for (const auto& command : commands_) {
      if (command != nullptr) {
        total = retained::saturating_add(total, command->retained_bytes());
      }
    }
    return total;
  }

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
  static constexpr size_t kNoApplyingIndex = static_cast<size_t>(-1);

  std::vector<EditCommandPtr> commands_;
  // These counters avoid allocating while a group is being replayed. History
  // has already committed the group by then, so tracking rollback state must
  // be allocation-free even when a child throws after mutating a sidecar.
  size_t applied_count_ = 0;
  size_t applying_index_ = kNoApplyingIndex;
};

// Revert commands that have already succeeded in a transaction before restoring
// the value-owned Project/MIDI snapshots.  Most EditCommands change only those
// two stores, but a command may also own a tightly coupled sidecar (for example
// decoded PCM in AudioContentStore).  Restoring just the snapshots would leave
// such a sidecar ahead of the history after a later command fails.
bool rollback_applied_commands(const std::vector<EditCommandPtr>& forwards,
                               const std::vector<EditCommandPtr>& inverses, Project& project,
                               MidiContentStore& store) {
  // A successful inverse normally restores both the value stores and any
  // command-owned sidecar. If that inverse itself changes a sidecar and then
  // fails, rolling back only the inverse returns the sidecar to the *forward*
  // state. Its paired forward command must then compensate it back to the
  // pre-transaction state before the Project/MIDI snapshots are restored.
  bool restored = forwards.size() == inverses.size();
  const size_t count = std::max(forwards.size(), inverses.size());
  for (size_t count_down = count; count_down > 0; --count_down) {
    const size_t index = count_down - 1;
    EditCommand* inverse = index < inverses.size() ? inverses[index].get() : nullptr;
    EditCommand* forward = index < forwards.size() ? forwards[index].get() : nullptr;
    bool inverse_succeeded = false;
    if (inverse == nullptr) {
      restored = false;
    } else {
      try {
        inverse_succeeded = inverse->apply(project, store);
      } catch (...) {
        inverse_succeeded = false;
      }
    }
    if (inverse_succeeded) continue;

    // The hooks are idempotent. Calling the inverse hook first erases its
    // partial mutation; the paired forward hook then erases the original
    // successful forward mutation. Continue unwinding all earlier pairs even
    // when one is malformed, so unrelated sidecars cannot be stranded.
    rollback_command(inverse, project, store);
    rollback_command(forward, project, store);
    restored = false;
  }
  return restored;
}

void rollback_command(EditCommand* command, Project& project, MidiContentStore& store) noexcept {
  if (command == nullptr) return;
  auto* rollback = dynamic_cast<EditCommandRollback*>(command);
  if (rollback != nullptr) rollback->rollback_apply(project, store);
}

void swap_project_snapshot(Project& project, Project& snapshot) noexcept {
  // Project is composed solely of standard-library value containers with the
  // default allocator. Their swaps are non-allocating and noexcept; keeping
  // the snapshot mutable avoids a copy assignment (and its possible throw) on
  // an exception path.
  std::swap(project, snapshot);
}

void swap_store_snapshot(MidiContentStore& store, MidiContentStore& snapshot) noexcept {
  std::swap(store, snapshot);
}

}  // namespace

EditHistory::Entry& EditHistory::reserve_undo_entry() {
  undo_stack_.emplace_back();
  return undo_stack_.back();
}

void EditHistory::trim_undo_stack() noexcept {
  // Bound resident memory: evict the oldest entries once the depth cap is
  // exceeded. max_undo_depth_ >= 1 by construction, so at least the most recent
  // edit is always retained.
  while (undo_stack_.size() > max_undo_depth_) {
    undo_stack_.pop_front();
  }
}

size_t EditHistory::retained_history_bytes() const noexcept {
  size_t total = 0;
  for (const Entry& entry : undo_stack_) {
    total = retained::saturating_add(total, entry.retained_bytes());
  }
  for (const Entry& entry : redo_stack_) {
    total = retained::saturating_add(total, entry.retained_bytes());
  }
  return total;
}

void EditHistory::trim_history_bytes() noexcept {
  // The cap is combined. Redo is deliberately discarded first because it is
  // the less recent branch and is already invalidated by a new edit in the
  // normal apply path. Popping a deque node only destroys command ownership;
  // no allocation or throwing operation is performed here.
  if (max_history_bytes_ == 0) {
    // Zero is the explicit no-retention policy, including for a synthetic
    // command that reports a zero-byte footprint.
    undo_stack_.clear();
    redo_stack_.clear();
    return;
  }
  // Carry the running total across evictions instead of re-summing both stacks
  // on every iteration. A saturated total carries no subtractable magnitude, so
  // that (unreachable in practice, 2^64 retained bytes) case re-sums.
  size_t total = retained_history_bytes();
  while (total > max_history_bytes_) {
    const bool saturated = total == std::numeric_limits<size_t>::max();
    size_t evicted = 0;
    if (!redo_stack_.empty()) {
      evicted = redo_stack_.front().retained_bytes();
      redo_stack_.pop_front();
    } else if (!undo_stack_.empty()) {
      evicted = undo_stack_.front().retained_bytes();
      undo_stack_.pop_front();
    } else {
      break;
    }
    total = saturated ? retained_history_bytes() : total - std::min(total, evicted);
  }
}

void EditHistory::trim_after_apply() noexcept {
  if (undo_stack_.empty()) return;
  // A newly applied command is a chain unit. If its pair alone cannot fit,
  // clear the whole undo branch rather than retaining an older partial chain.
  if (undo_stack_.back().retained_bytes() > max_history_bytes_) {
    undo_stack_.clear();
    return;
  }
  trim_history_bytes();
}

void EditHistory::trim_after_undo() noexcept {
  if (!redo_stack_.empty() && redo_stack_.back().retained_bytes() > max_history_bytes_) {
    // The nearest redo entry is the just-undone command. It is indivisible and
    // cannot fit, so discard the complete redo branch.
    redo_stack_.clear();
    trim_history_bytes();
    return;
  }
  trim_history_bytes();
}

void EditHistory::trim_after_redo() noexcept {
  if (!undo_stack_.empty() && undo_stack_.back().retained_bytes() > max_history_bytes_) {
    // The just-redone command is the newest undo chain unit. Do not evict its
    // inverse piecemeal; clearing undo makes the successful edit non-undoable.
    undo_stack_.clear();
    trim_history_bytes();
    return;
  }
  trim_history_bytes();
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
  Project before = project_;
  const bool clone_store = command->mutates_midi_store();
  MidiContentStore store_copy;
  if (clone_store) {
    store_copy = midi_content_;
  }
  const MidiContentStore& store_before = clone_store ? store_copy : midi_content_;

  // Reserve the history destination before apply().  A deque growth failure
  // therefore occurs while the command and every sidecar are still untouched;
  // the post-apply commit below only moves unique_ptrs and destroys redo data.
  Entry& pending = reserve_undo_entry();
  try {
    if (!command->apply(project_, midi_content_)) {
      // Apply failed: leave the project untouched (apply() must not partially
      // mutate on its failure paths) and push nothing. A command can own an
      // external sidecar and change it before returning false.
      rollback_command(command.get(), project_, midi_content_);
      undo_stack_.pop_back();
      swap_project_snapshot(project_, before);
      if (clone_store) {
        swap_store_snapshot(midi_content_, store_copy);
      }
      return false;
    }

    EditCommandPtr inverse = command->invert(before, store_before);
    if (inverse == nullptr) {
      // No inverse means the command is not safely undoable; reject it without
      // leaving a sidecar ahead of the history.
      rollback_command(command.get(), project_, midi_content_);
      undo_stack_.pop_back();
      swap_project_snapshot(project_, before);
      if (clone_store) {
        swap_store_snapshot(midi_content_, store_copy);
      }
      return false;
    }

    pending.forward = std::move(command);
    pending.inverse = std::move(inverse);
    redo_stack_.clear();
    trim_undo_stack();
    trim_after_apply();
    return true;
  } catch (...) {
    // In particular, invert() may throw std::bad_alloc after apply() has
    // transferred a sidecar.  The optional hook restores that external state
    // without allocating; value stores are then restored with noexcept swaps.
    rollback_command(command.get(), project_, midi_content_);
    undo_stack_.pop_back();
    swap_project_snapshot(project_, before);
    if (clone_store) {
      swap_store_snapshot(midi_content_, store_copy);
    }
    throw;
  }
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

  Project transaction_before = project_;
  MidiContentStore transaction_store_before;
  if (batch_mutates_store) {
    transaction_store_before = midi_content_;
  }

  // Keep the two vectors in original apply order until commit. Rollback needs
  // the pair at each index: if an inverse itself fails after changing a
  // sidecar, its corresponding successful forward command compensates it.
  std::vector<EditCommandPtr> forward;
  std::vector<EditCommandPtr> inverse;
  forward.reserve(commands.size());
  inverse.reserve(commands.size());

  // Allocate both wrappers before a child can change either the value Project
  // or a command-owned sidecar. Constructing the second group after moving
  // `forward` would otherwise lose the paired rollback hook if it threw.
  auto forward_group = std::make_unique<EditCommandGroup>();
  auto inverse_group = std::make_unique<EditCommandGroup>();

  // As with apply(), reserve the history destination before any command can
  // mutate the Project or an external sidecar.  Filling this slot is entirely
  // noexcept once the vectors above have been prepared.
  Entry& pending = reserve_undo_entry();
  bool pending_active = true;
  EditCommand* active_command = nullptr;

  const auto restore_snapshots = [&]() noexcept {
    swap_project_snapshot(project_, transaction_before);
    if (batch_mutates_store) {
      swap_store_snapshot(midi_content_, transaction_store_before);
    }
  };

  const auto rollback = [&]() noexcept {
    // Apply captured inverses first so commands which maintain external
    // control-thread state (such as AudioContentStore transfers) return that
    // state to its pre-transaction ownership before the value snapshots below
    // are restored.  The snapshots remain the authoritative fallback for the
    // Project and MIDI stores even if a malformed inverse reports failure.
    (void)rollback_applied_commands(forward, inverse, project_, midi_content_);
    restore_snapshots();
  };

  try {
    for (auto& command : commands) {
      if (command == nullptr) {
        rollback();
        pending_active = false;
        undo_stack_.pop_back();
        return false;
      }

      Project before = project_;
      const bool clone_store = command->mutates_midi_store();
      MidiContentStore store_copy;
      if (clone_store) {
        store_copy = midi_content_;
      }
      const MidiContentStore& store_before = clone_store ? store_copy : midi_content_;
      active_command = command.get();
      if (!command->apply(project_, midi_content_)) {
        // A false result does not prove that the current command left its
        // sidecar untouched. Roll it back before prior inverses/snapshots.
        rollback_command(active_command, project_, midi_content_);
        active_command = nullptr;
        rollback();
        pending_active = false;
        undo_stack_.pop_back();
        return false;
      }

      EditCommandPtr undo;
      try {
        undo = command->invert(before, store_before);
      } catch (...) {
        // This command has no captured inverse yet, so use its direct
        // sidecar rollback seam before reverting earlier commands.
        rollback_command(command.get(), project_, midi_content_);
        active_command = nullptr;
        rollback();
        pending_active = false;
        undo_stack_.pop_back();
        throw;
      }
      if (undo == nullptr) {
        rollback_command(command.get(), project_, midi_content_);
        active_command = nullptr;
        rollback();
        pending_active = false;
        undo_stack_.pop_back();
        return false;
      }
      // reserve() above makes both moves non-throwing; no allocation occurs
      // after a successful command apply.
      active_command = nullptr;
      forward.push_back(std::move(command));
      inverse.push_back(std::move(undo));
    }

    // All potentially-throwing work is complete. The preallocated wrappers
    // adopt the vectors without allocating, preserving every forward/inverse
    // pair until rollback is no longer needed.
    std::reverse(inverse.begin(), inverse.end());
    forward_group->adopt(std::move(forward));
    inverse_group->adopt(std::move(inverse));
    pending.forward = std::move(forward_group);
    pending.inverse = std::move(inverse_group);
    redo_stack_.clear();
    trim_undo_stack();
    trim_after_apply();
    pending_active = false;
    return true;
  } catch (...) {
    // The explicit failure branches above have already rolled back and removed
    // the placeholder.  This catch handles group-allocation failures (and any
    // unexpected exception after a command was captured).
    if (pending_active) {
      rollback_command(active_command, project_, midi_content_);
      rollback();
      undo_stack_.pop_back();
    }
    throw;
  }
}

bool EditHistory::undo() {
  if (undo_stack_.empty()) {
    return false;
  }

  // Snapshot before applying the inverse so we can build a fresh inverse-of-the
  // -inverse, keeping redo exact even for commands whose inverse differs by id.
  Project before = project_;
  Entry& entry = undo_stack_.back();
  const bool clone_store = entry.inverse->mutates_midi_store();
  MidiContentStore store_copy;
  if (clone_store) {
    store_copy = midi_content_;
  }

  // Reserve the redo destination before applying the inverse.  A deque growth
  // failure cannot strand a successfully detached sidecar in the live state.
  redo_stack_.emplace_back();
  Entry& pending = redo_stack_.back();
  try {
    if (!entry.inverse->apply(project_, midi_content_)) {
      // Should not happen for a well-formed entry; restore and report failure.
      rollback_command(entry.inverse.get(), project_, midi_content_);
      redo_stack_.pop_back();
      swap_project_snapshot(project_, before);
      if (clone_store) {
        swap_store_snapshot(midi_content_, store_copy);
      }
      return false;
    }

    // The entry is transferred to redo without allocation.  Its command pair
    // already contains the exact inverse needed for the next redo.
    pending = std::move(entry);
    undo_stack_.pop_back();
    trim_after_undo();
    return true;
  } catch (...) {
    rollback_command(entry.inverse.get(), project_, midi_content_);
    redo_stack_.pop_back();
    swap_project_snapshot(project_, before);
    if (clone_store) {
      swap_store_snapshot(midi_content_, store_copy);
    }
    throw;
  }
}

bool EditHistory::redo() {
  if (redo_stack_.empty()) {
    return false;
  }

  Entry& entry = redo_stack_.back();
  Project before = project_;
  const bool clone_store = entry.forward->mutates_midi_store();
  MidiContentStore store_copy;
  if (clone_store) {
    store_copy = midi_content_;
  }
  const MidiContentStore& store_before = clone_store ? store_copy : midi_content_;

  // Reserve the undo destination before reapplying the forward command.  The
  // redo entry remains in place until commit, so every failure leaves history
  // exactly as it was.
  undo_stack_.emplace_back();
  Entry& pending = undo_stack_.back();
  try {
    if (!entry.forward->apply(project_, midi_content_)) {
      rollback_command(entry.forward.get(), project_, midi_content_);
      undo_stack_.pop_back();
      swap_project_snapshot(project_, before);
      if (clone_store) {
        swap_store_snapshot(midi_content_, store_copy);
      }
      return false;
    }

    // Refresh the inverse against the pre-redo state so it remains exact even
    // when the forward command's effect depends on current state.  This is the
    // only post-apply allocation, and a failure is rolled back before either
    // stack is changed.
    // EditCommandGroup deliberately has no standalone invert(): its exact
    // inverse is already retained in the entry.  Preserve that established
    // redo convention while replacing inverses for ordinary stateful commands.
    EditCommandPtr inverse = entry.forward->invert(before, store_before);
    pending.forward = std::move(entry.forward);
    pending.inverse = inverse != nullptr ? std::move(inverse) : std::move(entry.inverse);
    redo_stack_.pop_back();
    trim_undo_stack();
    trim_after_redo();
    return true;
  } catch (...) {
    rollback_command(entry.forward.get(), project_, midi_content_);
    undo_stack_.pop_back();
    swap_project_snapshot(project_, before);
    if (clone_store) {
      swap_store_snapshot(midi_content_, store_copy);
    }
    throw;
  }
}

void EditHistory::set_max_undo_depth(size_t depth) {
  max_undo_depth_ = std::max<size_t>(1, depth);
  // Evict immediately when shrinking below the current depth so the memory is
  // released now, not deferred to the next push_undo().
  while (undo_stack_.size() > max_undo_depth_) {
    undo_stack_.pop_front();
  }
  trim_history_bytes();
}

void EditHistory::set_max_history_bytes(size_t bytes) {
  max_history_bytes_ = bytes;
  trim_history_bytes();
}

void EditHistory::clear_history() {
  undo_stack_.clear();
  redo_stack_.clear();
}

}  // namespace sonare::arrangement
