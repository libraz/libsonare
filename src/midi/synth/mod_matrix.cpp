#include "midi/synth/mod_matrix.h"

#include <algorithm>

namespace sonare::midi::synth {

namespace {

float source_value(ModSource source, const ModSourceValues& values) noexcept {
  switch (source) {
    case ModSource::kNone:
      return 0.0f;
    case ModSource::kAmpEnv:
      return values.amp_env;
    case ModSource::kFilterEnv:
      return values.filter_env;
    case ModSource::kLfo1:
      return values.lfo1;
    case ModSource::kLfo2:
      return values.lfo2;
    case ModSource::kVelocity:
      return values.velocity;
    case ModSource::kKeyTrack:
      return values.key_track;
    case ModSource::kModWheel:
      return values.mod_wheel;
    case ModSource::kRandom:
      return values.random;
  }
  return 0.0f;
}

}  // namespace

ModOffsets evaluate_mod_matrix(const ModMatrix& matrix, const ModSourceValues& values) noexcept {
  ModOffsets out;
  for (const ModRoute& route : matrix.routes) {
    if (route.source == ModSource::kNone || route.destination == ModDestination::kNone ||
        route.depth == 0.0f) {
      continue;
    }
    const float amount = route.depth * source_value(route.source, values);
    switch (route.destination) {
      case ModDestination::kNone:
        break;
      case ModDestination::kPitchCents:
        out.pitch_cents += amount;
        break;
      case ModDestination::kCutoffCents:
        out.cutoff_cents += amount;
        break;
      case ModDestination::kAmpGain:
        out.amp_gain *= 1.0f + amount;
        break;
      case ModDestination::kPanUnits:
        out.pan_units += amount;
        break;
    }
  }
  out.amp_gain = std::clamp(out.amp_gain, 0.0f, 4.0f);
  out.pan_units = std::clamp(out.pan_units, -500.0f, 500.0f);
  return out;
}

}  // namespace sonare::midi::synth
