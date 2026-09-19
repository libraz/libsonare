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
    case ModSource::kBreath:
      return values.breath;
    case ModSource::kAftertouch:
      return values.aftertouch;
    case ModSource::kExpressionCc:
      return values.expression_cc;
    case ModSource::kPitchBend:
      return values.pitch_bend;
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
      case ModDestination::kResonanceQ:
        out.resonance_q += amount;
        break;
      case ModDestination::kVibratoDepthCents:
        out.vibrato_depth_cents += amount;
        break;
      case ModDestination::kFilterEnvDepth:
        out.filter_env_depth *= 1.0f + amount;
        break;
      case ModDestination::kLfo1RateScale:
        out.lfo1_rate_scale *= 1.0f + amount;
        break;
      case ModDestination::kExcitationForce:
        out.excitation_force += amount;
        break;
      case ModDestination::kExcitationPosition:
        out.excitation_position += amount;
        break;
      case ModDestination::kExcitationBrightness:
        out.excitation_brightness += amount;
        break;
      case ModDestination::kSpectrumMorph:
        out.spectrum_morph += amount;
        break;
    }
  }
  out.amp_gain = std::clamp(out.amp_gain, 0.0f, 4.0f);
  out.pan_units = std::clamp(out.pan_units, -500.0f, 500.0f);
  out.filter_env_depth = std::clamp(out.filter_env_depth, 0.0f, 4.0f);
  // Four octaves either way. The floor is not zero: a rate of zero freezes the
  // LFO at whatever phase it stopped on, which reads as a stuck detune rather
  // than as no vibrato.
  out.lfo1_rate_scale = std::clamp(out.lfo1_rate_scale, 0.0625f, 16.0f);
  // A full-span offset either way; the engine clamps the sum to its own axis.
  out.excitation_force = std::clamp(out.excitation_force, -1.0f, 1.0f);
  out.excitation_position = std::clamp(out.excitation_position, -1.0f, 1.0f);
  out.excitation_brightness = std::clamp(out.excitation_brightness, -1.0f, 1.0f);
  out.spectrum_morph = std::clamp(out.spectrum_morph, -1.0f, 1.0f);
  return out;
}

}  // namespace sonare::midi::synth
