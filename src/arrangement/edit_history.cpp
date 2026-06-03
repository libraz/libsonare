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

 private:
  std::vector<EditCommandPtr> commands_;
};

}  // namespace

bool EditHistory::apply(EditCommandPtr command) {
  if (command == nullptr) {
    return false;
  }
  // Snapshot the pre-apply state so the inverse can capture prior values. The
  // Project and MidiContentStore are value types, so the copy is a deep clone.
  const Project before = project_;
  const MidiContentStore store_before = midi_content_;

  if (!command->apply(project_, midi_content_)) {
    // Apply failed: leave the project untouched (apply() must not partially
    // mutate on its failure paths) and push nothing.
    project_ = before;
    midi_content_ = store_before;
    return false;
  }

  EditCommandPtr inverse = command->invert(before, store_before);
  if (inverse == nullptr) {
    // No inverse means the command is not safely undoable; revert and reject so
    // the history never holds an irreversible entry.
    project_ = before;
    midi_content_ = store_before;
    return false;
  }

  Entry entry;
  entry.forward = std::move(command);
  entry.inverse = std::move(inverse);
  undo_stack_.push_back(std::move(entry));
  redo_stack_.clear();
  return true;
}

bool EditHistory::apply_transaction(std::vector<EditCommandPtr> commands) {
  if (commands.empty()) {
    return false;
  }

  const Project transaction_before = project_;
  const MidiContentStore transaction_store_before = midi_content_;

  std::vector<EditCommandPtr> forward;
  std::vector<EditCommandPtr> inverse;
  forward.reserve(commands.size());
  inverse.reserve(commands.size());

  for (auto& command : commands) {
    if (command == nullptr) {
      project_ = transaction_before;
      midi_content_ = transaction_store_before;
      return false;
    }

    const Project before = project_;
    const MidiContentStore store_before = midi_content_;
    if (!command->apply(project_, midi_content_)) {
      project_ = transaction_before;
      midi_content_ = transaction_store_before;
      return false;
    }

    EditCommandPtr undo = command->invert(before, store_before);
    if (undo == nullptr) {
      project_ = transaction_before;
      midi_content_ = transaction_store_before;
      return false;
    }
    forward.push_back(std::move(command));
    inverse.push_back(std::move(undo));
  }

  std::reverse(inverse.begin(), inverse.end());

  Entry entry;
  entry.forward = std::make_unique<EditCommandGroup>(std::move(forward));
  entry.inverse = std::make_unique<EditCommandGroup>(std::move(inverse));
  undo_stack_.push_back(std::move(entry));
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
  const MidiContentStore store_before = midi_content_;

  if (!entry.inverse->apply(project_, midi_content_)) {
    // Should not happen for a well-formed entry; restore and report failure.
    project_ = before;
    midi_content_ = store_before;
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
  const MidiContentStore store_before = midi_content_;

  if (!entry.forward->apply(project_, midi_content_)) {
    project_ = before;
    midi_content_ = store_before;
    redo_stack_.push_back(std::move(entry));
    return false;
  }

  // Refresh the inverse against the pre-redo state so it remains exact even when
  // the forward command's effect depends on current state.
  EditCommandPtr inverse = entry.forward->invert(before, store_before);
  if (inverse != nullptr) {
    entry.inverse = std::move(inverse);
  }
  undo_stack_.push_back(std::move(entry));
  return true;
}

void EditHistory::clear_history() {
  undo_stack_.clear();
  redo_stack_.clear();
}

}  // namespace sonare::arrangement
