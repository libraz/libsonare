#include "midi/synth/native_synth.h"

namespace sonare::midi::synth {

ExcitationAxes NativeSynth::channel_excitation(const ChannelState& st, uint32_t& present) noexcept {
  ExcitationAxes axes{};
  present = kAxisNone;
  if (st.excitation_force != 255) {
    axes.force = static_cast<float>(st.excitation_force) / 127.0f;
    present |= kAxisForce;
  }
  if (st.excitation_bright != 255) {
    // CC74 is the second axis whichever engine reads it, and the two spellings
    // never coexist: a bowed string has no bell and nothing else has a contact
    // point, so filling both is unambiguous rather than a guess.
    axes.brightness = static_cast<float>(st.excitation_bright) / 127.0f;
    axes.position = axes.brightness;
    present |= kAxisBrightness | kAxisPosition;
  }
  return axes;
}

void NativeSynth::push_excitation_control(uint8_t channel) noexcept {
  const uint8_t ch = channel & 0x0Fu;
  const ChannelState& st = channels_[ch];
  uint32_t present = kAxisNone;
  const ExcitationAxes base = channel_excitation(st, present);
  const float speed_scale = static_cast<float>(st.expression) / 127.0f;
  for (NativeSynthVoice& v : pool_) {
    if (!v.active || v.channel != ch || v.patch == nullptr) continue;
    // Expression scales the bow speed (identity at CC11 == 127); every other
    // engine takes loudness through the shared expression VCA instead.
    if (v.patch->mode == SynthEngineMode::kBowedString) {
      v.bowed_string.set_bow_speed_scale(speed_scale);
    }
    // The axes override the preset only once the host has sent the CC, so a
    // channel that has sent neither leaves every voice on its own voicing.
    if (present != kAxisNone) v.push_excitation(base, present);
  }
}

}  // namespace sonare::midi::synth
