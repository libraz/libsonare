#include "midi/synth/native_synth.h"

#include <algorithm>
#include <cmath>

#include "midi/builtin_synth.h"
#include "midi/synth/articulation.h"
#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/voice_random.h"
#include "midi/ump.h"
#include "util/constants.h"
#include "util/non_finite_sample.h"

namespace sonare::midi::synth {

namespace {

/// Sympathetic-string bank t60 (seconds): how long the plucked-string sound
/// halo rings. Shared by the bank tuning (prepare_custom) and the tail estimate.
constexpr float kKsSympatheticRingS = 1.5f;

/// Shared-bus residual attribution memory: how quickly a source's learned
/// share of the bus-wide residual (body/reverb tail after its dry voice
/// stops) decays toward the other live sources.
constexpr float kResidualTauSeconds = 0.5f;

/// Registered Controller 0/0 in bank 0: pitch bend sensitivity.
constexpr uint8_t kRcPitchBendSensitivity = 0;
/// Registered Controller 0/7 in bank 0: per-note pitch bend sensitivity.
constexpr uint8_t kRcPerNoteBendSensitivity = 7;
/// Note On attribute #3 carries Pitch 7.9 (M2-104-UM §7.4.15).
constexpr uint8_t kAttributePitch79 = 0x03;
/// Registered Per-Note Controller #3 is Pitch 7.25 (M2-104-UM §7.4.12).
constexpr uint8_t kRpncPitch725 = 3;

/// Adds a relative controller's two's-complement delta to @p current, saturating at the ends of
/// the 32-bit range rather than wrapping.
Control32 add_saturating(Control32 current, Control32 delta) noexcept {
  const int64_t sum =
      static_cast<int64_t>(current.raw) + static_cast<int64_t>(static_cast<int32_t>(delta.raw));
  const int64_t clamped = std::min<int64_t>(std::max<int64_t>(sum, 0), int64_t{0xFFFFFFFF});
  return Control32::from_raw(static_cast<uint32_t>(clamped));
}

/// The pitch offset from the key the engine was started on, in cents. Exactly 0 while the key
/// carries no per-note pitch, so a MIDI 1.0 render is untouched.
float per_note_cents(const Sf2PerNoteVoice& state, const ComposedPitch& pitch) noexcept {
  const double semitones =
      pitch.per_note_semitones +
      static_cast<double>(static_cast<int>(state.binding.note) - static_cast<int>(state.zone_key));
  return semitones == 0.0 ? 0.0f : static_cast<float>(semitones * constants::kCentsPerSemitone);
}

}  // namespace

// ---------------------------------------------------------------------------
// NativeSynth (MidiInstrument)
// ---------------------------------------------------------------------------

NativeSynth::NativeSynth(const NativeSynthConfig& config) : config_(config) {
  config_.patch = clamp_synth_patch(config_.patch);
  // An explicit 0 is silence; only a negative or non-finite gain is not a level.
  if (config_.gain < 0.0f || !std::isfinite(config_.gain)) config_.gain = 0.5f;
  config_.gain = std::min(config_.gain, 4.0f);
  config_.polyphony = config_.polyphony > 0 ? std::min(config_.polyphony, kMaxSynthVoices) : 16;
  config_.bus_drive =
      std::isfinite(config_.bus_drive) ? std::clamp(config_.bus_drive, 0.0f, 1.0f) : 0.0f;
  // The stage owns the factory and the bank-rig switch from here on. The GS
  // units take the modern realisation; no field selects another.
  PartFxStageConfig fx;
  fx.insert_factory = std::move(config_.insert_factory);
  fx.bank_rig_binding = config_.bank_rig_binding;
  part_fx_ = PartFxStage(std::move(fx));
}

void NativeSynth::prepare(double sample_rate, int /*max_block_size*/) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  residual_splitter_.configure(sample_rate_, kResidualTauSeconds);
  residual_splitter_.reset();
  piano_residual_splitter_.configure(sample_rate_, kResidualTauSeconds);
  piano_residual_splitter_.reset();
  guitar_residual_splitter_.configure(sample_rate_, kResidualTauSeconds);
  guitar_residual_splitter_.reset();
  for (SourceResidualSplitter& s : part_bus_splitters_) {
    s.configure(sample_rate_, kResidualTauSeconds);
    s.reset();
  }
  for (SourceResidualSplitter& s : unit_splitters_) {
    s.configure(sample_rate_, kResidualTauSeconds);
    s.reset();
  }
  part_fx_.clear_mirror();
  part_fx_.prepare(sample_rate_);
  for (PianoResonanceBank& halo : part_halo_) halo.reset();
  part_halo_armed_.fill(false);
  pool_.prepare(config_.polyphony);
  // One entry per voice bounds the notes one channel can be sounding, and this
  // is the only place the attribution scratch is sized.
  mpe_notes_.assign(pool_.size(), MpeNote{});
  mpe_note_ages_.assign(pool_.size(), 0);
  per_note_.assign(pool_.size(), Sf2PerNoteVoice{});
  per_note_pitch_.clear();
  per_note_bend_sensitivity_.fill(kDefaultPerNoteBendSensitivity);
  // GM mode resolves the engine per program at note-on, so any engine can be
  // selected regardless of the configured patch: every per-voice delay slab has
  // to exist up front or the waveguide engines render silence (their cores
  // return 0 while unattached). Mirrors Sf2Player::prepare(), which allocates
  // every fallback slab whenever the fallback floor is reachable.
  const bool any_engine = config_.use_gm_programs;
  // KS strings need a per-voice delay slab (the only allocation site; voices
  // attach their span at note-on).
  ks_capacity_ = ks_buffer_capacity(sample_rate_);
  guitar_halo_active_ = false;
  if (any_engine || config_.patch.mode == SynthEngineMode::kKarplusStrong) {
    ks_buffers_.assign(pool_.size() * static_cast<size_t>(ks_slab_capacity(sample_rate_)), 0.0f);
    // Sympathetic-string "sound halo": a bank tuned to the standard-tuning
    // open strings (E2 A2 D3 G3 B3 E4) plus their low harmonics — the
    // undamped strings ringing behind the played note. Gated by the same
    // sustain-pedal state as the piano board's bank (process_impl's
    // damper_open): a guitarist's hand damps the open strings unless
    // something is holding them open. Armed here for a configured
    // Karplus-Strong patch; a GM render arms it lazily at the first
    // qualifying note-on instead, since the program is not known yet.
    if (config_.patch.mode == SynthEngineMode::kKarplusStrong && config_.patch.ks.sympathetic) {
      guitar_halo_.prepare_guitar_sympathetic(sample_rate_);
      guitar_halo_active_ = true;
    }
  } else {
    ks_buffers_.clear();
  }
  piano_string_capacity_ = piano_string_capacity(sample_rate_);
  piano_mode_ = config_.patch.mode == SynthEngineMode::kPiano;
  if (any_engine || piano_mode_) {
    piano_buffers_.assign(pool_.size() * static_cast<size_t>(piano_slab_capacity(sample_rate_)),
                          0.0f);
  } else {
    piano_buffers_.clear();
  }
  // The modal soundboard and the pedal-gated sympathetic bank are bus-level.
  // A configured piano tunes them here; in GM mode the program that voices a
  // piano is not known until note-on, so the bank is tuned there instead (the
  // same lazy rule Sf2Player uses for its per-part bodies).
  piano_body_active_ = piano_mode_;
  piano_body_soundboard_ = -1.0f;
  if (piano_mode_) {
    resonance_.prepare(sample_rate_);
    soundboard_.prepare(sample_rate_, config_.patch.piano.soundboard);
    piano_body_soundboard_ = config_.patch.piano.soundboard;
  }
  // Pipe organ: one delay slab per voice slot (kMaxPipeRanks bore+jet span pairs,
  // so a registration's ranks all have their own self-oscillating jet pipe). The
  // only allocation site; voices attach their slab at note-on.
  pipe_organ_capacity_ = pipe_organ_buffer_capacity(sample_rate_);
  pipe_organ_mode_ = config_.patch.mode == SynthEngineMode::kPipeOrgan;
  if (any_engine || pipe_organ_mode_) {
    pipe_organ_buffers_.assign(
        pool_.size() * static_cast<size_t>(pipe_organ_slab_capacity(sample_rate_)), 0.0f);
  } else {
    pipe_organ_buffers_.clear();
  }
  // Wind chest and swell box are bus-level (shared by every sounding pipe), so
  // they stay tied to the configured patch.
  if (pipe_organ_mode_) {
    wind_.prepare(sample_rate_, config_.patch.pipe_organ.tremulant_rate_hz,
                  config_.patch.pipe_organ.tremulant_depth, config_.patch.pipe_organ.wind_sag);
    swell_depth_ = config_.patch.pipe_organ.swell;
  } else {
    swell_depth_ = 0.0f;
  }
  // Bowed string: one delay slab per voice slot (two delay-line spans, the neck
  // and bridge). The only allocation site; voices attach their slab at note-on.
  bowed_string_capacity_ = bowed_string_buffer_capacity(sample_rate_);
  bowed_string_mode_ = any_engine || config_.patch.mode == SynthEngineMode::kBowedString;
  if (bowed_string_mode_) {
    bowed_string_buffers_.assign(
        pool_.size() * static_cast<size_t>(bowed_string_slab_capacity(sample_rate_)), 0.0f);
  } else {
    bowed_string_buffers_.clear();
  }
  // Reed woodwind: one bore delay span per voice slot. The only allocation site;
  // voices attach their span at note-on.
  reed_capacity_ = reed_buffer_capacity(sample_rate_);
  reed_mode_ = any_engine || config_.patch.mode == SynthEngineMode::kReed;
  if (reed_mode_) {
    reed_buffers_.assign(pool_.size() * static_cast<size_t>(reed_slab_capacity(sample_rate_)),
                         0.0f);
  } else {
    reed_buffers_.clear();
  }
  // Brass / lip reed: one bore delay span per voice slot (same as the reed).
  brass_capacity_ = brass_buffer_capacity(sample_rate_);
  brass_mode_ = any_engine || config_.patch.mode == SynthEngineMode::kBrass;
  if (brass_mode_) {
    brass_buffers_.assign(pool_.size() * static_cast<size_t>(brass_slab_capacity(sample_rate_)),
                          0.0f);
  } else {
    brass_buffers_.clear();
  }
  // Air-jet flute: a bore span plus a jet span per voice slot.
  flute_capacity_ = flute_buffer_capacity(sample_rate_);
  flute_mode_ = any_engine || config_.patch.mode == SynthEngineMode::kFlute;
  if (flute_mode_) {
    flute_buffers_.assign(pool_.size() * static_cast<size_t>(flute_slab_capacity(sample_rate_)),
                          0.0f);
  } else {
    flute_buffers_.clear();
  }
  // Plucked string: one string delay span per voice slot. The only allocation
  // site; voices attach their span at note-on.
  plucked_string_capacity_ = plucked_string_buffer_capacity(sample_rate_);
  plucked_string_mode_ = any_engine || config_.patch.mode == SynthEngineMode::kPluckedString;
  if (plucked_string_mode_) {
    plucked_string_buffers_.assign(
        pool_.size() * static_cast<size_t>(plucked_string_slab_capacity(sample_rate_)), 0.0f);
  } else {
    plucked_string_buffers_.clear();
  }
  // Harpsichord: a registration slab per voice slot. The only allocation site;
  // voices attach theirs at note-on.
  harpsichord_capacity_ = harpsichord_buffer_capacity(sample_rate_);
  harpsichord_stride_ = harpsichord_slab_capacity(sample_rate_);
  harpsichord_mode_ = any_engine || config_.patch.mode == SynthEngineMode::kHarpsichord;
  if (harpsichord_mode_) {
    harpsichord_buffers_.assign(pool_.size() * static_cast<size_t>(harpsichord_stride_), 0.0f);
  } else {
    harpsichord_buffers_.clear();
  }
  swell_lp_l_ = 0.0f;
  swell_lp_r_ = 0.0f;
  channels_ = {};
  // GM power-on: channel 10 is the rhythm part (no SysEx needed). MPE mode is
  // off until an MCM turns it on, which is what a power-on default of "no zone
  // configured" means (2.2.1); the refresh below therefore takes the ordinary
  // per-channel path.
  channels_[kDrumChannelIndex].drums = true;
  mpe_.reset();
  refresh_all_channel_mods();
  // GM mode (and the GM drum kit) can voice any fallback patch, so the tail has
  // to cover the slowest release in the fallback tables rather than the
  // configured patch's own release.
  const bool gm_tables =
      config_.use_gm_programs ||
      (config_.patch.mode == SynthEngineMode::kPercussion && config_.patch.percussion.gm_kit);
  // A zero-sustain (percussive) envelope never reaches Release — it ends at the
  // decay floor — so for those the decay is the stage that actually terminates
  // the voice. The GM tables already bound both stages; the configured patch
  // has to as well, or a one-shot voice is cut mid-decay at a bounce boundary.
  const DahdsrConfig& amp = config_.patch.amp_env;
  const float patch_tail_ms = amp.sustain <= DahdsrEnvelope::kSilenceLevel
                                  ? std::max(amp.release_ms, amp.decay_ms)
                                  : amp.release_ms;
  tail_samples_ = DahdsrEnvelope::release_tail_samples(
      sample_rate_, gm_tables ? gm_fallback_max_release_ms() : patch_tail_ms);
  if (guitar_halo_active_ || gm_tables) {
    // The halo bank keeps ringing after the last voice releases; fold its t60
    // into the tail so a bounce does not clip it. GM is included because
    // prepare() cannot know which program a later note-on will pick.
    tail_samples_ += static_cast<int64_t>(sample_rate_ * kKsSympatheticRingS);
  }
  if (piano_mode_ || gm_tables) {
    // Same story for the piano bus body: the soundboard and the sympathetic
    // bank ring far past the ~120 ms voice release, so a bounce would cut the
    // bloom off the last chord. GM mode is included because program 0 resolves
    // to the piano patch there and tunes the body at note-on.
    tail_samples_ += static_cast<int64_t>(sample_rate_ * kPianoBodyRingS);
  }
  // Mix-bus polish: ~8 Hz DC blocker pole and the gain-neutral drive factor.
  dc_r_ = 1.0f - static_cast<float>(constants::kTwoPiD * 8.0 / sample_rate_);
  dc_x1_ = {};
  dc_y1_ = {};
  bus_drive_gain_ = config_.bus_drive > 0.0f ? 1.0f + 3.0f * config_.bus_drive : 0.0f;
  prepared_ = true;
  // The rig entries and the bank rigs the parts' power-on programs bind, so a
  // bussed part routes from the first block.
  for (uint8_t ch = 0; ch < 16; ++ch) refresh_part_rig(ch);
  part_fx_.publish();
  part_fx_.clear_dirty();
}

void NativeSynth::reset() {
  pool_.reset();
  dc_x1_ = {};
  dc_y1_ = {};
  residual_splitter_.reset();
  piano_residual_splitter_.reset();
  guitar_residual_splitter_.reset();
  for (SourceResidualSplitter& s : part_bus_splitters_) s.reset();
  for (SourceResidualSplitter& s : unit_splitters_) s.reset();
  part_fx_.clear_mirror();
  resonance_.reset();
  soundboard_.reset();
  guitar_halo_.reset();
  for (PianoResonanceBank& halo : part_halo_) halo.reset();
  part_halo_armed_.fill(false);
  // A GM-mode body was tuned by a note-on, so it goes back to untuned; a
  // configured piano keeps the tuning prepare() gave it.
  piano_body_active_ = piano_mode_;
  if (!piano_mode_) piano_body_soundboard_ = -1.0f;
  // Same rule for the halo: a GM-mode arming goes back to unarmed, while a
  // configured Karplus-Strong sympathetic patch keeps the arming prepare() gave it.
  guitar_halo_active_ =
      config_.patch.mode == SynthEngineMode::kKarplusStrong && config_.patch.ks.sympathetic;
  wind_.reset();
  swell_lp_l_ = 0.0f;
  swell_lp_r_ = 0.0f;
  channels_ = {};
  // GM power-on: channel 10 is the rhythm part (no SysEx needed). MPE mode is
  // off until an MCM turns it on, which is what a power-on default of "no zone
  // configured" means (2.2.1); the refresh below therefore takes the ordinary
  // per-channel path.
  channels_[kDrumChannelIndex].drums = true;
  mpe_.reset();
  skipped_events_ = 0;
  per_note_.assign(per_note_.size(), Sf2PerNoteVoice{});
  per_note_pitch_.clear();
  per_note_bend_sensitivity_.fill(kDefaultPerNoteBendSensitivity);
  refresh_all_channel_mods();
  for (uint8_t ch = 0; ch < 16; ++ch) refresh_part_rig(ch);
  // A fresh snapshot rebuilds the chains, which is their reset.
  if (prepared_) part_fx_.publish();
  part_fx_.clear_dirty();
}

void NativeSynth::refresh_channel_mod(uint8_t channel) noexcept {
  const uint8_t ch = channel & 0x0Fu;
  const ChannelState& st = channels_[ch];
  Sf2ChannelMod& mod = channel_mods_[ch];
  // Inside an MPE zone the bend range and the pressure belong to the zone: the
  // range is the one the MCM installed rather than the channel's own, and both
  // dimensions carry the manager's contribution folded in (M1-100-UM v1.1
  // sections 2.2.5 - 2.2.7). Outside one -- which is every channel until an MCM
  // arrives -- this is the arithmetic that was always here.
  const bool zoned = mpe_.role(ch) != MpeChannelRole::kUnassigned;
  mod.pitch_cents = zoned ? mpe_.bend_semitones(ch) * 100.0f
                          : (st.pitch_bend.f14() - 8192.0f) / 8192.0f * st.bend_range_cents;
  mod.gain = sf2_cc_gain(st.volume) * sf2_cc_gain(st.expression);
  mod.mod_wheel01 = st.mod_wheel.f7() / 127.0f;
  mod.extra_vibrato_cents = st.mod_depth_cents * mod.mod_wheel01;
  mod.pan_units = (st.pan.f7() - 64.0f) / 63.0f * 500.0f;
  mod.breath01 = st.breath.f7() / 127.0f;
  mod.aftertouch01 = (zoned ? mpe_.pressure(ch) : st.pressure.f7()) / 127.0f;
  mod.expression01 = st.expression.f7() / 127.0f;
  mod.pitch_bend01 = (st.pitch_bend.f14() - 8192.0f) / 8192.0f;
  // The three axes that land on the channel rather than inside an engine. Each
  // is folded only when a binding has reached it, so a profile that names none
  // leaves this arithmetic untouched rather than multiplying by an identity.
  if (st.axes.has(ControllerAxis::kLoudness)) {
    mod.gain *= st.axes.values[static_cast<size_t>(ControllerAxis::kLoudness)];
  }
  if (st.axes.has(ControllerAxis::kPitchCents)) {
    mod.pitch_cents += st.axes.values[static_cast<size_t>(ControllerAxis::kPitchCents)];
  }
  if (st.axes.has(ControllerAxis::kVibratoDepth)) {
    mod.extra_vibrato_cents += st.axes.values[static_cast<size_t>(ControllerAxis::kVibratoDepth)];
  }
  refresh_mpe_note_mods(ch);
}

void NativeSynth::refresh_all_channel_mods() noexcept {
  for (uint8_t ch = 0; ch < 16; ++ch) refresh_channel_mod(ch);
}

void NativeSynth::apply_mcm(uint8_t manager_channel, uint8_t member_count) noexcept {
  uint16_t moved = 0;
  if (!mpe_.apply_mcm(manager_channel, member_count, &moved)) return;
  for (uint8_t ch = 0; ch < 16; ++ch) {
    if ((moved & (uint16_t{1} << ch)) == 0) continue;
    all_sound_off(ch);
    // Which clears the zone model's tracked controllers for the channel too,
    // so a channel that left carries nothing into its conventional use and one
    // that arrived starts from the initial state 2.2.7 and 2.2.8 name.
    reset_controllers(ch);
  }
  // Every channel, not only the ones that moved: an MCM reinstalls the whole
  // zone's bend sensitivity, so a member that kept its place still changed
  // range.
  refresh_all_channel_mods();
}

namespace {

/// The deepest pitch any rank of @p patch sounds, relative to the key. A pipe
/// organ voices one key at several pitches at once, so the 16' rank runs out of
/// delay line an octave before the 8' rank does and it is the 16' that decides
/// whether the voice can be carried. 1.0 for every other engine, which sounds
/// the key and nothing below it.
float lowest_pitch_mult(const NativeSynthPatch& patch) noexcept {
  if (patch.mode != SynthEngineMode::kPipeOrgan || patch.pipe_organ.rank_count <= 0) return 1.0f;
  float lowest = 1.0f;
  const int count = std::min(patch.pipe_organ.rank_count, kMaxPipeRanks);
  for (int r = 0; r < count; ++r) {
    const float mult = patch.pipe_organ.ranks[static_cast<size_t>(r)].footage_mult;
    if (mult > 0.01f && mult < lowest) lowest = mult;
  }
  return lowest;
}

}  // namespace

NativeSynthVoice* NativeSynth::find_sounding(uint8_t ch, uint8_t note,
                                             uint32_t source_track_id) noexcept {
  // Key-down and not releasing, because a released loop cannot be carried: a
  // waveguide's release lowers its loop gain irreversibly, so a voice re-tuned
  // after note-off would arrive at the new pitch already decaying.
  for (NativeSynthVoice& v : pool_) {
    if (v.active && v.channel == ch && v.note == note && v.source_track_id == source_track_id &&
        v.key_down && !v.releasing) {
      return &v;
    }
  }
  return nullptr;
}

void NativeSynth::note_on(uint8_t channel, uint8_t note, Velocity16 velocity,
                          uint8_t attribute_type, uint16_t attribute_data,
                          uint32_t source_track_id) noexcept {
  if (!prepared_) return;
  // A profile that calls velocity meaningless takes every note at full scale,
  // so the bound axes carry the dynamics on their own. 127 rather than some
  // mid value because it is the scale's identity: nothing is attenuated here
  // that a controller is not asking for.
  if (!controller_profile_.velocity_meaningful) velocity = Velocity16::from7(127);
  const uint8_t ch = channel & 0x0Fu;
  // Generic MIDI rendering resolves every channel through the shared GM/GS bank
  // rule, so a rhythm part (channel 10, a GM2 percussion bank, or a GS-assigned
  // part) reaches the drum map and a melodic channel its variation bank —
  // identically to Sf2Player. Explicit percussion patches retain their existing
  // GM-kit behavior when this mode is disabled. Resolved BEFORE the pool is
  // touched: the exclusive-group choke below reads the resolved patch, and a
  // voice allocated first would still carry the previous note's patch pointer.
  const ChannelState& st = channels_[ch];
  const NativeSynthPatch* patch = &config_.patch;
  uint8_t drum_kit = 0;
  if (config_.use_gm_programs) {
    const uint16_t bank = gs_effective_bank(st.bank_msb, st.bank_lsb, st.drums);
    const GsToneMap map = gs_effective_tone_map(st.bank_msb, st.bank_lsb);
    if (bank == kDrumBank) {
      patch = &gm_fallback_drum_patch(note);
      drum_kit = gm_fallback_drum_kit(st.program, map);
    } else {
      patch = &gm_fallback_patch(bank, st.program, map);
    }
  } else if (patch->mode == SynthEngineMode::kPercussion && patch->percussion.gm_kit) {
    patch = &gm_fallback_drum_patch(note);
    drum_kit = gm_fallback_drum_kit(st.program);
  }
  // GM kit exclusive/mute groups: a new hi-hat / triangle / whistle / surdo
  // strike chokes the ringing voice in its group before the new one allocates.
  // Keyed on the RESOLVED patch's group, so it works in GM mode too.
  // Both sides are mode-independent: a kit piece voiced from the sample engine
  // carries its group in the same field, and a hi-hat that cannot choke is a
  // hi-hat with no pedal. A melodic patch leaves the field at 0 and never
  // reaches here.
  if (patch->percussion.exclusive_class != 0) {
    const uint8_t excl = patch->percussion.exclusive_class;
    for (NativeSynthVoice& v : pool_) {
      if (v.active && v.channel == ch && v.patch != nullptr && v.exclusive_class == excl) {
        v.choke();
      }
    }
  }
  // Legato continuation, before a voice is allocated: on a carrying channel a
  // note-on under a held key moves that voice to the new key rather than
  // starting one, so the exciter, the delay line and both envelopes run on.
  ChannelState& live = channels_[ch];
  if (live.articulation != ArticulationMode::kPoly && live.newest_key() >= 0) {
    NativeSynthVoice* held =
        find_sounding(ch, static_cast<uint8_t>(live.newest_key()), source_track_id);
    if (held != nullptr) {
      if (live.articulation == ArticulationMode::kMonoLegato &&
          accepts_legato(patch->mode, held->note, note, lowest_pitch_mult(*patch))) {
        const uint8_t from = held->note;
        held->retune(note, sample_rate_);
        carry_per_note(*held, from, note, attribute_type, attribute_data);
        live.hold_key(note);
        live.last_freq_hz = synth_note_to_hz(static_cast<float>(note));
        return;
      }
      if (live.articulation == ArticulationMode::kMonoLegato) ++legato_fallbacks_;
      // Monophonic either way: the previous note stops. Fast rather than the
      // patch's own release, which on a sustaining patch runs past a second and
      // would leave the note it replaced audible under the new one.
      held->choke_fast(sample_rate_);
    }
  }
  live.hold_key(note);

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
  voice->sampler.attach(sample_bank_);
  if (!harpsichord_buffers_.empty()) {
    voice->harpsichord.attach(
        harpsichord_buffers_.data() + static_cast<size_t>(voice_index) * harpsichord_stride_,
        harpsichord_capacity_);
  }
  // Portamento: glide from the channel's previous note when enabled.
  const float glide_from = patch->glide_ms > 0.0f ? st.last_freq_hz : 0.0f;
  // Drawbar percussion spends the channel's charge; note_off recharges it once
  // the last key is up.
  const bool organ_percussion = patch->mode == SynthEngineMode::kAdditive &&
                                patch->additive.percussion_harmonic >= 2 && st.percussion_armed;
  if (organ_percussion) channels_[ch].percussion_armed = false;
  // Per-note pitch (M2-104-UM §7.4.15). An absolute pitch starts the engine on the key of its
  // integer part, so a sample zone and a delay line are chosen for the pitch that sounds; the
  // struck note stays the voice's own, which is what a note-off matches.
  Sf2PerNoteVoice per_note;
  bind_per_note(per_note, ch, note, attribute_type, attribute_data, note);
  const ComposedPitch note_pitch = compose_per_note(per_note);
  DrumVoiceMod voice_mod{};
  if (note_pitch.absolute) {
    per_note.zone_key = static_cast<uint8_t>(std::clamp(
        static_cast<int>(std::floor(static_cast<double>(note) + note_pitch.per_note_semitones)), 0,
        127));
    voice_mod.play_note = per_note.zone_key;
  }
  per_note.cents = per_note_cents(per_note, note_pitch);
  voice->start(*patch, sample_rate_, velocity, voice_index, glide_from, st.una_corda, drum_kit,
               voice_mod, organ_percussion);
  per_note_[voice_index] = per_note;
  // Seed the engine's excitation axes at the channel's current controllers (no
  // glide on the first sample) so a note struck mid-phrase starts at the live
  // breath / brightness rather than gliding in from the preset. An engine reads
  // only the axes it declares, and one that declares none is untouched.
  {
    uint32_t present = kAxisNone;
    const ExcitationAxes base = channel_excitation(st.axes, present);
    voice->seed_excitation(base, present);
  }
  // Bus-level piano body (the direct-share attenuation, the modal soundboard
  // and the pedal-gated sympathetic bank). In GM mode the engine is resolved
  // per program, so the body is tuned at the first piano note-on rather than in
  // prepare(); a later program asking for a different board mix only re-states
  // the return level, since other notes may still be ringing through the bank.
  // Allocation-free, like the lazy per-part prepare on the Sf2Player fallback path.
  if (patch->mode == SynthEngineMode::kPiano) {
    if (piano_body_soundboard_ < 0.0f) {
      soundboard_.prepare(sample_rate_, patch->piano.soundboard);
      resonance_.prepare(sample_rate_);
    } else if (piano_body_soundboard_ != patch->piano.soundboard) {
      soundboard_.set_mix(patch->piano.soundboard);
    }
    piano_body_soundboard_ = patch->piano.soundboard;
    piano_body_active_ = true;
    // The blow into the structure, which the board is struck with once rather
    // than driven by. After any prepare() above, which clears the network.
    soundboard_.strike(voice->piano.case_strike());
    soundboard_.strike_board(voice->piano.board_strike());
  }
  // Bus-level open-string halo, the same lazy-arming rule as the piano body
  // above but for a Karplus-Strong voice that asks for it. Guarded so an
  // already-armed bank (configured, or armed by an earlier note-on) is never
  // re-prepared, which would clear its ringing state.
  if (patch->mode == SynthEngineMode::kKarplusStrong && patch->ks.sympathetic &&
      !guitar_halo_active_) {
    guitar_halo_.prepare_guitar_sympathetic(sample_rate_);
    guitar_halo_active_ = true;
  }
  // The part's own halo too, which the render drives in place of the shared
  // one whenever the part is bussed.
  if (patch->mode == SynthEngineMode::kKarplusStrong && patch->ks.sympathetic &&
      part_fx_.enabled() && !part_halo_armed_[ch]) {
    part_halo_[ch].prepare_guitar_sympathetic(sample_rate_);
    part_halo_armed_[ch] = true;
  }
  channels_[ch].last_freq_hz = voice->base_freq_hz;
  // A second note on a member channel is what makes the channel's bend and
  // pressure ambiguous, so the attribution is recomputed as the set changes.
  // The new voice needs it anyway: it starts reading a mod on the next sample.
  refresh_mpe_note_mods(ch);
}

void NativeSynth::note_off(uint8_t channel, uint8_t note, uint32_t source_track_id) noexcept {
  if (!prepared_) return;
  const uint8_t ch = channel & 0x0Fu;
  ChannelState& st = channels_[ch];
  st.release_key(note);
  // Releasing a key under a slur returns the voice to the key still held rather
  // than ending the phrase. A note-off whose key is NOT the one sounding falls
  // straight past here and past the release loop below, which is what keeps a
  // slur alive when the old key is let go late.
  if (st.articulation == ArticulationMode::kMonoLegato && st.newest_key() >= 0) {
    NativeSynthVoice* sounding = find_sounding(ch, note, source_track_id);
    if (sounding != nullptr && sounding->patch != nullptr) {
      const uint8_t back = static_cast<uint8_t>(st.newest_key());
      if (accepts_legato(sounding->patch->mode, sounding->note, back,
                         lowest_pitch_mult(*sounding->patch))) {
        sounding->retune(back, sample_rate_);
        carry_per_note(*sounding, note, back, 0, 0);
        st.last_freq_hz = synth_note_to_hz(static_cast<float>(back));
        return;
      }
      // The key still held is out of the engine's reach, so the phrase ends on
      // the release below and the held key stays silent. Counted, because
      // nothing in the sound says which of the two happened.
      ++legato_fallbacks_;
    }
  }
  for (NativeSynthVoice& v : pool_) {
    if (v.active && v.note == note && v.channel == ch && v.source_track_id == source_track_id &&
        v.key_down) {
      v.key_down = false;
      // A sostenuto capture holds the note regardless of the sustain pedal.
      if (v.sostenuto) continue;
      if (!st.sustain) {
        v.release();  // pedal up: the damper falls now
      } else if (st.sustain_level.f7() < 127.0f && v.patch != nullptr &&
                 v.patch->mode == SynthEngineMode::kPiano) {
        // Half-pedal: the partially raised damper rests on the string.
        v.piano.damp((127.0f - st.sustain_level.f7()) / 63.0f);
      }
      // else (full pedal): the string rings on freely.
    }
  }
  recharge_percussion(ch);
  // The set of notes a channel-addressed value can be attributed to just
  // changed, and a released note is not one of them.
  refresh_mpe_note_mods(ch);
}

void NativeSynth::recharge_percussion(uint8_t ch) noexcept {
  // The keys, not the voices: a percussion charge returns when the player's
  // hands leave the manual, and a released note whose tail is still sounding
  // (or whose damper the sustain pedal is holding) has left it.
  for (const NativeSynthVoice& v : pool_) {
    if (v.active && v.channel == ch && v.key_down) return;
  }
  channels_[ch].percussion_armed = true;
}

void NativeSynth::sustain_cc(uint8_t channel, Control32 value) noexcept {
  if (!prepared_) return;
  const uint8_t ch = channel & 0x0Fu;
  ChannelState& st = channels_[ch];
  const bool was_down = st.sustain;
  st.sustain_level = value;
  st.sustain = value.u7() >= 64;
  if (st.sustain) {
    // Half-pedal: a partially raised damper still rests on the strings, so held
    // (key-up) notes ring on at an intermediate rate; a full lift (127) leaves
    // them ringing freely. Key-down and sostenuto-captured notes keep their
    // dampers mechanically off, so they are untouched.
    const float level = value.f7();
    const float strength = (127.0f - level) / 63.0f;
    for (NativeSynthVoice& v : pool_) {
      if (v.active && v.channel == ch && !v.key_down && !v.releasing && !v.sostenuto &&
          v.patch != nullptr && v.patch->mode == SynthEngineMode::kPiano) {
        v.piano.damp(strength);
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
  // Edge-triggered: only a change of pedal position captures or releases.
  if (channels_[ch].sostenuto_down == down) return;
  channels_[ch].sostenuto_down = down;
  for (NativeSynthVoice& v : pool_) {
    if (!v.active || v.channel != ch) continue;
    if (down) {
      // Capture only the notes whose keys are down at the moment of the press.
      if (v.key_down) v.sostenuto = true;
    } else if (v.sostenuto) {
      v.sostenuto = false;
      if (!v.key_down && !v.releasing) {
        if (!channels_[ch].sustain) {
          v.release();
        } else if (v.patch != nullptr && v.patch->mode == SynthEngineMode::kPiano) {
          const float strength = (127.0f - channels_[ch].sustain_level.f7()) / 63.0f;
          v.piano.damp(strength);
        }
      }
    }
  }
}

void NativeSynth::all_notes_off(uint8_t channel) noexcept {
  if (!prepared_) return;
  const uint8_t ch = channel & 0x0Fu;
  ChannelState& st = channels_[ch];
  // A released key left in the legato stack would be retuned back to later.
  st.held_count = 0;
  for (NativeSynthVoice& v : pool_) {
    if (v.active && v.channel == ch && !v.releasing) {
      v.key_down = false;
      // All Notes Off is a key-up gesture. Sustain and sostenuto still hold a
      // captured voice, exactly as they do for an ordinary note-off.
      if (v.sostenuto) continue;
      if (!st.sustain) {
        v.release();
      } else if (st.sustain_level.f7() < 127.0f && v.patch != nullptr &&
                 v.patch->mode == SynthEngineMode::kPiano) {
        v.piano.damp((127.0f - st.sustain_level.f7()) / 63.0f);
      }
    }
  }
  recharge_percussion(ch);
}

void NativeSynth::all_sound_off(uint8_t channel) noexcept {
  if (!prepared_) return;
  const uint8_t ch = channel & 0x0Fu;
  channels_[ch].held_count = 0;
  for (NativeSynthVoice& v : pool_) {
    if (v.active && v.channel == ch) v.kill();
  }
  recharge_percussion(ch);
  if (pool_.active_count() == 0) {
    // All Sound Off means silence NOW, and the instrument's bus resonators are
    // part of its output: the piano soundboard, the sympathetic bank and the
    // guitar halo ring for ~1.5 s and the swell one-pole holds a residual, so
    // killing the voices alone would leak an audible wash past the stop. They
    // are bus-level (all 16 channels feed one), so they are cleared only once
    // nothing is sounding on any channel. The DC blocker goes with them for
    // the same reason.
    resonance_.reset();
    soundboard_.reset();
    guitar_halo_.reset();
    for (PianoResonanceBank& halo : part_halo_) halo.reset();
    swell_lp_l_ = 0.0f;
    swell_lp_r_ = 0.0f;
    dc_x1_ = {};
    dc_y1_ = {};
  }
}

void NativeSynth::channel_pressure(uint8_t channel, Control32 pressure) noexcept {
  const uint8_t ch = channel & 0x0Fu;
  channels_[ch].pressure = pressure;
  // A manager's pressure is a bias on every member of its zone, so it reaches
  // further than the channel it arrived on (2.2.7).
  if (mpe_.role(ch) == MpeChannelRole::kManager) {
    refresh_all_channel_mods();
  } else {
    refresh_channel_mod(ch);
  }
}

void NativeSynth::poly_pressure(uint8_t channel, uint8_t note, Control32 pressure) noexcept {
  const uint8_t ch = channel & 0x0Fu;
  // Prohibited on a member channel, where pressure is the channel's and belongs
  // to the one note living on it (2.2.7).
  if (mpe_.ignores(ch, MpeIgnorable::kPolyKeyPressure)) return;
  const float value = pressure.f7() / 127.0f;
  // Every sounding voice on the note takes it: a layered patch is several
  // voices of one key, and pressure is a property of the key.
  for (NativeSynthVoice& v : pool_) {
    if (v.active && v.note == note && v.channel == ch) v.poly_pressure01 = value;
  }
}

void NativeSynth::reset_controllers(uint8_t channel) noexcept {
  // MIDI RP-015: reset performance controllers, keep volume/pan.
  const uint8_t ch = channel & 0x0Fu;
  ChannelState& st = channels_[ch];
  st.mod_wheel = Control32::from7(0);
  st.expression = Control32::from7(127);
  st.pitch_bend = Bend32::center();
  st.breath = Control32::from7(0);
  st.pressure = Control32::from7(0);
  for (const size_t controller :
       {size_t{1}, size_t{2}, size_t{64}, size_t{65}, size_t{66}, size_t{67}}) {
    st.cc_position[controller] = Control32::from7(0);
  }
  for (const size_t controller : {size_t{11}, size_t{98}, size_t{99}, size_t{100}, size_t{101}}) {
    st.cc_position[controller] = Control32::from7(gs_default_cc_positions()[controller]);
  }
  for (NativeSynthVoice& v : pool_) {
    if (v.channel == ch) v.poly_pressure01 = 0.0f;
  }
  st.axes.reset();
  st.params.reset();
  // Inside a zone the same three controllers are tracked by the zone model,
  // which is where every reader of them takes their combined value -- leaving
  // them here would reset the channel and change nothing that is heard.
  mpe_.reset_controls(static_cast<uint16_t>(uint16_t{1} << ch));
  sustain_cc(ch, Control32::from7(0));
  sostenuto_pedal(ch, false);
  st.una_corda = false;
  if (mpe_.role(ch) == MpeChannelRole::kManager) {
    // The manager's values were a bias on every member, so each member has to
    // resolve its own again without them.
    const MpeZone zone = mpe_.zone_of(ch);
    for (uint8_t member = 0; member < 16; ++member) {
      if (mpe_.role(member) != MpeChannelRole::kMember || mpe_.zone_of(member) != zone) continue;
      for (const MpeDimension dimension : {MpeDimension::kPressure, MpeDimension::kTimbre}) {
        if (mpe_.has(member, dimension)) push_mpe_controller_axis(member, dimension);
      }
    }
    refresh_all_channel_mods();
  } else {
    refresh_channel_mod(ch);
  }
  restore_excitation_control(ch);
}

void NativeSynth::bend_range_msb(uint8_t channel, uint8_t semitones) noexcept {
  const uint8_t ch = channel & 0x0Fu;
  // Inside a zone the range is the zone's, and a value sent to one member
  // reaches every member of it (2.2.5), so the refresh is zone-wide.
  if (mpe_.apply_bend_sensitivity(ch, static_cast<float>(semitones))) {
    refresh_all_channel_mods();
  } else {
    channels_[ch].bend_range_cents = 100.0f * static_cast<float>(semitones);
    refresh_channel_mod(ch);
  }
}

void NativeSynth::bend_range_lsb(uint8_t channel, uint8_t cents) noexcept {
  const uint8_t ch = channel & 0x0Fu;
  ChannelState& st = channels_[ch];
  // The fractional semitone. MPE recommends senders leave it at zero and
  // permits rather than requires a receiver to answer it, so the zone
  // takes it on the same terms the channel always has.
  if (mpe_.role(ch) != MpeChannelRole::kUnassigned) {
    const float whole = std::floor(mpe_.bend_sensitivity(ch));
    mpe_.apply_bend_sensitivity(ch, whole + static_cast<float>(cents) / 100.0f);
    refresh_all_channel_mods();
  } else {
    st.bend_range_cents =
        100.0f * std::floor(st.bend_range_cents / 100.0f) + static_cast<float>(cents);
    refresh_channel_mod(ch);
  }
}

void NativeSynth::control_change(uint8_t channel, uint8_t controller, Control32 control) noexcept {
  const uint8_t ch = channel & 0x0Fu;
  ChannelState& st = channels_[ch];
  // Continuous controllers keep the full-width value for their float laws; the
  // switches, bank select, the pedals and the parameter-number machinery read
  // the 7-bit value.
  const uint8_t value = control.u7();
  st.cc_position[controller & 0x7Fu] = control;
  switch (controller) {
    case 0:
      // Bank select is prohibited on a member channel in MIDI Mode 3 and
      // permitted in Mode 4 (Appendix E Table 5), where a controller gives each
      // string its own program.
      if (mpe_.ignores(ch, MpeIgnorable::kBankSelect)) break;
      st.bank_msb = value;
      refresh_part_rig(ch);
      break;
    case 1:
      st.mod_wheel = control;
      refresh_channel_mod(ch);
      break;
    case 7:
      st.volume = control;
      refresh_channel_mod(ch);
      break;
    case 10:
      st.pan = control;
      refresh_channel_mod(ch);
      break;
    case 2:
      // The matrix source. Which axis CC2 additionally reaches, if any, is the
      // controller profile's to say and is applied before this switch runs.
      st.breath = control;
      refresh_channel_mod(ch);
      break;
    case 11:
      st.expression = control;
      refresh_channel_mod(ch);  // every engine's loudness rides the expression VCA
      break;
    case 32:
      if (mpe_.ignores(ch, MpeIgnorable::kBankSelect)) break;
      st.bank_lsb = value;
      refresh_part_rig(ch);
      break;
    case 6:
      // RPN 00 06 is the MPE Configuration Message, which every MPE-compatible
      // device shall support (2.2.1). It is tried first because it is the one
      // that can turn the zone model on.
      if (st.params.selected_rpn(0, 6)) {
        apply_mcm(ch, value);
      } else if (st.params.selected_rpn(0, 0)) {
        bend_range_msb(ch, value);
      }
      break;
    case 38:
      if (st.params.selected_rpn(0, 0)) bend_range_lsb(ch, value);
      break;
    case kMpeTimbreCc:
      // The third per-note dimension (2.2.8) reaches an axis through the
      // controller profile alone, which has already run, and the value itself
      // was tracked ahead of it -- a receiver shall go on tracking it while the
      // channel is silent so the next note starts from it. Nothing is left to
      // do here, and the case stands so the default below cannot claim it.
      break;
    case 64:
      sustain_cc(ch, control);
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
      all_notes_off(ch);
      break;
    case 126:
    case 127:
      // The two mode messages. Prohibited on a manager channel, where they are
      // ignored outright, and on a member channel they select the zone's mode
      // (2.2.4.3). Outside a zone they keep the channel-mode meaning they have
      // always had here, which is why the all-notes-off stays below them.
      if (mpe_.ignores(ch, MpeIgnorable::kModeMessage)) break;
      mpe_.apply_midi_mode(ch, controller == 126 ? MpeMidiMode::kMono : MpeMidiMode::kPoly);
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
    // Offline the stream carries the file's GS EFX; live the host pushes it
    // through on_control_sysex, so a SysEx scheduled in a clip is not applied.
    if (config_.realize_efx_inline && config_.use_gm_programs && part_fx_.enabled() &&
        event.sysex_payload != nullptr && event.sysex_payload_size > 0) {
      apply_efx_sysex(event.sysex_payload, event.sysex_payload_size);
    }
    return;
  }
  // Tracking comes ahead of both, because the profile reads the combined value
  // a member channel's controller carries and that value is only current once
  // the message in hand has been tracked.
  track_mpe_input(u);
  // The profile runs before the protocol dispatch so a note-on whose velocity
  // is bound to an axis starts from its own value rather than the previous
  // note's.
  apply_controller_input(u);
  ChannelVoiceEvent ev;
  if (!decode_channel_voice(u, &ev)) {
    ++skipped_events_;  // Reserved status.
    return;
  }
  const uint8_t ch = ev.channel & 0x0Fu;
  switch (ev.kind) {
    case ChannelVoiceKind::NoteOn:
      note_on(ch, ev.note, ev.velocity, ev.index, ev.attribute_data, event.source_track_id);
      break;
    case ChannelVoiceKind::NoteOff:
      note_off(ch, ev.note, event.source_track_id);
      break;
    case ChannelVoiceKind::PitchBend:
      channels_[ch].pitch_bend = ev.bend;
      mpe_.track_bend(ch, ev.bend);
      // A manager's bend applies to every sounding note in its zone (2.2.6), so
      // like its pressure it reaches past the channel it arrived on.
      if (mpe_.role(ch) == MpeChannelRole::kManager) {
        refresh_all_channel_mods();
      } else {
        refresh_channel_mod(ch);
      }
      break;
    case ChannelVoiceKind::ChannelPressure:
      channel_pressure(ch, ev.value);
      break;
    case ChannelVoiceKind::PolyPressure:
      poly_pressure(ch, ev.note, ev.value);
      break;
    case ChannelVoiceKind::ControlChange:
      control_change(ch, ev.note, ev.value);
      break;
    case ChannelVoiceKind::RegisteredController:
    case ChannelVoiceKind::AssignableController:
      registered_controller(u, ev);
      break;
    case ChannelVoiceKind::ProgramChange:
      // GS drum-kit select: in gm_kit mode the drum channel's program picks the
      // kit variation (Room/Power/808/...). Melodic patches ignore it.
      // In MIDI Mode 3 a zone is monotimbral, so a program change reaching a
      // member channel is ignored rather than splitting the zone across two
      // patches (2.3.3). Mode 4 is the case that permits it.
      if (mpe_.ignores(ch, MpeIgnorable::kProgramChange)) break;
      channels_[ch].program = ev.program;
      // The MIDI 2.0 bank-valid flag: the bank bytes carry meaning only when it
      // is set, and an unset flag leaves the channel's bank alone.
      if ((ev.flags & 0x01u) != 0) {
        channels_[ch].bank_msb = ev.bank_msb;
        channels_[ch].bank_lsb = ev.bank_lsb;
      }
      refresh_part_rig(ch);
      break;
    case ChannelVoiceKind::RelativeRegistered:
    case ChannelVoiceKind::RelativeAssignable:
      if (!relative_controller(ev)) ++skipped_events_;  // A parameter this synth does not hold.
      break;
    case ChannelVoiceKind::PerNotePitchBend:
      per_note_pitch_.set_per_note_bend(ch, ev.note, ev.bend);
      refresh_per_note_voices(ch, ev.note, false);
      break;
    case ChannelVoiceKind::RegisteredPerNote:
      if (ev.index != kRpncPitch725) {
        ++skipped_events_;
        break;
      }
      per_note_pitch_.set_pitch_7_25(ch, ev.note, ev.value);
      refresh_per_note_voices(ch, ev.note, false);
      break;
    case ChannelVoiceKind::AssignablePerNote:
      ++skipped_events_;
      break;
    case ChannelVoiceKind::PerNoteManagement:
      apply_per_note_management(
          per_note_pitch_, ch, ev.note, (ev.flags & 0x02u) != 0, (ev.flags & 0x01u) != 0,
          pool_.begin(), pool_.end(), [this](NativeSynthVoice& v) noexcept -> PerNoteBinding* {
            return v.active ? &per_note_[static_cast<size_t>(&v - pool_.data())].binding : nullptr;
          });
      refresh_per_note_voices(ch, ev.note, false);
      break;
  }
}

void NativeSynth::registered_controller(const Ump& ump, const ChannelVoiceEvent& ev) noexcept {
  const uint8_t ch = ev.channel & 0x0Fu;
  if (ev.kind == ChannelVoiceKind::RegisteredController && ev.bank == 0 &&
      ev.index == kRcPitchBendSensitivity) {
    // RPN 0/0's Data Entry MSB and LSB, read by truncation, applied in the order
    // the MIDI 1.0 gesture applies them.
    const uint16_t range14 = ev.value.u14();
    bend_range_msb(ch, static_cast<uint8_t>(range14 >> 7));
    bend_range_lsb(ch, static_cast<uint8_t>(range14 & 0x7Fu));
    return;
  }
  if (ev.kind == ChannelVoiceKind::RegisteredController && ev.bank == 0 &&
      ev.index == kRcPerNoteBendSensitivity) {
    per_note_bend_sensitivity_[ch] = ev.value;
    refresh_per_note_voices(ch, 0, true);
    return;
  }
  // The RPN / NRPN gesture in its MIDI 2.0 form takes the path its four MIDI
  // 1.0 messages do, so parameter selection and data entry treat it the same.
  const Midi1MessageList lowered = midi2_to_midi1_messages(ump);
  for (uint8_t i = 0; i < lowered.count; ++i) {
    control_change(ch, lowered.messages[i].note_number(),
                   Control32::from7(lowered.messages[i].data2_7bit()));
  }
}

bool NativeSynth::relative_controller(const ChannelVoiceEvent& ev) noexcept {
  if (ev.kind != ChannelVoiceKind::RelativeRegistered || ev.bank != 0) return false;
  const uint8_t ch = ev.channel & 0x0Fu;
  switch (ev.index) {
    case kRcPitchBendSensitivity: {
      // The held range in RPN 0/0 form (semitones in the top seven bits, cents below them), moved
      // and applied as the absolute message would be.
      const bool zoned = mpe_.role(ch) != MpeChannelRole::kUnassigned;
      const float held_cents = zoned ? mpe_.bend_sensitivity(ch) * constants::kCentsPerSemitone
                                     : channels_[ch].bend_range_cents;
      const int semitones =
          std::clamp(static_cast<int>(held_cents / constants::kCentsPerSemitone), 0, 127);
      const int cents = std::clamp(
          static_cast<int>(held_cents -
                           constants::kCentsPerSemitone * static_cast<float>(semitones) + 0.5f),
          0, 127);
      const auto held = static_cast<uint32_t>((semitones << 7) | cents) << 18;
      const uint16_t range14 = add_saturating(Control32::from_raw(held), ev.value).u14();
      bend_range_msb(ch, static_cast<uint8_t>(range14 >> 7));
      bend_range_lsb(ch, static_cast<uint8_t>(range14 & 0x7Fu));
      return true;
    }
    case kRcPerNoteBendSensitivity:
      per_note_bend_sensitivity_[ch] = add_saturating(per_note_bend_sensitivity_[ch], ev.value);
      refresh_per_note_voices(ch, 0, true);
      return true;
    default:
      return false;
  }
}

void NativeSynth::bind_per_note(Sf2PerNoteVoice& state, uint8_t channel, uint8_t note,
                                uint8_t attribute_type, uint16_t attribute_data,
                                uint8_t zone_key) const noexcept {
  state.binding.bind(channel, note);
  state.has_attribute_pitch = attribute_type == kAttributePitch79;
  state.attribute_pitch_q7_9 = state.has_attribute_pitch ? attribute_data : uint16_t{0};
  state.zone_key = zone_key;
  state.cents = 0.0f;
}

ComposedPitch NativeSynth::compose_per_note(const Sf2PerNoteVoice& state) const noexcept {
  NotePitchRequest req;
  req.note = state.binding.note;
  req.has_attribute_pitch = state.has_attribute_pitch;
  req.attribute_pitch_q7_9 = state.attribute_pitch_q7_9;
  req.per_note = state.binding.pitch_inputs(per_note_pitch_);
  req.per_note_bend_sensitivity = per_note_bend_sensitivity_[state.binding.channel & 0x0Fu];
  // The channel terms stay on the channel mod; only the per-note share is taken.
  return compose_note_pitch(req);
}

void NativeSynth::refresh_per_note_pitch(Sf2PerNoteVoice& state) const noexcept {
  state.cents = per_note_cents(state, compose_per_note(state));
}

void NativeSynth::refresh_per_note_voices(uint8_t channel, uint8_t note, bool all_notes) noexcept {
  const uint8_t ch = channel & 0x0Fu;
  for (size_t i = 0; i < per_note_.size(); ++i) {
    Sf2PerNoteVoice& state = per_note_[i];
    if (pool_.data()[i].active && state.binding.channel == ch &&
        (all_notes || state.binding.note == note)) {
      refresh_per_note_pitch(state);
    }
  }
}

void NativeSynth::carry_per_note(const NativeSynthVoice& voice, uint8_t from_note, uint8_t to_note,
                                 uint8_t attribute_type, uint16_t attribute_data) noexcept {
  Sf2PerNoteVoice& state = per_note_[static_cast<size_t>(&voice - pool_.data())];
  // retune() moves the engine by the interval between the keys, so the key it nominally sounds
  // moves by the same interval from the one it was started on.
  const int zone_key = std::clamp(
      static_cast<int>(state.zone_key) + static_cast<int>(to_note) - static_cast<int>(from_note), 0,
      127);
  bind_per_note(state, voice.channel, to_note, attribute_type, attribute_data,
                static_cast<uint8_t>(zone_key));
  refresh_per_note_pitch(state);
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
  // Offline a pending rig or EFX change is realised here, before the block
  // renders (this allocates); live the control thread has already published it.
  if (part_fx_.enabled()) {
    if (config_.realize_efx_inline && part_fx_.dirty()) realize_part_fx();
    part_fx_.acquire();
    part_fx_.drain_param_updates();
    part_fx_.apply_controls(*this);
  }
  // Diagnostic, offline only: the path this block renders through.
  if (path_recorder_ != nullptr) record_render_path();
  const PartFxSnapshot* fx = part_fx_.enabled() ? part_fx_.current() : nullptr;
  // With no part bussed every sample takes exactly the arithmetic it took
  // before the stage existed.
  const bool any_bussed = fx != nullptr && fx->any_bussed && part_fx_.has_buses();
  const bool any_unit = any_bussed && fx->any_unit && part_fx_.has_unit_buses();
  const uint64_t fx_discards_before = any_bussed ? part_fx_.discard_sum(nullptr) : 0;
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
  // A bus output reaches the lanes of the voices that fed it, at the master gain.
  const auto flush_bus = [&](SourceResidualSplitter& splitter, const float* bus_l,
                             const float* bus_r, int n, int offset) noexcept {
    float scaled_l[kPartFxChunkFrames];
    float scaled_r[kPartFxChunkFrames];
    for (int c = 0; c < n; ++c) {
      scaled_l[c] = bus_l[c] * config_.gain;
      scaled_r[c] = bus_r[c] * config_.gain;
    }
    splitter.flush(source_outputs, source_output_count, n, scaled_l, scaled_r, offset, add_output);
  };

  // Sympathetic resonance is gated by the dampers being lifted on any channel
  // (sustain pedal down). Sustain state is fixed for the block (events are
  // applied before process()). Shared by the piano board and the guitar
  // halo below -- two bus-level banks, one gate rule.
  bool damper_open = false;
  if (piano_body_active_ || guitar_halo_active_) {
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
    uint32_t closed = Control32::from7(127).raw;
    for (const ChannelState& ch : channels_) closed = std::min(closed, ch.expression.raw);
    const float shut = (1.0f - Control32::from_raw(closed).f7() / 127.0f) * swell_depth_;
    const float fc = std::exp(std::log(20000.0f) + shut * (std::log(300.0f) - std::log(20000.0f)));
    if (fc < 19000.0f) {
      swell_active = true;
      swell_coeff_ = std::clamp(
          1.0f - std::exp(-constants::kTwoPi * fc / static_cast<float>(sample_rate_)), 0.0f, 1.0f);
    }
  }

  // Set when any sample in this call's per-sample loop below actually
  // discarded; bumped once after the loop rather than per sample, since the
  // unit is one process() call, not one sample.
  bool discarded = false;
  for (int offset = 0; offset < num_samples; offset += kPartFxChunkFrames) {
    const int n = std::min(kPartFxChunkFrames, num_samples - offset);
    if (any_bussed) part_fx_.clear_part_buses();
    // First pass: the voices. A bussed part's go into its bus; every other
    // voice is summed as it always was.
    for (int c = 0; c < n; ++c) {
      const int i = offset + c;
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
      // Piano voices are summed apart from the rest: the body below attenuates
      // and re-radiates only them, so in GM mode — where one bus carries many
      // programs — the flute sharing the render must not be pulled through the
      // soundboard. With a single-patch configuration one of the two legs stays
      // at zero, so the sum is unchanged.
      float piano_l = 0.0f;
      float piano_r = 0.0f;
      // Guitar/harp/banjo halo drive: the same isolation as the piano leg above,
      // so drums and brass sharing a GM bus never reach a bank tuned to open
      // strings. Unlike the piano leg this one stays IN mix_l/mix_r too — the
      // halo is additive to the dry KS voice rather than replacing it.
      float guitar_l = 0.0f;
      float guitar_r = 0.0f;
      float board_l = 0.0f;
      float board_r = 0.0f;
      std::array<float, 16> halo_dry;
      if (any_bussed) halo_dry.fill(0.0f);
      for (NativeSynthVoice& v : pool_) {
        if (!v.active) continue;
        // The channel's, except on a member channel sounding more than one note,
        // where the voice carries the part of the channel's the note was
        // attributed (M1-100-UM v1.1 section 2.2.4.1).
        const Sf2ChannelMod& channel_mod =
            v.mpe_mod_active ? v.mpe_mod : channel_mods_[v.channel & 0x0Fu];
        // A key carrying per-note pitch renders through a copy of that mod with its offset
        // added; every other voice reads the mod itself.
        const float per_note_offset = per_note_[static_cast<size_t>(&v - pool_.data())].cents;
        float s = 0.0f;
        if (per_note_offset == 0.0f) {
          s = v.render(channel_mod, wind.pitch_ratio, wind.gain);
        } else {
          Sf2ChannelMod tuned = channel_mod;
          tuned.pitch_cents += per_note_offset;
          s = v.render(tuned, wind.pitch_ratio, wind.gain);
        }
        const bool piano_voice =
            piano_body_active_ && v.patch != nullptr && v.patch->mode == SynthEngineMode::kPiano;
        const bool halo_voice = v.patch != nullptr &&
                                v.patch->mode == SynthEngineMode::kKarplusStrong &&
                                v.patch->ks.sympathetic;
        const int part = v.channel & 0x0F;
        if (any_bussed && fx->part_bussed[static_cast<size_t>(part)]) {
          // The rig's processors are recursive state a non-finite sample would poison.
          discarded |= resolve_non_finite(SampleDestination::kRecursiveState, s);
          const float bus_l = s * v.gain_left;
          const float bus_r = s * v.gain_right;
          const bool mono_rig = fx->mono_prefix[static_cast<size_t>(part)] != 0;
          // A piano's direct share rides the bus; its board stays the shared one.
          const float direct = piano_voice ? kPianoDirectGain : 1.0f;
          if (mono_rig) {
            // A mono pickup feeds the rig's amp, so CC10 cannot move its drive.
            part_fx_.add_mono(part, c, direct * constants::kInvSqrt2 * s);
          } else {
            part_fx_.add_stereo(part, c, direct * bus_l, direct * bus_r);
          }
          if (piano_voice) {
            board_l += bus_l;
            board_r += bus_r;
          }
          if (halo_voice && part_halo_armed_[static_cast<size_t>(part)]) {
            halo_dry[static_cast<size_t>(part)] +=
                mono_rig ? constants::kInvSqrt2 * s : 0.5f * (bus_l + bus_r);
          }
          if (source_render) {
            const float src_l = bus_l * config_.gain;
            const float src_r = bus_r * config_.gain;
            residual_splitter_.accumulate(v.source_track_id, src_l, src_r);
            if (piano_voice) piano_residual_splitter_.accumulate(v.source_track_id, src_l, src_r);
            const uint8_t unit = fx->part_unit[static_cast<size_t>(part)];
            SourceResidualSplitter& bus_splitter =
                unit != PartFxSnapshot::kNoUnit && any_unit
                    ? unit_splitters_[unit]
                    : part_bus_splitters_[static_cast<size_t>(part)];
            bus_splitter.accumulate(v.source_track_id, src_l, src_r);
          }
          continue;
        }
        const float voice_l = s * v.gain_left;
        const float voice_r = s * v.gain_right;
        if (piano_voice) {
          piano_l += voice_l;
          piano_r += voice_r;
        } else {
          mix_l += voice_l;
          mix_r += voice_r;
          if (guitar_halo_active_ && halo_voice) {
            guitar_l += voice_l;
            guitar_r += voice_r;
          }
        }
        if (source_render) {
          const float src_l = voice_l * config_.gain;
          const float src_r = voice_r * config_.gain;
          add_output(target_for(v.source_track_id), i, src_l, src_r);
          // The remainder follows every source; the piano body and guitar halo
          // only the voices that drive them.
          residual_splitter_.accumulate(v.source_track_id, src_l, src_r);
          if (piano_voice) piano_residual_splitter_.accumulate(v.source_track_id, src_l, src_r);
          if (guitar_halo_active_ && halo_voice) {
            guitar_residual_splitter_.accumulate(v.source_track_id, src_l, src_r);
          }
        }
      }
      chunk_mix_l_[static_cast<size_t>(c)] = mix_l;
      chunk_mix_r_[static_cast<size_t>(c)] = mix_r;
      chunk_piano_l_[static_cast<size_t>(c)] = piano_l;
      chunk_piano_r_[static_cast<size_t>(c)] = piano_r;
      chunk_guitar_l_[static_cast<size_t>(c)] = guitar_l;
      chunk_guitar_r_[static_cast<size_t>(c)] = guitar_r;
      if (any_bussed) {
        chunk_board_l_[static_cast<size_t>(c)] = board_l;
        chunk_board_r_[static_cast<size_t>(c)] = board_r;
        // A bussed part's open strings ring into its own bus, gated by its own
        // pedal, so the halo goes through the part's rig with the voice.
        for (size_t part = 0; part < 16; ++part) {
          if (!part_halo_armed_[part] || !fx->part_bussed[part]) continue;
          const float add = part_halo_[part].process(halo_dry[part], channels_[part].sustain);
          if (add == 0.0f) continue;
          if (fx->mono_prefix[part] != 0) {
            part_fx_.add_mono(static_cast<int>(part), c, add);
          } else {
            part_fx_.add_stereo(static_cast<int>(part), c, add, add);
          }
        }
      }
    }

    // The rig chains, then each bussed part into its insertion unit or straight
    // to the mix, ahead of the master gain.
    if (any_bussed) {
      if (any_unit) part_fx_.clear_unit_buses();
      part_fx_.run_part_chains(n, fx->part_bussed, fx->mono_prefix, *this);
      for (int part = 0; part < 16; ++part) {
        if (!fx->part_bussed[static_cast<size_t>(part)]) continue;
        const float* bus_l = part_fx_.bus_l(part);
        const float* bus_r = part_fx_.bus_r(part);
        const uint8_t unit = fx->part_unit[static_cast<size_t>(part)];
        if (unit != PartFxSnapshot::kNoUnit && any_unit) {
          float* unit_l = part_fx_.unit_bus_l(unit);
          float* unit_r = part_fx_.unit_bus_r(unit);
          for (int c = 0; c < n; ++c) {
            unit_l[c] += bus_l[c];
            unit_r[c] += bus_r[c];
          }
          continue;
        }
        for (int c = 0; c < n; ++c) {
          chunk_mix_l_[static_cast<size_t>(c)] += bus_l[c];
          chunk_mix_r_[static_cast<size_t>(c)] += bus_r[c];
        }
        if (source_render) {
          flush_bus(part_bus_splitters_[static_cast<size_t>(part)], bus_l, bus_r, n, offset);
        }
      }
      if (any_unit) {
        // One pass per unit over the sum of the parts feeding it.
        part_fx_.run_units(n, fx->unit_fed, nullptr);
        for (size_t unit = 0; unit < kGsEfxUnitCount; ++unit) {
          if (!fx->unit_fed[unit]) continue;
          const float* unit_l = part_fx_.unit_bus_l(unit);
          const float* unit_r = part_fx_.unit_bus_r(unit);
          for (int c = 0; c < n; ++c) {
            chunk_mix_l_[static_cast<size_t>(c)] += unit_l[c];
            chunk_mix_r_[static_cast<size_t>(c)] += unit_r[c];
          }
          if (source_render) flush_bus(unit_splitters_[unit], unit_l, unit_r, n, offset);
        }
      }
    }

    // Second pass: the bus-level post stages, in the order they always ran.
    for (int c = 0; c < n; ++c) {
      const int i = offset + c;
      const size_t at = static_cast<size_t>(c);
      float mix_l = chunk_mix_l_[at] * config_.gain;
      float mix_r = chunk_mix_r_[at] * config_.gain;
      const float piano_l = chunk_piano_l_[at] * config_.gain;
      const float piano_r = chunk_piano_r_[at] * config_.gain;
      const float guitar_l = chunk_guitar_l_[at] * config_.gain;
      const float guitar_r = chunk_guitar_r_[at] * config_.gain;
      const float dry_l = mix_l + piano_l;
      const float dry_r = mix_r + piano_r;
      // Swell box shutter: a one-pole lowpass on the bus as the louvres close.
      if (swell_active) {
        swell_lp_l_ += swell_coeff_ * (mix_l - swell_lp_l_);
        swell_lp_r_ += swell_coeff_ * (mix_r - swell_lp_r_);
        mix_l = swell_lp_l_;
        mix_r = swell_lp_r_;
      }
      // Shared modal soundboard plus pedal-gated sympathetic resonance, both
      // driven by the summed dry piano mix. The sympathetic bank returns to the
      // centre; the board does not, because its two radiation paths differ. Runs
      // independently of the halo below them, since GM can voice a piano and a
      // guitar together.
      // What the piano body and the guitar halo add beyond their dry voices.
      float piano_res_l = 0.0f;
      float piano_res_r = 0.0f;
      float guitar_res_l = 0.0f;
      float guitar_res_r = 0.0f;
      if (piano_body_active_) {
        float drive_l = piano_l;
        float drive_r = piano_r;
        if (any_bussed) {
          drive_l += chunk_board_l_[at] * config_.gain;
          drive_r += chunk_board_r_[at] * config_.gain;
        }
        // Radiation split: the board returns the phase-diffused complement of
        // the direct share (plus the modal colour), so most of the note reaches
        // the mix through the board rather than as the raw string waveform.
        const float dry_mono = 0.5f * (drive_l + drive_r);
        const float body = soundboard_.process(dry_mono);
        // The board's two radiation paths differ in phase, so its return is not
        // the same signal on both legs. Zero at a zero board width.
        const float side = soundboard_.last_side();
        const float symp = resonance_.process(soundboard_.last_diffused(), damper_open);
        const float piano_out_l = kPianoDirectGain * piano_l + body + side + symp;
        const float piano_out_r = kPianoDirectGain * piano_r + body - side + symp;
        piano_res_l = piano_out_l - piano_l;
        piano_res_r = piano_out_r - piano_r;
        mix_l += piano_out_l;
        mix_r += piano_out_r;
      }
      if (guitar_halo_active_) {
        // Plucked-string sound halo: the open strings ring behind the note,
        // gated by damper_open exactly as the piano board is above -- a guitar's
        // open strings are damped unless the sustain pedal is holding them open.
        // Driven by guitar_l/guitar_r alone (the halo-eligible voices' own dry
        // mix), never by the rest of the GM bus. Skipped entirely when no
        // eligible voice has ever sounded, so every existing KS voicing with no
        // halo renders bit-identically.
        const float dry_mono = 0.5f * (guitar_l + guitar_r);
        const float symp = guitar_halo_.process(dry_mono, damper_open);
        guitar_res_l = symp;
        guitar_res_r = symp;
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
      // input, which is not the same as safe for it -- the blocker is a feedback
      // cell whose worst-case gain is well over unity, so a large enough finite
      // sample still leaves float range. Mirrors the host-side scrub in
      // au_instrument_provider.
      discarded |= resolve_non_finite(SampleDestination::kRecursiveState, mix_l);
      discarded |= resolve_non_finite(SampleDestination::kRecursiveState, mix_r);
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
        // Shared bodies, bus drive and DC filtering are destination-scoped; their
        // residual (mix minus dry, where a bus output counts as dry) is split in
        // three components, each across the sources that produced it. With one
        // live source each split adds its share unmultiplied, so that source's
        // target is bit-identical to a slot-0-only render; with several it is
        // exact only up to floating-point rounding. Staged per chunk and flushed
        // at its end.
        piano_residual_l_[at] = piano_res_l;
        piano_residual_r_[at] = piano_res_r;
        guitar_residual_l_[at] = guitar_res_l;
        guitar_residual_r_[at] = guitar_res_r;
        residual_l_[at] = (mix_l - dry_l) - piano_res_l - guitar_res_l;
        residual_r_[at] = (mix_r - dry_r) - piano_res_r - guitar_res_r;
      } else {
        if (left != nullptr) {
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
    if (source_render) {
      residual_splitter_.flush(source_outputs, source_output_count, n, residual_l_.data(),
                               residual_r_.data(), offset, add_output);
      piano_residual_splitter_.flush(source_outputs, source_output_count, n,
                                     piano_residual_l_.data(), piano_residual_r_.data(), offset,
                                     add_output);
      guitar_residual_splitter_.flush(source_outputs, source_output_count, n,
                                      guitar_residual_l_.data(), guitar_residual_r_.data(), offset,
                                      add_output);
    }
  }
  if (any_bussed && part_fx_.discard_sum(nullptr) != fx_discards_before) discarded = true;
  if (discarded) note_non_finite_discard();
}

}  // namespace sonare::midi::synth
