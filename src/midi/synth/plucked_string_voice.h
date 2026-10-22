#pragma once

/// @file plucked_string_voice.h
/// @brief Buzzing-bridge plucked-string core for the NativeSynth voice — the
///        harp / koto / sitar / tanpura family whose defining timbre is the
///        distributed nonlinear bridge contact (the Indian jawari / Japanese
///        sawari) that the plain Karplus-Strong core (ks_voice.h) does not
///        model as a first-class element.
///
/// The core is the KS core's travelling-wave loop (string_loop.h) with the same
/// loss solve, dispersion cascade and pick click, plus a curved
/// bridge: at the string termination the displacement rides over a shallow
/// curved surface, so on each loop the returning wave is passed through a soft
/// one-sided limiter whose threshold the string amplitude periodically grazes.
/// That grazing sprays energy into the high partials on every period — the
/// shimmering metallic buzz of a sitar / tanpura. With the buzz at zero the
/// core is a clean plucked string (harp / koto).
///
/// The delay buffer is NOT owned by the core: the host instrument allocates one
/// slab per voice slot in prepare() (the only allocation site) and attach()es a
/// span before start().
///
/// RT contract: attach()/start()/render() are allocation-free (start zeroes the
/// attached span). Determinism: the excitation noise is the counter-based
/// (voice_index, note, age) stream, so identical event streams render
/// bit-identically. The buzz limiter is memoryless (no RNG), preserving that.

#include <cstddef>
#include <cstdint>

#include "midi/control_value.h"
#include "midi/synth/string_loop.h"
#include "midi/synth/voice_random.h"

namespace sonare::midi::synth {

/// Lowest fundamental the delay line is sized for; notes below clamp to the
/// buffer (their pitch lands sharp instead of overflowing).
inline constexpr float kPluckedStringMinFundamentalHz = 20.0f;

/// Per-LINE delay-buffer capacity (samples): one string span. attach() carves
/// the slab into spans of this size.
inline int plucked_string_buffer_capacity(double sample_rate) noexcept {
  const double sr = sample_rate > 0.0 ? sample_rate : 48000.0;
  return static_cast<int>(sr / kPluckedStringMinFundamentalHz) + 8;
}

/// Per-voice delay-SLAB capacity (samples) the host must allocate: a single
/// string span (the second polarization / sympathetic extensions are out of
/// scope for this core).
inline int plucked_string_slab_capacity(double sample_rate) noexcept {
  return plucked_string_buffer_capacity(sample_rate);
}

/// First-order allpass stages realising @c inharmonicity_b inside the loop.
inline constexpr int kPluckedDispersionStages = 2;

/// Buzzing-bridge plucked-string section of a NativeSynthPatch (used when
/// mode == kPluckedString).
struct PluckedStringPatchParams {
  /// Loop-lowpass openness in [0,1]: how slowly the upper harmonics decay
  /// relative to the fundamental (1 = bright/metallic, 0 = dull/nylon).
  float brightness = 0.7f;
  /// String t60 at A4 in seconds (fundamental decay to -60 dB).
  float decay_s = 4.0f;
  /// Decay stretching in [0,1]: t60 scales by 2^(stretch * octaves below A4),
  /// so low strings ring longer.
  float decay_stretch = 0.5f;
  /// Plucking point as a fraction of the string period in [0, 0.5]; the
  /// excitation comb notches harmonics with a node there (0 = no comb).
  float pick_position = 0.2f;
  /// Octaves the pick position rises per octave of fundamental, quoted at A2 and capped at the
  /// string's midpoint, in [0, 4]: 1 holds the finger at a fixed distance from the string's end
  /// as the strings shorten. 0 = the same fraction at every note (render bit-identical).
  float pick_position_per_octave = 0.0f;
  /// Excitation lowpass openness at full velocity in [0,1].
  float exc_brightness = 0.85f;
  /// Octaves the excitation lowpass corner rises per octave of fundamental, quoted at A2, in
  /// [0, 4]: 1 holds the corner at a fixed harmonic of the note. 0 = the corner stays in Hz
  /// at every note (render bit-identical).
  float exc_brightness_per_octave = 0.0f;
  /// Velocity -> excitation brightness amount in [0,1].
  float vel_to_brightness = 0.6f;
  /// Damped t60 in seconds applied at note-off (finger/palm mute).
  float release_damp_s = 0.12f;
  /// t60 in seconds of the partials at a fixed 4 kHz, the string's own and the
  /// air's loss quoted where they are measured: the loss filter is solved from
  /// this and @c decay_s instead of from @c brightness, so the treble keeps its
  /// partials rather than losing them to a pole crossed on every traversal. The
  /// same two-target solve the Karplus-Strong core uses. 0 = off -> the
  /// brightness pole and the render is bit-identical.
  float hf_decay_s = 0.0f;
  /// Optional mid-band anchor: t60 in seconds at 1 kHz, between the fundamental
  /// and the 4 kHz quote, carried by a second cascaded loss pole. Only with
  /// @c hf_decay_s > 0. 0 = off.
  float mid_decay_s = 0.0f;
  /// Inharmonicity coefficient B (f_n = n f0 sqrt(1 + B n^2)), the same at
  /// every note: the captured sitar, shamisen and koto show no growth of B
  /// across notes 48-72. Realised by the shared in-loop allpass cascade with its
  /// phase delay folded into the tuning. 0 = a harmonic string, bit-identical.
  float inharmonicity_b = 0.0f;
  /// Pick / plectrum contact-click level in [0,1]: a short lowpassed noise
  /// burst at note-on, scaled by velocity, outside the loop. 0 = off, render
  /// bit-identical.
  float pick_noise = 0.0f;
  /// Buzzing-bridge (jawari / sawari) intensity in [0,1]: how strongly the
  /// curved bridge limits and sprays the returning wave into high partials
  /// (0 = a clean plucked string, harp / koto; higher = the shimmering buzz of
  /// a sitar / tanpura). The limiter is a soft one-sided saturation applied in
  /// the loop; at 0 the loop is the plain string and render is bit-identical.
  float buzz = 0.0f;
  /// Exponent of the voice-amplifier velocity curve, (velocity/127)^e; 2 is the
  /// SoundFont curve, bit-identical to it.
  float velocity_exponent = 2.0f;
};

/// Per-voice plucked-string state, embedded in NativeSynthVoice. The voice's
/// amplitude envelope / filter / mod matrix wrap around this core; render()
/// returns the raw string sample.
class PluckedStringVoiceCore {
 public:
  /// CONTROL-thread wiring (or audio-thread pointer assignment before start()):
  /// hands the core its delay slab (one span of @p per_line_capacity). The slab
  /// outlives the voice.
  void attach(float* slab, int per_line_capacity) noexcept {
    buffer_ = slab;
    capacity_ = per_line_capacity;
  }

  /// Configures the string for @p note / @p velocity and injects the seeded
  /// excitation burst state. Zeroes the attached span.
  void start(const PluckedStringPatchParams& params, double sample_rate, uint8_t note,
             Velocity16 velocity, uint64_t seed) noexcept;
  /// Renders one sample; @p pitch_ratio is the common per-sample pitch factor
  /// (bend / vibrato / drift / glide), 1 = on pitch.
  float render(float pitch_ratio) noexcept;
  /// Note-off: damp the loop towards release_damp_s (the string keeps sounding
  /// through the host's release envelope, muted).
  void release() noexcept;
  /// Immediate silence.
  void kill() noexcept;
  /// The factor the loop runs at per host sample, settled at start(); 1 when it is not short.
  int oversample() const noexcept { return decimator_.factor(); }

 private:
  // Builds the whole voice for a loop running at @p sr (the host rate times the oversampling
  // factor) and returns the loop's budget at that rate.
  LoopBudget configure(const PluckedStringPatchParams& params, double sr, uint8_t note,
                       Velocity16 velocity, uint64_t seed) noexcept;
  // One sample of the loop at the internal rate.
  float render_internal(float pitch_ratio) noexcept;
  // Brings the oversampled loop back to the host rate; the identity at factor 1.
  LoopDecimator decimator_;
  bool killed_ = true;
  float* buffer_ = nullptr;
  int capacity_ = 0;

  /// Ideal loop period (samples) at pitch_ratio == 1.
  float base_period_ = 0.0f;

  /// Buzzing-bridge limiter threshold (0 = off -> loop path bit-identical). The
  /// returning wave past +/- this bound is softly saturated each loop.
  float buzz_threshold_ = 0.0f;
  float buzz_amount_ = 0.0f;

  // Excitation burst (one period of combed, lowpassed seeded noise; the
  // dynamic-level lowpass is a single one-pole).
  VoiceRandomSequence noise_;
  int exc_total_ = 0;
  int exc_pos_ = 0;
  int pick_delay_ = 0;
  float exc_alpha_ = 1.0f;
  /// Burst noise level holding its power per Hz at the internal rate.
  float exc_noise_gain_ = 1.0f;
  float exc_lp_ = 0.0f;

  /// Output trim bringing the raw string sample up to a musical voice level.
  float output_scale_ = 1.0f;

  /// The string: delay line, loss filter and per-traversal gain.
  StringLoop string_;
  /// In-loop dispersion allpass coefficient (0 = off) and per-stage state.
  float disp_a_ = 0.0f;
  float disp_state_[kPluckedDispersionStages] = {};
  /// Pick contact click, armed at start().
  NoiseBurst pick_;
};

}  // namespace sonare::midi::synth
