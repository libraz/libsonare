#pragma once

/// @file piano_voice.h
/// @brief Extended-waveguide acoustic-piano core for the NativeSynth voice —
///        the no-SF2 data-free grand sketch (synthesis method "piano" of the
///        instrument build plan; Bensa et al. 2003, Bank & Valimaki,
///        Jaffe & Smith).
///
/// Four elements separate "piano" from "guitar/organ", and all four are here:
///   1. STIFF-STRING DISPERSION: real strings are stiff, so partials stretch
///      sharp (f_n = n*f0*sqrt(1 + B*n^2), B rising from ~1e-4 in the bass to
///      ~1e-2 at the top). Implemented as a cascade of first-order allpasses
///      inside each waveguide loop (high frequencies travel faster), with
///      the EXACT allpass + loop-filter phase delay at the fundamental
///      compensated in the fractional loop length so the f0 tuning stays
///      accurate.
///   2. NONLINEAR FELT HAMMER: the felt spring F = K*y^p (p ~ 2-3) is folded
///      in analytically — the excitation is a raised-cosine force pulse
///      whose contact time shrinks as v^-((p-1)/(p+1)) and whose amplitude
///      grows as v^(2p/(p+1)) (the Hertz-contact scaling laws), so hard
///      strikes are shorter (= brighter) the way felt physics dictates, and
///      the strike-position comb is applied analytically to the pulse.
///   3. COUPLED UNISON STRINGS / TWO-STAGE DECAY: 2-3 micro-detuned string
///      loops share a bridge; the coherent (bridge-moving) component decays
///      at the fast "prompt sound" rate while the residual decays at the
///      slow "aftersound" rate — the double decay + shimmer signature.
///   4. SOUNDBOARD: a small fixed bank of low-Q resonators driven by the
///      bridge sum approximates the soundboard's dominant modes (the cheap
///      end of commuted synthesis), mixed in at a patch-set level.
///
/// The string delay slabs are NOT owned by the core: the host instrument
/// allocates one slab per voice slot in prepare() (the only allocation
/// site) and attach()es it before start().
///
/// RT contract: attach()/start()/render() are allocation-free. Determinism:
/// per-string detune jitter derives from the (voice_index, note, age)
/// stream; the hammer pulse is analytic.

#include <array>
#include <cstddef>
#include <cstdint>

namespace sonare::midi::synth {

inline constexpr int kMaxPianoStrings = 3;
inline constexpr int kPianoDispersionStages = 4;
/// Lowest fundamental the piano string loops are sized for (A0 = 27.5 Hz).
inline constexpr float kPianoMinFundamentalHz = 26.0f;

/// Per-string delay capacity (samples) for @p sample_rate.
inline int piano_string_capacity(double sample_rate) noexcept {
  const double sr = sample_rate > 0.0 ? sample_rate : 48000.0;
  return static_cast<int>(sr / kPianoMinFundamentalHz) + 8;
}

/// Whole-voice slab capacity (samples) the host must allocate.
inline int piano_slab_capacity(double sample_rate) noexcept {
  return kMaxPianoStrings * piano_string_capacity(sample_rate);
}

/// Piano section of a NativeSynthPatch (used when mode == kPiano).
struct PianoPatchParams {
  /// Coupled unison strings per note (clamped to [1, kMaxPianoStrings]).
  int strings = 3;
  /// Full micro-detune spread between the outer unison strings (cents).
  float detune_cents = 1.6f;
  /// Prompt-sound (coupled) t60 at A4 in seconds.
  float decay_fast_s = 3.0f;
  /// Aftersound (residual) t60 at A4 in seconds.
  float decay_slow_s = 12.0f;
  /// t60 scales by 2^(stretch * octaves below A4).
  float decay_stretch = 0.7f;
  /// Loop-lowpass openness in [0,1] (frequency-dependent string damping).
  float brightness = 0.75f;
  /// Dispersion amount in [0,1]: scales the keyboard-graded stiffness
  /// stretch (0 = harmonic string).
  float dispersion = 1.0f;
  /// Hammer strike point as a fraction of the string period in [0, 0.5].
  float strike_position = 0.12f;
  /// Felt compression exponent p in F = K*y^p (sets the velocity scaling
  /// laws of contact time and force).
  float hammer_exponent = 2.5f;
  /// Hammer-felt contact time at A4 / mezzo-forte (ms).
  float hammer_contact_ms = 1.2f;
  /// Soundboard resonator mix in [0,1].
  float soundboard = 0.25f;
  /// Damped t60 in seconds applied at note-off (the damper falling back).
  float release_damp_s = 0.1f;
};

/// Per-voice piano state, embedded in NativeSynthVoice.
class PianoVoiceCore {
 public:
  /// Wiring: hands the core its delay slab (>= piano_slab_capacity()).
  void attach(float* buffer, int per_string_capacity) noexcept {
    slab_ = buffer;
    string_capacity_ = per_string_capacity;
  }

  void start(const PianoPatchParams& params, double sample_rate, uint8_t note, uint8_t velocity,
             uint64_t seed) noexcept;
  /// Renders one sample; @p pitch_ratio is the common per-sample pitch factor.
  float render(float pitch_ratio) noexcept;
  /// Note-off: the damper caps both decay stages at release_damp_s.
  void release() noexcept;
  /// Immediate silence.
  void kill() noexcept;

 private:
  struct String {
    float* buffer = nullptr;
    int size = 0;
    size_t write_index = 0;
    float base_period = 0.0f;  // ideal loop period / detune included
    float comp = 1.0f;         // loop delay not in the line (fb + lp + allpass)
    float lp_state = 0.0f;
    std::array<float, kPianoDispersionStages> ap_state{};
    float ap_a = 0.0f;  // shared first-order allpass coefficient
    float g_slow = 0.0f;
    float g_fast = 0.0f;
  };

  struct SoundboardMode {
    float a1 = 0.0f;
    float a2 = 0.0f;
    float gain = 0.0f;
    float y1 = 0.0f;
    float y2 = 0.0f;
  };

  /// Analytic raised-cosine hammer force at sample @p n (0 outside contact).
  float hammer_force(int64_t n) const noexcept;

  float* slab_ = nullptr;
  int string_capacity_ = 0;

  std::array<String, kMaxPianoStrings> strings_{};
  int num_strings_ = 0;
  float loop_alpha_ = 1.0f;
  float bridge_ = 0.0f;
  /// Damper radius cap installed by release().
  float release_gain_ = 0.0f;

  // Hammer pulse (analytic; combed by the strike position, then the
  // velocity-driven felt-stiffness lowpass).
  int64_t exc_pos_ = 0;
  int contact_samples_ = 0;
  int comb_delay_ = 0;
  float hammer_amp_ = 0.0f;
  float exc_alpha_ = 1.0f;
  float exc_lp_ = 0.0f;

  // Soundboard bank.
  std::array<SoundboardMode, 4> soundboard_{};
  float soundboard_mix_ = 0.0f;
};

}  // namespace sonare::midi::synth
