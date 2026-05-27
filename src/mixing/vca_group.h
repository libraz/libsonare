#pragma once

/// @file vca_group.h
/// @brief Control-only VCA group for gain processors.

#include <vector>

#include "mixing/gain.h"

namespace sonare::mixing {

class VcaGroup {
 public:
  bool add_member(GainProcessor* gain);
  bool remove_member(GainProcessor* gain);
  void set_vca_gain_db(float gain_db) noexcept;
  float vca_gain_db() const noexcept { return vca_gain_db_; }
  size_t size() const noexcept { return members_.size(); }

 private:
  std::vector<GainProcessor*> members_;
  float vca_gain_db_ = 0.0f;
};

}  // namespace sonare::mixing
