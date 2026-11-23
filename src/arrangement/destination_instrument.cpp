#include "arrangement/edit_command.h"

namespace sonare::arrangement {

bool SetDestinationInstrument::apply(Project& project, MidiContentStore& /*store*/) {
  if (binding_.has_value()) {
    if (restore_dangling_) {
      project.restore_destination_instrument(destination_id_, binding_);
    } else {
      project.set_destination_instrument(destination_id_, *binding_);
    }
  } else {
    (void)project.remove_destination_instrument(destination_id_);
  }
  return true;
}

EditCommandPtr SetDestinationInstrument::invert(const Project& before,
                                                const MidiContentStore& /*store_before*/) const {
  const DestinationInstrumentBinding* prior = before.find_destination_instrument(destination_id_);
  if (prior == nullptr) {
    return std::make_unique<SetDestinationInstrument>(destination_id_, std::nullopt);
  }
  return EditCommandPtr(new SetDestinationInstrument(destination_id_, *prior, true));
}

std::size_t SetDestinationInstrument::retained_bytes() const noexcept {
  std::size_t total = sizeof(*this);
  if (binding_.has_value()) {
    total = retained::saturating_add(total, retained::dynamic_bytes(*binding_));
  }
  return total;
}

}  // namespace sonare::arrangement
