/// @file edit_command_part_rig.cpp
/// @brief Part-rig edit-command apply/invert definitions.

#include "arrangement/edit_command.h"
#include "arrangement/edit_command_internal.h"

namespace sonare::arrangement {

bool SetPartRig::apply(Project& project, MidiContentStore& /*store*/) {
  return project.set_part_rig(entry_);
}

EditCommandPtr SetPartRig::invert(const Project& before,
                                  const MidiContentStore& /*store_before*/) const {
  if (const ProjectPartRig* prior = before.find_part_rig(entry_.destination_id, entry_.part)) {
    return std::make_unique<SetPartRig>(prior->destination_id, prior->part, prior->rig);
  }
  return std::make_unique<ClearPartRig>(entry_.destination_id, entry_.part);
}

bool ClearPartRig::apply(Project& project, MidiContentStore& /*store*/) {
  return project.remove_part_rig(destination_id_, part_);
}

EditCommandPtr ClearPartRig::invert(const Project& before,
                                    const MidiContentStore& /*store_before*/) const {
  const ProjectPartRig* prior = before.find_part_rig(destination_id_, part_);
  if (prior == nullptr) return nullptr;
  return std::make_unique<SetPartRig>(prior->destination_id, prior->part, prior->rig);
}

}  // namespace sonare::arrangement
