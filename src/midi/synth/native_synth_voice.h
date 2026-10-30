#pragma once

/// @file native_synth_voice.h
/// @brief NativeSynthVoice: one playing voice of the patch-driven engine, with its
///        tail-length helpers, per-note drum overrides and live-change crossfade.

#include <array>
#include <cstdint>

#include "midi/control_value.h"
#include "midi/controller_profile.h"
#include "midi/mpe.h"
#include "midi/per_note_state.h"
#include "midi/synth/additive_voice.h"
#include "midi/synth/body_resonator.h"
#include "midi/synth/bowed_string_voice.h"
#include "midi/synth/brass_voice.h"
#include "midi/synth/channel_param_state.h"
#include "midi/synth/envelope.h"
#include "midi/synth/excitation_axes.h"
#include "midi/synth/filter_models.h"
#include "midi/synth/flute_voice.h"
#include "midi/synth/fm_voice.h"
#include "midi/synth/free_reed_voice.h"
#include "midi/synth/gs_layer.h"
#include "midi/synth/harpsichord_voice.h"
#include "midi/synth/ks_voice.h"
#include "midi/synth/mod_matrix.h"
#include "midi/synth/modal_voice.h"
#include "midi/synth/native_synth_patch.h"
#include "midi/synth/oscillator.h"
#include "midi/synth/percussion_voice.h"
#include "midi/synth/piano_voice.h"
#include "midi/synth/pipe_organ_voice.h"
#include "midi/synth/plucked_string_voice.h"
#include "midi/synth/reed_voice.h"
#include "midi/synth/sample_bank.h"
#include "midi/synth/sample_voice.h"
#include "midi/synth/sf2_voice.h"
#include "midi/synth/vocal_voice.h"
#include "midi/synth/voice_pool.h"

namespace sonare::midi::synth {

/// How long a voice of @p patch can sound after its last event, in samples. A
/// zero-sustain envelope is ended by its decay, and a one-shot by its envelope;
/// a one-shot whose envelope holds is ended by its engine
/// (sustained_one_shot_tail_samples). INT64_MAX when nothing ends the voice.
int64_t native_patch_tail_samples(const NativeSynthPatch& patch, double sample_rate,
                                  const EnvelopeTimeScales& scales,
                                  const SampleBank* bank) noexcept;

/// What ends a one-shot whose amplitude envelope holds, which no note-off can:
/// FM carriers finishing, a percussion piece falling silent, an unlooped sample
/// running out. INT64_MAX for every other engine, which nothing ends.
int64_t sustained_one_shot_tail_samples(const NativeSynthPatch& patch, double sample_rate,
                                        const SampleBank* bank) noexcept;

/// Per-note GS drum overrides applied to a fallback percussion voice at
/// note-on (NRPN pitch coarse / TVA level / absolute pan). Defaults are no-ops,
/// so a voice with no per-note edit renders exactly as before.
struct DrumVoiceMod {
  float pitch_ratio = 1.0f;  ///< 2^(pitch_coarse / 12)
  float level_gain = 1.0f;   ///< (level / 127)^2 (same square law as CC7)
  float pan_units = 1.0e9f;  ///< absolute pan units (1e9 = untouched: keep channel pan)
  /// Send multiplicands over [0,1] (41 m5/m6/m9 rr): each scales this note's
  /// whole contribution to that unit's bus.
  float reverb_scale = 1.0f;
  float chorus_scale = 1.0f;
  float delay_scale = 1.0f;
  /// The note this strike SOUNDS, which PLAY NOTE NUMBER (41 m1 rr) and a user
  /// drum set's SOURCE NOTE NUMBER (21 dC rr) both move. -1 = the struck one.
  /// Carried here rather than substituted into the voice's own note, which stays
  /// the struck one because that is what a note-off matches.
  int16_t play_note = -1;
  /// ASSIGN GROUP (41 m3 rr): the exclusive group this strike joins, 0 = none.
  /// -1 = the kit piece's own, which is what a host with no GS layer means.
  int16_t exclusive_class = -1;
};

/// Crossfade length for a live change that crosses a stage's off point (drive,
/// body), where the law itself is discontinuous at 0.
inline constexpr float kLawFadeSeconds = 0.005f;

/// Progress of a crossfade from a retiring law to the current one; step 0 means
/// no fade is running and only the current law is computed.
struct LawFade {
  float fade01 = 1.0f;
  float step = 0.0f;
};

/// One playing subtractive voice (lives in a VoicePool inside NativeSynth and
/// in Sf2Player's fallback pool). Renders mono; the mixer applies
/// gain_left/right (refreshed from the channel pan like Sf2Voice).
struct NativeSynthVoice : VoiceState {
  const NativeSynthPatch* patch = nullptr;
  std::array<VaOscillator, kMaxUnisonOscs> oscs{};
  std::array<float, kMaxUnisonOscs> detune_ratio{};
  int unison = 1;
  float osc_norm = 1.0f;  // 1/sqrt(unison)
  /// The subtractive section's second oscillator and noise, latched at note-on.
  VaOscillator osc2;
  VaOscillator noise_osc;
  VaOscillator am_noise_osc;
  TptSvf am_noise_lp;
  float osc2_ratio = 1.0f;
  float osc2_level = 0.0f;
  float osc2_dry = 0.0f;
  float osc2_dry_sample = 0.0f;
  /// The unison stack's stereo difference (patch va.unison_width): each
  /// oscillator's weight, the filter and highpass copies it runs through, and
  /// the sample the mixer reads beside render()'s, L = mid - side, R = mid + side.
  float unison_width = 0.0f;
  std::array<float, kMaxUnisonOscs> side_weight{};
  SynthFilter side_filter;
  TptSvf side_hp;
  float side_hold = 0.0f;
  float side_sample = 0.0f;
  float noise_level = 0.0f;
  float noise_am = 0.0f;  // depth already divided by the lowpassed noise's RMS
  VaOscillator band_noise_osc;
  TptSvf band_noise_hp;
  TptSvf band_noise_lp;
  TptSvf band_noise_lp2;
  float band_noise_level = 0.0f;
  float base_freq_hz = constants::kA4Hz;
  float velocity_gain = 1.0f;
  /// The exclusive/mute group this voice was STARTED in, which a GS ASSIGN
  /// GROUP write moves away from the kit piece's own. Held per voice because
  /// one kit piece is shared by every note that resolves to it.
  uint8_t exclusive_class = 0;
  /// Static cutoff offset precomputed at start (velocity + key tracking).
  float static_cutoff_cents = 0.0f;
  /// Pre-filter drive gain / makeup (precomputed from patch->drive; 0 = off).
  float drive_gain = 0.0f;
  float drive_makeup = 1.0f;
  /// The drive law being faded out after a change across 0 (gain 0 = bypass).
  float drive_fade_gain = 0.0f;
  float drive_fade_makeup = 1.0f;
  LawFade drive_fade;
  DahdsrEnvelope amp_env;
  DahdsrEnvelope filter_env;
  SynthFilter filter;
  /// False while the filter stage is skipped as inaudible, so its state is stale.
  bool filter_state_live = true;
  /// Series highpass stage (patch hp_cutoff_hz); idle when the patch asks for
  /// no highpass. A bare SVF rather than a second SynthFilter: this stage is
  /// always the SVF highpass tap, so the other three models would be dead
  /// state on every voice.
  TptSvf hp_stage;
  /// Whether this voice runs that stage. Decided at note-on and changed only by
  /// refresh_live(), which prepares the stage when it opens and retunes it in
  /// place while it runs; render() never reads the patch cutoff directly, which
  /// would open the stage on a voice that never prepared or set it.
  bool hp_active = false;
  // Converter stage. `hold_step` is how much of a held period one sample
  // advances, so the rate is compared against the mix rate once at note-on
  // rather than per sample.
  float hold_step = 0.0f;
  float hold_phase = 0.0f;
  float hold_value = 0.0f;
  float quant_step = 0.0f;
  FmVoiceCore fm;
  /// KS string core; the host attach()es its delay span before start() (the
  /// slab is owned by the instrument and allocated in prepare()).
  KsVoiceCore ks;
  ModalVoiceCore modal;
  AdditiveVoiceCore additive;
  PercussionVoiceCore percussion;
  /// Piano string core; like KS, the host attach()es its delay slab before
  /// start().
  PianoVoiceCore piano;
  /// A factory-backed NativeSynth routes piano bodies through a per-part bank.
  /// These are captured after start() and consumed by the host on the first
  /// render sample, once the published routing snapshot is known. Deferring
  /// the strike avoids exciting the shared no-EFX board for a note whose EFX
  /// assignment was still dirty at note-on.
  float piano_case_strike_pending = 0.0f;
  float piano_board_strike_pending = 0.0f;
  /// Flue-pipe core; like KS, the host attach()es its delay span before
  /// start().
  PipeOrganVoiceCore pipe_organ;
  /// Bowed-string core; like KS, the host attach()es its delay slab before
  /// start().
  BowedStringVoiceCore bowed_string;
  /// Reed-woodwind core; like KS, the host attach()es its bore span before
  /// start().
  ReedVoiceCore reed;
  /// Brass / lip-reed core; like KS, the host attach()es its bore span before
  /// start().
  BrassVoiceCore brass;
  /// Air-jet flute core; like KS, the host attach()es its delay slab (bore +
  /// jet spans) before start().
  FluteVoiceCore flute;
  /// Buzzing-bridge plucked-string core; like KS, the host attach()es its delay
  /// span before start().
  PluckedStringVoiceCore plucked_string;
  /// Source-filter vocal core; like KS, the host attach()es its section history
  /// before start().
  VocalVoiceCore vocal;
  /// Free-reed core (no host slab; the driven tongue oscillator is feed-forward).
  FreeReedVoiceCore free_reed;
  /// Harpsichord core; the host attach()es its registration slab before start().
  HarpsichordVoiceCore harpsichord;
  /// Host PCM source; like KS, the host attach()es the bank before start().
  SampleVoiceCore sampler;
  BodyResonator body;
  /// Body crossfade after a bodyMix change across 0; body_fade_in names the
  /// direction (true = dry to body). A finished fade-out stops the bank.
  LawFade body_fade;
  bool body_fade_in = false;
  Sf2Lfo vibrato_lfo;
  Sf2Lfo lfo2;
  Sf2Lfo drift_lfo;
  float drift_depth_cents = 0.0f;
  // Mod-matrix source constants (precomputed at start).
  bool has_matrix = false;
  /// At least one live route lands on an axis the engine owns rather than the
  /// wrapper, so its control setters are worth calling each sample.
  bool has_engine_control_routes = false;
  float velocity01 = 0.0f;
  /// Polyphonic aftertouch for this note, added to the channel's own before it
  /// reaches the matrix. Per voice because that is the granularity the message
  /// carries; cleared at note-on so a voice never inherits the last one's.
  float poly_pressure01 = 0.0f;
  float key_track_octaves = 0.0f;
  float random_value = 0.0f;
  /// Previous sample's ModDestination::kLfo1RateScale, applied to LFO1 on the
  /// next one. LFO1 feeds the matrix that sets this, so the delay is what keeps
  /// the routing acyclic.
  float matrix_lfo1_rate_scale = 1.0f;
  // Glide: pitch offset in cents decaying to zero through a one-pole.
  float glide_cents = 0.0f;
  float glide_coeff = 0.0f;
  /// Interval in cents from the note this voice was STARTED on to the note it
  /// is sounding now, which a legato continuation moves and nothing else does.
  /// It joins the per-sample pitch sum rather than base_freq_hz because that is
  /// the one argument every engine's render() already takes: a waveguide reads
  /// its line at the new period, an oscillator retunes, and no engine needs a
  /// second entry point to be carried into a new note. Permanent, unlike
  /// glide_cents, which only carries the transition into it.
  float retune_cents = 0.0f;
  /// Seeded per-voice pan scatter (patch stereo_spread; pan units).
  float pan_spread_units = 0.0f;
  /// The channel mod this voice reads instead of its channel's, and whether it
  /// has one. Set only on a member channel carrying more than one sounding
  /// note, where the channel's bend and pressure belong to the note the
  /// tracking rule names and the others keep the manager's contribution alone.
  /// MPE's normal form is one note per member channel, so this is inert
  /// wherever a sender is doing what the specification expects.
  bool mpe_mod_active = false;
  Sf2ChannelMod mpe_mod{};
  /// Once a member key has gone up, its own MPE pressure/timbre/bend stay at
  /// their NoteOff values while ordinary channel controls and manager bend can
  /// continue to update the sounding tail.
  bool mpe_release_frozen = false;
  uint32_t mpe_frozen_axis_mask = kAxisNone;
  uint32_t mpe_frozen_axis_present = kAxisNone;
  uint32_t mpe_frozen_source_mask = kAxisNone;
  std::array<float, kControllerAxisCount> mpe_frozen_axis_values{};
  float mpe_frozen_member_bend_cents = 0.0f;
  float mpe_frozen_aftertouch01 = 0.0f;
  /// Last engine-axis values written to this voice, including per-note
  /// profile bindings. The source masks are mutually exclusive per axis;
  /// they let a manager reset remove only its own folded contribution while
  /// leaving a member or ordinary per-note writer in place.
  ControllerAxisState live_excitation_axes;
  uint32_t live_mpe_pressure_axes = kAxisNone;
  uint32_t live_mpe_timbre_axes = kAxisNone;
  uint32_t live_mpe_bend_axes = kAxisNone;
  /// The member's own MPE values at the time this voice last received them.
  /// Manager pressure/timbre is combined with these raw values for each voice
  /// separately, so a manager update cannot collapse several tracked notes
  /// into the current channel attribution.
  Control32 mpe_member_pressure = Control32::from_raw(0);
  Control32 mpe_member_timbre = Control32::from_raw(0);
  bool mpe_member_pressure_present = false;
  bool mpe_member_timbre_present = false;
  bool key_down = false;
  /// Captured by the sostenuto pedal (CC66): held past key-up until the pedal
  /// lifts, regardless of the sustain pedal.
  bool sostenuto = false;
  // Cached stereo gains for the channel pan; recomputed on change. Seeded
  // centred, which under the constant-power law is 1/sqrt(2) a side.
  float cached_pan_units = 1.0e9f;
  float gain_left = ::sonare::constants::kInvSqrt2;
  float gain_right = ::sonare::constants::kInvSqrt2;
  /// GS per-note drum overrides (pitch coarse / absolute pan; level is folded
  /// into velocity_gain at start). Defaults are no-ops.
  float drum_pitch_ratio = 1.0f;
  float drum_pan_units = 1.0e9f;
  /// GS per-note drum send multiplicands; the mixer scales the channel's send
  /// into each unit by them.
  float drum_reverb_scale = 1.0f;
  float drum_chorus_scale = 1.0f;
  float drum_delay_scale = 1.0f;
  /// GS melodic part edits that the render reads rather than start() folding
  /// away: the patch fields they modify are automation targets, so latching a
  /// finished value here would freeze a sweep on a held note.
  float gs_resonance_gain = 1.0f;
  float gs_vib_depth_cents = 0.0f;
  /// GS SCALE TUNING for the key that started this voice, in cents (0 = in
  /// tune). Constant for the voice's life, and in the render's pitch sum rather
  /// than in base_freq_hz so it reaches every engine.
  float gs_scale_cents = 0.0f;
  /// Set by a cutoff or resonance edit; engages the filter stage the way the
  /// SoundFont bank's filter_bypass = false does.
  bool gs_filter_edited = false;

  // Note-on inputs refresh_live() re-derives patch-dependent state from, so a
  // refreshed voice matches one started at the new value.
  float note_offset_semitones = 0.0f;  ///< voiced note - 60
  uint8_t base_voiced_note = 60;       ///< voiced note base_freq_hz was computed for
  float part_cutoff_cents = 0.0f;
  float part_attack_scale = 1.0f;
  float part_decay_scale = 1.0f;
  float part_release_scale = 1.0f;
  float part_vib_rate_scale = 1.0f;
  std::array<float, kMaxUnisonOscs> unison_spread{};
  float drift_seed = 0.0f;
  float drift_rate_scale = 1.0f;  ///< seeded per-voice factor on drift_rate_hz
  float pan_scatter = 0.0f;
  float sampler_pan_units = 0.0f;
  /// Set by choke(): the voice is being cut and takes no live refresh.
  bool choked = false;
  /// Consecutive released samples whose chain output stayed under the silence floor, and the run
  /// (a time quantity set in start()) at which the slot is freed.
  uint32_t silent_run = 0;
  uint32_t silent_run_limit = 0;
  /// Amp level latched at note-off while the undamped 4' tail sounds; the tail
  /// holds it past the shared envelope's idle. Choke paths fade it with the
  /// envelope's release law.
  bool harpsichord_tail_latched = false;
  float harpsichord_tail_level = 0.0f;
  float harpsichord_tail_choke_multiplier = 0.0f;

  /// Starts the voice for @p p. note/channel/age must already be set (the
  /// pool fills them in allocate()); @p voice_index seeds the deterministic
  /// per-voice variation unless the patch retriggers from the note. @p p must outlive the voice. @p
  /// glide_from_hz != 0 glides the pitch from that frequency (portamento; needs p.glide_ms > 0).
  /// @p una_corda engages the soft-pedal voicing (piano mode only).
  /// @p drum_kit != 0 applies a GS kit variation (Room/Power/808/...) to the
  /// resolved drum patch at note-on (percussion mode only; see apply_gs_drum_kit).
  /// @p drum_mod carries GS per-note drum NRPN edits (pitch coarse / level /
  /// pan / the three send multiplicands); default is a no-op. @p organ_percussion fires the drawbar
  /// organ's single-shot on this note (additive mode only; the channel decides which note gets it).
  /// @p part_mod carries the GS melodic part edits (TONE MODIFY / the part NRPNs); default is a
  /// no-op.
  void start(const NativeSynthPatch& p, double sample_rate, Velocity16 velocity,
             uint32_t voice_index, float glide_from_hz = 0.0f, bool una_corda = false,
             uint8_t drum_kit = 0, DrumVoiceMod drum_mod = {}, bool organ_percussion = false,
             GsPartMod part_mod = {}) noexcept;
  /// True when the patch's filter stage cannot colour this voice: a wide-open
  /// static SVF lowpass, no resonance, no envelope depth and no negative
  /// static offset. Read from the LIVE patch on every sample rather than
  /// latched at start(): `cutoff_hz` and `resonance_q` are automation targets
  /// that apply to already-sounding voices, so a sweep on a held note has to
  /// engage the stage on the block it starts, not on the next note-on.
  bool filter_inaudible() const noexcept {
    return !gs_filter_edited && patch->filter_model == SynthFilterModel::kSvf &&
           patch->filter_output == SynthFilterOutput::kLowpass && patch->cutoff_hz >= 18000.0f &&
           patch->env_to_cutoff_cents == 0.0f && static_cutoff_cents >= 0.0f &&
           patch->resonance_q <= 0.71f;
  }
  /// Hands the engine its excitation-axis bases and jumps the smoothing to
  /// them, so a note struck mid-phrase starts at the host's live controller
  /// positions rather than gliding in from the preset. @p present names the
  /// axes the caller filled; an axis it omits keeps the patch's own value.
  /// A mode that owns no axis is a no-op.
  void seed_excitation(const ExcitationAxes& base, uint32_t present) noexcept;
  /// Pushes live excitation-axis bases to a sounding voice (a CC arriving
  /// mid-note), without the snap: the engine's own ramp owns the approach.
  void push_excitation(const ExcitationAxes& base, uint32_t present) noexcept;
  /// Drops a synthesized onset the engine would play over the player's own (a
  /// profile that calls velocity meaningless). A mode with none is a no-op.
  void hold_onset() noexcept;
  /// Restores each engine's start() excitation base (before any controller seeded it) without
  /// restarting its phase, envelope, or live matrix offset.
  void restore_excitation_base() noexcept;
  /// Renders one mono sample. Deactivates when the amp envelope ends.
  /// @p wind_pitch / @p wind_gain carry the shared organ wind modulation
  /// (tremulant / wind sag); 1.0 leaves the voice unmodulated.
  float render(const Sf2ChannelMod& mod, float wind_pitch = 1.0f, float wind_gain = 1.0f) noexcept;
  /// The left and right legs of render()'s @p s at this voice's pan gains,
  /// with the unison stack's stereo difference when the patch spreads it.
  void stereo_legs(float s, float* left, float* right) const noexcept {
    if (unison_width > 0.0f) {
      *left = (s - side_sample) * gain_left;
      *right = (s + side_sample) * gain_right;
    } else {
      *left = s * gain_left;
      *right = s * gain_right;
    }
  }
  /// Legato continuation: carry this sounding voice to @p new_note, whose
  /// tracking inputs (filter and body) are those of a fresh voice on
  /// @p voiced_note.
  ///
  /// The exciter, the delay line and both envelopes are left exactly as they
  /// are; only the pitch moves, through retune_cents, with glide_cents taking
  /// up the difference so the transition is continuous rather than a step. A
  /// patch with no portamento lands on the new pitch immediately, which is what
  /// a slur with no glide is. `note` becomes the new key, because that is what
  /// the late note-off of the old key must NOT match (holding the old key here
  /// is what would cut the slur). @p per_note_shift_cents is the change of the
  /// per-note pitch correction applied beside retune_cents, which the glide
  /// origin has to account for to stay on the sounding pitch.
  void retune(uint8_t new_note, uint8_t voiced_note, float per_note_shift_cents,
              double sample_rate) noexcept;
  /// Starts the body stage of @p p at @p note_hz: a bowed voice naming a series corpus runs
  /// its member's admittance in series; every other voice runs the wrapper's parallel bank.
  void start_body(const NativeSynthPatch& p, double sample_rate, float note_hz) noexcept;
  /// Frequency of the voiced key, which is what a note-tracked body follows.
  float voiced_freq_hz() const noexcept {
    return base_freq_hz * std::exp2(voiced_shift_cents() * (1.0f / constants::kCentsPerOctave));
  }
  /// Cents between the key base_freq_hz was computed for and the voiced key now.
  float voiced_shift_cents() const noexcept {
    return 100.0f * (note_offset_semitones + 60.0f - static_cast<float>(base_voiced_note));
  }
  /// Re-derives the state start() latched from @p p for the parameters in
  /// @p mask (bit = NativeSynthParamId), from the same derivations and the
  /// note-on inputs saved above. Running state (phases, levels) is kept.
  /// Audio thread; no allocation.
  void refresh_live(const NativeSynthPatch& p, double sample_rate, uint64_t mask) noexcept;
  /// Note-off: enter release (ignored by one-shot patches).
  void release() noexcept;
  /// Immediate silence (All Sound Off / steal-kill).
  void kill() noexcept;
  /// Exclusive-group choke: force the amp envelope into release even for
  /// one-shot (drum) voices, so a same-group strike (hi-hat / triangle / ...)
  /// cuts this ringing voice with a short fade instead of an abrupt kill. The
  /// fade length is the patch's amp release_ms.
  void choke() noexcept;
  /// The same, over kChokeReleaseMs instead of the patch's own release. GS
  /// ASSIGN MODE SINGLE needs this: on a sustaining patch the patch release runs
  /// past a second, which would leave audible the note it was told to replace.
  void choke_fast(double sample_rate) noexcept;
};

}  // namespace sonare::midi::synth
