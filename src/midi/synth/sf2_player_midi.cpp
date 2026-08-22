#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "midi/builtin_synth.h"
#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/sf2_player.h"
#include "midi/ump.h"

namespace sonare::midi::synth {

void Sf2Player::note_on(uint8_t channel, uint8_t note, uint8_t velocity,
                        uint32_t source_track_id) noexcept {
  if (!prepared_) return;
  const ChannelState& ch = channels_[channel & 0x0Fu];
  const uint16_t bank = effective_bank(channel);
  const bool is_drum = bank == kDrumBank;
  if (config_.synth_fallback && config_.prefer_model_for_modeled_families && !is_drum &&
      gm_program_has_dedicated_model(bank, ch.program)) {
    fallback_note_on(channel, note, velocity, source_track_id);
    return;
  }
  // No SoundFont / uncovered program -> the data-free synth floor.
  const int preset_idx = soundfont_ != nullptr ? resolve_preset(bank, ch.program) : -1;
  if (preset_idx < 0) {
    if (config_.synth_fallback) fallback_note_on(channel, note, velocity, source_track_id);
    return;
  }
  const Sf2Preset& preset = soundfont_->presets()[static_cast<size_t>(preset_idx)];
  const auto& instruments = soundfont_->instruments();
  const float* pool_data = soundfont_->sample_pool().data();
  const float vel_gain = sf2_velocity_gain(velocity);

  const Sf2Zone* preset_global =
      !preset.zones.empty() && preset.zones[0].is_global() ? &preset.zones[0] : nullptr;

  // SoundFont 2.04 section 8.1.2 scopes exclusiveClass to notes that are ALREADY
  // sounding, so the layers this one note-on allocates must not choke each
  // other — a stereo hi-hat's two legs, or a layered kit piece, share one class
  // by design. Voice ages are monotonic, so every voice allocated below carries
  // an age at or above this mark and is excluded from the choke.
  const uint64_t age_before_note_on = pool_.next_age();

  bool has_renderable_zone = false;
  for (const Sf2Zone& pzone : preset.zones) {
    if (pzone.is_global() || !pzone.matches(note, velocity)) continue;
    if (pzone.instrument < 0 || static_cast<size_t>(pzone.instrument) >= instruments.size()) {
      continue;
    }
    const Sf2Instrument& inst = instruments[static_cast<size_t>(pzone.instrument)];
    const Sf2Zone* inst_global =
        !inst.zones.empty() && inst.zones[0].is_global() ? &inst.zones[0] : nullptr;
    for (const Sf2Zone& izone : inst.zones) {
      if (izone.is_global() || !izone.matches(note, velocity)) continue;
      if (izone.sample < 0 || static_cast<size_t>(izone.sample) >= soundfont_->samples().size()) {
        continue;
      }
      const Sf2Sample& sample = soundfont_->samples()[static_cast<size_t>(izone.sample)];
      if (sample.is_rom() || !valid_sf2_sample_rate(sample.sample_rate) ||
          sample.end <= sample.start || sample.end > soundfont_->sample_pool().size()) {
        continue;
      }

      // Stack generators: defaults -> instrument global -> instrument zone
      // (absolute), then + preset global + preset zone (relative).
      Sf2GenSet gens;
      if (inst_global != nullptr) gens.apply_absolute(*inst_global);
      gens.apply_absolute(izone);
      if (preset_global != nullptr) gens.add_relative(*preset_global);
      gens.add_relative(pzone);

      Sf2VoiceParams params = resolve_voice_params(gens, sample, note, velocity, sample_rate_);
      if (params.end <= params.start || params.end > soundfont_->sample_pool().size() ||
          !std::isfinite(params.pitch_increment) || params.pitch_increment <= 0.0) {
        continue;
      }
      has_renderable_zone = true;

      // GS layer: NRPN part edits + per-note drum-kit overrides.
      apply_gs_part_params(params, ch.gs);
      if (is_drum) {
        apply_gs_drum_params(params, drum_params_[channel & 0x0Fu][note & 0x7Fu]);
      }

      // Exclusive class: choke same-class voices on this channel (hi-hats).
      if (params.exclusive_class != 0) {
        for (Sf2Voice& v : pool_) {
          if (v.active && v.age < age_before_note_on && v.channel == (channel & 0x0Fu) &&
              v.params.exclusive_class == params.exclusive_class) {
            v.release();
          }
        }
      }

      Sf2Voice* voice = pool_.allocate(channel & 0x0Fu, note, source_track_id);
      if (voice == nullptr) continue;
      voice->start(pool_data, params, sample_rate_, vel_gain);
    }
  }
  if (!has_renderable_zone && config_.synth_fallback) {
    fallback_note_on(channel, note, velocity, source_track_id);
  }
}

void Sf2Player::fallback_note_on(uint8_t channel, uint8_t note, uint8_t velocity,
                                 uint32_t source_track_id) noexcept {
  const ChannelState& ch = channels_[channel & 0x0Fu];
  const uint16_t bank = effective_bank(channel);
  const GsToneMap tone_map = gs_effective_tone_map(ch.bank_msb, ch.bank_lsb);
  const bool is_drum = bank == kDrumBank;
  const NativeSynthPatch& patch =
      is_drum ? gm_fallback_drum_patch(note) : gm_fallback_patch(bank, ch.program, tone_map);
  // GM kit exclusive/mute groups (hi-hats etc.): choke the ringing group voice
  // on this channel before allocating the new strike.
  if (is_drum && patch.percussion.exclusive_class != 0) {
    const uint8_t excl = patch.percussion.exclusive_class;
    for (NativeSynthVoice& v : fallback_pool_) {
      if (v.active && v.channel == (channel & 0x0Fu) && v.patch != nullptr &&
          v.patch->mode == SynthEngineMode::kPercussion &&
          v.patch->percussion.exclusive_class == excl) {
        v.choke();
      }
    }
  }
  NativeSynthVoice* voice = fallback_pool_.allocate(channel & 0x0Fu, note, source_track_id);
  if (voice == nullptr) return;
  const uint32_t voice_index = static_cast<uint32_t>(voice - fallback_pool_.data());
  // KS patches get their delay span before start() (pointer wiring only).
  if (!fallback_ks_buffers_.empty()) {
    voice->ks.attach(
        fallback_ks_buffers_.data() + static_cast<size_t>(voice_index) * 3 * fallback_ks_capacity_,
        fallback_ks_capacity_);
  }
  if (!fallback_piano_buffers_.empty()) {
    voice->piano.attach(fallback_piano_buffers_.data() + static_cast<size_t>(voice_index) *
                                                             kMaxPianoStrings *
                                                             fallback_piano_string_capacity_,
                        fallback_piano_string_capacity_);
  }
  if (!fallback_pipe_organ_buffers_.empty()) {
    // Each rank holds TWO spans (bore + jet), so the per-voice stride is
    // 2 * kMaxPipeRanks spans; without the 2, adjacent voices' slabs overlap
    // and simultaneous (legato) organ voices corrupt each other's bores.
    voice->pipe_organ.attach(
        fallback_pipe_organ_buffers_.data() +
            static_cast<size_t>(voice_index) * 2 * kMaxPipeRanks * fallback_pipe_organ_capacity_,
        fallback_pipe_organ_capacity_);
  }
  if (!fallback_bowed_buffers_.empty()) {
    voice->bowed_string.attach(fallback_bowed_buffers_.data() +
                                   static_cast<size_t>(voice_index) * 3 * fallback_bowed_capacity_,
                               fallback_bowed_capacity_);
  }
  if (!fallback_reed_buffers_.empty()) {
    voice->reed.attach(
        fallback_reed_buffers_.data() + static_cast<size_t>(voice_index) * fallback_reed_capacity_,
        fallback_reed_capacity_);
  }
  if (!fallback_brass_buffers_.empty()) {
    voice->brass.attach(fallback_brass_buffers_.data() +
                            static_cast<size_t>(voice_index) * fallback_brass_capacity_,
                        fallback_brass_capacity_);
  }
  if (!fallback_flute_buffers_.empty()) {
    voice->flute.attach(fallback_flute_buffers_.data() +
                            static_cast<size_t>(voice_index) * 2 * fallback_flute_capacity_,
                        fallback_flute_capacity_);
  }
  if (!fallback_plucked_string_buffers_.empty()) {
    voice->plucked_string.attach(
        fallback_plucked_string_buffers_.data() +
            static_cast<size_t>(voice_index) * fallback_plucked_string_capacity_,
        fallback_plucked_string_capacity_);
  }
  // GS drum-kit variation: the drum channel's program picks the kit (Room /
  // Power / TR-808 / ...); melodic fallback voices pass 0 (no kit).
  const uint8_t drum_kit = is_drum ? gm_fallback_drum_kit(ch.program, tone_map) : 0;
  // GS per-note drum NRPN edits (pitch coarse / TVA level / absolute pan),
  // mirroring apply_gs_drum_params for the model floor (reverb/chorus sends stay
  // on the SF2 path).
  DrumVoiceMod drum_mod;
  if (is_drum) {
    const GsDrumNoteParams& gd = drum_params_[channel & 0x0Fu][note & 0x7Fu];
    if ((gd.flags & GsDrumNoteParams::kPitch) != 0 && gd.pitch_coarse != 0) {
      drum_mod.pitch_ratio = std::exp2(static_cast<float>(gd.pitch_coarse) / 12.0f);
    }
    if ((gd.flags & GsDrumNoteParams::kLevel) != 0) {
      const float v = static_cast<float>(gd.level & 0x7Fu) / 127.0f;
      drum_mod.level_gain = v * v;  // same square law as CC7 / velocity
    }
    if ((gd.flags & GsDrumNoteParams::kPan) != 0) {
      drum_mod.pan_units = (static_cast<float>(gd.pan & 0x7Fu) - 64.0f) / 63.0f * 500.0f;
    }
  }
  voice->start(patch, sample_rate_, velocity, voice_index, 0.0f, ch.una_corda, drum_kit, drum_mod);

  // Pipe-organ patches share a per-part wind chest (tremulant / wind sag).
  // Re-prepare only when the parameters change so the tremulant phase stays
  // continuous across notes.
  const uint8_t part = channel & 0x0Fu;
  if (patch.mode == SynthEngineMode::kPipeOrgan) {
    FallbackWindParams& wp = fallback_wind_params_[part];
    const float rate = patch.pipe_organ.tremulant_rate_hz;
    const float depth = patch.pipe_organ.tremulant_depth;
    const float sag = patch.pipe_organ.wind_sag;
    if (wp.rate != rate || wp.depth != depth || wp.sag != sag) {
      wp = {rate, depth, sag};
      fallback_wind_[part].prepare(sample_rate_, rate, depth, sag);
    }
  }

  // Bus-level body resonators (the components the NativeSynth host folds in):
  // the piano's modal soundboard + pedal-gated sympathetic bank, and the
  // plucked-string open-string halo. Re-prepared only when the part's patch
  // kind or soundboard mix changes.
  FallbackBodyState& body = fallback_body_[part];
  if (patch.mode == SynthEngineMode::kPiano) {
    if (body.kind != FallbackBodyKind::kPiano || body.soundboard_mix != patch.piano.soundboard) {
      body.kind = FallbackBodyKind::kPiano;
      body.soundboard_mix = patch.piano.soundboard;
      fallback_board_[part].prepare(sample_rate_, patch.piano.soundboard);
      fallback_reso_[part].prepare(sample_rate_);
    }
  } else if (patch.mode == SynthEngineMode::kKarplusStrong && patch.ks.sympathetic) {
    if (body.kind != FallbackBodyKind::kGuitarHalo) {
      body.kind = FallbackBodyKind::kGuitarHalo;
      body.soundboard_mix = -1.0f;
      fallback_reso_[part].prepare_guitar_sympathetic(sample_rate_);
    }
  } else if (body.kind != FallbackBodyKind::kNone && !is_drum) {
    // The part moved to a program with no body resonator.
    body.kind = FallbackBodyKind::kNone;
    body.soundboard_mix = -1.0f;
    body.ringout = 0;
  }
}

void Sf2Player::note_off(uint8_t channel, uint8_t note, uint32_t source_track_id) noexcept {
  if (!prepared_) return;
  const uint8_t ch = channel & 0x0Fu;
  const ChannelState& st = channels_[ch];
  for (Sf2Voice& v : pool_) {
    if (v.active && v.note == note && v.channel == ch && v.source_track_id == source_track_id &&
        v.key_down) {
      v.key_down = false;
      // A sostenuto capture holds the note regardless of the sustain pedal.
      if (v.sostenuto) continue;
      if (!st.sustain) v.release();
    }
  }
  for (NativeSynthVoice& v : fallback_pool_) {
    if (v.active && v.note == note && v.channel == ch && v.source_track_id == source_track_id &&
        v.key_down) {
      v.key_down = false;
      if (v.sostenuto) continue;
      if (!st.sustain) {
        v.release();
      } else if (st.sustain_level < 127 && v.patch != nullptr &&
                 v.patch->mode == SynthEngineMode::kPiano) {
        // Half-pedal: the partially raised damper rests on the string.
        v.piano.damp(static_cast<float>(127 - st.sustain_level) / 63.0f);
      }
    }
  }
}

void Sf2Player::sustain_pedal(uint8_t channel, bool down) noexcept {
  sustain_cc(channel, down ? 127 : 0);
}

void Sf2Player::sustain_cc(uint8_t channel, uint8_t value) noexcept {
  if (!prepared_) return;
  const uint8_t ch = channel & 0x0Fu;
  ChannelState& st = channels_[ch];
  const bool was_down = st.sustain;
  st.sustain_level = value;
  st.sustain = value >= 64;
  if (st.sustain) {
    // Half-pedal: a partially raised damper still rests on the piano strings,
    // so held-but-released fallback notes are damped rather than ringing
    // freely. Key-down and sostenuto-captured notes keep their dampers off.
    if (value < 127) {
      const float strength = static_cast<float>(127 - value) / 63.0f;
      for (NativeSynthVoice& v : fallback_pool_) {
        if (v.active && v.channel == ch && !v.key_down && !v.sostenuto && v.patch != nullptr &&
            v.patch->mode == SynthEngineMode::kPiano) {
          v.piano.damp(strength);
        }
      }
    }
    return;
  }
  if (!was_down) return;
  // Pedal up: the dampers fall on every held-but-released note. A
  // sostenuto-captured note stays held even when the sustain pedal lifts.
  for (Sf2Voice& v : pool_) {
    if (v.active && v.channel == ch && !v.key_down && !v.releasing && !v.sostenuto) v.release();
  }
  for (NativeSynthVoice& v : fallback_pool_) {
    if (v.active && v.channel == ch && !v.key_down && !v.releasing && !v.sostenuto) v.release();
  }
}

void Sf2Player::sostenuto_pedal(uint8_t channel, bool down) noexcept {
  if (!prepared_) return;
  const uint8_t ch = channel & 0x0Fu;
  ChannelState& st = channels_[ch];
  if (st.sostenuto_down == down) return;
  st.sostenuto_down = down;
  if (down) {
    // Capture exactly the keys held at the down edge.
    for (Sf2Voice& v : pool_) {
      if (v.active && v.channel == ch && v.key_down) v.sostenuto = true;
    }
    for (NativeSynthVoice& v : fallback_pool_) {
      if (v.active && v.channel == ch && v.key_down) v.sostenuto = true;
    }
    return;
  }
  for (Sf2Voice& v : pool_) {
    if (v.active && v.channel == ch && v.sostenuto) {
      v.sostenuto = false;
      if (!v.key_down && !v.releasing && !st.sustain) v.release();
    }
  }
  for (NativeSynthVoice& v : fallback_pool_) {
    if (v.active && v.channel == ch && v.sostenuto) {
      v.sostenuto = false;
      if (!v.key_down && !v.releasing && !st.sustain) v.release();
    }
  }
}

void Sf2Player::all_notes_off(uint8_t channel) noexcept {
  if (!prepared_) return;
  const uint8_t ch = channel & 0x0Fu;
  channels_[ch].sustain = false;
  for (Sf2Voice& v : pool_) {
    if (v.active && v.channel == ch && !v.releasing) {
      v.key_down = false;
      v.release();
    }
  }
  for (NativeSynthVoice& v : fallback_pool_) {
    if (v.active && v.channel == ch && !v.releasing) {
      v.key_down = false;
      v.release();
    }
  }
}

void Sf2Player::all_sound_off(uint8_t channel) noexcept {
  if (!prepared_) return;
  const uint8_t ch = channel & 0x0Fu;
  channels_[ch].sustain = false;
  for (Sf2Voice& v : pool_) {
    if (v.active && v.channel == ch) {
      v.env.kill();
      v.active = false;
      v.releasing = false;
    }
  }
  for (NativeSynthVoice& v : fallback_pool_) {
    if (v.active && v.channel == ch) v.kill();
  }
}

void Sf2Player::apply_nrpn(uint8_t channel, uint8_t value) noexcept {
  const uint8_t ch = channel & 0x0Fu;
  ChannelState& st = channels_[ch];
  const int8_t offset = static_cast<int8_t>(static_cast<int>(value & 0x7Fu) - 64);
  if (st.params.nrpn_msb == 0x01) {
    // GS part parameters (relative offsets onto the SoundFont generators).
    switch (st.params.nrpn_lsb) {
      case 0x08:
        st.gs.vibrato_rate = offset;
        break;
      case 0x09:
        st.gs.vibrato_depth = offset;
        break;
      case 0x0A:
        st.gs.vibrato_delay = offset;
        break;
      case 0x20:
        st.gs.tvf_cutoff = offset;
        break;
      case 0x21:
        st.gs.tvf_resonance = offset;
        break;
      case 0x63:
        st.gs.eg_attack = offset;
        break;
      case 0x64:
        st.gs.eg_decay = offset;
        break;
      case 0x66:
        st.gs.eg_release = offset;
        break;
      default:
        break;
    }
    return;
  }
  const bool is_drum = effective_bank(ch) == kDrumBank;
  if (!is_drum) return;
  // GS drum-kit NRPNs: msb selects the parameter, lsb is the drum note.
  GsDrumNoteParams& d = drum_params_[ch][st.params.nrpn_lsb & 0x7Fu];
  switch (st.params.nrpn_msb) {
    case 0x18:
      d.pitch_coarse = offset;
      d.flags |= GsDrumNoteParams::kPitch;
      break;
    case 0x1A:
      d.level = value;
      d.flags |= GsDrumNoteParams::kLevel;
      break;
    case 0x1C:
      d.pan = value;
      d.flags |= GsDrumNoteParams::kPan;
      break;
    case 0x1D:
      d.reverb = value;
      d.flags |= GsDrumNoteParams::kReverb;
      break;
    case 0x1E:
      d.chorus = value;
      d.flags |= GsDrumNoteParams::kChorus;
      break;
    default:
      break;
  }
}

void Sf2Player::reset_controllers(uint8_t channel) noexcept {
  // MIDI RP-015: reset performance controllers, keep program/bank/volume/pan.
  const uint8_t ch = channel & 0x0Fu;
  ChannelState& st = channels_[ch];
  st.mod_wheel = 0;
  st.expression = 127;
  st.pitch_bend = 8192;
  st.params.reset();
  sustain_cc(ch, 0);
  sostenuto_pedal(ch, false);
  st.una_corda = false;
  refresh_channel_mod(ch);
}

void Sf2Player::control_change(uint8_t channel, uint8_t controller, uint8_t value) noexcept {
  const uint8_t ch = channel & 0x0Fu;
  ChannelState& st = channels_[ch];
  switch (controller) {
    case 0:  // Bank select MSB (GS variation bank)
      st.bank_msb = value;
      // The fallback ambience floor is keyed on (effective bank, program), so
      // a bank change moves it just like a program change does.
      refresh_channel_mod(ch);
      break;
    case 32:  // Bank select LSB
      st.bank_lsb = value;
      refresh_channel_mod(ch);
      break;
    case 1:
      st.mod_wheel = value;
      refresh_channel_mod(ch);
      break;
    case 6:  // Data entry MSB -> active RPN (bend range) or GS NRPN
      if (st.params.selected_rpn(0, 0)) {
        st.bend_range_cents = 100.0f * static_cast<float>(value);
        refresh_channel_mod(ch);
      } else if (st.params.selected_nrpn()) {
        apply_nrpn(ch, value);
      }
      break;
    case 38:  // Data entry LSB -> bend range cents
      if (st.params.selected_rpn(0, 0)) {
        st.bend_range_cents =
            100.0f * std::floor(st.bend_range_cents / 100.0f) + static_cast<float>(value);
        refresh_channel_mod(ch);
      }
      break;
    case 7:
      st.volume = value;
      refresh_channel_mod(ch);
      break;
    case 10:
      st.pan = value;
      refresh_channel_mod(ch);
      break;
    case 11:
      st.expression = value;
      refresh_channel_mod(ch);
      break;
    case 91:
      st.reverb_send = value;
      refresh_channel_mod(ch);
      break;
    case 93:
      st.chorus_send = value;
      refresh_channel_mod(ch);
      break;
    case 94:  // GS delay send (no SF2 generator; channel-level only)
      st.delay_send = value;
      refresh_channel_mod(ch);
      break;
    case 98:  // NRPN LSB
      st.params.select_nrpn_lsb(value);
      break;
    case 99:  // NRPN MSB
      st.params.select_nrpn_msb(value);
      break;
    case 100:  // RPN LSB
      st.params.select_rpn_lsb(value);
      break;
    case 101:  // RPN MSB
      st.params.select_rpn_msb(value);
      break;
    case 64:
      sustain_cc(ch, value);
      break;
    case 66:
      sostenuto_pedal(ch, value >= 64);
      break;
    case 67:
      // Una corda: shifts the piano fallback action for notes STARTED while
      // down (the hammer strikes fewer strings); sample-playback voices keep
      // their recorded voicing.
      st.una_corda = value >= 64;
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

void Sf2Player::on_event(uint32_t /*destination_id*/, const MidiEvent& event) noexcept {
  if (!prepared_) return;
  const Ump& u = event.ump;
  if (u.message_type() != UmpMessageType::kMidi1ChannelVoice &&
      u.message_type() != UmpMessageType::kMidi2ChannelVoice) {
    // SysEx events arrive with a control-thread-resolved payload view (the UMP
    // itself only carries a handle): feed the GS layer so GS Reset / GM System
    // On / "use for rhythm part" inside an arrangement take effect.
    if (event.sysex_payload != nullptr && event.sysex_payload_size > 0) {
      handle_sysex(event.sysex_payload, event.sysex_payload_size);
    }
    return;
  }
  if (u.is_note_on()) {
    const uint8_t vel7 =
        u.message_type() == UmpMessageType::kMidi1ChannelVoice
            ? u.data2_7bit()
            : scale_note_on_velocity_16_to_7(static_cast<uint16_t>(u.words[1] >> 16));
    note_on(u.channel(), u.note_number(), vel7, event.source_track_id);
  } else if (u.is_note_off()) {
    note_off(u.channel(), u.note_number(), event.source_track_id);
  } else if (u.status_nibble() == static_cast<uint8_t>(UmpStatus::kProgramChange)) {
    const uint8_t ch = u.channel() & 0x0Fu;
    if (u.message_type() == UmpMessageType::kMidi2ChannelVoice) {
      channels_[ch].program = static_cast<uint8_t>((u.words[1] >> 24) & 0x7Fu);
      if ((u.words[0] & 0x01u) != 0) {
        channels_[ch].bank_msb = static_cast<uint8_t>((u.words[1] >> 8) & 0x7Fu);
        channels_[ch].bank_lsb = static_cast<uint8_t>(u.words[1] & 0x7Fu);
      }
    } else {
      channels_[ch].program = u.note_number();
    }
    // The fallback ambience floor is program-keyed; keep the mod snapshot
    // in step with the new program.
    refresh_channel_mod(ch);
  } else if (u.status_nibble() == static_cast<uint8_t>(UmpStatus::kPitchBend)) {
    const uint8_t ch = u.channel() & 0x0Fu;
    if (u.message_type() == UmpMessageType::kMidi1ChannelVoice) {
      // MIDI 1.0: 14-bit value, LSB in data1 (bits 8..14), MSB in data2.
      channels_[ch].pitch_bend =
          static_cast<uint16_t>((static_cast<uint16_t>(u.data2_7bit()) << 7) | u.note_number());
    } else {
      // MIDI 2.0: 32-bit value in word[1]; keep the top 14 bits.
      channels_[ch].pitch_bend = static_cast<uint16_t>(u.words[1] >> 18);
    }
    refresh_channel_mod(ch);
  } else if (u.status_nibble() == static_cast<uint8_t>(UmpStatus::kControlChange)) {
    const uint8_t controller = u.note_number();
    const uint8_t value7 = u.message_type() == UmpMessageType::kMidi1ChannelVoice
                               ? u.data2_7bit()
                               : scale_cc_32_to_7(u.words[1]);
    control_change(u.channel(), controller, value7);
  }
}

}  // namespace sonare::midi::synth
