#pragma once

/// @file filter_models.h
/// @brief Selectable virtual-analog filter models for the NativeSynth voice —
///        the core of each classic synth "character":
///          - kSvf:         TPT state-variable filter (Oberheim/SEM family;
///                          clean resonance, simultaneous LP/BP/HP).
///          - kMoogLadder:  4-pole transistor ladder (ZDF cascade with global
///                          feedback; saturating loop, self-oscillates).
///          - kDiodeLadder: diode ladder (EMS VCS3 / TB-303 family; ZDF with
///                          the bidirectionally coupled stages of Pirkle's
///                          AN-6 derivation; creamier resonance, self-osc).
///          - kSallenKey:   Korg35 Sallen-Key lowpass (MS-10 / early MS-20;
///                          ZDF two-LPF1 + HPF1 loop of Pirkle's AN-5;
///                          aggressive resonance, self-oscillates).
///
/// All models are topology-preserving-transform (zero-delay-feedback)
/// discretizations — stable and zipper-free under per-sample cutoff/resonance
/// modulation, with a tanh nonlinearity inside the resonance loop (the
/// "analog" saturation cue) that also bounds self-oscillation deterministically.
/// References: Zavalishin, "The Art of VA Filter Design"; Stilson & Smith
/// 1996 (Moog ladder); Stinchcombe (diode ladders); Pirkle AN-5/AN-6.
///
/// Resonance is normalized: set() takes the same Q the TPT SVF uses and maps
/// it per model so one patch knob covers all models; the self-oscillation
/// threshold sits at Q >= kSelfOscQ.
///
/// RT contract: all methods are allocation-free; set() recomputes
/// coefficients (one tan(), cached against unchanged cutoff/resonance) and
/// may be called per-sample.

#include "midi/synth/svf.h"

namespace sonare::midi::synth {

/// Filter model selector (patch field; the enum is mode-tagged for the ABI).
enum class SynthFilterModel : int {
  kSvf = 0,
  kMoogLadder = 1,
  kDiodeLadder = 2,
  kSallenKey = 3,
};

/// Which TPT SVF output the voice mixes (highpass enables noise hats /
/// cymbals). The ladder and Sallen-Key models are lowpass-only.
enum class SynthFilterOutput : int {
  kLowpass = 0,
  kBandpass = 1,
  kHighpass = 2,
};

/// Q at (and above) which the ladder / Sallen-Key models self-oscillate.
inline constexpr float kSelfOscQ = 25.0f;

/// Maps the patch Q knob to the models' normalized resonance in [0, 1]
/// (1 = self-oscillation threshold).
float filter_resonance01_from_q(float q) noexcept;

/// 4-pole transistor-ladder lowpass (ZDF cascade, global feedback k in [0,4+]
/// with tanh loop saturation).
class MoogLadderFilter {
 public:
  void prepare(double sample_rate) noexcept;
  void reset() noexcept;
  /// @p resonance01 in [0,1]; 1 reaches self-oscillation.
  void set(float cutoff_hz, float resonance01) noexcept;
  float process(float x) noexcept;

 private:
  double sample_rate_ = 48000.0;
  float last_cutoff_ = -1.0f;
  float last_resonance_ = -1.0f;
  // Coefficients.
  float g_over_gp1_ = 0.0f;  // G = g/(1+g)
  float inv_gp1_ = 1.0f;     // 1/(1+g)
  float k_ = 0.0f;           // feedback amount [0, ~4.1]
  float g4_ = 0.0f;          // G^4
  // One-pole states.
  float z1_ = 0.0f, z2_ = 0.0f, z3_ = 0.0f, z4_ = 0.0f;
};

/// Diode-ladder lowpass (Pirkle AN-6 ZDF formulation: four coupled one-pole
/// stages, feedback K in [0, ~17.5] with tanh loop saturation).
class DiodeLadderFilter {
 public:
  void prepare(double sample_rate) noexcept;
  void reset() noexcept;
  void set(float cutoff_hz, float resonance01) noexcept;
  float process(float x) noexcept;

 private:
  double sample_rate_ = 48000.0;
  float last_cutoff_ = -1.0f;
  float last_resonance_ = -1.0f;
  // Coefficients (AN-6 naming).
  float alpha_ = 0.0f;                          // g/(1+g)
  float gamma_ = 0.0f;                          // G4*G3*G2*G1
  float k_ = 0.0f;                              // feedback amount
  float sg1_ = 0.0f, sg2_ = 0.0f, sg3_ = 0.0f;  // SG4 == 1
  float beta1_ = 0.0f, beta2_ = 0.0f, beta3_ = 0.0f, beta4_ = 0.0f;
  float gamma1_ = 1.0f, gamma2_ = 1.0f, gamma3_ = 1.0f;
  float delta1_ = 0.0f, delta2_ = 0.0f;  // delta3 == delta2
  float eps1_ = 0.0f, eps2_ = 0.0f, eps3_ = 0.0f;
  // One-pole states.
  float z1_ = 0.0f, z2_ = 0.0f, z3_ = 0.0f, z4_ = 0.0f;
};

/// Korg35 Sallen-Key lowpass (Pirkle AN-5 ZDF: LPF1 -> loop{LPF2, HPF1},
/// loop gain K in [0.01, ~2.05] with tanh loop saturation).
class Korg35Filter {
 public:
  void prepare(double sample_rate) noexcept;
  void reset() noexcept;
  void set(float cutoff_hz, float resonance01) noexcept;
  float process(float x) noexcept;

 private:
  double sample_rate_ = 48000.0;
  float last_cutoff_ = -1.0f;
  float last_resonance_ = -1.0f;
  // Coefficients.
  float g_over_gp1_ = 0.0f;  // G = g/(1+g)
  float k_ = 0.01f;          // loop gain
  float lpf2_beta_ = 0.0f;
  float hpf1_beta_ = 0.0f;
  float alpha0_ = 1.0f;
  // One-pole states + the loop feedback sum carried across samples.
  float z1_ = 0.0f, z2_ = 0.0f, z3_ = 0.0f;
  float s35_ = 0.0f;
};

/// Model-dispatched voice filter: owns one instance of every model so a patch
/// (or later a mod-matrix) can switch character without reallocation. Only
/// the selected model's state advances; switching models resets state.
class SynthFilter {
 public:
  void prepare(double sample_rate) noexcept;
  void reset() noexcept;
  void set_model(SynthFilterModel model) noexcept;
  SynthFilterModel model() const noexcept { return model_; }

  /// Cutoff in Hz and the patch Q (SVF uses Q directly; the ladder /
  /// Sallen-Key models map it through filter_resonance01_from_q()).
  void set(float cutoff_hz, float q) noexcept;

  /// Renders one sample. @p output selects the SVF tap; the ladder and
  /// Sallen-Key models are lowpass-only and ignore it.
  float process(float x, SynthFilterOutput output) noexcept;

 private:
  SynthFilterModel model_ = SynthFilterModel::kSvf;
  TptSvf svf_;
  MoogLadderFilter moog_;
  DiodeLadderFilter diode_;
  Korg35Filter sallen_key_;
};

}  // namespace sonare::midi::synth
