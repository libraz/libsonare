#include "midi/synth/native_synth.h"

namespace sonare::midi::synth {

void NativeSynth::push_bow_control(uint8_t channel) noexcept {
  if (!bowed_string_mode_) return;
  const uint8_t ch = channel & 0x0Fu;
  const ChannelState& st = channels_[ch];
  const float speed_scale = static_cast<float>(st.expression) / 127.0f;
  for (NativeSynthVoice& v : pool_) {
    if (!v.active || v.channel != ch || v.patch == nullptr ||
        v.patch->mode != SynthEngineMode::kBowedString) {
      continue;
    }
    // Expression scales the bow speed (identity at CC11 == 127); breath / CC74
    // override the preset's force / position only once the host sends them.
    v.bowed_string.set_bow_speed_scale(speed_scale);
    if (st.bow_force != 255)
      v.bowed_string.set_bow_force(static_cast<float>(st.bow_force) / 127.0f);
    if (st.bow_position != 255) {
      v.bowed_string.set_bow_position(static_cast<float>(st.bow_position) / 127.0f);
    }
  }
}

void NativeSynth::push_reed_control(uint8_t channel) noexcept {
  if (!reed_mode_) return;
  const uint8_t ch = channel & 0x0Fu;
  const ChannelState& st = channels_[ch];
  for (NativeSynthVoice& v : pool_) {
    if (!v.active || v.channel != ch || v.patch == nullptr ||
        v.patch->mode != SynthEngineMode::kReed) {
      continue;
    }
    // Breath / brightness override the preset only once the host sends the CC;
    // loudness is the shared expression VCA, not a reed-specific push.
    if (st.reed_breath != 255) v.reed.set_breath(static_cast<float>(st.reed_breath) / 127.0f);
    if (st.reed_bright != 255) v.reed.set_brightness(static_cast<float>(st.reed_bright) / 127.0f);
  }
}

void NativeSynth::push_brass_control(uint8_t channel) noexcept {
  if (!brass_mode_) return;
  const uint8_t ch = channel & 0x0Fu;
  const ChannelState& st = channels_[ch];
  for (NativeSynthVoice& v : pool_) {
    if (!v.active || v.channel != ch || v.patch == nullptr ||
        v.patch->mode != SynthEngineMode::kBrass) {
      continue;
    }
    // Breath / brightness override the preset only once the host sends the CC;
    // loudness is the shared expression VCA, not a brass-specific push.
    if (st.brass_breath != 255) v.brass.set_breath(static_cast<float>(st.brass_breath) / 127.0f);
    if (st.brass_bright != 255)
      v.brass.set_brightness(static_cast<float>(st.brass_bright) / 127.0f);
  }
}

void NativeSynth::push_flute_control(uint8_t channel) noexcept {
  if (!flute_mode_) return;
  const uint8_t ch = channel & 0x0Fu;
  const ChannelState& st = channels_[ch];
  for (NativeSynthVoice& v : pool_) {
    if (!v.active || v.channel != ch || v.patch == nullptr ||
        v.patch->mode != SynthEngineMode::kFlute) {
      continue;
    }
    // Breath / brightness override the preset only once the host sends the CC;
    // loudness is the shared expression VCA, not a flute-specific push, and CC1
    // vibrato rides the shared mod-wheel LFO.
    if (st.flute_breath != 255) v.flute.set_breath(static_cast<float>(st.flute_breath) / 127.0f);
    if (st.flute_bright != 255)
      v.flute.set_brightness(static_cast<float>(st.flute_bright) / 127.0f);
  }
}

}  // namespace sonare::midi::synth
