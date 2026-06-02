#include "mixing/vca_group.h"

#include <algorithm>

namespace sonare::mixing {

// A strip may belong to several VCA groups at once. Each group contributes its
// own gain (in dB) additively to the member's single vca_offset_db_; summing in
// the dB domain is equivalent to multiplying the linear VCA gains, which is the
// intended cascaded-fader behaviour. To keep this correct under add/remove and
// gain changes, every group applies *deltas* to the shared offset rather than
// overwriting it, so it only ever touches its own contribution.

bool VcaGroup::add_member(GainProcessor* gain) {
  if (gain == nullptr || std::find(members_.begin(), members_.end(), gain) != members_.end()) {
    return false;
  }
  members_.push_back(gain);
  // Add this group's contribution on top of any offset already applied by other
  // groups the strip belongs to.
  gain->set_vca_offset_db(gain->vca_offset_db() + vca_gain_db_);
  return true;
}

bool VcaGroup::remove_member(GainProcessor* gain) {
  const auto found = std::find(members_.begin(), members_.end(), gain);
  if (found == members_.end()) {
    return false;
  }
  // Subtract only this group's contribution, preserving any offset still owed to
  // the strip by other groups.
  (*found)->set_vca_offset_db((*found)->vca_offset_db() - vca_gain_db_);
  members_.erase(found);
  return true;
}

void VcaGroup::set_vca_gain_db(float gain_db) noexcept {
  // Apply the change to each member as a delta from the group's previous gain so
  // contributions from other groups remain intact and accumulate correctly.
  const float delta = gain_db - vca_gain_db_;
  vca_gain_db_ = gain_db;
  for (GainProcessor* member : members_) {
    member->set_vca_offset_db(member->vca_offset_db() + delta);
  }
}

}  // namespace sonare::mixing
