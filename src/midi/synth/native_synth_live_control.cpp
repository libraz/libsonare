#include "midi/synth/native_synth.h"

namespace sonare::midi::synth {

ControllerProfile default_controller_profile() noexcept {
  ControllerProfile profile;
  ControllerProfile::preset("gm", &profile);
  return profile;
}

ExcitationAxes NativeSynth::channel_excitation(const ControllerAxisState& axes,
                                               uint32_t& present) noexcept {
  ExcitationAxes out{};
  present = kAxisNone;
  // One row per axis the two layers share. Everything that decides which
  // controller fills an axis is the profile's, so this is the whole bridge.
  if (axes.has(ControllerAxis::kExcitation)) {
    out.force = axes.values[static_cast<size_t>(ControllerAxis::kExcitation)];
    present |= kAxisForce;
  }
  if (axes.has(ControllerAxis::kPosition)) {
    out.position = axes.values[static_cast<size_t>(ControllerAxis::kPosition)];
    present |= kAxisPosition;
  }
  if (axes.has(ControllerAxis::kBrightness)) {
    out.brightness = axes.values[static_cast<size_t>(ControllerAxis::kBrightness)];
    present |= kAxisBrightness;
  }
  if (axes.has(ControllerAxis::kMorph)) {
    out.morph = axes.values[static_cast<size_t>(ControllerAxis::kMorph)];
    present |= kAxisMorph;
  }
  return out;
}

void NativeSynth::push_excitation_control(uint8_t channel) noexcept {
  const uint8_t ch = channel & 0x0Fu;
  const ChannelState& st = channels_[ch];
  uint32_t present = kAxisNone;
  const ExcitationAxes base = channel_excitation(st.axes, present);
  const float speed_scale = static_cast<float>(st.expression) / 127.0f;
  for (NativeSynthVoice& v : pool_) {
    if (!v.active || v.channel != ch || v.patch == nullptr) continue;
    // Expression scales the bow speed (identity at CC11 == 127); every other
    // engine takes loudness through the shared expression VCA instead.
    if (v.patch->mode == SynthEngineMode::kBowedString) {
      v.bowed_string.set_bow_speed_scale(speed_scale);
    }
    // The axes override the preset only once a controller has reached them, so
    // a channel nothing has bound leaves every voice on its own voicing.
    if (present != kAxisNone) v.push_excitation(base, present);
  }
}

bool NativeSynth::set_controller_profile(const ControllerProfile& profile) noexcept {
  controller_profile_ = profile;
  for (uint8_t ch = 0; ch < 16; ++ch) {
    channels_[ch].axes.reset();
    refresh_channel_mod(ch);
    push_excitation_control(ch);
  }
  return true;
}

void NativeSynth::apply_controller_input(const Ump& ump) noexcept {
  std::array<ControllerAxisValue, kMaxControllerBindings> resolved{};
  const size_t count = controller_profile_.resolve(ump, resolved.data(), resolved.size());
  if (count == 0) return;
  const uint8_t ch = ump.channel() & 0x0Fu;
  bool channel_moved = false;
  for (size_t i = 0; i < count; ++i) {
    const ControllerAxisValue& value = resolved[i];
    if (value.note == kControllerAnyNote) {
      channels_[ch].axes.set(value.axis, value.value);
      channel_moved = true;
      continue;
    }
    // A per-note value reaches the voices on that note and no further. It is
    // not stored on the channel, so a later channel-wide value on the same axis
    // overwrites it in those voices: per-voice controller state is what MPE
    // adds, and until then the precedence is simply last writer wins.
    ControllerAxisState note_axes = channels_[ch].axes;
    note_axes.set(value.axis, value.value);
    uint32_t present = kAxisNone;
    const ExcitationAxes axes = channel_excitation(note_axes, present);
    if (present == kAxisNone) continue;
    for (NativeSynthVoice& v : pool_) {
      if (v.active && v.note == value.note && v.channel == ch && v.patch != nullptr) {
        v.push_excitation(axes, present);
      }
    }
  }
  if (!channel_moved) return;
  refresh_channel_mod(ch);
  push_excitation_control(ch);
}

}  // namespace sonare::midi::synth
