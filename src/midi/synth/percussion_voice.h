#pragma once

/// @file percussion_voice.h
/// @brief Membrane-modal + filtered-noise percussion core for the NativeSynth
///        voice — the data-free GM drum kit (synthesis method (6) of the
///        instrument build plan; Rossing, Cook).
///
/// Two summed layers per kit piece:
///   - TONE: a small modal bank at the circular-membrane (Rayleigh) ratios
///     1 : 1.59 : 2.14 : 2.30 : 2.65 with a DESCENDING pitch envelope (the
///     struck-membrane tension release that makes a kick/tom read as a drum
///     and not a sine blip). The base frequency tracks the struck key or is
///     pinned per piece (snare shell, cymbal bell).
///   - NOISE: a seeded noise burst with an exponential level decay through a
///     dedicated TPT SVF band (snare wires = band-pass crack, hats/cymbals =
///     high-pass shimmer).
/// Pieces are config PODs in the GM fallback drum map; voices play one-shot
/// (the patch's one_shot flag) so note-off never chokes a strike.
///
/// RT contract: start()/render() are allocation-free. Determinism: noise is
/// the counter-based (voice_index, note, age) stream — every bounce is
/// bit-identical while distinct strikes still decorrelate.

#include <array>
#include <cstdint>

#include "midi/synth/body_resonator.h"
#include "midi/synth/filter_models.h"
#include "midi/synth/svf.h"
#include "midi/synth/voice_random.h"

namespace sonare::midi::synth {

inline constexpr int kMaxPercussionModes = 6;
inline constexpr int kMaxShellModes = 4;

/// Percussion section of a NativeSynthPatch (used when mode == kPercussion).
struct PercussionPatchParams {
  /// GM kit mode: instead of playing this single kit piece on every key,
  /// note-on resolves the struck note through the GM drum map
  /// (gm_fallback_drum_patch), so one patch is the whole kit — the
  /// `drum-kit` preset. The remaining fields are ignored when set.
  bool gm_kit = false;

  /// GM exclusive/mute group (0 = none). Same-group note-ons on the channel
  /// choke each other: hi-hats (closed/pedal/open), mute/open triangle, the
  /// two whistles, mute/open surdo. Resolved per note in the GM drum map;
  /// unused outside kit playback. C-ABI non-exposed like the rest of this POD.
  uint8_t exclusive_class = 0;

  // --- membrane/tone layer ---
  int num_modes = 0;
  /// Mode ratios to the base frequency (circular membrane: 1, 1.59, 2.14,
  /// 2.30, 2.65).
  std::array<float, kMaxPercussionModes> mode_ratios = {1.0f, 1.59f, 2.14f, 2.3f, 2.65f, 0.0f};
  /// Fundamental t60 (seconds) of the tone layer.
  float mode_decay_s = 0.3f;
  /// Tone layer mix gain.
  float tone_gain = 1.0f;
  /// Base frequency override in Hz (0 = the struck key's frequency).
  float base_freq_hz = 0.0f;
  /// Strike pitch overshoot: the tone starts (1 + pitch_drop) x the base
  /// frequency and falls back through a one-pole (0 = static pitch).
  float pitch_drop = 0.0f;
  float pitch_drop_ms = 40.0f;

  // --- strike point (membrane excitation weighting) ---
  /// Normalized strike radius, 0 = membrane centre .. 1 = rim. At 0 every
  /// mode keeps its base strike gain (legacy uniform excitation); above 0
  /// each mode is weighted by its shape J_m(alpha_mn * strike_r) *
  /// cos(m * strike_theta) evaluated at the strike, so a centre hit drops the
  /// m>=1 modes (J_{m>0}(0) = 0) to a pitchless thump and a rim hit excites
  /// them.
  float strike_r = 0.0f;
  /// Strike angle (radians); orients the m>=1 degenerate sin/cos pair.
  float strike_theta = 0.0f;
  /// Per-mode angular order m (nodal diameters), parallel to mode_ratios.
  /// Defaults to the ideal circular-membrane set (0,1)(1,1)(2,1)(0,2)(3,1).
  std::array<uint8_t, kMaxPercussionModes> mode_m = {0, 1, 2, 0, 3, 0};
  /// Per-mode Bessel zero alpha_mn (the spatial argument scale), parallel to
  /// mode_ratios. mode_ratios[k] == mode_alpha[k] / mode_alpha[0] for the
  /// ideal membrane, but the two serve different roles: ratio scales
  /// frequency, alpha scales the strike-shape argument.
  std::array<float, kMaxPercussionModes> mode_alpha = {2.4048f, 3.8317f, 5.1356f,
                                                       5.5201f, 6.3802f, 0.0f};

  // --- noise layer ---
  float noise_gain = 0.0f;
  float noise_decay_ms = 150.0f;
  float noise_cutoff_hz = 2500.0f;
  float noise_q = 1.0f;
  SynthFilterOutput noise_output = SynthFilterOutput::kBandpass;

  // --- radiated upper bound ---
  /// Upper bound (Hz) on every noise stream the struck head or plate radiates
  /// — the burst, the wire rattle and the shimmer wash. 0 = unbounded, the
  /// voicing that predates this field.
  ///
  /// It exists because a high-pass has no top. The SVF's high-pass output is
  /// flat above its corner, so white noise driven through it stays white all
  /// the way to Nyquist, and third-octave bands get wider in proportion to
  /// their centre frequency — which puts the most energy in the highest band
  /// there is, whatever the corner was set to. Every open-topped piece
  /// therefore measures its spectral peak at the top of the analysis range
  /// rather than where the plate actually speaks: against a sampled kit, a
  /// closed hat peaked at 12.5 kHz where the reference peaked between 315 Hz
  /// and 4 kHz, and a crash at 12.5 kHz against 2.5 kHz. No corner setting can
  /// correct that, because the corner is a floor and the defect is the missing
  /// ceiling.
  ///
  /// This is a bound rather than a band, and it is a bound only while it sits
  /// above the corner it is bounding. A two-pole low-pass above the corner
  /// removes the tail past it and leaves the corner's own voicing alone, and
  /// the measured peak follows the bound down. Taken *below* the corner the two
  /// stop composing and start squeezing: a wash high-passed at 5.5 kHz and
  /// bounded at 2 kHz stalls with its peak near 4 kHz and cannot be pushed
  /// lower, because what remains is the overlap of two slopes rather than a
  /// band anyone chose. A piece that has to speak below its corner needs the
  /// corner moved, not the bound.
  ///
  /// Removing that tail also removes its level — the energy above the corner
  /// was most of what a flat-topped wash had. A crash measured 13.6 dB quieter
  /// at a 4 kHz bound, so enabling this on a calibrated piece means re-gaining
  /// it in the same change.
  ///
  /// The particle layer is excluded — it is a separate excitation model, shaped
  /// by resonance stages of its own (`phisem_res_hz`, `phisem_body_hz`). Those
  /// are peaks and not bounds: a pole pair falls 6 dB per octave above its
  /// centre, so a shaker or scraper that needs a hard upper edge does not have
  /// one here.
  float noise_air_hz = 0.0f;

  // --- shell resonance ---
  /// Mix of the drum-shell resonance over the summed tone+noise hit (0 =
  /// bypass, the legacy dry voice). The shell is a small fixed bandpass bank
  /// (normalized to unit peak, so it never blows up), letting the strike ring
  /// through the body of the drum.
  float shell_mix = 0.0f;
  int shell_num_modes = 0;
  std::array<float, kMaxShellModes> shell_freq_hz = {0.0f, 0.0f, 0.0f, 0.0f};
  std::array<float, kMaxShellModes> shell_t60_s = {0.08f, 0.06f, 0.05f, 0.04f};
  std::array<float, kMaxShellModes> shell_weight = {1.0f, 0.7f, 0.5f, 0.35f};

  // --- snare wire rattle ---
  /// Wire-against-head buzz amount (0 = off, no rattle). While the membrane
  /// displacement exceeds wire_threshold the snare wires contact the bottom
  /// head and rattle: a high-passed noise burst gated by how far the head is
  /// over the threshold and scaled by strike velocity, so hard hits buzz
  /// louder and longer. Couples to the tone layer (no membrane => no rattle).
  float wire_buzz = 0.0f;
  /// Membrane level at which the wires start contacting the head.
  float wire_threshold = 0.1f;
  /// Cutoff of the high-pass through which the rattle is voiced.
  float wire_cutoff_hz = 4000.0f;

  // --- nonlinear shimmer (cymbal/gong) ---
  /// Weakly-nonlinear energy transfer to a high shimmer band: the membrane
  /// energy (tone^2, the quadratic nonlinearity) pumps a high-passed wash that
  /// swells *after* the strike and rings as long as the inharmonic modes
  /// sound -- the cymbal "shimmer" growth a static modal bank cannot produce.
  /// One-way (modes -> shimmer, no feedback), so it is unconditionally stable.
  /// 0 = off.
  float shimmer = 0.0f;
  /// Buildup time of the wash (the follower lag that delays the shimmer onset).
  float shimmer_attack_ms = 40.0f;
  /// High-pass cutoff of the shimmer band.
  float shimmer_cutoff_hz = 8000.0f;

  // --- stochastic particle excitation (PhISEM: shakers / scrapers) ---
  /// Effective particle (bean) count driving the collision rate. 0 = off (no
  /// PhISEM layer, bit-identical). Cook's PhISEM statistical model: the sum of
  /// many exponentially-decaying bead-collision noises collapses to one noise
  /// source times an energy that each collision bumps. Voices maracas, cabasa,
  /// shaker, tambourine, guiro (scrape) and cuica (scrape + gliding resonance).
  float phisem_beans = 0.0f;
  /// System-energy decay of one shake gesture (ms): how long the burst lasts.
  float phisem_energy_ms = 100.0f;
  /// Per-collision sound decay (ms): the grain length of one bead click.
  float phisem_sound_ms = 3.0f;
  /// Centre of the band the collisions radiate directly (Hz; 0 = raw particle
  /// noise, no resonance). This is the bright end of the instrument — the bead
  /// against the shell, the ridge under the scraper — and it sits in the low
  /// kilohertz, well above the body.
  float phisem_res_hz = 0.0f;
  /// Q of that band (cabasa weak .. maraca / jingle stronger).
  float phisem_res_q = 1.0f;
  /// Body resonance centre (Hz; 0 = off, bit-identical): the gourd, shell or
  /// frame the collisions happen inside. Separate from the band above because a
  /// real shaker radiates two of them at once and they are octaves apart — a
  /// guiro's gourd is a narrow peak near 265 Hz under a scrape whose own band is
  /// at 3 kHz, and a maraca and a tambourine carry the same shape with a wider
  /// body. One resonance cannot be both, and placed between them it is neither.
  float phisem_body_hz = 0.0f;
  /// Q of the body resonance, per pole pair. Two identical pairs are cascaded,
  /// because the measured peak is far narrower than one of them: a guiro's gourd
  /// falls 23 dB in the third of an octave under it, where a single pole pair
  /// falls 6 dB per octave and leaves a skirt that fills the bass with particle
  /// noise the instrument does not radiate. A gourd reads about 4 here; a
  /// tambourine's frame and head are broad, nearer 1.5.
  float phisem_body_q = 4.0f;
  /// Level of the body against the direct band, as a fraction of the raw
  /// collision amplitude. 0 = off.
  float phisem_body_gain = 0.0f;
  /// Scrape ridge rate (Hz; 0 = pure random shaker). >0 makes the collisions
  /// quasi-periodic — a ratchet/guiro/cuica scrape.
  float phisem_scrape_hz = 0.0f;
  /// Resonance pitch glide (cuica): the resonance centre starts at
  /// res_hz * (1 + glide) and eases to res_hz over the note. 0 = static.
  float phisem_pitch_glide = 0.0f;
};

/// Per-voice percussion state, embedded in NativeSynthVoice.
class PercussionVoiceCore {
 public:
  void start(const PercussionPatchParams& params, double sample_rate, uint8_t note,
             uint8_t velocity, uint64_t seed) noexcept;
  /// Renders one sample; @p pitch_ratio is the common per-sample pitch factor
  /// (multiplied with the internal descending pitch envelope).
  float render(float pitch_ratio) noexcept;
  /// Immediate silence.
  void kill() noexcept;

 private:
  struct Mode {
    float omega = 0.0f;
    float r = 0.0f;
    float gain = 0.0f;
    float a1 = 0.0f;
    float a2 = 0.0f;
    float y1 = 0.0f;
    float y2 = 0.0f;
  };

  std::array<Mode, kMaxPercussionModes> modes_{};
  int num_modes_ = 0;
  float tone_gain_ = 1.0f;
  // Descending pitch envelope: ratio = 1 + drop_state_ (one-pole decay).
  float drop_state_ = 0.0f;
  float drop_coeff_ = 0.0f;
  float cached_ratio_ = 0.0f;
  bool excite_ = false;

  VoiceRandomSequence noise_;
  uint64_t noise_index_ = 0;
  float noise_level_ = 0.0f;
  float noise_coeff_ = 0.0f;
  TptSvf noise_filter_;
  SynthFilterOutput noise_output_ = SynthFilterOutput::kBandpass;

  // Radiated upper bound (noise_air_hz). One low-pass per stream rather than
  // one over their sum: the filter is linear, so the two are the same signal,
  // but bounding each stream where it is summed leaves the accumulation order
  // untouched and makes the disabled state bit-identical to the voicing that
  // predates the field — which is what lets a calibration done before it was
  // added be trusted afterwards.
  float noise_air_hz_ = 0.0f;
  TptSvf noise_air_;
  TptSvf wire_air_;
  TptSvf shimmer_air_;

  BodyResonator shell_;

  // Snare wire rattle: gated, velocity-scaled high-passed noise driven by the
  // membrane displacement crossing wire_threshold_.
  float wire_buzz_ = 0.0f;
  float wire_threshold_ = 0.1f;
  float wire_vel01_ = 0.0f;
  uint64_t wire_index_ = 0;
  TptSvf wire_filter_;

  // Nonlinear cymbal shimmer: a high-passed wash whose level follows the
  // membrane energy (tone^2) through a slow attack, so it swells after the
  // strike. One-way pump => stable.
  float shimmer_ = 0.0f;
  float shimmer_env_ = 0.0f;
  float shimmer_attack_coeff_ = 0.0f;
  uint64_t shimmer_index_ = 0;
  TptSvf shimmer_filter_;

  // Stochastic particle excitation (PhISEM: shakers / scrapers). A single noise
  // source scaled by an energy that each bead/ridge collision bumps, with the
  // system energy decaying over the shake, optionally through a gourd/shell
  // resonance (with a cuica pitch glide).
  float phisem_beans_ = 0.0f;
  float phisem_shake_energy_ = 0.0f;
  float phisem_sys_decay_ = 0.0f;
  float phisem_sound_level_ = 0.0f;
  float phisem_sound_decay_ = 0.0f;
  float phisem_rate_ = 0.0f;  // random collisions per bean per unit energy per sample
  float phisem_scrape_phase_ = 0.0f;
  float phisem_scrape_inc_ = 0.0f;
  float phisem_res_hz_ = 0.0f;
  float phisem_res_q_ = 1.0f;
  float phisem_body_gain_ = 0.0f;
  float phisem_glide_state_ = 0.0f;
  float phisem_glide_coeff_ = 0.0f;
  float phisem_sr_ = 48000.0f;
  uint64_t phisem_prob_index_ = 0;
  uint64_t phisem_noise_index_ = 0;
  TptSvf phisem_filter_;
  TptSvf phisem_body_;
  TptSvf phisem_body2_;
};

}  // namespace sonare::midi::synth
