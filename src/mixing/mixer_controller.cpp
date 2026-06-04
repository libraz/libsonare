#include "mixing/mixer_controller.h"

#include <algorithm>

#include "mixing/solo_mute.h"

namespace sonare::mixing {

bool MixerController::add_strip(ChannelStrip* strip) {
  if (strip == nullptr || std::find(strips_.begin(), strips_.end(), strip) != strips_.end()) {
    return false;
  }
  strips_.push_back(strip);
  recompute_solo_mutes();
  return true;
}

bool MixerController::remove_strip(ChannelStrip* strip) {
  const auto found = std::find(strips_.begin(), strips_.end(), strip);
  if (found == strips_.end()) {
    return false;
  }
  (*found)->set_implied_mute(false);
  strips_.erase(found);
  recompute_solo_mutes();
  return true;
}

void MixerController::set_mute(ChannelStrip& strip, bool muted) { strip.set_muted(muted); }

void MixerController::set_solo(ChannelStrip& strip, bool soloed) {
  strip.set_soloed(soloed);
  recompute_solo_mutes();
}

void MixerController::set_solo_safe(ChannelStrip& strip, bool solo_safe) {
  strip.set_solo_safe(solo_safe);
  recompute_solo_mutes();
}

void MixerController::recompute_solo_mutes() {
  bool any_solo = false;
  for (const ChannelStrip* strip : strips_) {
    any_solo = any_solo || strip->soloed();
  }
  for (ChannelStrip* strip : strips_) {
    // A solo-safe strip (e.g. a reverb-return or talkback bus) is never
    // implied-muted by another strip's solo, matching effectively_muted()'s
    // `implied_mute() && !solo_safe()` guard.
    strip->set_implied_mute(solo_implies_mute(any_solo, strip->soloed(), strip->solo_safe()));
  }
}

}  // namespace sonare::mixing
