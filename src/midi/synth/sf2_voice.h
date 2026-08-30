#pragma once

/// @file sf2_voice.h
/// @brief SF2 generator resolution and the per-voice playback state used by
///        Sf2Player: zone-pair (preset x instrument) generator stacking per
///        the SoundFont 2.04 model, sample playback with root-key tuning,
///        loop modes and linear interpolation, the volume/modulation DAHDSRs,
///        mod/vibrato LFOs, the resonant lowpass (TPT SVF) and the default
///        modulator set (velocity -> attenuation/filter, CC-driven channel
///        modulation, pitch bend).
///
/// Generator semantics: INSTRUMENT-level generators are absolute values that
/// override the spec defaults; PRESET-level generators are RELATIVE additions
/// on top. A zone's effective value therefore stacks
///   default -> instrument global zone -> instrument zone (override)
///   + preset global zone + preset zone (add).
///
/// Supported modulators: the SF2 2.04 default modulators are implemented as
/// fixed behaviour:
/// velocity -> attenuation is the spec concave 960 cB curve, which works out
/// to exactly gain = (vel/127)^2; CC7/CC11 use the same law; velocity ->
/// initialFilterFc darkens soft notes; CC1 adds vibrato depth; CC10 offsets
/// pan; the pitch wheel applies the RPN0 bend range. CC91/93 resolve to
/// per-voice reverb/chorus send levels (consumed by the GS effect units).
/// Parsed custom preset/instrument modulator records are preserved on Sf2Zone
/// for inspection, but the realtime voice layer does not yet evaluate the
/// arbitrary SF2 modulator matrix.
///
/// RT contract: resolve_voice_params() / Sf2Voice methods are allocation-free
/// (audio thread); the referenced Sf2File data is read-only.

#include <cstdint>

#include "midi/synth/envelope.h"
#include "midi/synth/sf2_file.h"
#include "midi/synth/svf.h"
#include "midi/synth/voice_pool.h"
#include "rt/pan_law.h"
#include "util/constants.h"

namespace sonare::midi::synth {

/// Full-scale magnitude of an SF2 pan generator / MIDI CC10, in 0.1% units.
inline constexpr float kPanUnitsFullScale = 500.0f;

/// @brief Constant-power gains for a voice at @p pan_units.
///
/// Shared by the SF2 voices and the native-synth voices so both spread a voice
/// across the stereo field with the project's one pan law. Raw gains, not
/// balance-normalized: a voice is a point source being placed, not a stereo
/// image being rebalanced, so a centred voice sits at 1/sqrt(2) per channel and
/// the pair carries unit energy wherever it is placed.
inline rt::PanGains voice_pan_gains(float pan_units) noexcept {
  return rt::compute_pan_gains(pan_units / kPanUnitsFullScale, rt::PanLaw::Const3dB);
}

/// Effective generator values for one (preset zone, instrument zone) pair.
/// Values are stored in raw SF2 units (timecents, centibels, cents, ...).
class Sf2GenSet {
 public:
  static constexpr int kNumGens = 61;

  Sf2GenSet() noexcept;  // spec defaults

  /// Instrument-level zone: absolute override.
  void apply_absolute(const Sf2Zone& zone) noexcept;
  /// Preset-level zone: relative addition (index/range/sample generators are
  /// never additive and are skipped).
  void add_relative(const Sf2Zone& zone) noexcept;

  int32_t get(uint16_t oper) const noexcept { return oper < kNumGens ? values_[oper] : 0; }

 private:
  int32_t values_[kNumGens];
};

/// SF2 timecents -> seconds (2^(tc/1200)); tc <= -12000 is ~1 ms or less.
float timecents_to_seconds(int32_t timecents) noexcept;
/// SF2 centibels (attenuation) -> linear gain (10^(-cb/200)).
float centibels_to_gain(float centibels) noexcept;
/// SF2 absolute cents -> Hz (8.176 Hz * 2^(cents/1200)).
float abs_cents_to_hz(float cents) noexcept;

/// Effect-send depth at a full-scale (127) controller. One definition for every
/// path that turns a 0..127 send amount into a linear send: the CC91/93/94
/// channel sends, the GS EFX unit's own send amounts, and the GS per-note drum
/// reverb/chorus block — a 0..127 send has to mean the same depth whichever
/// message carried it, so a GS drum kit is not drier than the melodic parts.
inline constexpr float kCcSendDepth = 0.35f;

/// MODULATION LFO1 PITCH DEPTH (40 2x 04) at full scale, from the manual's
/// 0-600 cents over 0-127. It is a per-part quantity rather than a constant, so
/// this is only the top of the range; the power-on 0A gives a fully-raised mod
/// wheel 47.2 cents of vibrato until a file writes the address.
inline constexpr float kGsModDepthMaxCents = 600.0f;

/// The GS power-on MODULATION LFO1 PITCH DEPTH.
inline constexpr uint8_t kGsModDepthDefault = 0x0A;

/// The vibrato depth a MODULATION LFO1 PITCH DEPTH byte asks for, in cents.
constexpr float gs_mod_depth_cents(uint8_t value) noexcept {
  return kGsModDepthMaxCents * static_cast<float>(value) / 127.0f;
}

/// Per-channel modulation snapshot the player passes into voice rendering
/// (recomputed when the channel's CC/bend state changes).
struct Sf2ChannelMod {
  /// Pitch wheel offset in cents (bend scaled by the RPN0 bend range).
  float pitch_cents = 0.0f;
  /// CC7 volume x CC11 expression as linear gain ((cc/127)^2 each).
  float gain = 1.0f;
  /// CC1 mod wheel x the part's MODULATION LFO1 PITCH DEPTH, in cents.
  float extra_vibrato_cents = 0.0f;
  /// CC1 mod wheel x the part's MODULATION TVF CUTOFF CONTROL (40 2x 01), in
  /// cents, added to the voice's cutoff. Read per sample rather than baked at
  /// the note-on: the wheel is what a player moves while a note is sounding.
  float mod_cutoff_cents = 0.0f;
  /// CC1 mod wheel x the part's MODULATION LFO1 RATE CONTROL (40 2x 03), as the
  /// frequency multiplier the vibrato LFO advances by. 1.0 is no change, and it
  /// is a multiplier rather than an added rate so the wheel scales the exponent
  /// and an unraised one is exactly inert.
  float vib_rate_scale = 1.0f;
  /// CC1 mod wheel x the part's MODULATION LFO1 TVA DEPTH (40 2x 06), as the
  /// fraction of the voice's amplitude the LFO's trough takes away. 0 is no
  /// tremolo. The swing is spent below the part's level rather than around it:
  /// a depth of 1 reaches silence, and no depth makes a part louder than the
  /// volume it was given.
  float tremolo_depth01 = 0.0f;
  /// CC1 itself in [0,1]. Carried rather than recovered from the line above,
  /// which stopped being a fixed multiple of it once the depth became a part
  /// parameter a file can write.
  float mod_wheel01 = 0.0f;
  /// CC10 pan offset in SF2 pan units (-500..500 added to the zone pan).
  float pan_units = 0.0f;
  /// CC91/CC93 -> effect send contribution in [0,1] (default mod: 200/1000
  /// at full controller). Added to the zone's send generators.
  float reverb_send = 0.0f;
  float chorus_send = 0.0f;
  /// CC94 -> GS delay send (channel-level only; SF2 has no delay generator).
  float delay_send = 0.0f;
  /// Effect sends for synth-fallback voices: the CC send plus the program's
  /// baked-in ambience floor (gm_fallback_sends). SF2 zones carry their own
  /// send generators, so the floor applies to the fallback path only.
  float fallback_reverb_send = 0.0f;
  float fallback_chorus_send = 0.0f;
};

/// SF2 triangle LFO: starts at zero after its delay, rises positive first.
class Sf2Lfo {
 public:
  void start(double sample_rate, float delay_seconds, float freq_hz) noexcept {
    const double sr = sample_rate > 0.0 ? sample_rate : 48000.0;
    delay_samples_ = delay_seconds > 0.0f ? static_cast<int64_t>(delay_seconds * sr) : 0;
    inc_ = freq_hz > 0.0f ? static_cast<float>(freq_hz / sr) : 0.0f;
    phase_ = 0.0f;
  }

  /// Advance one sample; returns the bipolar triangle value in [-1, 1].
  /// @p rate_scale multiplies the frequency for this sample only, which is how
  /// a controller retunes the LFO under a held note: the phase carries across
  /// unchanged, so a scale that moves stays continuous and 1.0 is exact.
  float next(float rate_scale = 1.0f) noexcept {
    if (delay_samples_ > 0) {
      --delay_samples_;
      return 0.0f;
    }
    // Triangle from phase [0,1): 0 -> +1 -> 0 -> -1 -> 0.
    const float p = phase_;
    phase_ += inc_ * rate_scale;
    if (phase_ >= 1.0f) phase_ -= 1.0f;
    if (p < 0.25f) return 4.0f * p;
    if (p < 0.75f) return 2.0f - 4.0f * p;
    return 4.0f * p - 4.0f;
  }

 private:
  int64_t delay_samples_ = 0;
  float inc_ = 0.0f;
  float phase_ = 0.0f;
};

/// Playback parameters resolved from a zone pair for one note-on.
struct Sf2VoiceParams {
  // Sample addressing (pool indices, offsets already applied).
  uint32_t start = 0;
  uint32_t end = 0;
  uint32_t loop_start = 0;
  uint32_t loop_end = 0;
  /// 0 = no loop, 1 = continuous loop, 3 = loop while key down.
  int loop_mode = 0;
  /// Sample-position increment per output sample at the voice's pitch.
  double pitch_increment = 1.0;
  /// Linear gain from initialAttenuation (velocity handling is separate).
  float attenuation_gain = 1.0f;
  /// Zone pan in SF2 units (-500..500); the mixer combines it with CC10.
  float pan_units = 0.0f;
  /// Volume envelope (already converted to ms / level units).
  DahdsrConfig volume_env;

  // --- filter (resonant lowpass) ---
  /// Effective initialFilterFc in absolute cents (velocity offset applied).
  float filter_fc_cents = 13500.0f;
  /// Resonance Q factor (from initialFilterQ centibels).
  float filter_q = 0.707f;
  /// True when the filter section can be bypassed (open Fc, no modulation).
  bool filter_bypass = true;

  // --- modulation envelope ---
  DahdsrConfig mod_env;
  float mod_env_to_pitch = 0.0f;      // cents at full envelope
  float mod_env_to_filter_fc = 0.0f;  // cents at full envelope

  // --- LFOs ---
  float mod_lfo_delay_s = 0.0f;
  float mod_lfo_freq_hz = 8.176f;
  float mod_lfo_to_pitch = 0.0f;      // cents
  float mod_lfo_to_filter_fc = 0.0f;  // cents
  float mod_lfo_to_volume_cb = 0.0f;  // centibels
  float vib_lfo_delay_s = 0.0f;
  float vib_lfo_freq_hz = 8.176f;
  float vib_lfo_to_pitch = 0.0f;  // cents

  // --- effect sends (consumed by the GS effect units) ---
  float reverb_send = 0.0f;  // [0,1] from reverbEffectsSend (0.1% units)
  float chorus_send = 0.0f;  // [0,1] from chorusEffectsSend
  /// Per-note drum send multiplicands (GS 41 m5/m6/m9 rr, NRPN 1D/1E/1F). Each
  /// scales everything the note sends into that unit — the zone generator above
  /// and the part's own CC send alike, after the render sums them. Delay is the
  /// degenerate case: SF2 has no delay generator, so the part's CC94 send is
  /// all there is to scale.
  float reverb_send_scale = 1.0f;
  float chorus_send_scale = 1.0f;
  float delay_send_scale = 1.0f;

  /// Exclusive class (hi-hat choke groups); 0 = none.
  int exclusive_class = 0;
};

/// Resolves the effective parameters for (@p key, @p velocity) at
/// @p output_sample_rate from the stacked generators and the sample header
/// (root key / correction / loops). Velocity applies the default-modulator
/// filter darkening; the velocity GAIN is returned separately by
/// sf2_velocity_gain().
Sf2VoiceParams resolve_voice_params(const Sf2GenSet& gens, const Sf2Sample& sample, uint8_t key,
                                    uint8_t velocity, double output_sample_rate) noexcept;

/// Spec default modulator: velocity -> attenuation concave 960 cB curve,
/// equivalent to (vel/127)^2 linear gain.
float sf2_velocity_gain(uint8_t velocity) noexcept;
/// Same law for CC7 volume / CC11 expression.
float sf2_cc_gain(uint8_t value) noexcept;

/// One playing SF2 voice (lives in a VoicePool inside Sf2Player).
struct Sf2Voice : VoiceState {
  const float* data = nullptr;  // sample pool base (read-only)
  Sf2VoiceParams params;
  double pos = 0.0;
  float velocity_gain = 1.0f;
  DahdsrEnvelope env;
  DahdsrEnvelope mod_env;
  Sf2Lfo mod_lfo;
  Sf2Lfo vib_lfo;
  TptSvf filter;
  bool key_down = false;
  /// Captured by the sostenuto pedal (CC66 down while the key was held): the
  /// note keeps ringing after note-off until the pedal lifts.
  bool sostenuto = false;
  /// Portamento (CC5 / CC65 / CC84): pitch offset in cents from the glide
  /// source note, decaying to zero through a one-pole. Same representation and
  /// same decay law as the NativeSynth voice's glide, so a part sounds alike
  /// whichever host plays it. Both zero = not gliding.
  float glide_cents = 0.0f;
  float glide_coeff = 0.0f;
  // Cached stereo gains for (zone pan + channel pan); recomputed on change.
  // Seeded centred, which under the constant-power law is 1/sqrt(2) a side.
  float cached_pan_units = 1.0e9f;
  float gain_left = ::sonare::constants::kInvSqrt2;
  float gain_right = ::sonare::constants::kInvSqrt2;

  /// Starts playback of @p p (pool base @p pool_data) at @p velocity_gain_in.
  void start(const float* pool_data, const Sf2VoiceParams& p, double sample_rate,
             float velocity_gain_in) noexcept;
  /// Renders one mono sample (envelope/filter/LFO applied; pan is applied by
  /// the mixer via gain_left/right, refreshed from @p mod). Returns 0 and
  /// deactivates once the sample / envelope ends.
  float render(const Sf2ChannelMod& mod) noexcept;
  /// Note-off: release the envelopes; mode-3 loops stop looping.
  void release() noexcept;
  /// Stop this voice now, over a fade short enough to read as a cut. Unlike
  /// release() it does not use the zone's own release, which on a pad runs for
  /// seconds and would leave a note it was told to silence audible.
  void choke(double sample_rate) noexcept;
};

}  // namespace sonare::midi::synth
