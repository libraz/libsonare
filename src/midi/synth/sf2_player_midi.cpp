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
#include "midi/synth/gs_layer.h"
#include "midi/synth/sf2_player.h"
#include "midi/ump.h"

namespace sonare::midi::synth {

namespace {

using ::sonare::constants::kCentsPerSemitone;

/// Longest CC5 portamento glide, matching the ceiling clamp_synth_patch puts on
/// a patch's own glide_ms.
constexpr float kPortamentoMaxMs = 5000.0f;

/// CC5 Portamento Time -> glide time in ms. GS fixes neither a unit nor a curve
/// for this controller, so the mapping is chosen monotone and zero at 0 (the
/// power-on value, i.e. no glide) with the useful few-hundred-ms range spread
/// over the lower half of the controller.
float portamento_time_ms(uint8_t value) noexcept {
  const float v = static_cast<float>(value & 0x7Fu) / 127.0f;
  return kPortamentoMaxMs * v * v;
}

/// RPN Null (7F 7F): leaves nothing selected, so later data entry is discarded.
/// Selecting an RPN already dropped a selected NRPN — this makes the neutral
/// state explicit rather than an RPN number nothing happens to answer.
void deselect_on_rpn_null(ChannelParamState& params) noexcept {
  if (params.rpn_msb == 0x7Fu && params.rpn_lsb == 0x7Fu) params.reset();
}

}  // namespace

Sf2Player::Portamento Sf2Player::take_portamento(uint8_t channel, uint8_t note) noexcept {
  ChannelState& st = channels_[channel & 0x0Fu];
  const uint8_t key = note & 0x7Fu;
  // CC84 names its own source note and outranks the previous key; CC65 glides
  // from whatever this part played last. Either way the arming is spent here.
  int source = -1;
  if (st.portamento_armed) {
    source = st.portamento_source;
    st.portamento_armed = false;
  } else if (st.portamento && st.last_note <= 127) {
    source = st.last_note;
  }
  st.last_note = key;
  Portamento porta;
  if (source < 0 || source == key) return porta;
  const float glide_ms = portamento_time_ms(st.portamento_time);
  if (glide_ms <= 0.0f || sample_rate_ <= 0.0) return porta;
  porta.cents = static_cast<float>(source - key) * kCentsPerSemitone;
  // Same one-pole sizing as the NativeSynth voice's glide: the pitch lands
  // within ~5% of the target in glide_ms.
  porta.coeff =
      static_cast<float>(std::exp(-3.0 / (static_cast<double>(glide_ms) * 0.001 * sample_rate_)));
  return porta;
}

void Sf2Player::choke_part(uint8_t channel, int note) noexcept {
  const uint8_t part = channel & 0x0Fu;
  // Both pools: a program change moves a part between the SoundFont and the
  // fallback bank, so the note still sounding can be in either one.
  for (Sf2Voice& v : pool_) {
    if (v.active && v.channel == part && (note < 0 || v.note == note)) v.choke(sample_rate_);
  }
  for (NativeSynthVoice& v : fallback_pool_) {
    if (v.active && v.channel == part && (note < 0 || v.note == note)) v.choke_fast(sample_rate_);
  }
}

const GsUserDrumSource* Sf2Player::user_drum_source(const ChannelState& ch, bool is_drum,
                                                    uint8_t note) const noexcept {
  if (!is_drum) return nullptr;
  const int set = ch.user_drum_set();
  if (set < 0) return nullptr;
  return &user_drum_sources_[static_cast<size_t>(set)][note & 0x7Fu];
}

GsDrumNoteParams Sf2Player::drum_note_params(const ChannelState& ch, bool is_drum,
                                             uint8_t note) const noexcept {
  if (!is_drum) return {};
  const GsDrumNoteParams& live = drum_params_[ch.drum_map_slot()][note & 0x7Fu];
  const int set = ch.user_drum_set();
  if (set < 0) return live;
  return gs_layer_drum_note_params(user_drum_params_[static_cast<size_t>(set)][note & 0x7Fu], live);
}

void Sf2Player::note_on(uint8_t channel, uint8_t note, uint8_t velocity,
                        uint32_t source_track_id) noexcept {
  if (!prepared_) return;
  const ChannelState& ch = channels_[channel & 0x0Fu];
  // GS KEY RANGE (40 1x 1D/1E): a key the part does not receive is not a silent
  // note. It takes no voice, chokes nothing and does not spend the armed
  // portamento, so the test precedes all three as well as both voice banks.
  if (!ch.receives_key(note)) return;
  const Portamento porta = take_portamento(channel, note);
  // Mono already stops everything the part is sounding, so it subsumes SINGLE.
  if (ch.mono_poly == kGsMonoPolyMono && !ch.is_drum()) {
    choke_part(channel, -1);
  } else if (ch.assign_mode == kGsAssignModeSingle) {
    choke_part(channel, note & 0x7F);
  }
  const uint16_t bank = effective_bank(channel);
  const bool is_drum = bank == kDrumBank;
  const float scale_cents = gs_scale_tuning_cents(ch.scale_tuning, note);
  const GsDrumNoteParams gd = drum_note_params(ch, is_drum, note);
  // GS RX NOTE ON (41 m8 rr / 21 d8 rr): a note the kit has switched off is not
  // sounded at all, so this precedes every choice of bank below — a note refused
  // here must not reach the model floor either.
  if ((gd.flags & GsDrumNoteParams::kRxNoteOn) != 0 && gd.rx_note_on == 0) return;
  if (config_.synth_fallback && config_.prefer_model_for_modeled_families && !is_drum &&
      gm_program_has_dedicated_model(bank, ch.program)) {
    fallback_note_on(channel, note, velocity, source_track_id, porta);
    return;
  }
  // GS user drum set (21 dn rr): rhythm programs 64 and 65 play a kit the file
  // built note by note, so both the kit this strike sounds and the note within
  // it come from the set rather than from the part and the key.
  const GsUserDrumSource* us = user_drum_source(ch, is_drum, note);
  const uint8_t kit_program = us != nullptr ? us->program : ch.program;
  // No SoundFont / uncovered program -> the data-free synth floor.
  const int preset_idx = soundfont_ != nullptr ? resolve_preset(bank, kit_program) : -1;
  if (preset_idx < 0) {
    if (config_.synth_fallback) fallback_note_on(channel, note, velocity, source_track_id, porta);
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

  // GS PLAY NOTE NUMBER (41 m1 rr): the note whose SOUND this strike plays.
  // Zone selection and the resolved params follow it; the per-note slab, the
  // choke and the voice's own note stay on the struck note. The map's edit sits
  // ON TOP of the user set's source note, which is the stored kit it edits.
  uint8_t sound_note = gs_user_drum_sound_note(us, note);
  if ((gd.flags & GsDrumNoteParams::kPlayNote) != 0) sound_note = gd.play_note;

  bool has_renderable_zone = false;
  for (const Sf2Zone& pzone : preset.zones) {
    if (pzone.is_global() || !pzone.matches(sound_note, velocity)) continue;
    if (pzone.instrument < 0 || static_cast<size_t>(pzone.instrument) >= instruments.size()) {
      continue;
    }
    const Sf2Instrument& inst = instruments[static_cast<size_t>(pzone.instrument)];
    const Sf2Zone* inst_global =
        !inst.zones.empty() && inst.zones[0].is_global() ? &inst.zones[0] : nullptr;
    for (const Sf2Zone& izone : inst.zones) {
      if (izone.is_global() || !izone.matches(sound_note, velocity)) continue;
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

      Sf2VoiceParams params =
          resolve_voice_params(gens, sample, sound_note, velocity, sample_rate_);
      if (params.end <= params.start || params.end > soundfont_->sample_pool().size() ||
          !std::isfinite(params.pitch_increment) || params.pitch_increment <= 0.0) {
        continue;
      }
      has_renderable_zone = true;

      // GS layer: NRPN part edits + per-note drum-kit overrides.
      apply_gs_part_params(params, ch.gs);
      apply_gs_drum_params(params, gd);
      // SCALE TUNING is the struck key's, not the sounding note's: it is a
      // temperament of the keyboard, so a kit piece PLAY NOTE NUMBER redirected
      // to keeps the tuning of the key that asked for it.
      if (scale_cents != 0.0f) {
        params.pitch_increment *= std::exp2(static_cast<double>(scale_cents) / 1200.0);
      }
      // A TVF CUTOFF CONTROL destination engages the filter the way a TONE
      // MODIFY cutoff does, and on the part rather than on the controller: a
      // controller rises after the note-on as often as before it, and a
      // bypassed filter cannot open. Either source is enough on its own.
      if (ch.mod_cutoff_cents != 0.0f || ch.caf_cutoff_cents != 0.0f) {
        params.filter_bypass = false;
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
      voice->glide_cents = porta.cents;
      voice->glide_coeff = porta.coeff;
    }
  }
  if (!has_renderable_zone && config_.synth_fallback) {
    fallback_note_on(channel, note, velocity, source_track_id, porta);
  }
}

void Sf2Player::fallback_note_on(uint8_t channel, uint8_t note, uint8_t velocity,
                                 uint32_t source_track_id, Portamento porta) noexcept {
  const ChannelState& ch = channels_[channel & 0x0Fu];
  const uint16_t bank = effective_bank(channel);
  const GsToneMap tone_map = gs_effective_tone_map(ch.bank_msb, ch.bank_lsb);
  const bool is_drum = bank == kDrumBank;
  // The per-note GS edits, read before the patch: PLAY NOTE NUMBER picks which
  // kit piece answers and ASSIGN GROUP which group it belongs to, and both are
  // needed before the choke below, let alone the voice.
  const GsDrumNoteParams gd = drum_note_params(ch, is_drum, note);
  // The user drum set under them: it says which stored kit and which note within
  // it this strike sounds, and the map's edits above are what edits that.
  const GsUserDrumSource* us = user_drum_source(ch, is_drum, note);
  uint8_t sound_note = gs_user_drum_sound_note(us, note);
  if ((gd.flags & GsDrumNoteParams::kPlayNote) != 0) sound_note = gd.play_note;
  const NativeSynthPatch& patch =
      is_drum ? gm_fallback_drum_patch(sound_note) : gm_fallback_patch(bank, ch.program, tone_map);
  uint8_t exclusive_class = is_drum ? patch.percussion.exclusive_class : 0;
  if ((gd.flags & GsDrumNoteParams::kAssignGroup) != 0) {
    exclusive_class = gd.assign_group & 0x7Fu;
  }
  // GM kit exclusive/mute groups (hi-hats etc.): choke the ringing group voice
  // on this channel before allocating the new strike. Compared against the
  // group each voice was STARTED in, which an ASSIGN GROUP write moves away
  // from the kit piece's own.
  if (exclusive_class != 0) {
    for (NativeSynthVoice& v : fallback_pool_) {
      if (v.active && v.channel == (channel & 0x0Fu) && v.patch != nullptr &&
          v.patch->mode == SynthEngineMode::kPercussion && v.exclusive_class == exclusive_class) {
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
  if (!fallback_harpsichord_buffers_.empty()) {
    voice->harpsichord.attach(fallback_harpsichord_buffers_.data() +
                                  static_cast<size_t>(voice_index) * fallback_harpsichord_stride_,
                              fallback_harpsichord_capacity_);
  }
  // GS drum-kit variation: the drum channel's program picks the kit (Room /
  // Power / TR-808 / ...); melodic fallback voices pass 0 (no kit).
  // A user drum set names the kit per note, so the program that picks the
  // variation is the note's source rather than the part's.
  const uint8_t kit_program = us != nullptr ? us->program : ch.program;
  const uint8_t drum_kit = is_drum ? gm_fallback_drum_kit(kit_program, tone_map) : 0;
  // GS per-note drum edits (pitch coarse / TVA level / absolute pan / the three
  // send multiplicands), mirroring apply_gs_drum_params for the model floor: a
  // parameter must not do something different because this bank answered.
  DrumVoiceMod drum_mod;
  if (is_drum) {
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
    if ((gd.flags & GsDrumNoteParams::kReverb) != 0) {
      drum_mod.reverb_scale = static_cast<float>(gd.reverb & 0x7Fu) / 127.0f;
    }
    if ((gd.flags & GsDrumNoteParams::kChorus) != 0) {
      drum_mod.chorus_scale = static_cast<float>(gd.chorus & 0x7Fu) / 127.0f;
    }
    if ((gd.flags & GsDrumNoteParams::kDelay) != 0) {
      drum_mod.delay_scale = static_cast<float>(gd.delay & 0x7Fu) / 127.0f;
    }
    // Resolved above, beside the choke that had to read it first.
    drum_mod.exclusive_class = static_cast<int16_t>(exclusive_class);
  }
  // The note the voice is STARTED at. Both PLAY NOTE NUMBER and a user set's
  // source note move it, and the patch above is already chosen by it, so it is
  // carried from the resolved note rather than from either of them: leaving it
  // on the struck note gives the right kit piece at the wrong pitch, which
  // renders plausibly and is not the note that was asked for.
  if (sound_note != (note & 0x7Fu)) {
    drum_mod.play_note = static_cast<int16_t>(sound_note);
  }
  // Drawbar percussion spends the channel's charge; note_off recharges it once
  // the last key is up.
  const bool organ_percussion = patch.mode == SynthEngineMode::kAdditive &&
                                patch.additive.percussion_harmonic >= 2 && ch.percussion_armed;
  if (organ_percussion) channels_[channel & 0x0Fu].percussion_armed = false;
  // GS melodic part edits (40 1x 30 TONE MODIFY and the part NRPNs), through
  // the same conversion the SoundFont bank's apply_gs_part_params uses: a
  // parameter must not do something different because this bank answered.
  GsPartMod part_mod = gs_part_mod(ch.gs);
  // SCALE TUNING is per note where the other eight are per part, so it is set
  // on the way past rather than built with them; the struck key indexes it, as
  // it does on the SoundFont bank.
  part_mod.pitch_cents = gs_scale_tuning_cents(ch.scale_tuning, note);
  // Same reason the SoundFont bank engages its filter here: the offset itself
  // arrives per sample from the controller, so what the note-on has to settle
  // is only whether there is a filter for it to reach.
  if (ch.mod_cutoff_cents != 0.0f || ch.caf_cutoff_cents != 0.0f) {
    part_mod.filter_edited = true;
  }
  voice->start(patch, sample_rate_, velocity, voice_index, 0.0f, ch.una_corda, drum_kit, drum_mod,
               organ_percussion, part_mod);
  // This host passes no glide_from_hz, so start() leaves the voice's glide at
  // rest; the CC5/65/84 portamento is what drives it here.
  voice->glide_cents = porta.cents;
  voice->glide_coeff = porta.coeff;

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
    // The blow into the structure, which the board is struck with once rather
    // than driven by. After any prepare() above, which clears the network.
    fallback_board_[part].strike(voice->piano.case_strike());
    fallback_board_[part].strike_board(voice->piano.board_strike());
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
  recharge_percussion(ch);
}

void Sf2Player::recharge_percussion(uint8_t ch) noexcept {
  // The keys, not the voices: a percussion charge returns when the player's
  // hands leave the manual, and a released note whose tail is still sounding
  // (or whose damper the sustain pedal is holding) has left it.
  for (const Sf2Voice& v : pool_) {
    if (v.active && v.channel == ch && v.key_down) return;
  }
  for (const NativeSynthVoice& v : fallback_pool_) {
    if (v.active && v.channel == ch && v.key_down) return;
  }
  channels_[ch].percussion_armed = true;
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
  recharge_percussion(ch);
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
  recharge_percussion(ch);
  // All Sound Off means silence NOW, and the part's bus resonators are part of
  // its output: the piano soundboard and sympathetic bank ring for ~1.5 s and
  // the wind chest holds its tremulant/sag state, so killing the voices alone
  // would leak an audible wash past the stop. These are per-part, so clearing
  // them touches no other channel; the body's kind/tuning is kept so the next
  // note-on does not re-prepare.
  const size_t part = ch;
  fallback_board_[part].reset();
  fallback_reso_[part].reset();
  fallback_wind_[part].reset();
  fallback_body_[part].ringout = 0;
  if (pool_.active_count() == 0 && fallback_pool_.active_count() == 0) {
    // Bus-wide (every part feeds one mix), so only once nothing is sounding.
    dc_x1_ = {};
    dc_y1_ = {};
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
  // A melodic part has no map for the edit to land in, so the write is dropped
  // here rather than reaching a slab — the same guard as before the re-key.
  const bool is_drum = effective_bank(ch) == kDrumBank;
  if (!is_drum) return;
  // GS drum-kit NRPNs: msb selects the parameter, lsb is the drum note. The
  // edit is stored under the writing part's map, so every part on that map
  // sees it.
  GsDrumNoteParams& d = drum_params_[st.drum_map_slot()][st.params.nrpn_lsb & 0x7Fu];
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
    case 0x1F:
      d.delay = value;
      d.flags |= GsDrumNoteParams::kDelay;
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
  // RP-015 lists Portamento On/Off among the controllers it turns off, and a
  // pending CC84 arming goes with it; Portamento Time is a setting, not a
  // performance controller, and is left alone.
  st.portamento = false;
  st.portamento_armed = false;
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
    case 5:  // Portamento time
      st.portamento_time = value;
      break;
    case 6:  // Data entry MSB -> active RPN or GS NRPN
      if (st.params.selected_rpn(0, 0)) {
        st.bend_range_cents = 100.0f * static_cast<float>(value);
        refresh_channel_mod(ch);
      } else if (st.params.selected_rpn(0, 1)) {
        // Master fine tuning: 14-bit, MSB is the top 7 bits.
        st.pitch_fine_tune = static_cast<uint16_t>((static_cast<uint16_t>(value) << 7) |
                                                   (st.pitch_fine_tune & 0x7Fu));
      } else if (st.params.selected_rpn(0, 2)) {
        // Master coarse tuning: MSB only, centre 0x40, +-24 semitones. A value
        // past the defined range is clamped rather than ignored — it is an
        // out-of-range value, not a malformed message, and clamping keeps the
        // parameter continuous at the boundary instead of leaving a stale one
        // no later message corrects.
        st.pitch_coarse_tune =
            static_cast<int8_t>(std::clamp(static_cast<int>(value & 0x7Fu) - 64, -24, 24));
      } else if (st.params.selected_nrpn()) {
        apply_nrpn(ch, value);
      }
      break;
    case 38:  // Data entry LSB
      if (st.params.selected_rpn(0, 0)) {
        st.bend_range_cents =
            100.0f * std::floor(st.bend_range_cents / 100.0f) + static_cast<float>(value);
        refresh_channel_mod(ch);
      } else if (st.params.selected_rpn(0, 1)) {
        st.pitch_fine_tune =
            static_cast<uint16_t>((st.pitch_fine_tune & 0x3F80u) | (value & 0x7Fu));
      }
      // Master coarse tuning has no LSB: the manual defines it as MSB only.
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
      deselect_on_rpn_null(st.params);
      break;
    case 101:  // RPN MSB
      st.params.select_rpn_msb(value);
      deselect_on_rpn_null(st.params);
      break;
    case 64:
      sustain_cc(ch, value);
      break;
    case 65:  // Portamento on/off
      st.portamento = value >= 64;
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
    case 84:
      // Portamento control: the next note-on on this part glides from the
      // source note this message carries, once, whatever CC65 says.
      st.portamento_source = value & 0x7Fu;
      st.portamento_armed = true;
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
      all_notes_off(ch);
      break;
    case 126:
    case 127:
      // Mono Mode On / Poly Mode On. Both are All Notes Off as well, which is
      // why they keep the case above's call. They write the one storage
      // location GS SysEx 40 1x 13 writes (docs/gs.md); CC126's data byte is a
      // voice count and any value of it still means mono.
      st.mono_poly = controller == 126 ? kGsMonoPolyMono : kGsMonoPolyPoly;
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
  // GS RX CHANNEL (40 1x 02): which parts a channel message reaches. At the
  // power-on map this word carries the channel's own part and nothing else, so
  // the loop is a direct index until a file says otherwise. Several parts on one
  // channel is what real files use the parameter for — a layer — and a part set
  // to RX CHANNEL OFF is in no word at all.
  uint16_t parts = rx_parts_[u.channel() & 0x0Fu];
  if (parts == 0) return;
  for (uint8_t ch = 0; ch < 16; ++ch) {
    if ((parts & (1u << ch)) == 0) continue;
    if (u.is_note_on()) {
      const uint8_t vel7 =
          u.message_type() == UmpMessageType::kMidi1ChannelVoice
              ? u.data2_7bit()
              : scale_note_on_velocity_16_to_7(static_cast<uint16_t>(u.words[1] >> 16));
      note_on(ch, u.note_number(), vel7, event.source_track_id);
    } else if (u.is_note_off()) {
      note_off(ch, u.note_number(), event.source_track_id);
    } else if (u.status_nibble() == static_cast<uint8_t>(UmpStatus::kProgramChange)) {
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
      if (u.message_type() == UmpMessageType::kMidi1ChannelVoice) {
        // MIDI 1.0: 14-bit value, LSB in data1 (bits 8..14), MSB in data2.
        channels_[ch].pitch_bend =
            static_cast<uint16_t>((static_cast<uint16_t>(u.data2_7bit()) << 7) | u.note_number());
      } else {
        // MIDI 2.0: 32-bit value in word[1]; keep the top 14 bits.
        channels_[ch].pitch_bend = static_cast<uint16_t>(u.words[1] >> 18);
      }
      refresh_channel_mod(ch);
    } else if (u.status_nibble() == static_cast<uint8_t>(UmpStatus::kChannelPressure)) {
      // MIDI 1.0 carries the pressure in the first data byte (there is only
      // one); MIDI 2.0 gives a 32-bit value, narrowed the way a CC is.
      channels_[ch].channel_pressure = u.message_type() == UmpMessageType::kMidi1ChannelVoice
                                           ? u.note_number()
                                           : scale_cc_32_to_7(u.words[1]);
      refresh_channel_mod(ch);
    } else if (u.status_nibble() == static_cast<uint8_t>(UmpStatus::kControlChange)) {
      const uint8_t controller = u.note_number();
      const uint8_t value7 = u.message_type() == UmpMessageType::kMidi1ChannelVoice
                                 ? u.data2_7bit()
                                 : scale_cc_32_to_7(u.words[1]);
      control_change(ch, controller, value7);
    }
  }
}

}  // namespace sonare::midi::synth
