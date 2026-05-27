#include "mixing/vca_group.h"

#include <algorithm>

namespace sonare::mixing {

bool VcaGroup::add_member(GainProcessor* gain) {
  if (gain == nullptr || std::find(members_.begin(), members_.end(), gain) != members_.end()) {
    return false;
  }
  members_.push_back(gain);
  gain->set_vca_offset_db(vca_gain_db_);
  return true;
}

bool VcaGroup::remove_member(GainProcessor* gain) {
  const auto found = std::find(members_.begin(), members_.end(), gain);
  if (found == members_.end()) {
    return false;
  }
  (*found)->set_vca_offset_db(0.0f);
  members_.erase(found);
  return true;
}

void VcaGroup::set_vca_gain_db(float gain_db) noexcept {
  vca_gain_db_ = gain_db;
  for (GainProcessor* member : members_) {
    member->set_vca_offset_db(gain_db);
  }
}

}  // namespace sonare::mixing
