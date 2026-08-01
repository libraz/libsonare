#include "midi/synth/native_synth.h"

#include <algorithm>
#include <cmath>

#include "midi/builtin_synth.h"
#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/voice_random.h"
#include "midi/ump.h"
#include "util/constants.h"

namespace sonare::midi::synth {

namespace {

/// Default modulator: full CC1 adds 50 cents of vibrato depth (matches
/// Sf2Player so the fallback and SF2 voices respond alike).
constexpr float kModWheelVibratoCents = 50.0f;

/// Sympathetic-string bank t60 (seconds): how long the plucked-string sound
/// halo rings. Shared by the bank tuning (prepare_custom) and the tail estimate.
constexpr float kKsSympatheticRingS = 1.5f;

}  // namespace

// ---------------------------------------------------------------------------
// NativeSynth (MidiInstrument)
// ---------------------------------------------------------------------------

NativeSynth::NativeSynth(const NativeSynthConfig& config) : config_(config) {
  config_.patch = clamp_synth_patch(config_.patch);
  if (!(config_.gain > 0.0f) || !std::isfinite(config_.gain)) config_.gain = 0.5f;
  config_.gain = std::min(config_.gain, 4.0f);
  config_.polyphony = config_.polyphony > 0 ? std::min(config_.polyphony, kMaxSynthVoices) : 16;
  config_.bus_drive =
      std::isfinite(config_.bus_drive) ? std::clamp(config_.bus_drive, 0.0f, 1.0f) : 0.0f;
}

void NativeSynth::prepare(double sample_rate, int /*max_block_size*/) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  pool_.prepare(config_.polyphony);
  // KS strings need a per-voice delay slab (the only allocation site; voices
  // attach their span at note-on).
  ks_capacity_ = ks_buffer_capacity(sample_rate_);
  sympathetic_active_ = false;
  if (config_.patch.mode == SynthEngineMode::kKarplusStrong) {
    ks_buffers_.assign(pool_.size() * static_cast<size_t>(ks_slab_capacity(sample_rate_)), 0.0f);
    // Sympathetic-string "sound halo": a shared bank tuned to the standard-
    // tuning open strings (E2 A2 D3 G3 B3 E4) plus their low harmonics — the
    // undamped strings ringing behind the played note. Plucked strings have no
    // dampers, so the bank is held open (process() passes damper_open == true).
    if (config_.patch.ks.sympathetic) {
      resonance_.prepare_guitar_sympathetic(sample_rate_);
      sympathetic_active_ = true;
    }
  } else {
    ks_buffers_.clear();
  }
  piano_string_capacity_ = piano_string_capacity(sample_rate_);
  piano_mode_ = config_.patch.mode == SynthEngineMode::kPiano;
  if (piano_mode_) {
    piano_buffers_.assign(pool_.size() * static_cast<size_t>(piano_slab_capacity(sample_rate_)),
                          0.0f);
    resonance_.prepare(sample_rate_);
    soundboard_.prepare(sample_rate_, config_.patch.piano.soundboard);
  } else {
    piano_buffers_.clear();
  }
  // Pipe organ: one delay slab per voice slot (kMaxPipeRanks bore+jet span pairs,
  // so a registration's ranks all have their own self-oscillating jet pipe). The
  // only allocation site; voices attach their slab at note-on.
  pipe_organ_capacity_ = pipe_organ_buffer_capacity(sample_rate_);
  pipe_organ_mode_ = config_.patch.mode == SynthEngineMode::kPipeOrgan;
  if (pipe_organ_mode_) {
    pipe_organ_buffers_.assign(
        pool_.size() * static_cast<size_t>(pipe_organ_slab_capacity(sample_rate_)), 0.0f);
    wind_.prepare(sample_rate_, config_.patch.pipe_organ.tremulant_rate_hz,
                  config_.patch.pipe_organ.tremulant_depth, config_.patch.pipe_organ.wind_sag);
    swell_depth_ = config_.patch.pipe_organ.swell;
  } else {
    pipe_organ_buffers_.clear();
    swell_depth_ = 0.0f;
  }
  // Bowed string: one delay slab per voice slot (two delay-line spans, the neck
  // and bridge). The only allocation site; voices attach their slab at note-on.
  bowed_string_capacity_ = bowed_string_buffer_capacity(sample_rate_);
  bowed_string_mode_ = config_.patch.mode == SynthEngineMode::kBowedString;
  if (bowed_string_mode_) {
    bowed_string_buffers_.assign(
        pool_.size() * static_cast<size_t>(bowed_string_slab_capacity(sample_rate_)), 0.0f);
  } else {
    bowed_string_buffers_.clear();
  }
  // Reed woodwind: one bore delay span per voice slot. The only allocation site;
  // voices attach their span at note-on.
  reed_capacity_ = reed_buffer_capacity(sample_rate_);
  reed_mode_ = config_.patch.mode == SynthEngineMode::kReed;
  if (reed_mode_) {
    reed_buffers_.assign(pool_.size() * static_cast<size_t>(reed_slab_capacity(sample_rate_)),
                         0.0f);
  } else {
    reed_buffers_.clear();
  }
  // Brass / lip reed: one bore delay span per voice slot (same as the reed).
  brass_capacity_ = brass_buffer_capacity(sample_rate_);
  brass_mode_ = config_.patch.mode == SynthEngineMode::kBrass;
  if (brass_mode_) {
    brass_buffers_.assign(pool_.size() * static_cast<size_t>(brass_slab_capacity(sample_rate_)),
                          0.0f);
  } else {
    brass_buffers_.clear();
  }
  // Air-jet flute: a bore span plus a jet span per voice slot.
  flute_capacity_ = flute_buffer_capacity(sample_rate_);
  flute_mode_ = config_.patch.mode == SynthEngineMode::kFlute;
  if (flute_mode_) {
    flute_buffers_.assign(pool_.size() * static_cast<size_t>(flute_slab_capacity(sample_rate_)),
                          0.0f);
  } else {
    flute_buffers_.clear();
  }
  // Plucked string: one string delay span per voice slot. The only allocation
  // site; voices attach their span at note-on.
  plucked_string_capacity_ = plucked_string_buffer_capacity(sample_rate_);
  plucked_string_mode_ = config_.patch.mode == SynthEngineMode::kPluckedString;
  if (plucked_string_mode_) {
    plucked_string_buffers_.assign(
        pool_.size() * static_cast<size_t>(plucked_string_slab_capacity(sample_rate_)), 0.0f);
  } else {
    plucked_string_buffers_.clear();
  }
  swell_lp_l_ = 0.0f;
  swell_lp_r_ = 0.0f;
  channels_ = {};
  for (uint8_t ch = 0; ch < 16; ++ch) refresh_channel_mod(ch);
  const bool gm_kit =
      config_.patch.mode == SynthEngineMode::kPercussion && config_.patch.percussion.gm_kit;
  tail_samples_ = DahdsrEnvelope::release_tail_samples(
      sample_rate_, gm_kit ? gm_fallback_max_release_ms() : config_.patch.amp_env.release_ms);
  if (sympathetic_active_) {
    // The shared sympathetic bank keeps ringing after the last voice releases;
    // fold its halo t60 into the tail so a bounce does not clip the sound halo.
    tail_samples_ += static_cast<int64_t>(sample_rate_ * kKsSympatheticRingS);
  }
  // Mix-bus polish: ~8 Hz DC blocker pole and the gain-neutral drive factor.
  dc_r_ = 1.0f - static_cast<float>(constants::kTwoPiD * 8.0 / sample_rate_);
  dc_x1_ = {};
  dc_y1_ = {};
  bus_drive_gain_ = config_.bus_drive > 0.0f ? 1.0f + 3.0f * config_.bus_drive : 0.0f;
  prepared_ = true;
}

void NativeSynth::reset() {
  pool_.reset();
  dc_x1_ = {};
  dc_y1_ = {};
  resonance_.reset();
  soundboard_.reset();
  wind_.reset();
  swell_lp_l_ = 0.0f;
  swell_lp_r_ = 0.0f;
  channels_ = {};
  for (uint8_t ch = 0; ch < 16; ++ch) refresh_channel_mod(ch);
}

void NativeSynth::refresh_channel_mod(uint8_t channel) noexcept {
  const uint8_t ch = channel & 0x0Fu;
  const ChannelState& st = channels_[ch];
  Sf2ChannelMod& mod = channel_mods_[ch];
  mod.pitch_cents = (static_cast<float>(st.pitch_bend) - 8192.0f) / 8192.0f * st.bend_range_cents;
  mod.gain = sf2_cc_gain(st.volume) * sf2_cc_gain(st.expression);
  mod.extra_vibrato_cents = kModWheelVibratoCents * static_cast<float>(st.mod_wheel) / 127.0f;
  mod.pan_units = (static_cast<float>(st.pan) - 64.0f) / 63.0f * 500.0f;
}

void NativeSynth::note_on(uint8_t channel, uint8_t note, uint8_t velocity,
                          uint32_t source_track_id) noexcept {
  if (!prepared_) return;
  const uint8_t ch = channel & 0x0Fu;
  // GM kit exclusive/mute groups: a new hi-hat / triangle / whistle / surdo
  // strike chokes the ringing voice in its group before the new one allocates.
  if (config_.patch.mode == SynthEngineMode::kPercussion && config_.patch.percussion.gm_kit) {
    const uint8_t excl = gm_fallback_drum_patch(note).percussion.exclusive_class;
    if (excl != 0) {
      for (NativeSynthVoice& v : pool_) {
        if (v.active && v.channel == ch && v.patch != nullptr &&
            v.patch->mode == SynthEngineMode::kPercussion &&
            v.patch->percussion.exclusive_class == excl) {
          v.choke();
        }
      }
    }
  }
  NativeSynthVoice* voice = pool_.allocate(ch, note, source_track_id);
  if (voice == nullptr) return;
  const uint32_t voice_index = static_cast<uint32_t>(voice - pool_.data());
  // KS patches get their delay span before start() (pointer wiring only).
  if (!ks_buffers_.empty()) {
    voice->ks.attach(ks_buffers_.data() + static_cast<size_t>(voice_index) * 3 * ks_capacity_,
                     ks_capacity_);
  }
  if (!piano_buffers_.empty()) {
    voice->piano.attach(piano_buffers_.data() + static_cast<size_t>(voice_index) *
                                                    kMaxPianoStrings * piano_string_capacity_,
                        piano_string_capacity_);
  }
  if (!pipe_organ_buffers_.empty()) {
    voice->pipe_organ.attach(pipe_organ_buffers_.data() + static_cast<size_t>(voice_index) * 2 *
                                                              kMaxPipeRanks * pipe_organ_capacity_,
                             pipe_organ_capacity_);
  }
  if (!bowed_string_buffers_.empty()) {
    voice->bowed_string.attach(bowed_string_buffers_.data() +
                                   static_cast<size_t>(voice_index) * 3 * bowed_string_capacity_,
                               bowed_string_capacity_);
  }
  if (!reed_buffers_.empty()) {
    voice->reed.attach(reed_buffers_.data() + static_cast<size_t>(voice_index) * reed_capacity_,
                       reed_capacity_);
  }
  if (!brass_buffers_.empty()) {
    voice->brass.attach(brass_buffers_.data() + static_cast<size_t>(voice_index) * brass_capacity_,
                        brass_capacity_);
  }
  if (!flute_buffers_.empty()) {
    voice->flute.attach(
        flute_buffers_.data() + static_cast<size_t>(voice_index) * 2 * flute_capacity_,
        flute_capacity_);
  }
  if (!plucked_string_buffers_.empty()) {
    voice->plucked_string.attach(plucked_string_buffers_.data() +
                                     static_cast<size_t>(voice_index) * plucked_string_capacity_,
                                 plucked_string_capacity_);
  }
  // GM kit mode: resolve the struck note through the drum map instead of
  // playing the single configured piece (static patches — safe to keep in the
  // voice for its whole life; every kit piece is kPercussion, so the KS/piano
  // slabs are never needed).
  const NativeSynthPatch* patch = &config_.patch;
  uint8_t drum_kit = 0;
  if (patch->mode == SynthEngineMode::kPercussion && patch->percussion.gm_kit) {
    patch = &gm_fallback_drum_patch(note);
    drum_kit = gm_fallback_drum_kit(channels_[ch].program);
  }
  // Portamento: glide from the channel's previous note when enabled.
  const float glide_from = patch->glide_ms > 0.0f ? channels_[ch].last_freq_hz : 0.0f;
  voice->start(*patch, sample_rate_, velocity, voice_index, glide_from, channels_[ch].una_corda,
               drum_kit);
  // Seed a bowed voice at the channel's current bow controllers (no glide on the
  // first sample) so a note struck mid-phrase starts at the live bow position /
  // force / expression rather than gliding in from the preset.
  if (patch->mode == SynthEngineMode::kBowedString) {
    const ChannelState& st = channels_[ch];
    voice->bowed_string.set_bow_speed_scale(static_cast<float>(st.expression) / 127.0f);
    if (st.bow_force != 255) {
      voice->bowed_string.set_bow_force(static_cast<float>(st.bow_force) / 127.0f);
    }
    if (st.bow_position != 255) {
      voice->bowed_string.set_bow_position(static_cast<float>(st.bow_position) / 127.0f);
    }
    voice->bowed_string.snap_bow_control();
  }
  // Seed a reed voice at the channel's current reed controllers (no glide on the
  // first sample) so a note struck mid-phrase starts at the live breath /
  // brightness rather than gliding in from the preset.
  if (patch->mode == SynthEngineMode::kReed) {
    const ChannelState& st = channels_[ch];
    if (st.reed_breath != 255) voice->reed.set_breath(static_cast<float>(st.reed_breath) / 127.0f);
    if (st.reed_bright != 255) {
      voice->reed.set_brightness(static_cast<float>(st.reed_bright) / 127.0f);
    }
    voice->reed.snap_reed_control();
  }
  // Seed a brass voice at the channel's current brass controllers (no glide on
  // the first sample) so a note struck mid-phrase starts at the live breath /
  // brightness rather than gliding in from the preset.
  if (patch->mode == SynthEngineMode::kBrass) {
    const ChannelState& st = channels_[ch];
    if (st.brass_breath != 255) {
      voice->brass.set_breath(static_cast<float>(st.brass_breath) / 127.0f);
    }
    if (st.brass_bright != 255) {
      voice->brass.set_brightness(static_cast<float>(st.brass_bright) / 127.0f);
    }
    voice->brass.snap_brass_control();
  }
  // Seed a flute voice at the channel's current flute controllers (no glide on
  // the first sample) so a note struck mid-phrase starts at the live breath /
  // brightness rather than gliding in from the preset.
  if (patch->mode == SynthEngineMode::kFlute) {
    const ChannelState& st = channels_[ch];
    if (st.flute_breath != 255) {
      voice->flute.set_breath(static_cast<float>(st.flute_breath) / 127.0f);
    }
    if (st.flute_bright != 255) {
      voice->flute.set_brightness(static_cast<float>(st.flute_bright) / 127.0f);
    }
    voice->flute.snap_flute_control();
  }
  channels_[ch].last_freq_hz = voice->base_freq_hz;
}

void NativeSynth::note_off(uint8_t channel, uint8_t note, uint32_t source_track_id) noexcept {
  if (!prepared_) return;
  const uint8_t ch = channel & 0x0Fu;
  const ChannelState& st = channels_[ch];
  for (NativeSynthVoice& v : pool_) {
    if (v.active && v.note == note && v.channel == ch && v.source_track_id == source_track_id &&
        v.key_down) {
      v.key_down = false;
      // A sostenuto capture holds the note regardless of the sustain pedal.
      if (v.sostenuto) continue;
      if (!st.sustain) {
        v.release();  // pedal up: the damper falls now
      } else if (st.sustain_level < 127 && v.patch != nullptr &&
                 v.patch->mode == SynthEngineMode::kPiano) {
        // Half-pedal: the partially raised damper rests on the string.
        v.piano.damp(static_cast<float>(127 - st.sustain_level) / 63.0f);
      }
      // else (full pedal): the string rings on freely.
    }
  }
}

void NativeSynth::sustain_cc(uint8_t channel, uint8_t value) noexcept {
  if (!prepared_) return;
  const uint8_t ch = channel & 0x0Fu;
  ChannelState& st = channels_[ch];
  const bool was_down = st.sustain;
  st.sustain_level = value;
  st.sustain = value >= 64;
  if (st.sustain) {
    // Half-pedal: a partially raised damper still rests on the strings, so held
    // (key-up) notes ring on at an intermediate rate; a full lift (127) leaves
    // them ringing freely. Key-down and sostenuto-captured notes keep their
    // dampers mechanically off, so they are untouched.
    if (value < 127) {
      const float strength = static_cast<float>(127 - value) / 63.0f;
      for (NativeSynthVoice& v : pool_) {
        if (v.active && v.channel == ch && !v.key_down && !v.sostenuto && v.patch != nullptr &&
            v.patch->mode == SynthEngineMode::kPiano) {
          v.piano.damp(strength);
        }
      }
    }
    return;
  }
  if (!was_down) return;
  for (NativeSynthVoice& v : pool_) {
    // A sostenuto-captured note stays held even when the sustain pedal lifts.
    if (v.active && v.channel == ch && !v.key_down && !v.releasing && !v.sostenuto) v.release();
  }
}

void NativeSynth::sostenuto_pedal(uint8_t channel, bool down) noexcept {
  if (!prepared_) return;
  const uint8_t ch = channel & 0x0Fu;
  for (NativeSynthVoice& v : pool_) {
    if (!v.active || v.channel != ch) continue;
    if (down) {
      // Capture only the notes whose keys are down at the moment of the press.
      if (v.key_down) v.sostenuto = true;
    } else if (v.sostenuto) {
      v.sostenuto = false;
      if (!v.key_down && !channels_[ch].sustain) v.release();
    }
  }
}

void NativeSynth::all_notes_off(uint8_t channel) noexcept {
  if (!prepared_) return;
  const uint8_t ch = channel & 0x0Fu;
  channels_[ch].sustain = false;
  channels_[ch].sustain_level = 0;
  for (NativeSynthVoice& v : pool_) {
    if (v.active && v.channel == ch && !v.releasing) {
      v.key_down = false;
      v.sostenuto = false;
      v.release();
    }
  }
}

void NativeSynth::all_sound_off(uint8_t channel) noexcept {
  if (!prepared_) return;
  const uint8_t ch = channel & 0x0Fu;
  channels_[ch].sustain = false;
  channels_[ch].sustain_level = 0;
  for (NativeSynthVoice& v : pool_) {
    if (v.active && v.channel == ch) v.kill();
  }
  if (pool_.active_count() == 0) {
    // All Sound Off means silence NOW: flush the bus DC blocker so its IIR
    // tail cannot keep a residual trickling out.
    dc_x1_ = {};
    dc_y1_ = {};
  }
}

void NativeSynth::reset_controllers(uint8_t channel) noexcept {
  // MIDI RP-015: reset performance controllers, keep volume/pan.
  const uint8_t ch = channel & 0x0Fu;
  ChannelState& st = channels_[ch];
  st.mod_wheel = 0;
  st.expression = 127;
  st.pitch_bend = 8192;
  st.bow_force = 255;
  st.bow_position = 255;
  st.reed_breath = 255;
  st.reed_bright = 255;
  st.brass_breath = 255;
  st.brass_bright = 255;
  st.flute_breath = 255;
  st.flute_bright = 255;
  st.params.reset();
  sustain_cc(ch, 0);
  sostenuto_pedal(ch, false);
  st.una_corda = false;
  refresh_channel_mod(ch);
  push_bow_control(ch);
  push_reed_control(ch);
  push_brass_control(ch);
  push_flute_control(ch);
}

void NativeSynth::control_change(uint8_t channel, uint8_t controller, uint8_t value) noexcept {
  const uint8_t ch = channel & 0x0Fu;
  ChannelState& st = channels_[ch];
  switch (controller) {
    case 1:
      st.mod_wheel = value;
      refresh_channel_mod(ch);
      break;
    case 7:
      st.volume = value;
      refresh_channel_mod(ch);
      break;
    case 10:
      st.pan = value;
      refresh_channel_mod(ch);
      break;
    case 2:
      st.bow_force = value;     // breath -> bowed-string bow force
      st.reed_breath = value;   // breath -> reed mouth pressure
      st.brass_breath = value;  // breath -> brass mouth pressure
      st.flute_breath = value;  // breath -> flute mouth pressure
      push_bow_control(ch);
      push_reed_control(ch);
      push_brass_control(ch);
      push_flute_control(ch);
      break;
    case 11:
      st.expression = value;
      refresh_channel_mod(ch);
      push_bow_control(ch);  // expression scales bowed-string bow speed (reed /
                             // brass loudness rides the shared expression VCA)
      break;
    case 74:
      st.bow_position = value;  // brightness/SC5 -> bowed-string bow position
      st.reed_bright = value;   // brightness/SC5 -> reed bell brightness
      st.brass_bright = value;  // brightness/SC5 -> brass bell brightness
      st.flute_bright = value;  // brightness/SC5 -> flute reflection brightness
      push_bow_control(ch);
      push_reed_control(ch);
      push_brass_control(ch);
      push_flute_control(ch);
      break;
    case 6:
      if (st.params.selected_rpn(0, 0)) {
        st.bend_range_cents = 100.0f * static_cast<float>(value);
        refresh_channel_mod(ch);
      }
      break;
    case 38:
      if (st.params.selected_rpn(0, 0)) {
        st.bend_range_cents =
            100.0f * std::floor(st.bend_range_cents / 100.0f) + static_cast<float>(value);
        refresh_channel_mod(ch);
      }
      break;
    case 64:
      sustain_cc(ch, value);
      break;
    case 66:
      sostenuto_pedal(ch, value >= 64);
      break;
    case 67:
      st.una_corda = value >= 64;  // soft pedal (affects notes struck while held)
      break;
    case 98:
      st.params.select_nrpn_lsb(value);
      break;
    case 99:
      st.params.select_nrpn_msb(value);
      break;
    case 100:
      st.params.select_rpn_lsb(value);
      break;
    case 101:
      st.params.select_rpn_msb(value);
      break;
    case 120:
      all_sound_off(ch);
      break;
    case 121:
      reset_controllers(ch);
      break;
    case 123:
    case 124:
    case 125:
    case 126:
    case 127:
      all_notes_off(ch);
      break;
    default:
      break;
  }
}

void NativeSynth::on_event(uint32_t /*destination_id*/, const MidiEvent& event) noexcept {
  if (!prepared_) return;
  const Ump& u = event.ump;
  if (u.message_type() != UmpMessageType::kMidi1ChannelVoice &&
      u.message_type() != UmpMessageType::kMidi2ChannelVoice) {
    return;
  }
  if (u.is_note_on()) {
    uint8_t vel7 = 0;
    if (u.message_type() == UmpMessageType::kMidi1ChannelVoice) {
      vel7 = u.data2_7bit();
    } else {
      vel7 = static_cast<uint8_t>(((u.words[1] >> 16) & 0xFFFFu) >> 9);
      if (vel7 == 0 && ((u.words[1] >> 16) & 0xFFFFu) != 0) vel7 = 1;
    }
    note_on(u.channel(), u.note_number(), vel7, event.source_track_id);
  } else if (u.is_note_off()) {
    note_off(u.channel(), u.note_number(), event.source_track_id);
  } else if (u.status_nibble() == static_cast<uint8_t>(UmpStatus::kPitchBend)) {
    const uint8_t ch = u.channel() & 0x0Fu;
    if (u.message_type() == UmpMessageType::kMidi1ChannelVoice) {
      channels_[ch].pitch_bend =
          static_cast<uint16_t>((static_cast<uint16_t>(u.data2_7bit()) << 7) | u.note_number());
    } else {
      channels_[ch].pitch_bend = static_cast<uint16_t>(u.words[1] >> 18);
    }
    refresh_channel_mod(ch);
  } else if (u.status_nibble() == static_cast<uint8_t>(UmpStatus::kControlChange)) {
    const uint8_t value7 = u.message_type() == UmpMessageType::kMidi1ChannelVoice
                               ? u.data2_7bit()
                               : scale_cc_32_to_7(u.words[1]);
    control_change(u.channel(), u.note_number(), value7);
  } else if (u.status_nibble() == static_cast<uint8_t>(UmpStatus::kProgramChange)) {
    // GS drum-kit select: in gm_kit mode the drum channel's program picks the
    // kit variation (Room/Power/808/...). Melodic patches ignore it.
    channels_[u.channel() & 0x0Fu].program = u.message_type() == UmpMessageType::kMidi2ChannelVoice
                                                 ? static_cast<uint8_t>((u.words[1] >> 24) & 0x7Fu)
                                                 : u.note_number();
  }
}

void NativeSynth::process(float* const* channels, int num_channels, int num_samples) {
  process_impl(channels, nullptr, 0, num_channels, num_samples);
}

bool NativeSynth::process_source_tracks(const MidiInstrumentSourceOutput* outputs,
                                        size_t output_count, int num_channels,
                                        int num_samples) noexcept {
  if (outputs == nullptr || output_count == 0 || outputs[0].source_track_id != 0 ||
      outputs[0].channels == nullptr) {
    return false;
  }
  process_impl(nullptr, outputs, output_count, num_channels, num_samples);
  return true;
}

void NativeSynth::process_impl(float* const* channels,
                               const MidiInstrumentSourceOutput* source_outputs,
                               size_t source_output_count, int num_channels,
                               int num_samples) noexcept {
  const bool source_render = source_outputs != nullptr;
  if (!prepared_ || num_channels <= 0 || num_samples <= 0 ||
      (!source_render && channels == nullptr)) {
    return;
  }
  float* left = source_render ? nullptr : channels[0];
  float* right = !source_render && num_channels > 1 ? channels[1] : nullptr;
  const bool mono = right == nullptr;

  const auto target_for = [&](uint32_t source_track_id) noexcept -> float* const* {
    if (source_render) {
      for (size_t index = 1; index < source_output_count; ++index) {
        if (source_outputs[index].source_track_id == source_track_id &&
            source_outputs[index].channels != nullptr) {
          return source_outputs[index].channels;
        }
      }
      return source_outputs[0].channels;
    }
    return nullptr;
  };
  const auto add_output = [&](float* const* target, int sample, float l, float r) noexcept {
    if (target == nullptr) return;
    if (target[0] != nullptr) {
      target[0][sample] += num_channels == 1 ? constants::kInvSqrt2 * (l + r) : l;
    }
    if (num_channels > 1 && target[1] != nullptr) target[1][sample] += r;
    for (int ch = 2; ch < num_channels; ++ch) {
      if (target[ch] != nullptr) target[ch][sample] += constants::kInvSqrt2 * (l + r);
    }
  };

  // Sympathetic resonance is gated by the dampers being lifted on any channel
  // (sustain pedal down). Sustain state is fixed for the block (events are
  // applied before process()).
  bool damper_open = false;
  if (piano_mode_) {
    for (const ChannelState& ch : channels_) {
      if (ch.sustain) {
        damper_open = true;
        break;
      }
    }
  }

  // Swell box: the expression pedal (CC11) sets the shutter. The most-closed
  // pedal across channels darkens the whole division (a bus lowpass). Expression
  // is fixed for the block, so the cutoff is computed once here. Above ~19 kHz
  // the shutter is effectively open, so the one-pole is bypassed (swell_active).
  bool swell_active = false;
  if (pipe_organ_mode_ && swell_depth_ > 0.0f) {
    uint8_t closed = 127;
    for (const ChannelState& ch : channels_) closed = std::min(closed, ch.expression);
    const float shut = (1.0f - static_cast<float>(closed) / 127.0f) * swell_depth_;
    const float fc = std::exp(std::log(20000.0f) + shut * (std::log(300.0f) - std::log(20000.0f)));
    if (fc < 19000.0f) {
      swell_active = true;
      swell_coeff_ = std::clamp(
          1.0f - std::exp(-constants::kTwoPi * fc / static_cast<float>(sample_rate_)), 0.0f, 1.0f);
    }
  }

  for (int i = 0; i < num_samples; ++i) {
    float mix_l = 0.0f;
    float mix_r = 0.0f;
    // Shared wind chest: the tremulant / wind-sag modulation common to every
    // sounding pipe. Demand is the count of active pipe voices (order-
    // independent), so the sag is deterministic across bounces.
    OrganWindSupply::State wind;
    if (pipe_organ_mode_ && wind_.active()) {
      int demand = 0;
      for (const NativeSynthVoice& v : pool_) demand += v.active ? 1 : 0;
      wind = wind_.process(demand);
    }
    for (NativeSynthVoice& v : pool_) {
      if (!v.active) continue;
      const Sf2ChannelMod& mod = channel_mods_[v.channel & 0x0Fu];
      const float s = v.render(mod, wind.pitch_ratio, wind.gain);
      const float voice_l = s * v.gain_left;
      const float voice_r = s * v.gain_right;
      mix_l += voice_l;
      mix_r += voice_r;
      if (source_render) {
        add_output(target_for(v.source_track_id), i, voice_l * config_.gain,
                   voice_r * config_.gain);
      }
    }
    mix_l *= config_.gain;
    mix_r *= config_.gain;
    const float dry_l = mix_l;
    const float dry_r = mix_r;
    // Swell box shutter: a one-pole lowpass on the bus as the louvres close.
    if (swell_active) {
      swell_lp_l_ += swell_coeff_ * (mix_l - swell_lp_l_);
      swell_lp_r_ += swell_coeff_ * (mix_r - swell_lp_r_);
      mix_l = swell_lp_l_;
      mix_r = swell_lp_r_;
    }
    // Shared modal soundboard plus pedal-gated sympathetic resonance, both
    // driven by the summed dry mix and folded back into both legs (centre).
    if (piano_mode_) {
      // Radiation split: the board returns the phase-diffused complement of
      // the direct share (plus the modal colour), so most of the note reaches
      // the mix through the board rather than as the raw string waveform.
      const float dry_mono = 0.5f * (mix_l + mix_r);
      const float body = soundboard_.process(dry_mono);
      const float symp = resonance_.process(soundboard_.last_diffused(), damper_open);
      mix_l = kPianoDirectGain * mix_l + body + symp;
      mix_r = kPianoDirectGain * mix_r + body + symp;
    } else if (sympathetic_active_) {
      // Plucked-string sound halo: the open strings ring behind the note. Held
      // open (no dampers). Skipped entirely for KS patches that did not opt in,
      // so every existing KS voicing renders bit-identically.
      const float dry_mono = 0.5f * (mix_l + mix_r);
      const float symp = resonance_.process(dry_mono, /*damper_open=*/true);
      mix_l += symp;
      mix_r += symp;
    }
    // Gentle gain-neutral bus saturation (glue), then the DC blocker — the
    // physical-model voices can carry a small DC component.
    if (bus_drive_gain_ > 0.0f) {
      mix_l = std::tanh(bus_drive_gain_ * mix_l) / bus_drive_gain_;
      mix_r = std::tanh(bus_drive_gain_ * mix_r) / bus_drive_gain_;
    }
    // Scrub any non-finite bus sample before it reaches the DC blocker: a single
    // NaN/Inf reaching dc_x1_/dc_y1_ would persist in the IIR state and poison
    // every subsequent sample for the whole render. Bit-identical for finite
    // input. Mirrors the host-side scrub in au_instrument_provider.
    if (!std::isfinite(mix_l)) mix_l = 0.0f;
    if (!std::isfinite(mix_r)) mix_r = 0.0f;
    if (config_.dc_block) {
      const float l = mix_l - dc_x1_[0] + dc_r_ * dc_y1_[0];
      dc_x1_[0] = mix_l;
      dc_y1_[0] = l;
      mix_l = l;
      const float r = mix_r - dc_x1_[1] + dc_r_ * dc_y1_[1];
      dc_x1_[1] = mix_r;
      dc_y1_[1] = r;
      mix_r = r;
    }
    if (source_render) {
      // Shared bodies, bus drive and DC filtering are destination-scoped DSP.
      // Keep their residual on the default target so the sum of all source
      // targets remains exactly the legacy destination render.
      add_output(source_outputs[0].channels, i, mix_l - dry_l, mix_r - dry_r);
    } else if (left != nullptr) {
      // Mono host: fold both pan legs so centre-panned voices keep level.
      left[i] += mono ? constants::kInvSqrt2 * (mix_l + mix_r) : mix_l;
    }
    if (right != nullptr) right[i] += mix_r;
    // Fan a mono fold-down to any additional channels.
    for (int ch = 2; ch < num_channels; ++ch) {
      if (channels[ch] != nullptr) {
        channels[ch][i] += constants::kInvSqrt2 * (mix_l + mix_r);
      }
    }
  }
}

}  // namespace sonare::midi::synth
