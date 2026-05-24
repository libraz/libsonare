#pragma once

/// @file mixer_controller.h
/// @brief Non-RT strip registry for solo/mute coordination.

#include <vector>

#include "mixing/channel_strip.h"

namespace sonare::mixing {

class MixerController {
 public:
  bool add_strip(ChannelStrip* strip);
  bool remove_strip(ChannelStrip* strip);

  void set_mute(ChannelStrip& strip, bool muted);
  void set_solo(ChannelStrip& strip, bool soloed);
  void set_solo_safe(ChannelStrip& strip, bool solo_safe);
  void recompute_solo_mutes();

  size_t size() const noexcept { return strips_.size(); }

 private:
  std::vector<ChannelStrip*> strips_;
};

}  // namespace sonare::mixing
