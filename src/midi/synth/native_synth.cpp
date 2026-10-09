#include "midi/synth/native_synth.h"

#include <algorithm>
#include <cmath>

#include "midi/builtin_synth.h"
#include "midi/synth/articulation.h"
#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/gs_address_table.h"
#include "midi/synth/voice_random.h"
#include "midi/ump.h"
#include "util/constants.h"
#include "util/non_finite_sample.h"
#include "util/numeric_validation.h"

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

using native_synth_detail::excitation_axis_mask;
using native_synth_detail::excitation_value;

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

int64_t NativeSynth::recompute_tail() const noexcept {
  // GM mode resolves the patch per program at note-on, so the fallback tables bound it.
  const bool gm_tables =
      config_.use_gm_programs ||
      (config_.patch.mode == SynthEngineMode::kPercussion && config_.patch.percussion.gm_kit);
  const EnvelopeTimeScales scales = gs_tail_scales();
  int64_t tail =
      gm_tables
          ? gm_fallback_max_tail_samples(sample_rate_, scales.attack, scales.decay, scales.release)
          : native_patch_tail_samples(config_.patch, sample_rate_, scales, sample_bank_);
  if (guitar_halo_active_ || gm_tables) {
    // The halo rings past the last voice; GM counts it since prepare() cannot know the program.
    tail = numeric::saturating_add(tail,
                                   numeric::ceil_sample_count(sample_rate_ * kKsSympatheticRingS));
  }
  if (piano_mode_ || gm_tables) {
    // The piano body rings past the voice release; in GM mode program 0 is the piano.
    tail =
        numeric::saturating_add(tail, numeric::ceil_sample_count(sample_rate_ * kPianoBodyRingS));
  }
  return tail;
}

void NativeSynth::raise_tail() noexcept {
  if (!prepared_) return;
  const int64_t tail = recompute_tail();
  int64_t current = tail_samples_->load(std::memory_order_relaxed);
  while (tail > current &&
         !tail_samples_->compare_exchange_weak(current, tail, std::memory_order_relaxed)) {
  }
}

bool NativeSynth::materialize_tail_probe() {
  if (!prepared_ || !config_.realize_efx_inline || !part_fx_.enabled()) return true;
  if (part_fx_.dirty()) realize_part_fx();
  part_fx_.settle_quiescent(*this);
  return true;
}

EnvelopeTimeScales NativeSynth::gs_tail_scales() const noexcept {
  return gs_slowest_eg_time_scales(
      channels_, [](const ChannelState& st) -> const GsPartParams& { return st.gs; });
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
  for (SourceResidualSplitter& s : part_piano_splitters_) {
    s.configure(sample_rate_, kResidualTauSeconds);
    s.reset();
  }
  for (SourceResidualSplitter& s : part_organ_splitters_) {
    s.configure(sample_rate_, kResidualTauSeconds);
    s.reset();
  }
  for (SourceResidualSplitter& s : part_guitar_splitters_) {
    s.configure(sample_rate_, kResidualTauSeconds);
    s.reset();
  }
  part_fx_.clear_mirror();
  part_fx_.prepare(sample_rate_);
  for (PianoResonanceBank& halo : part_halo_) halo.reset();
  part_halo_armed_.fill(false);
  part_soundboards_.clear();
  part_resonance_.clear();
  if (part_fx_.enabled()) {
    part_soundboards_.resize(16);
    part_resonance_.resize(16);
  }
  for (PianoSoundboard& board : part_soundboards_) board.reset();
  for (PianoResonanceBank& bank : part_resonance_) bank.reset();
  for (PianoPartState& state : part_piano_state_) state = {};
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
  // Organ wind and swell arm per part at note-on, since GM programs resolve after prepare().
  for (OrganWindSupply& wind : part_wind_) wind.reset();
  for (OrganPartParams& params : organ_part_params_) params = {};
  part_swell_depth_.fill(0.0f);
  part_swell_coeff_.fill(1.0f);
  part_swell_lp_l_.fill(0.0f);
  part_swell_lp_r_.fill(0.0f);
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
  channels_ = {};
  // GM power-on: channel 10 is the rhythm part (no SysEx needed). MPE mode is
  // off until an MCM turns it on, which is what a power-on default of "no zone
  // configured" means (2.2.1); the refresh below therefore takes the ordinary
  // per-channel path.
  channels_[kDrumChannelIndex].drums = true;
  mpe_.reset();
  bus_fed_channels_ = 0;
  refresh_all_channel_mods();
  tail_samples_->store(recompute_tail(), std::memory_order_relaxed);
  // Mix-bus polish: ~8 Hz DC blocker pole and the gain-neutral drive factor.
  dc_r_ = 1.0f - static_cast<float>(constants::kTwoPiD * 8.0 / sample_rate_);
  dc_x1_ = {};
  dc_y1_ = {};
  bus_drive_gain_ = config_.bus_drive > 0.0f ? 1.0f + 3.0f * config_.bus_drive : 0.0f;
  live_dirty_ = 0;
  prepared_ = true;
  // The rig entries and the bank rigs the parts' power-on programs bind, so a
  // bussed part routes from the first block.
  for (uint8_t ch = 0; ch < 16; ++ch) refresh_part_rig(ch);
  part_fx_.publish();
  part_fx_.settle_quiescent(*this);
  part_fx_.clear_dirty();
}

void NativeSynth::reset() {
  pool_.reset();
  live_dirty_ = 0;
  dc_x1_ = {};
  dc_y1_ = {};
  residual_splitter_.reset();
  piano_residual_splitter_.reset();
  guitar_residual_splitter_.reset();
  for (SourceResidualSplitter& s : part_bus_splitters_) s.reset();
  for (SourceResidualSplitter& s : unit_splitters_) s.reset();
  for (SourceResidualSplitter& s : part_piano_splitters_) s.reset();
  for (SourceResidualSplitter& s : part_organ_splitters_) s.reset();
  for (SourceResidualSplitter& s : part_guitar_splitters_) s.reset();
  part_fx_.clear_mirror();
  resonance_.reset();
  soundboard_.reset();
  guitar_halo_.reset();
  for (PianoResonanceBank& halo : part_halo_) halo.reset();
  part_halo_armed_.fill(false);
  for (PianoSoundboard& board : part_soundboards_) board.reset();
  for (PianoResonanceBank& bank : part_resonance_) bank.reset();
  for (PianoPartState& state : part_piano_state_) state = {};
  // A GM-mode body was tuned by a note-on, so it goes back to untuned; a
  // configured piano keeps the tuning prepare() gave it.
  piano_body_active_ = piano_mode_;
  if (!piano_mode_) piano_body_soundboard_ = -1.0f;
  // Same rule for the halo: a GM-mode arming goes back to unarmed, while a
  // configured Karplus-Strong sympathetic patch keeps the arming prepare() gave it.
  guitar_halo_active_ =
      config_.patch.mode == SynthEngineMode::kKarplusStrong && config_.patch.ks.sympathetic;
  for (OrganWindSupply& wind : part_wind_) wind.reset();
  for (OrganPartParams& params : organ_part_params_) params = {};
  part_swell_depth_.fill(0.0f);
  part_swell_coeff_.fill(1.0f);
  part_swell_lp_l_.fill(0.0f);
  part_swell_lp_r_.fill(0.0f);
  channels_ = {};
  // GM power-on: channel 10 is the rhythm part (no SysEx needed). MPE mode is
  // off until an MCM turns it on, which is what a power-on default of "no zone
  // configured" means (2.2.1); the refresh below therefore takes the ordinary
  // per-channel path.
  channels_[kDrumChannelIndex].drums = true;
  mpe_.reset();
  bus_fed_channels_ = 0;
  skipped_events_ = 0;
  per_note_.assign(per_note_.size(), Sf2PerNoteVoice{});
  per_note_pitch_.clear();
  per_note_bend_sensitivity_.fill(kDefaultPerNoteBendSensitivity);
  refresh_all_channel_mods();
  for (uint8_t ch = 0; ch < 16; ++ch) refresh_part_rig(ch);
  // A fresh snapshot rebuilds the chains, which is their reset.
  if (prepared_) {
    part_fx_.publish();
    part_fx_.settle_quiescent(*this);
  }
  part_fx_.clear_dirty();
  if (prepared_) tail_samples_->store(recompute_tail(), std::memory_order_relaxed);
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

void NativeSynth::refresh_channel_mods_in_scope(uint8_t channel) noexcept {
  const uint16_t scope = mpe_.channels_in_scope(channel);
  for (uint8_t ch = 0; ch < 16; ++ch) {
    if ((scope & (uint16_t{1} << ch)) != 0) refresh_channel_mod(ch);
  }
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

/// The deepest pitch any SOUNDING rank of @p patch plays, relative to the key. A pipe organ voices
/// one key at several pitches at once, so its lowest audible rank runs out of delay line first
/// and decides whether the voice can be carried; a muted rank decides nothing, and an
/// upperwork-only registration reaches above 1. 1.0 for every other engine and for the implicit
/// single rank, which sounds the key and nothing below it.
float lowest_pitch_mult(const NativeSynthPatch& patch) noexcept {
  if (patch.mode != SynthEngineMode::kPipeOrgan || patch.pipe_organ.rank_count <= 0) return 1.0f;
  float lowest = 0.0f;
  const int count = std::min(patch.pipe_organ.rank_count, kMaxPipeRanks);
  for (int r = 0; r < count; ++r) {
    const PipeOrganRank& rank = patch.pipe_organ.ranks[static_cast<size_t>(r)];
    if (!rank.sounding()) continue;
    // A footage the voice cannot use sounds at the key, which is how the voice reads it too.
    const float mult = rank.footage_mult > 0.01f ? rank.footage_mult : 1.0f;
    if (lowest == 0.0f || mult < lowest) lowest = mult;
  }
  return lowest > 0.0f ? lowest : 1.0f;
}

/// The integer key an engine is started on for @p note under @p pitch: the note itself, or the
/// floor of an absolute pitch, which is also what picks a drum piece or a sample zone.
uint8_t voiced_key(uint8_t note, const ComposedPitch& pitch) noexcept {
  if (!pitch.absolute) return note;
  return static_cast<uint8_t>(std::clamp(
      static_cast<int>(std::floor(static_cast<double>(note) + pitch.per_note_semitones)), 0, 127));
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

NativeSynth::LegatoResult NativeSynth::legato_continue(uint8_t ch, bool release, uint8_t note,
                                                       uint8_t attribute_type,
                                                       uint16_t attribute_data,
                                                       uint32_t source_track_id,
                                                       const NativeSynthPatch* patch) noexcept {
  ChannelState& st = channels_[ch];
  const bool legato = st.articulation == ArticulationMode::kMonoLegato;
  NativeSynthVoice* voice = nullptr;
  uint8_t to_note = note;
  uint8_t to_type = attribute_type;
  uint16_t to_data = attribute_data;
  if (release) {
    if (!legato) return {};
    const ChannelState::HeldKey* back = st.newest_held_key(true, source_track_id);
    if (back == nullptr) return {};
    voice = find_sounding(ch, note, source_track_id);
    if (voice == nullptr || voice->patch == nullptr) return {};
    to_note = back->note;
    to_type = back->attribute_type;
    to_data = back->attribute_data;
  } else {
    if (st.articulation == ArticulationMode::kPoly) return {};
    // The channel is monophonic across sources: the voice to continue from is whoever sounds
    // the newest held key.
    const ChannelState::HeldKey* held = st.newest_held_key();
    if (held == nullptr) return {};
    voice = find_sounding(ch, held->note, held->source_track_id);
    if (voice == nullptr) return {};
  }

  // Reach is a question about the sounding pitch, not the binding key the pitch was composed from.
  Sf2PerNoteVoice target;
  bind_per_note(target, ch, to_note, to_type, to_data, to_note);
  const ComposedPitch to_pitch = compose_per_note(target);
  const size_t index = static_cast<size_t>(voice - pool_.data());
  const bool carry =
      legato && voice->patch != nullptr && voice->source_track_id == source_track_id &&
      (release || voice->patch == patch) &&
      accepts_legato(voice->patch->mode,
                     voiced_key(voice->note, compose_per_note(per_note_[index])),
                     voiced_key(to_note, to_pitch), lowest_pitch_mult(*voice->patch));
  if (!carry) {
    // Counted, because nothing in the sound says whether the phrase ended or was declined.
    if (legato) ++legato_fallbacks_;
    if (!release) {
      // Monophonic either way: the previous note stops. Fast rather than the patch's own release,
      // which on a sustaining patch runs past a second and would leave the note it replaced
      // audible under the new one.
      freeze_mpe_voice(*voice, ch);
      voice->choke_fast(sample_rate_);
    }
    return {LegatoOutcome::kReplaced, nullptr};
  }
  const float cents_before = per_note_[index].cents;
  carry_per_note(*voice, voice->note, to_note, to_type, to_data);
  voice->retune(to_note, voiced_key(to_note, to_pitch), per_note_[index].cents - cents_before,
                sample_rate_);
  st.last_freq_hz = voice->voiced_freq_hz();
  return {LegatoOutcome::kCarried, voice};
}

void NativeSynth::note_on(uint8_t channel, uint8_t note, Velocity16 velocity,
                          uint8_t attribute_type, uint16_t attribute_data, uint32_t source_track_id,
                          const ExcitationAxes& velocity_excitation,
                          uint32_t velocity_excitation_mask) noexcept {
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
  // Compose the per-note pitch first: an absolute pitch picks the drum piece at the target key.
  Sf2PerNoteVoice per_note;
  bind_per_note(per_note, ch, note, attribute_type, attribute_data, note);
  const ComposedPitch note_pitch = compose_per_note(per_note);
  const uint8_t drum_lookup_note = voiced_key(note, note_pitch);
  const NativeSynthPatch* patch = &config_.patch;
  uint8_t drum_kit = 0;
  if (config_.use_gm_programs) {
    const uint16_t bank = gs_effective_bank(st.bank_msb, st.bank_lsb, st.drums);
    const GsToneMap map = gs_effective_tone_map(st.bank_msb, st.bank_lsb);
    if (bank == kDrumBank) {
      patch = &gm_fallback_drum_patch(drum_lookup_note);
      drum_kit = gm_fallback_drum_kit(st.program, map);
    } else {
      patch = &gm_fallback_patch(bank, st.program, map);
    }
  } else if (patch->mode == SynthEngineMode::kPercussion && patch->percussion.gm_kit) {
    patch = &gm_fallback_drum_patch(drum_lookup_note);
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
        if (v.key_down) freeze_mpe_voice(v, ch);
        v.choke();
      }
    }
  }
  // Legato continuation, before a voice is allocated: on a carrying channel a
  // note-on under a held key moves that voice to the new key rather than
  // starting one, so the exciter, the delay line and both envelopes run on.
  ChannelState& live = channels_[ch];
  const LegatoResult legato =
      legato_continue(ch, false, note, attribute_type, attribute_data, source_track_id, patch);
  if (legato.outcome == LegatoOutcome::kCarried) {
    if (velocity_excitation_mask != kAxisNone) {
      record_velocity_axes(*legato.voice, velocity_excitation, velocity_excitation_mask);
      legato.voice->push_excitation(velocity_excitation, velocity_excitation_mask);
    }
    live.hold_key(note, attribute_type, attribute_data, source_track_id);
    return;
  }
  live.hold_key(note, attribute_type, attribute_data, source_track_id);

  NativeSynthVoice* voice = pool_.allocate(ch, note, source_track_id);
  if (voice == nullptr) return;
  bus_fed_channels_ |= static_cast<uint16_t>(uint16_t{1} << ch);
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
  DrumVoiceMod voice_mod{};
  if (note_pitch.absolute) {
    per_note.zone_key = voiced_key(note, note_pitch);
    voice_mod.play_note = per_note.zone_key;
  }
  per_note.cents = per_note_cents(per_note, note_pitch);
  const GsPartMod part_mod = gs_part_mod(st.gs);
  voice->start(*patch, sample_rate_, velocity, voice_index, glide_from, st.una_corda, drum_kit,
               voice_mod, organ_percussion, part_mod);
  voice->piano_case_strike_pending = 0.0f;
  voice->piano_board_strike_pending = 0.0f;
  voice->mpe_mod_active = false;
  voice->mpe_release_frozen = false;
  voice->mpe_frozen_axis_mask = kAxisNone;
  voice->mpe_frozen_axis_present = kAxisNone;
  voice->mpe_frozen_source_mask = kAxisNone;
  voice->mpe_frozen_axis_values = {};
  voice->mpe_frozen_member_bend_cents = 0.0f;
  voice->mpe_frozen_aftertouch01 = 0.0f;
  voice->live_excitation_axes.reset();
  voice->live_mpe_pressure_axes = kAxisNone;
  voice->live_mpe_timbre_axes = kAxisNone;
  voice->live_mpe_bend_axes = kAxisNone;
  voice->mpe_member_pressure = Control32::from_raw(0);
  voice->mpe_member_timbre = Control32::from_raw(0);
  voice->mpe_member_pressure_present = false;
  voice->mpe_member_timbre_present = false;
  per_note_[voice_index] = per_note;
  // Seed the engine's excitation axes at the channel's current controllers (no
  // glide on the first sample) so a note struck mid-phrase starts at the live
  // breath / brightness rather than gliding in from the preset. An engine reads
  // only the axes it declares, and one that declares none is untouched.
  {
    uint32_t present = kAxisNone;
    ExcitationAxes base = channel_excitation(st.axes, present);
    for (size_t i = 0; i < kControllerAxisCount; ++i) {
      const ControllerAxis axis = static_cast<ControllerAxis>(i);
      if (excitation_axis_mask(axis) == kAxisNone || !st.axes.has(axis)) continue;
      voice->live_excitation_axes.set(axis, st.axes.values[i]);
    }
    for (size_t i = 0; i < kControllerAxisCount; ++i) {
      const ControllerAxis axis = static_cast<ControllerAxis>(i);
      if (excitation_axis_mask(axis) == kAxisNone || !st.axes.has(axis)) continue;
      const uint32_t axis_bit = 1u << static_cast<uint32_t>(i);
      if ((st.mpe_pressure_axes & axis_bit) != 0u) {
        voice->live_mpe_pressure_axes |= axis_bit;
      }
      if ((st.mpe_timbre_axes & axis_bit) != 0u) {
        voice->live_mpe_timbre_axes |= axis_bit;
      }
      if ((st.mpe_bend_axes & axis_bit) != 0u) {
        voice->live_mpe_bend_axes |= axis_bit;
      }
    }
    if (mpe_.role(ch) == MpeChannelRole::kMember) {
      voice->mpe_member_pressure_present =
          mpe_.own_control(ch, MpeDimension::kPressure, &voice->mpe_member_pressure);
      voice->mpe_member_timbre_present =
          mpe_.own_control(ch, MpeDimension::kTimbre, &voice->mpe_member_timbre);
    }
    if (velocity_excitation_mask != kAxisNone) {
      if ((velocity_excitation_mask & kAxisForce) != 0u) {
        base.force = velocity_excitation.force;
      }
      if ((velocity_excitation_mask & kAxisPosition) != 0u) {
        base.position = velocity_excitation.position;
      }
      if ((velocity_excitation_mask & kAxisBrightness) != 0u) {
        base.brightness = velocity_excitation.brightness;
      }
      if ((velocity_excitation_mask & kAxisMorph) != 0u) {
        base.morph = velocity_excitation.morph;
      }
      present |= velocity_excitation_mask;
      record_velocity_axes(*voice, velocity_excitation, velocity_excitation_mask);
    }
    voice->seed_excitation(base, present);
  }
  // Bus-level piano body (the direct-share attenuation, the modal soundboard
  // and the pedal-gated sympathetic bank). In GM mode the engine is resolved
  // per program, so the body is tuned at the first piano note-on rather than in
  // prepare(); a later program asking for a different board mix only re-states
  // the return level, since other notes may still be ringing through the bank.
  // Allocation-free, like the lazy per-part prepare on the Sf2Player fallback path.
  if (patch->mode == SynthEngineMode::kPiano) {
    if (part_fx_.enabled()) {
      PianoPartState& state = part_piano_state_[ch];
      if (!state.prepared) {
        part_soundboards_[ch].prepare(sample_rate_, patch->piano.soundboard);
        part_resonance_[ch].prepare(sample_rate_);
        state.prepared = true;
      } else if (state.soundboard_mix != patch->piano.soundboard) {
        part_soundboards_[ch].set_mix(patch->piano.soundboard);
      }
      state.soundboard_mix = patch->piano.soundboard;
      // The current routing snapshot may still be dirty. Queue the strike for
      // the first process() sample, where the part-owned board is known to be
      // the destination regardless of whether the part is bussed today.
      voice->piano_case_strike_pending = voice->piano.case_strike();
      voice->piano_board_strike_pending = voice->piano.board_strike();
    } else {
      if (piano_body_soundboard_ < 0.0f) {
        soundboard_.prepare(sample_rate_, patch->piano.soundboard);
        resonance_.prepare(sample_rate_);
      } else if (piano_body_soundboard_ != patch->piano.soundboard) {
        soundboard_.set_mix(patch->piano.soundboard);
      }
      piano_body_soundboard_ = patch->piano.soundboard;
      // The blow into the structure, which the board is struck with once rather
      // than driven by. After any prepare() above, which clears the network.
      soundboard_.strike(voice->piano.case_strike());
      soundboard_.strike_board(voice->piano.board_strike());
    }
    piano_body_active_ = true;
  }
  // GM resolution can select a pipe organ even when the configured patch is
  // subtractive. Prepare wind and swell from the resolved patch on this part;
  // non-organ voices receive unity wind in process_impl().
  if (patch->mode == SynthEngineMode::kPipeOrgan) {
    OrganPartParams& params = organ_part_params_[ch];
    const float trem_rate = patch->pipe_organ.tremulant_rate_hz;
    const float trem_depth = patch->pipe_organ.tremulant_depth;
    const float sag = patch->pipe_organ.wind_sag;
    if (!params.armed || params.tremulant_rate_hz != trem_rate ||
        params.tremulant_depth != trem_depth || params.wind_sag != sag) {
      part_wind_[ch].prepare(sample_rate_, trem_rate, trem_depth, sag);
      params.tremulant_rate_hz = trem_rate;
      params.tremulant_depth = trem_depth;
      params.wind_sag = sag;
      params.armed = true;
    }
    part_swell_depth_[ch] = patch->pipe_organ.swell;
  }
  // Bus-level open-string halo, the same lazy-arming rule as the piano body
  // above but for a Karplus-Strong voice that asks for it. Guarded so an
  // already-armed bank (configured, or armed by an earlier note-on) is never
  // re-prepared, which would clear its ringing state.
  if (patch->mode == SynthEngineMode::kKarplusStrong && patch->ks.sympathetic &&
      !part_fx_.enabled() && !guitar_halo_active_) {
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

void NativeSynth::record_velocity_axes(NativeSynthVoice& voice, const ExcitationAxes& velocity,
                                       uint32_t mask) noexcept {
  for (size_t i = 0; i < kControllerAxisCount; ++i) {
    const ControllerAxis axis = static_cast<ControllerAxis>(i);
    if ((mask & excitation_axis_mask(axis)) == 0u) continue;
    const uint32_t axis_bit = 1u << static_cast<uint32_t>(i);
    voice.live_excitation_axes.set(axis, excitation_value(velocity, axis));
    voice.live_mpe_pressure_axes &= ~axis_bit;
    voice.live_mpe_timbre_axes &= ~axis_bit;
    voice.live_mpe_bend_axes &= ~axis_bit;
  }
}

void NativeSynth::note_off(uint8_t channel, uint8_t note, uint32_t source_track_id) noexcept {
  if (!prepared_) return;
  const uint8_t ch = channel & 0x0Fu;
  ChannelState& st = channels_[ch];
  st.release_key(note, source_track_id);
  // Releasing a key under a slur returns the voice to the key still held rather
  // than ending the phrase. A note-off whose key is NOT the one sounding falls
  // straight past here and past the release loop below, which is what keeps a
  // slur alive when the old key is let go late.
  if (legato_continue(ch, true, note, 0, 0, source_track_id, nullptr).outcome ==
      LegatoOutcome::kCarried) {
    return;
  }
  for (NativeSynthVoice& v : pool_) {
    if (v.active && v.note == note && v.channel == ch && v.source_track_id == source_track_id &&
        v.key_down) {
      freeze_mpe_voice(v, ch);
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
  const uint16_t scope = mpe_.channels_in_scope(channel);
  for (uint8_t ch = 0; ch < 16; ++ch) {
    if ((scope & (uint16_t{1} << ch)) != 0) sustain_channel(ch, value);
  }
}

void NativeSynth::sustain_channel(uint8_t channel, Control32 value) noexcept {
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
  const uint16_t scope = mpe_.channels_in_scope(channel);
  for (uint8_t ch = 0; ch < 16; ++ch) {
    if ((scope & (uint16_t{1} << ch)) != 0) sostenuto_channel(ch, down);
  }
}

void NativeSynth::sostenuto_channel(uint8_t channel, bool down) noexcept {
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
      if (v.key_down) freeze_mpe_voice(v, ch);
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
  // A part's factory-backed physical-model state belongs to the channel that
  // was silenced. Clear it even when another part still has voices, so an
  // All Sound Off on one part cannot leave a board, swell filter, or wind
  // chest tail audible on a later note.
  if (part_fx_.enabled()) {
    part_soundboards_[ch].reset();
    part_resonance_[ch].reset();
  }
  part_piano_splitters_[ch].reset();
  part_organ_splitters_[ch].reset();
  part_guitar_splitters_[ch].reset();
  part_piano_state_[ch] = {};
  part_halo_[ch].reset();
  part_halo_armed_[ch] = false;
  part_wind_[ch].reset();
  organ_part_params_[ch] = {};
  part_swell_depth_[ch] = 0.0f;
  part_swell_coeff_[ch] = 1.0f;
  part_swell_lp_l_[ch] = 0.0f;
  part_swell_lp_r_[ch] = 0.0f;
  bus_fed_channels_ &= static_cast<uint16_t>(~(uint16_t{1} << ch));
  if (bus_fed_channels_ == 0) {
    // All Sound Off means silence NOW, and the instrument's bus resonators are
    // part of its output: the piano soundboard, the sympathetic bank and the
    // guitar halo ring for ~1.5 s and the swell one-pole holds a residual, so
    // killing the voices alone would leak an audible wash past the stop. They
    // are bus-level (all 16 channels feed one), so they are cleared only once
    // no other channel has fed them. The DC blocker goes with them for
    // the same reason.
    resonance_.reset();
    soundboard_.reset();
    guitar_halo_.reset();
    // An inactive voice does not imply its part's body is silent. Preserve
    // other parts' body state and the shared DC filter while they can ring.
    bool other_body = false;
    for (size_t part = 0; part < 16; ++part) {
      if (part == ch) continue;
      other_body = other_body || (part_fx_.enabled() && part_piano_state_[part].prepared) ||
                   part_halo_armed_[part] || organ_part_params_[part].armed;
    }
    if (!other_body) {
      dc_x1_ = {};
      dc_y1_ = {};
    }
  }
}

void NativeSynth::channel_pressure(uint8_t channel, Control32 pressure) noexcept {
  const uint8_t ch = channel & 0x0Fu;
  channels_[ch].pressure = pressure;
  // A manager's pressure is a bias on every member of its zone, so it reaches
  // further than the channel it arrived on (2.2.7).
  refresh_channel_mods_in_scope(ch);
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
  const uint32_t previous_pressure_axes = st.mpe_pressure_axes;
  const uint32_t previous_timbre_axes = st.mpe_timbre_axes;
  st.axes.reset();
  st.mpe_pressure_axes = kAxisNone;
  st.mpe_timbre_axes = kAxisNone;
  st.mpe_bend_axes = kAxisNone;
  st.params.reset();
  // Inside a zone the same three controllers are tracked by the zone model,
  // which is where every reader of them takes their combined value -- leaving
  // them here would reset the channel and change nothing that is heard.
  mpe_.reset_controls(static_cast<uint16_t>(uint16_t{1} << ch));
  sustain_channel(ch, Control32::from7(0));
  sostenuto_channel(ch, false);
  st.una_corda = false;
  if (mpe_.role(ch) == MpeChannelRole::kManager) {
    // The manager's values were a bias on every member, so each member has to
    // resolve its own again without them.
    const auto resolve_candidate = [&](uint8_t member, MpeDimension dimension, Control32 own,
                                       bool present) noexcept {
      ControllerAxisState candidate;
      if (!present) return candidate;
      const Ump message = dimension == MpeDimension::kPressure
                              ? make_midi2_channel_pressure(0, member, own.raw)
                              : make_midi2_control_change(0, member, kMpeTimbreCc, own.raw);
      std::array<ControllerAxisValue, kMaxControllerBindings> resolved{};
      const size_t count = controller_profile_.resolve(message, resolved.data(), resolved.size());
      for (size_t i = 0; i < count; ++i) candidate.set(resolved[i].axis, resolved[i].value);
      return candidate;
    };
    const auto reconcile = [&](ControllerAxisState& axes, uint32_t& pressure_mask,
                               uint32_t& timbre_mask, const ControllerAxisState& pressure,
                               const ControllerAxisState& timbre) noexcept {
      for (size_t i = 0; i < kControllerAxisCount; ++i) {
        const uint32_t bit = 1u << static_cast<uint32_t>(i);
        const ControllerAxis axis = static_cast<ControllerAxis>(i);
        const ControllerAxisState* candidate = (pressure_mask & bit) != 0u ? &pressure
                                               : (timbre_mask & bit) != 0u ? &timbre
                                                                           : nullptr;
        if (candidate == nullptr) continue;
        if (candidate->has(axis)) {
          axes.set(axis, candidate->values[i]);
        } else {
          axes.present &= ~bit;
          axes.values[i] = 0.0f;
          pressure_mask &= ~bit;
          timbre_mask &= ~bit;
        }
      }
    };
    const uint16_t members = static_cast<uint16_t>(mpe_.channels_in_scope(ch) & ~(1u << ch));
    for (uint8_t member = 0; member < 16; ++member) {
      if ((members & (uint16_t{1} << member)) == 0) continue;
      ChannelState& member_state = channels_[member];
      Control32 pressure = Control32::from_raw(0);
      Control32 timbre = Control32::from_raw(0);
      const bool pressure_present = mpe_.own_control(member, MpeDimension::kPressure, &pressure);
      const bool timbre_present = mpe_.own_control(member, MpeDimension::kTimbre, &timbre);
      reconcile(member_state.axes, member_state.mpe_pressure_axes, member_state.mpe_timbre_axes,
                resolve_candidate(member, MpeDimension::kPressure, pressure, pressure_present),
                resolve_candidate(member, MpeDimension::kTimbre, timbre, timbre_present));
      for (NativeSynthVoice& voice : pool_) {
        if (!voice.active || !voice.key_down || voice.channel != member || voice.patch == nullptr) {
          continue;
        }
        // The voice's own captured member input, so a newer note cannot reassign it.
        reconcile(voice.live_excitation_axes, voice.live_mpe_pressure_axes,
                  voice.live_mpe_timbre_axes,
                  resolve_candidate(member, MpeDimension::kPressure, voice.mpe_member_pressure,
                                    voice.mpe_member_pressure_present),
                  resolve_candidate(member, MpeDimension::kTimbre, voice.mpe_member_timbre,
                                    voice.mpe_member_timbre_present));
        voice.restore_excitation_base();
        uint32_t present = kAxisNone;
        const ExcitationAxes live = channel_excitation(voice.live_excitation_axes, present);
        if (present != kAxisNone) voice.push_excitation(live, present);
      }
    }
    restore_excitation_control(ch);
    refresh_all_channel_mods();
  } else {
    if (mpe_.role(ch) != MpeChannelRole::kMember) restore_excitation_control(ch);
    if (mpe_.role(ch) == MpeChannelRole::kMember) {
      // A released tail drops the ordinary values and keeps its NoteOff MPE snapshot.
      for (NativeSynthVoice& voice : pool_) {
        if (!voice.active || voice.channel != ch || voice.key_down || !voice.mpe_release_frozen ||
            voice.patch == nullptr) {
          continue;
        }
        const uint32_t ordinary_frozen = voice.mpe_frozen_axis_mask & ~voice.mpe_frozen_source_mask;
        voice.mpe_frozen_axis_present &= ~ordinary_frozen;
        for (size_t i = 0; i < kControllerAxisCount; ++i) {
          if ((ordinary_frozen & (1u << static_cast<uint32_t>(i))) != 0u) {
            voice.mpe_frozen_axis_values[i] = 0.0f;
          }
        }
        voice.restore_excitation_base();
        voice.live_excitation_axes.reset();
        voice.live_mpe_pressure_axes = kAxisNone;
        voice.live_mpe_timbre_axes = kAxisNone;
        voice.live_mpe_bend_axes = kAxisNone;
        ControllerAxisState frozen_axes;
        for (size_t i = 0; i < kControllerAxisCount; ++i) {
          const ControllerAxis axis = static_cast<ControllerAxis>(i);
          const uint32_t bit = 1u << static_cast<uint32_t>(i);
          if ((voice.mpe_frozen_source_mask & bit) == 0u ||
              (voice.mpe_frozen_axis_present & bit) == 0u ||
              excitation_axis_mask(axis) == kAxisNone) {
            continue;
          }
          frozen_axes.set(axis, voice.mpe_frozen_axis_values[i]);
          voice.live_excitation_axes.set(axis, voice.mpe_frozen_axis_values[i]);
          voice.live_mpe_pressure_axes |= bit;
        }
        uint32_t frozen_present = kAxisNone;
        const ExcitationAxes frozen = channel_excitation(frozen_axes, frozen_present);
        if (frozen_present != kAxisNone) voice.push_excitation(frozen, frozen_present);
      }
    }
    if (mpe_.role(ch) == MpeChannelRole::kMember) {
      const uint8_t manager =
          mpe_.zone_of(ch) == MpeZone::kLower ? kMpeLowerManagerChannel : kMpeUpperManagerChannel;
      const auto candidate = [&](MpeDimension dimension) noexcept {
        ControllerAxisState result;
        Control32 value = Control32::from_raw(0);
        if (!mpe_.own_control(manager, dimension, &value)) return result;
        const Ump message = dimension == MpeDimension::kPressure
                                ? make_midi2_channel_pressure(0, ch, value.raw)
                                : make_midi2_control_change(0, ch, kMpeTimbreCc, value.raw);
        std::array<ControllerAxisValue, kMaxControllerBindings> resolved{};
        const size_t count = controller_profile_.resolve(message, resolved.data(), resolved.size());
        for (size_t i = 0; i < count; ++i) result.set(resolved[i].axis, resolved[i].value);
        return result;
      };
      const ControllerAxisState pressure = candidate(MpeDimension::kPressure);
      const ControllerAxisState timbre = candidate(MpeDimension::kTimbre);
      const auto restore_manager_axes = [&](ControllerAxisState& axes, uint32_t& pressure_mask,
                                            uint32_t& timbre_mask, bool engine_only) noexcept {
        const uint32_t old_pressure = pressure_mask;
        const uint32_t old_timbre = timbre_mask;
        axes.reset();
        pressure_mask = timbre_mask = kAxisNone;
        for (size_t i = 0; i < kControllerAxisCount; ++i) {
          const ControllerAxis axis = static_cast<ControllerAxis>(i);
          if (engine_only && excitation_axis_mask(axis) == kAxisNone) continue;
          const uint32_t bit = 1u << static_cast<uint32_t>(i);
          const ControllerAxisState* selected = nullptr;
          if ((old_pressure & bit) != 0u && pressure.has(axis))
            selected = &pressure;
          else if ((old_timbre & bit) != 0u && timbre.has(axis))
            selected = &timbre;
          else if (channels_[manager].last_mpe_controller == MpeDimension::kPressure &&
                   pressure.has(axis))
            selected = &pressure;
          else if (timbre.has(axis))
            selected = &timbre;
          else if (pressure.has(axis))
            selected = &pressure;
          if (selected == nullptr) continue;
          axes.set(axis, selected->values[i]);
          (selected == &pressure ? pressure_mask : timbre_mask) |= bit;
        }
      };
      // The reset clears the member's contribution, not a surviving manager's source.
      st.mpe_pressure_axes = previous_pressure_axes;
      st.mpe_timbre_axes = previous_timbre_axes;
      restore_manager_axes(st.axes, st.mpe_pressure_axes, st.mpe_timbre_axes, false);
      for (NativeSynthVoice& voice : pool_) {
        if (!voice.active || !voice.key_down || voice.channel != ch) continue;
        voice.restore_excitation_base();
        restore_manager_axes(voice.live_excitation_axes, voice.live_mpe_pressure_axes,
                             voice.live_mpe_timbre_axes, true);
        voice.live_mpe_bend_axes = kAxisNone;
        voice.mpe_member_pressure = Control32::from_raw(0);
        voice.mpe_member_timbre = Control32::from_raw(0);
        voice.mpe_member_pressure_present = false;
        voice.mpe_member_timbre_present = false;
        uint32_t present = kAxisNone;
        const ExcitationAxes live = channel_excitation(voice.live_excitation_axes, present);
        if (present != kAxisNone) voice.push_excitation(live, present);
      }
    }
    refresh_channel_mod(ch);
  }
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
      } else if (st.params.selected_nrpn() && st.rx_nrpn) {
        const GsPartParams before = st.gs;
        gs_apply_part_nrpn(st.gs, st.params.nrpn_msb, st.params.nrpn_lsb, value);
        if (gs_eg_times_differ(before, st.gs)) raise_tail();
      }
      break;
    case 38:
      if (st.params.selected_rpn(0, 0)) bend_range_lsb(ch, value);
      break;
    // TONE MODIFY 1-8 by controller: the storage 40 1x 30-37 and NRPN 01 xx write (docs/gs.md).
    case 71:
    case 72:
    case 73:
    case kMpeTimbreCc:
    case 75:
    case 76:
    case 77:
    case 78: {
      // Inside a zone CC74 is the timbre dimension (2.2.8), already tracked and profiled.
      if (controller == kMpeTimbreCc && mpe_.role(ch) != MpeChannelRole::kUnassigned) break;
      // A controller the profile routes to an axis is the profile's, not the alias's.
      if (profile_binds_cc(controller)) break;
      const GsPartParams before = st.gs;
      gs_apply_tone_modify_cc(st.gs, controller, value);
      if (gs_eg_times_differ(before, st.gs)) raise_tail();
      break;
    }
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
      if (st.rx_nrpn) st.params.select_nrpn_lsb(value);
      break;
    case 99:
      if (st.rx_nrpn) st.params.select_nrpn_msb(value);
      break;
    case 100:
      st.params.select_rpn_lsb(value);
      st.params.deselect_on_rpn_null();
      break;
    case 101:
      st.params.select_rpn_msb(value);
      st.params.deselect_on_rpn_null();
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
      if (mpe_.role(ch) == MpeChannelRole::kUnassigned) {
        st.articulation =
            controller == 126 ? ArticulationMode::kMonoRetrigger : ArticulationMode::kPoly;
      }
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
    if (event.sysex_payload == nullptr || event.sysex_payload_size == 0) return;
    // Part state is the render thread's on both paths, as Sf2Player's is.
    apply_part_sysex(event.sysex_payload, event.sysex_payload_size);
    // Offline the stream carries the file's GS EFX; live the host pushes it
    // through on_control_sysex, so a SysEx scheduled in a clip is not applied.
    if (config_.realize_efx_inline && config_.use_gm_programs && part_fx_.enabled()) {
      apply_efx_sysex(event.sysex_payload, event.sysex_payload_size);
    }
    return;
  }
  // Tracking comes ahead of both, because the profile reads the combined value
  // a member channel's controller carries and that value is only current once
  // the message in hand has been tracked.
  track_mpe_input(u);
  ChannelVoiceEvent ev;
  if (!decode_channel_voice(u, &ev)) {
    ++skipped_events_;  // Reserved status.
    return;
  }
  const uint8_t ch = ev.channel & 0x0Fu;
  // A message the zone ignores must not reach a profile axis either.
  if (ev.kind == ChannelVoiceKind::PolyPressure &&
      mpe_.ignores(ch, MpeIgnorable::kPolyKeyPressure)) {
    return;
  }
  if (ev.kind == ChannelVoiceKind::ControlChange &&
      (((ev.note == 0 || ev.note == 32) && mpe_.ignores(ch, MpeIgnorable::kBankSelect)) ||
       ((ev.note == 126 || ev.note == 127) && mpe_.ignores(ch, MpeIgnorable::kModeMessage)))) {
    return;
  }
  // The profile runs before the protocol dispatch so a note-on whose velocity
  // is bound to an axis starts from its own value rather than the previous
  // note's.
  ExcitationAxes velocity_excitation{};
  uint32_t velocity_excitation_mask = kAxisNone;
  if (ev.kind == ChannelVoiceKind::NoteOn) {
    velocity_excitation = apply_note_on_controller_input(u, &velocity_excitation_mask);
  } else {
    apply_controller_input(u);
  }
  switch (ev.kind) {
    case ChannelVoiceKind::NoteOn:
      note_on(ch, ev.note, ev.velocity, ev.index, ev.attribute_data, event.source_track_id,
              velocity_excitation, velocity_excitation_mask);
      break;
    case ChannelVoiceKind::NoteOff:
      note_off(ch, ev.note, event.source_track_id);
      break;
    case ChannelVoiceKind::PitchBend:
      channels_[ch].pitch_bend = ev.bend;
      mpe_.track_bend(ch, ev.bend);
      // A manager's bend applies to every sounding note in its zone (2.2.6), so
      // like its pressure it reaches past the channel it arrived on.
      refresh_channel_mods_in_scope(ch);
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

bool NativeSynth::profile_binds_cc(uint8_t controller) const noexcept {
  for (size_t i = 0; i < controller_profile_.binding_count(); ++i) {
    const ControllerBinding& binding = controller_profile_.binding_at(i);
    if (binding.input == ControllerInput::kControlChange && binding.index == controller) {
      return true;
    }
  }
  return false;
}

void NativeSynth::apply_part_sysex(const uint8_t* data, size_t size) noexcept {
  const GsSysEx msg = parse_gs_sysex(data, size);
  if (gs_sysex_resets(msg.kind)) {
    // GM System On closes NRPN reception, the NRPNs being Roland's; GS Reset reopens it.
    const bool gm = msg.kind != GsSysExKind::kGsReset;
    uint16_t rig_dirty = 0;
    for (uint8_t ch = 0; ch < 16; ++ch) {
      // A reset silences the pre-reset voices and returns the part to its power-on state --
      // program, bank variation, volume, pan, expression and the rest of the controllers -- in
      // place. What the host configured (articulation) and what no reset touches (the tone map,
      // carried by the bank LSB) are kept.
      all_sound_off(ch);
      ChannelState& st = channels_[ch];
      const ArticulationMode articulation = st.articulation;
      const uint8_t bank_lsb = st.bank_lsb;
      const bool was_drums = st.drums;
      st = ChannelState{};
      st.articulation = articulation;
      st.bank_lsb = bank_lsb;
      st.rx_nrpn = !gm;
      st.drums = ch == kDrumChannelIndex;
      if (was_drums != st.drums) rig_dirty |= static_cast<uint16_t>(1u << ch);
    }
    refresh_all_channel_mods();
    for (uint8_t ch = 0; ch < 16; ++ch) {
      if ((rig_dirty & (uint16_t{1} << ch)) != 0) refresh_part_rig(ch);
    }
    return;
  }
  uint16_t rig_dirty = 0;
  bool eg_moved = false;
  gs_for_each_sysex_write(data, size, [&](const GsWrite& w) noexcept {
    const GsAddressEntry* entry = gs_lookup_address(w.addr);
    const bool rhythm_write = w.param == GsParam::kUseForRhythmPart;
    // USE FOR RHYTHM PART is the one GS row whose out-of-range value reads as
    // map 1 (docs/gs.md). NativeSynth stores only the rhythm/melodic choice,
    // so every nonzero byte, including >2, means rhythm here.
    if (entry == nullptr || (!rhythm_write && !gs_value_in_range(*entry, w.value))) return false;
    ChannelState& st = channels_[w.part & 0x0Fu];
    if (rhythm_write) {
      const bool drums = w.value != 0;
      if (st.drums != drums) {
        st.drums = drums;
        rig_dirty |= static_cast<uint16_t>(1u << (w.part & 0x0Fu));
      }
    } else if (w.param == GsParam::kPartToneModify) {
      const GsPartParams before = st.gs;
      gs_apply_tone_modify(st.gs, w.index, w.value);
      eg_moved |= gs_eg_times_differ(before, st.gs);
    } else if (w.param == GsParam::kPartRxNrpn) {
      st.rx_nrpn = w.value != 0;
    }
    return true;
  });
  for (uint8_t ch = 0; ch < 16; ++ch) {
    if ((rig_dirty & (uint16_t{1} << ch)) != 0) refresh_part_rig(ch);
  }
  if (eg_moved) raise_tail();
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
                                int16_t zone_key) const noexcept {
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
  const int16_t zone_key = static_cast<int16_t>(
      static_cast<int>(state.zone_key) + static_cast<int>(to_note) - static_cast<int>(from_note));
  bind_per_note(state, voice.channel, to_note, attribute_type, attribute_data, zone_key);
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
    part_fx_.settle_block(*this);
  }
  // Automation written since the last block: re-derive the latched state of
  // the voices playing config_.patch (GM voices point elsewhere).
  if (live_dirty_ != 0) {
    for (NativeSynthVoice& v : pool_) {
      if (v.active && v.patch == &config_.patch && !v.choked) {
        v.refresh_live(config_.patch, sample_rate_, live_dirty_);
      }
    }
    live_dirty_ = 0;
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

  // Swell boxes are per organ part. A GM organ may share the host with another
  // engine, and each part's CC11 must affect only the pipe leg it owns.
  std::array<bool, 16> part_swell_active{};
  for (size_t part = 0; part < part_swell_active.size(); ++part) {
    const float depth = part_swell_depth_[part];
    if (!organ_part_params_[part].armed || depth <= 0.0f) {
      part_swell_coeff_[part] = 1.0f;
      continue;
    }
    const float expression = channels_[part].expression.f7();
    const float shut = (1.0f - expression / 127.0f) * depth;
    const float fc = std::exp(std::log(20000.0f) + shut * (std::log(300.0f) - std::log(20000.0f)));
    // Above ~19 kHz the shutter is effectively open.
    if (fc < 19000.0f) {
      part_swell_active[part] = true;
      part_swell_coeff_[part] = std::clamp(
          1.0f - std::exp(-constants::kTwoPi * fc / static_cast<float>(sample_rate_)), 0.0f, 1.0f);
    }
  }

  // The per-part board is independent of routing, so a queued strike can be
  // committed once the audio block begins. The board return itself is routed
  // from the same current snapshot as the dry part below.
  if (part_fx_.enabled()) {
    for (NativeSynthVoice& v : pool_) {
      if (!v.active || v.patch == nullptr || v.patch->mode != SynthEngineMode::kPiano ||
          (v.piano_case_strike_pending == 0.0f && v.piano_board_strike_pending == 0.0f)) {
        continue;
      }
      const size_t part = static_cast<size_t>(v.channel & 0x0Fu);
      part_soundboards_[part].strike(v.piano_case_strike_pending);
      part_soundboards_[part].strike_board(v.piano_board_strike_pending);
      v.piano_case_strike_pending = 0.0f;
      v.piano_board_strike_pending = 0.0f;
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
      // Per-sample, per-part pipe count: a release ending mid-block must stop loading its chest.
      std::array<int, 16> organ_demand{};
      for (const NativeSynthVoice& active_voice : pool_) {
        if (!active_voice.active || active_voice.patch == nullptr ||
            active_voice.patch->mode != SynthEngineMode::kPipeOrgan) {
          continue;
        }
        const size_t organ_part = static_cast<size_t>(active_voice.channel & 0x0Fu);
        // Counting pipes keeps the sag order-independent; a zero-level rank adds none.
        organ_demand[organ_part] += active_voice.pipe_organ.sounding_pipe_count();
      }
      // The default state is unity, so non-organ voices never inherit an organ's wind.
      std::array<OrganWindSupply::State, 16> wind_state{};
      const OrganWindSupply::State unity_wind{};
      for (size_t part = 0; part < wind_state.size(); ++part) {
        if (organ_part_params_[part].armed && part_wind_[part].active()) {
          // Zero demand lets pressure recover while this part is silent.
          wind_state[part] = part_wind_[part].process(organ_demand[part]);
        }
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
      for (size_t part = 0; part < 16; ++part) {
        chunk_part_piano_l_[part][static_cast<size_t>(c)] = 0.0f;
        chunk_part_piano_r_[part][static_cast<size_t>(c)] = 0.0f;
        chunk_part_organ_l_[part][static_cast<size_t>(c)] = 0.0f;
        chunk_part_organ_r_[part][static_cast<size_t>(c)] = 0.0f;
        chunk_part_guitar_l_[part][static_cast<size_t>(c)] = 0.0f;
        chunk_part_guitar_r_[part][static_cast<size_t>(c)] = 0.0f;
      }
      for (NativeSynthVoice& v : pool_) {
        if (!v.active) continue;
        // The channel's, except on a member channel sounding more than one note,
        // where the voice carries the part of the channel's the note was
        // attributed (M1-100-UM v1.1 section 2.2.4.1).
        const Sf2ChannelMod& channel_mod =
            v.mpe_mod_active ? v.mpe_mod : channel_mods_[v.channel & 0x0Fu];
        const size_t part_index = static_cast<size_t>(v.channel & 0x0Fu);
        const bool organ_voice = v.patch != nullptr && v.patch->mode == SynthEngineMode::kPipeOrgan;
        const OrganWindSupply::State& voice_wind =
            organ_voice ? wind_state[part_index] : unity_wind;
        // A key carrying per-note pitch renders through a copy of that mod with its offset
        // added; every other voice reads the mod itself.
        const float per_note_offset = per_note_[static_cast<size_t>(&v - pool_.data())].cents;
        float s = 0.0f;
        if (per_note_offset == 0.0f) {
          s = v.render(channel_mod, voice_wind.pitch_ratio, voice_wind.gain);
        } else {
          Sf2ChannelMod tuned = channel_mod;
          tuned.pitch_cents += per_note_offset;
          s = v.render(tuned, voice_wind.pitch_ratio, voice_wind.gain);
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
          if (organ_voice) {
            // Organ swell is a part stage before the rig, so retain its stereo
            // legs until the per-part filter pass below.
            if (mono_rig) {
              const float raw = constants::kInvSqrt2 * s;
              chunk_part_organ_l_[static_cast<size_t>(part)][static_cast<size_t>(c)] += raw;
              chunk_part_organ_r_[static_cast<size_t>(part)][static_cast<size_t>(c)] += raw;
            } else {
              chunk_part_organ_l_[static_cast<size_t>(part)][static_cast<size_t>(c)] += bus_l;
              chunk_part_organ_r_[static_cast<size_t>(part)][static_cast<size_t>(c)] += bus_r;
            }
          } else if (part_fx_.enabled() && piano_voice) {
            // Factory-backed piano bodies are also part stages. A mono pickup
            // feeds the board with the same common leg the rig receives.
            const float raw = constants::kInvSqrt2 * s;
            if (mono_rig) {
              chunk_part_piano_l_[static_cast<size_t>(part)][static_cast<size_t>(c)] += raw;
              chunk_part_piano_r_[static_cast<size_t>(part)][static_cast<size_t>(c)] += raw;
            } else {
              chunk_part_piano_l_[static_cast<size_t>(part)][static_cast<size_t>(c)] += bus_l;
              chunk_part_piano_r_[static_cast<size_t>(part)][static_cast<size_t>(c)] += bus_r;
            }
          } else if (part_fx_.enabled() && halo_voice &&
                     part_halo_armed_[static_cast<size_t>(part)]) {
            if (mono_rig) {
              const float raw = constants::kInvSqrt2 * s;
              chunk_part_guitar_l_[static_cast<size_t>(part)][static_cast<size_t>(c)] += raw;
              chunk_part_guitar_r_[static_cast<size_t>(part)][static_cast<size_t>(c)] += raw;
            } else {
              chunk_part_guitar_l_[static_cast<size_t>(part)][static_cast<size_t>(c)] += bus_l;
              chunk_part_guitar_r_[static_cast<size_t>(part)][static_cast<size_t>(c)] += bus_r;
            }
          } else {
            // Remaining voices feed the part bus directly (pianos at their direct gain).
            const float direct = piano_voice ? kPianoDirectGain : 1.0f;
            if (mono_rig) {
              // A mono pickup feeds the rig's amp, so CC10 cannot move its drive.
              part_fx_.add_mono(part, c, direct * constants::kInvSqrt2 * s);
            } else {
              part_fx_.add_stereo(part, c, direct * bus_l, direct * bus_r);
            }
            if (halo_voice && part_halo_armed_[static_cast<size_t>(part)]) {
              // Without per-part FX the halo leg is driven from the bussed dry signal.
              if (any_bussed) {
                const float dry = mono_rig ? constants::kInvSqrt2 * s : 0.5f * (bus_l + bus_r);
                chunk_part_guitar_l_[static_cast<size_t>(part)][static_cast<size_t>(c)] += dry;
                chunk_part_guitar_r_[static_cast<size_t>(part)][static_cast<size_t>(c)] += dry;
              }
            }
          }
          if (source_render) {
            const float src_l = bus_l * config_.gain;
            const float src_r = bus_r * config_.gain;
            residual_splitter_.accumulate(v.source_track_id, src_l, src_r);
            if (piano_voice && !part_fx_.enabled()) {
              piano_residual_splitter_.accumulate(v.source_track_id, src_l, src_r);
            }
            // Keep a body owner's source weights independent of the current
            // EFX assignment. If a held note moves from a bussed part to a
            // dry part, its ringing return must follow its original source
            // lane instead of falling back to lane 0.
            if (piano_voice) {
              part_piano_splitters_[static_cast<size_t>(part)].accumulate(v.source_track_id, src_l,
                                                                          src_r);
            }
            if (organ_voice) {
              part_organ_splitters_[static_cast<size_t>(part)].accumulate(v.source_track_id, src_l,
                                                                          src_r);
            }
            if (halo_voice && part_halo_armed_[static_cast<size_t>(part)]) {
              part_guitar_splitters_[static_cast<size_t>(part)].accumulate(v.source_track_id, src_l,
                                                                           src_r);
            }
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
        if (organ_voice) {
          // Keep the unbussed dry leg in the mix and on its source lane; the
          // pre-rig swell residual is added and attributed per part below.
          mix_l += voice_l;
          mix_r += voice_r;
          chunk_part_organ_l_[static_cast<size_t>(part_index)][static_cast<size_t>(c)] += voice_l;
          chunk_part_organ_r_[static_cast<size_t>(part_index)][static_cast<size_t>(c)] += voice_r;
        } else if (part_fx_.enabled() && piano_voice) {
          // Keep the raw string contribution in the per-part stage. The full
          // body return is added below in one operation, matching the legacy
          // unbussed piano arithmetic while still giving a bussed part a
          // complete pre-rig return.
          chunk_part_piano_l_[static_cast<size_t>(part_index)][static_cast<size_t>(c)] += voice_l;
          chunk_part_piano_r_[static_cast<size_t>(part_index)][static_cast<size_t>(c)] += voice_r;
        } else if (part_fx_.enabled() && halo_voice && part_halo_armed_[part_index]) {
          mix_l += voice_l;
          mix_r += voice_r;
          chunk_part_guitar_l_[part_index][static_cast<size_t>(c)] += voice_l;
          chunk_part_guitar_r_[part_index][static_cast<size_t>(c)] += voice_r;
        } else if (piano_voice) {
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
          if (piano_voice && !part_fx_.enabled()) {
            piano_residual_splitter_.accumulate(v.source_track_id, src_l, src_r);
          }
          if (guitar_halo_active_ && halo_voice && !part_fx_.enabled()) {
            guitar_residual_splitter_.accumulate(v.source_track_id, src_l, src_r);
          }
          if (part_fx_.enabled() && piano_voice) {
            part_piano_splitters_[part_index].accumulate(v.source_track_id, src_l, src_r);
          }
          if (organ_voice) {
            part_organ_splitters_[part_index].accumulate(v.source_track_id, src_l, src_r);
          }
          if (part_fx_.enabled() && halo_voice && part_halo_armed_[part_index]) {
            part_guitar_splitters_[part_index].accumulate(v.source_track_id, src_l, src_r);
          }
        }
      }
      // Part-owned body returns form before the part chain; unbussed parts add only the residual.
      for (size_t part = 0; part < 16; ++part) {
        const bool bussed_part = any_bussed && fx->part_bussed[part];

        // A swell box is a part pre-rig stage even when the factory is absent.
        // This keeps a configured pipe-organ render on its historical path,
        // while its source residual remains isolated from other parts.
        const float organ_raw_l = chunk_part_organ_l_[part][static_cast<size_t>(c)];
        const float organ_raw_r = chunk_part_organ_r_[part][static_cast<size_t>(c)];
        float organ_out_l = organ_raw_l;
        float organ_out_r = organ_raw_r;
        if (organ_part_params_[part].armed && part_swell_depth_[part] > 0.0f) {
          if (part_swell_active[part]) {
            part_swell_lp_l_[part] +=
                part_swell_coeff_[part] * (organ_raw_l - part_swell_lp_l_[part]);
            part_swell_lp_r_[part] +=
                part_swell_coeff_[part] * (organ_raw_r - part_swell_lp_r_[part]);
            organ_out_l = part_swell_lp_l_[part];
            organ_out_r = part_swell_lp_r_[part];
          } else {
            // Track the live input while the shutter is effectively open. If
            // CC11 closes on a held note, the lowpass therefore starts at the
            // current sample instead of a stale zero and cannot click down.
            part_swell_lp_l_[part] = organ_raw_l;
            part_swell_lp_r_[part] = organ_raw_r;
          }
        }
        const float organ_res_l = organ_out_l - organ_raw_l;
        const float organ_res_r = organ_out_r - organ_raw_r;
        chunk_part_organ_l_[part][static_cast<size_t>(c)] = organ_res_l;
        chunk_part_organ_r_[part][static_cast<size_t>(c)] = organ_res_r;
        if (bussed_part) {
          if (fx->mono_prefix[part] != 0) {
            part_fx_.add_mono(static_cast<int>(part), c, 0.5f * (organ_out_l + organ_out_r));
          } else {
            part_fx_.add_stereo(static_cast<int>(part), c, organ_out_l, organ_out_r);
          }
        } else {
          mix_l += organ_res_l;
          mix_r += organ_res_r;
        }

        if (part_fx_.enabled() && part_piano_state_[part].prepared) {
          const float piano_raw_l = chunk_part_piano_l_[part][static_cast<size_t>(c)];
          const float piano_raw_r = chunk_part_piano_r_[part][static_cast<size_t>(c)];
          PianoSoundboard& board = part_soundboards_[part];
          PianoResonanceBank& resonance = part_resonance_[part];
          const float body = board.process(0.5f * (piano_raw_l + piano_raw_r));
          const float side = board.last_side();
          const float symp = resonance.process(board.last_diffused(), channels_[part].sustain);
          const float piano_out_l = kPianoDirectGain * piano_raw_l + body + side + symp;
          const float piano_out_r = kPianoDirectGain * piano_raw_r + body - side + symp;
          const float piano_res_l = piano_out_l - piano_raw_l;
          const float piano_res_r = piano_out_r - piano_raw_r;
          chunk_part_piano_l_[part][static_cast<size_t>(c)] = piano_res_l;
          chunk_part_piano_r_[part][static_cast<size_t>(c)] = piano_res_r;
          if (bussed_part) {
            if (fx->mono_prefix[part] != 0) {
              part_fx_.add_mono(static_cast<int>(part), c, 0.5f * (piano_out_l + piano_out_r));
            } else {
              part_fx_.add_stereo(static_cast<int>(part), c, piano_out_l, piano_out_r);
            }
          } else {
            mix_l += piano_out_l;
            mix_r += piano_out_r;
          }
        }

        if (part_fx_.enabled() && part_halo_armed_[part]) {
          const float guitar_raw_l = chunk_part_guitar_l_[part][static_cast<size_t>(c)];
          const float guitar_raw_r = chunk_part_guitar_r_[part][static_cast<size_t>(c)];
          const float halo = part_halo_[part].process(0.5f * (guitar_raw_l + guitar_raw_r),
                                                      channels_[part].sustain);
          chunk_part_guitar_l_[part][static_cast<size_t>(c)] = halo;
          chunk_part_guitar_r_[part][static_cast<size_t>(c)] = halo;
          if (bussed_part) {
            if (fx->mono_prefix[part] != 0) {
              part_fx_.add_mono(static_cast<int>(part), c,
                                0.5f * (guitar_raw_l + guitar_raw_r) + halo);
            } else {
              part_fx_.add_stereo(static_cast<int>(part), c, guitar_raw_l + halo,
                                  guitar_raw_r + halo);
            }
          } else {
            mix_l += halo;
            mix_r += halo;
          }
        }
      }

      chunk_mix_l_[static_cast<size_t>(c)] = mix_l;
      chunk_mix_r_[static_cast<size_t>(c)] = mix_r;
      chunk_piano_l_[static_cast<size_t>(c)] = piano_l;
      chunk_piano_r_[static_cast<size_t>(c)] = piano_r;
      chunk_guitar_l_[static_cast<size_t>(c)] = guitar_l;
      chunk_guitar_r_[static_cast<size_t>(c)] = guitar_r;
    }

    if (source_render && any_bussed) {
      for (size_t part = 0; part < 16; ++part) {
        if (!fx->part_bussed[part]) continue;
        const uint8_t unit = fx->part_unit[part];
        SourceResidualSplitter& destination = unit != PartFxSnapshot::kNoUnit && any_unit
                                                  ? unit_splitters_[unit]
                                                  : part_bus_splitters_[part];
        const auto bridge = [&](SourceResidualSplitter& owners, const float* l, const float* r) {
          float energy = 0.0f;
          for (int c = 0; c < n; ++c) {
            const float scaled_l = l[c] * config_.gain;
            const float scaled_r = r[c] * config_.gain;
            energy += scaled_l * scaled_l + scaled_r * scaled_r;
          }
          // Learn/decay once even while bussed. Only the downstream bus writes
          // audio; this pass transfers the body's source ownership.
          owners.flush(source_outputs, source_output_count, n, l, r, offset,
                       [](float* const*, int, float, float) {});
          destination.accumulate_residual_energy(owners, energy);
        };
        bridge(part_piano_splitters_[part], chunk_part_piano_l_[part].data(),
               chunk_part_piano_r_[part].data());
        bridge(part_organ_splitters_[part], chunk_part_organ_l_[part].data(),
               chunk_part_organ_r_[part].data());
        bridge(part_guitar_splitters_[part], chunk_part_guitar_l_[part].data(),
               chunk_part_guitar_r_[part].data());
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

    // Routing can leave a bus unused for many chunks. Keep its ownership
    // clock running so a later source does not inherit frozen old weights.
    if (source_render) {
      static constexpr std::array<float, kPartFxChunkFrames> silence{};
      const auto age = [&](SourceResidualSplitter& splitter) {
        splitter.flush(source_outputs, source_output_count, n, silence.data(), silence.data(),
                       offset, [](float* const*, int, float, float) {});
      };
      for (size_t part = 0; part < 16; ++part) {
        const bool flushed = any_bussed && fx->part_bussed[part] &&
                             (!any_unit || fx->part_unit[part] == PartFxSnapshot::kNoUnit);
        if (!flushed) age(part_bus_splitters_[part]);
      }
      for (size_t unit = 0; unit < kGsEfxUnitCount; ++unit) {
        if (!any_unit || !fx->unit_fed[unit]) age(unit_splitters_[unit]);
      }
    }

    // Second pass: the bus-level post stages, in the order they always ran.
    for (int c = 0; c < n; ++c) {
      const int i = offset + c;
      const size_t at = static_cast<size_t>(c);
      float mix_l = chunk_mix_l_[at] * config_.gain;
      float mix_r = chunk_mix_r_[at] * config_.gain;
      const float piano_raw_l = chunk_piano_l_[at];
      const float piano_raw_r = chunk_piano_r_[at];
      const float piano_l = piano_raw_l * config_.gain;
      const float piano_r = piano_raw_r * config_.gain;
      const float guitar_raw_l = chunk_guitar_l_[at];
      const float guitar_raw_r = chunk_guitar_r_[at];
      // Per-part residuals are already in the mix; flush each through its own source splitter.
      float part_piano_res_l = 0.0f;
      float part_piano_res_r = 0.0f;
      float part_organ_res_l = 0.0f;
      float part_organ_res_r = 0.0f;
      float part_guitar_res_l = 0.0f;
      float part_guitar_res_r = 0.0f;
      for (size_t part = 0; part < 16; ++part) {
        if (any_bussed && fx->part_bussed[part]) {
          chunk_part_piano_l_[part][at] = 0.0f;
          chunk_part_piano_r_[part][at] = 0.0f;
          chunk_part_organ_l_[part][at] = 0.0f;
          chunk_part_organ_r_[part][at] = 0.0f;
          chunk_part_guitar_l_[part][at] = 0.0f;
          chunk_part_guitar_r_[part][at] = 0.0f;
          continue;
        }
        const float piano_component_l = chunk_part_piano_l_[part][at] * config_.gain;
        const float piano_component_r = chunk_part_piano_r_[part][at] * config_.gain;
        const float organ_component_l = chunk_part_organ_l_[part][at] * config_.gain;
        const float organ_component_r = chunk_part_organ_r_[part][at] * config_.gain;
        const float guitar_component_l = chunk_part_guitar_l_[part][at] * config_.gain;
        const float guitar_component_r = chunk_part_guitar_r_[part][at] * config_.gain;
        part_piano_res_l += piano_component_l;
        part_piano_res_r += piano_component_r;
        part_organ_res_l += organ_component_l;
        part_organ_res_r += organ_component_r;
        part_guitar_res_l += guitar_component_l;
        part_guitar_res_r += guitar_component_r;
        if (source_render) {
          chunk_part_piano_l_[part][at] = piano_component_l;
          chunk_part_piano_r_[part][at] = piano_component_r;
          chunk_part_organ_l_[part][at] = organ_component_l;
          chunk_part_organ_r_[part][at] = organ_component_r;
          chunk_part_guitar_l_[part][at] = guitar_component_l;
          chunk_part_guitar_r_[part][at] = guitar_component_r;
        }
      }
      const float dry_l = mix_l + piano_l - part_piano_res_l - part_organ_res_l - part_guitar_res_l;
      const float dry_r = mix_r + piano_r - part_piano_res_r - part_organ_res_r - part_guitar_res_r;
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
      if (piano_body_active_ && !part_fx_.enabled()) {
        const float drive_l = piano_raw_l;
        const float drive_r = piano_raw_r;
        // Radiation split: the board returns the phase-diffused complement of
        // the direct share (plus the modal colour), so most of the note reaches
        // the mix through the board rather than as the raw string waveform.
        const float dry_mono = 0.5f * (drive_l + drive_r);
        const float body = soundboard_.process(dry_mono);
        // The board's two radiation paths differ in phase, so its return is not
        // the same signal on both legs. Zero at a zero board width.
        const float side = soundboard_.last_side();
        const float symp = resonance_.process(soundboard_.last_diffused(), damper_open);
        const float piano_out_l =
            config_.gain * (kPianoDirectGain * piano_raw_l + body + side + symp);
        const float piano_out_r =
            config_.gain * (kPianoDirectGain * piano_raw_r + body - side + symp);
        piano_res_l = piano_out_l - piano_l;
        piano_res_r = piano_out_r - piano_r;
        mix_l += piano_out_l;
        mix_r += piano_out_r;
      }
      if (guitar_halo_active_ && !part_fx_.enabled()) {
        // Plucked-string sound halo: the open strings ring behind the note,
        // gated by damper_open exactly as the piano board is above -- a guitar's
        // open strings are damped unless the sustain pedal is holding them open.
        // Driven by guitar_l/guitar_r alone (the halo-eligible voices' own dry
        // mix), never by the rest of the GM bus. Skipped entirely when no
        // eligible voice has ever sounded, so every existing KS voicing with no
        // halo renders bit-identically.
        const float dry_mono = 0.5f * (guitar_raw_l + guitar_raw_r);
        const float symp = config_.gain * guitar_halo_.process(dry_mono, damper_open);
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
        residual_l_[at] = (mix_l - dry_l) - piano_res_l - guitar_res_l - part_piano_res_l -
                          part_organ_res_l - part_guitar_res_l;
        residual_r_[at] = (mix_r - dry_r) - piano_res_r - guitar_res_r - part_piano_res_r -
                          part_organ_res_r - part_guitar_res_r;
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
      // A factory-backed body's unbussed return was added to the common mix
      // after its dry source write. Attribute that return to the same part's
      // sources, while bussed returns have already been flushed through the
      // part or unit splitter above.
      for (size_t part = 0; part < 16; ++part) {
        if (any_bussed && fx->part_bussed[part]) continue;
        part_piano_splitters_[part].flush(source_outputs, source_output_count, n,
                                          chunk_part_piano_l_[part].data(),
                                          chunk_part_piano_r_[part].data(), offset, add_output);
        part_organ_splitters_[part].flush(source_outputs, source_output_count, n,
                                          chunk_part_organ_l_[part].data(),
                                          chunk_part_organ_r_[part].data(), offset, add_output);
        part_guitar_splitters_[part].flush(source_outputs, source_output_count, n,
                                           chunk_part_guitar_l_[part].data(),
                                           chunk_part_guitar_r_[part].data(), offset, add_output);
      }
    }
  }
  if (any_bussed && part_fx_.discard_sum(nullptr) != fx_discards_before) discarded = true;
  if (discarded) note_non_finite_discard();
}

}  // namespace sonare::midi::synth
