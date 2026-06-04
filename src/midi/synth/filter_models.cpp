#include "midi/synth/filter_models.h"

#include <algorithm>
#include <cmath>

namespace sonare::midi::synth {

namespace {

constexpr float kPi = 3.14159265358979323846f;

/// Bilinear prewarp: g = tan(pi * fc / sr), cutoff clamped like the SVF.
float prewarp(float cutoff_hz, double sample_rate) noexcept {
  const float sr = static_cast<float>(sample_rate);
  const float fc = std::clamp(cutoff_hz, 10.0f, 0.49f * sr);
  return std::tan(kPi * fc / sr);
}

/// Loop saturation shared by the ladder / Sallen-Key models: keeps the
/// resonance feedback bounded (deterministic self-oscillation amplitude) and
/// contributes the soft "analog" drive cue.
float loop_tanh(float x) noexcept { return std::tanh(x); }

}  // namespace

float filter_resonance01_from_q(float q) noexcept {
  return std::clamp((q - 0.707f) / (kSelfOscQ - 0.707f), 0.0f, 1.0f);
}

// ---------------------------------------------------------------------------
// MoogLadderFilter — ZDF 4-pole cascade, global feedback (Stilson & Smith /
// Zavalishin). Per stage: v = (x - z) * G; y = v + z; z = y + v, with the
// delay-free loop solved through the cascade prediction
//   y4 = G^4 * u + S,  S = G^3*(1-G)z1 + G^2*(1-G)z2 + G*(1-G)z3 + (1-G)z4
//   u  = (x - k*S) / (1 + k*G^4)
// (1-G == 1/(1+g), the corrected state contribution).
// ---------------------------------------------------------------------------

void MoogLadderFilter::prepare(double sample_rate) noexcept {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  last_cutoff_ = -1.0f;
  last_resonance_ = -1.0f;
  reset();
}

void MoogLadderFilter::reset() noexcept { z1_ = z2_ = z3_ = z4_ = 0.0f; }

void MoogLadderFilter::set(float cutoff_hz, float resonance01) noexcept {
  if (cutoff_hz == last_cutoff_ && resonance01 == last_resonance_) return;
  last_cutoff_ = cutoff_hz;
  last_resonance_ = resonance01;
  const float g = prewarp(cutoff_hz, sample_rate_);
  inv_gp1_ = 1.0f / (1.0f + g);
  g_over_gp1_ = g * inv_gp1_;
  const float g2 = g_over_gp1_ * g_over_gp1_;
  g4_ = g2 * g2;
  // Self-oscillation at k = 4; push slightly past it at full resonance so the
  // tanh-bounded oscillation is robustly sustained.
  k_ = 4.1f * std::clamp(resonance01, 0.0f, 1.0f);
}

float MoogLadderFilter::process(float x) noexcept {
  const float G = g_over_gp1_;
  const float s = inv_gp1_ * (G * G * G * z1_ + G * G * z2_ + G * z3_ + z4_);
  float u = (x - k_ * s) / (1.0f + k_ * g4_);
  u = loop_tanh(u);

  float v = (u - z1_) * G;
  float y = v + z1_;
  z1_ = y + v;
  v = (y - z2_) * G;
  y = v + z2_;
  z2_ = y + v;
  v = (y - z3_) * G;
  y = v + z3_;
  z3_ = y + v;
  v = (y - z4_) * G;
  y = v + z4_;
  z4_ = y + v;
  return y;
}

// ---------------------------------------------------------------------------
// DiodeLadderFilter — Pirkle AN-6 ZDF formulation (the Csound diode_ladder
// reference): four one-pole stages with bidirectional coupling, global
// feedback K (self-oscillation at 17).
// ---------------------------------------------------------------------------

void DiodeLadderFilter::prepare(double sample_rate) noexcept {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  last_cutoff_ = -1.0f;
  last_resonance_ = -1.0f;
  reset();
}

void DiodeLadderFilter::reset() noexcept { z1_ = z2_ = z3_ = z4_ = 0.0f; }

void DiodeLadderFilter::set(float cutoff_hz, float resonance01) noexcept {
  if (cutoff_hz == last_cutoff_ && resonance01 == last_resonance_) return;
  last_cutoff_ = cutoff_hz;
  last_resonance_ = resonance01;
  // The diode topology's coupled stages spread the poles, putting the
  // resonant peak ~1/sqrt(2) below the per-stage corner (Stinchcombe). Scale
  // the internal corner so the audible peak / self-oscillation lands on the
  // requested cutoff and the knob means the same across models.
  const float g = prewarp(cutoff_hz * 1.41421356f, sample_rate_);
  const float gp1 = 1.0f + g;
  const float g4 = 0.5f * g / gp1;
  const float g3 = 0.5f * g / (gp1 - 0.5f * g * g4);
  const float g2 = 0.5f * g / (gp1 - 0.5f * g * g3);
  const float g1 = g / (gp1 - g * g2);
  gamma_ = g4 * g3 * g2 * g1;

  sg1_ = g4 * g3 * g2;
  sg2_ = g4 * g3;
  sg3_ = g4;

  alpha_ = g / gp1;

  beta1_ = 1.0f / (gp1 - g * g2);
  beta2_ = 1.0f / (gp1 - 0.5f * g * g3);
  beta3_ = 1.0f / (gp1 - 0.5f * g * g4);
  beta4_ = 1.0f / gp1;

  gamma1_ = 1.0f + g1 * g2;
  gamma2_ = 1.0f + g2 * g3;
  gamma3_ = 1.0f + g3 * g4;

  delta1_ = g;
  delta2_ = 0.5f * g;

  eps1_ = g2;
  eps2_ = g3;
  eps3_ = g4;

  // Self-oscillation at K = 17; nudge past it at full resonance.
  k_ = 17.5f * std::clamp(resonance01, 0.0f, 1.0f);
}

float DiodeLadderFilter::process(float x) noexcept {
  // Feedback inputs (stage n sees stage n+1's state).
  const float fb4 = beta4_ * z4_;
  const float fb3 = beta3_ * (z3_ + fb4 * delta2_);
  const float fb2 = beta2_ * (z2_ + fb3 * delta2_);
  const float fbo1 = beta1_ * (z1_ + fb2 * delta1_);

  const float sigma = sg1_ * fbo1 + sg2_ * fb2 + sg3_ * fb3 + fb4;

  float u = (x - k_ * sigma) / (1.0f + k_ * gamma_);
  u = loop_tanh(u);

  // Stage 1 (a0 = 1), stages 2-4 (a0 = 0.5).
  float xin = u * gamma1_ + fb2 + eps1_ * fbo1;
  float v = (xin - z1_) * alpha_;
  float lp = v + z1_;
  z1_ = lp + v;

  xin = lp * gamma2_ + fb3 + eps2_ * fb2;
  v = (0.5f * xin - z2_) * alpha_;
  lp = v + z2_;
  z2_ = lp + v;

  xin = lp * gamma3_ + fb4 + eps3_ * fb3;
  v = (0.5f * xin - z3_) * alpha_;
  lp = v + z3_;
  z3_ = lp + v;

  v = (0.5f * lp - z4_) * alpha_;
  lp = v + z4_;
  z4_ = lp + v;
  return lp;
}

// ---------------------------------------------------------------------------
// Korg35Filter — Pirkle AN-5 ZDF formulation (the Csound k35_lpf reference):
// LPF1 feeding a {LPF2, HPF1} loop with gain K (self-oscillation at 2).
// ---------------------------------------------------------------------------

void Korg35Filter::prepare(double sample_rate) noexcept {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  last_cutoff_ = -1.0f;
  last_resonance_ = -1.0f;
  reset();
}

void Korg35Filter::reset() noexcept {
  z1_ = z2_ = z3_ = 0.0f;
  s35_ = 0.0f;
}

void Korg35Filter::set(float cutoff_hz, float resonance01) noexcept {
  if (cutoff_hz == last_cutoff_ && resonance01 == last_resonance_) return;
  last_cutoff_ = cutoff_hz;
  last_resonance_ = resonance01;
  const float g = prewarp(cutoff_hz, sample_rate_);
  const float G = g / (1.0f + g);
  g_over_gp1_ = G;
  // Self-oscillation at K = 2; keep a small floor so the forward path never
  // vanishes, nudge past the threshold at full resonance.
  k_ = 0.01f + 2.04f * std::clamp(resonance01, 0.0f, 1.0f);
  lpf2_beta_ = (k_ - k_ * G) / (1.0f + g);
  hpf1_beta_ = -1.0f / (1.0f + g);
  alpha0_ = 1.0f / (1.0f - k_ * G + k_ * G * G);
}

float Korg35Filter::process(float x) noexcept {
  const float G = g_over_gp1_;
  // LPF1.
  const float v1 = (x - z1_) * G;
  const float lp1 = v1 + z1_;
  z1_ = lp1 + v1;

  float u = alpha0_ * (lp1 + s35_);
  u = loop_tanh(u);

  // LPF2.
  const float v2 = (u - z2_) * G;
  const float lp2 = v2 + z2_;
  z2_ = lp2 + v2;
  const float y = k_ * lp2;

  // HPF1 (driven by the loop output).
  const float v3 = (y - z3_) * G;
  const float lp3 = v3 + z3_;
  z3_ = lp3 + v3;

  s35_ = lpf2_beta_ * z2_ + hpf1_beta_ * z3_;
  return k_ > 0.0f ? y / k_ : y;
}

// ---------------------------------------------------------------------------
// SynthFilter — model dispatch
// ---------------------------------------------------------------------------

void SynthFilter::prepare(double sample_rate) noexcept {
  svf_.prepare(sample_rate);
  moog_.prepare(sample_rate);
  diode_.prepare(sample_rate);
  sallen_key_.prepare(sample_rate);
}

void SynthFilter::reset() noexcept {
  svf_.reset();
  moog_.reset();
  diode_.reset();
  sallen_key_.reset();
}

void SynthFilter::set_model(SynthFilterModel model) noexcept {
  if (model == model_) return;
  model_ = model;
  reset();
}

void SynthFilter::set(float cutoff_hz, float q) noexcept {
  switch (model_) {
    case SynthFilterModel::kSvf:
      svf_.set(cutoff_hz, q);
      break;
    case SynthFilterModel::kMoogLadder:
      moog_.set(cutoff_hz, filter_resonance01_from_q(q));
      break;
    case SynthFilterModel::kDiodeLadder:
      diode_.set(cutoff_hz, filter_resonance01_from_q(q));
      break;
    case SynthFilterModel::kSallenKey:
      sallen_key_.set(cutoff_hz, filter_resonance01_from_q(q));
      break;
  }
}

float SynthFilter::process(float x, SynthFilterOutput output) noexcept {
  switch (model_) {
    case SynthFilterModel::kSvf: {
      const TptSvf::Outputs out = svf_.process(x);
      switch (output) {
        case SynthFilterOutput::kLowpass:
          return out.lp;
        case SynthFilterOutput::kBandpass:
          return out.bp;
        case SynthFilterOutput::kHighpass:
          return out.hp;
      }
      return out.lp;
    }
    case SynthFilterModel::kMoogLadder:
      return moog_.process(x);
    case SynthFilterModel::kDiodeLadder:
      return diode_.process(x);
    case SynthFilterModel::kSallenKey:
      return sallen_key_.process(x);
  }
  return x;
}

}  // namespace sonare::midi::synth
