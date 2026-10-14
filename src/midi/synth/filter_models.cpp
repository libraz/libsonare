#include "midi/synth/filter_models.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include "util/constants.h"

namespace sonare::midi::synth {

namespace {

/// Bilinear prewarp: g = tan(pi * fc / sr), cutoff clamped like the SVF.
float prewarp(float cutoff_hz, double sample_rate) noexcept {
  const float sr = static_cast<float>(sample_rate);
  const float fc = std::clamp(cutoff_hz, 10.0f, 0.49f * sr);
  return std::tan(constants::kPi * fc / sr);
}

/// Loop saturation shared by the ladder / Sallen-Key models: keeps the
/// resonance feedback bounded (deterministic self-oscillation amplitude) and
/// contributes the soft "analog" drive cue.
float loop_tanh(float x) noexcept { return std::tanh(x); }

/// Halvings of the [-1, 1] bracket in loop_fixed_point(): 2^-32 of it, below float resolution.
constexpr int kBracketHalvings = 32;

/// Solves u = tanh(a + b * u), the DC steady state of a tanh-saturated feedback loop. h(u) =
/// tanh(a + b*u) - u is non-negative at -1 and non-positive at 1, so the bracket always holds a
/// root; it is unique wherever the loop is not past self-oscillation.
float loop_fixed_point(double a, double b) noexcept {
  double lo = -1.0;
  double hi = 1.0;
  for (int i = 0; i < kBracketHalvings; ++i) {
    const double mid = 0.5 * (lo + hi);
    if (std::tanh(a + b * mid) - mid > 0.0) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return static_cast<float>(0.5 * (lo + hi));
}

}  // namespace

float filter_resonance01_from_q(float q) noexcept {
  return std::clamp((q - constants::kButterworthQ) / (kSelfOscQ - constants::kButterworthQ), 0.0f,
                    1.0f);
}

// ---------------------------------------------------------------------------
// TransistorLadderFilter — ZDF 4-pole cascade, global feedback (Stilson & Smith /
// Zavalishin). Per stage: v = (x - z) * G; y = v + z; z = y + v, with the
// delay-free loop solved through the cascade prediction
//   y4 = G^4 * u + S,  S = G^3*(1-G)z1 + G^2*(1-G)z2 + G*(1-G)z3 + (1-G)z4
//   u  = (x - k*S) / (1 + k*G^4)
// (1-G == 1/(1+g), the corrected state contribution).
// ---------------------------------------------------------------------------

void TransistorLadderFilter::prepare(double sample_rate) noexcept {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  last_cutoff_ = -1.0f;
  last_resonance_ = -1.0f;
  reset();
}

void TransistorLadderFilter::reset() noexcept { z1_ = z2_ = z3_ = z4_ = 0.0f; }

void TransistorLadderFilter::set(float cutoff_hz, float resonance01) noexcept {
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

float TransistorLadderFilter::process(float x) noexcept {
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

// At DC every one-pole stage settles to z = its input, so the cascade state sum is
// s = (1 - G^4) * u and u = tanh((x - k*s) / (1 + k*G^4)).
void TransistorLadderFilter::settle(float x) noexcept {
  const double den = 1.0 + static_cast<double>(k_) * g4_;
  const double u = loop_fixed_point(x / den, -static_cast<double>(k_) * (1.0 - g4_) / den);
  z1_ = z2_ = z3_ = z4_ = static_cast<float>(u);
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
  const float g = prewarp(cutoff_hz * constants::kSqrt2, sample_rate_);
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

// At DC each stage holds z = (a0 * its input), with the coupling terms linear in the four
// states, so z = c * u for a fixed vector c; u then follows from the tanh loop.
void DiodeLadderFilter::settle(float x) noexcept {
  using Vec = std::array<double, 4>;
  const auto scaled = [](const Vec& v, double f) {
    Vec r{};
    for (size_t i = 0; i < 4; ++i) r[i] = v[i] * f;
    return r;
  };
  const auto plus = [](const Vec& a, const Vec& b) {
    Vec r{};
    for (size_t i = 0; i < 4; ++i) r[i] = a[i] + b[i];
    return r;
  };
  // Feedback taps of process() as linear forms over (z1..z4).
  const Vec fb4 = {0.0, 0.0, 0.0, beta4_};
  const Vec fb3 = scaled(plus(Vec{0.0, 0.0, 1.0, 0.0}, scaled(fb4, delta2_)), beta3_);
  const Vec fb2 = scaled(plus(Vec{0.0, 1.0, 0.0, 0.0}, scaled(fb3, delta2_)), beta2_);
  const Vec fbo1 = scaled(plus(Vec{1.0, 0.0, 0.0, 0.0}, scaled(fb2, delta1_)), beta1_);

  // Rows of (I - M) z = rhs * u, from z1 = xin1, z2 = xin2/2, z3 = xin3/2, z4 = z3/2.
  std::array<Vec, 4> m{};
  Vec rhs = {gamma1_, 0.0, 0.0, 0.0};
  const Vec x1 = plus(fb2, scaled(fbo1, eps1_));
  const Vec x2 = scaled(plus(plus(Vec{gamma2_, 0.0, 0.0, 0.0}, fb3), scaled(fb2, eps2_)), 0.5);
  const Vec x3 = scaled(plus(plus(Vec{0.0, gamma3_, 0.0, 0.0}, fb4), scaled(fb3, eps3_)), 0.5);
  const Vec x4 = {0.0, 0.0, 0.5, 0.0};
  const Vec* rows[4] = {&x1, &x2, &x3, &x4};
  for (size_t i = 0; i < 4; ++i) {
    for (size_t j = 0; j < 4; ++j) m[i][j] = (i == j ? 1.0 : 0.0) - (*rows[i])[j];
  }

  // Gaussian elimination with partial pivoting for c in m * c = rhs.
  for (size_t col = 0; col < 4; ++col) {
    size_t pivot = col;
    for (size_t r = col + 1; r < 4; ++r) {
      if (std::fabs(m[r][col]) > std::fabs(m[pivot][col])) pivot = r;
    }
    std::swap(m[col], m[pivot]);
    std::swap(rhs[col], rhs[pivot]);
    for (size_t r = col + 1; r < 4; ++r) {
      const double f = m[r][col] / m[col][col];
      for (size_t j = col; j < 4; ++j) m[r][j] -= f * m[col][j];
      rhs[r] -= f * rhs[col];
    }
  }
  Vec c{};
  for (size_t i = 4; i-- > 0;) {
    double acc = rhs[i];
    for (size_t j = i + 1; j < 4; ++j) acc -= m[i][j] * c[j];
    c[i] = acc / m[i][i];
  }

  const auto dot = [&c](const Vec& v) {
    double acc = 0.0;
    for (size_t i = 0; i < 4; ++i) acc += v[i] * c[i];
    return acc;
  };
  const double sigma = sg1_ * dot(fbo1) + sg2_ * dot(fb2) + sg3_ * dot(fb3) + dot(fb4);
  const double den = 1.0 + static_cast<double>(k_) * gamma_;
  const double u = loop_fixed_point(x / den, -static_cast<double>(k_) * sigma / den);
  z1_ = static_cast<float>(c[0] * u);
  z2_ = static_cast<float>(c[1] * u);
  z3_ = static_cast<float>(c[2] * u);
  z4_ = static_cast<float>(c[3] * u);
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

// At DC z1 = x, z2 = u, z3 = k*u and the carried loop sum s35 = (lpf2_beta + hpf1_beta*k) * u,
// with u = tanh(alpha0 * (x + s35)).
void Korg35Filter::settle(float x) noexcept {
  const double beta = static_cast<double>(lpf2_beta_) + static_cast<double>(hpf1_beta_) * k_;
  const double u =
      loop_fixed_point(static_cast<double>(alpha0_) * x, static_cast<double>(alpha0_) * beta);
  z1_ = x;
  z2_ = static_cast<float>(u);
  z3_ = static_cast<float>(k_ * u);
  s35_ = static_cast<float>(beta * u);
}

// ---------------------------------------------------------------------------
// SynthFilter — model dispatch
// ---------------------------------------------------------------------------

void SynthFilter::prepare(double sample_rate) noexcept {
  svf_.prepare(sample_rate);
  transistor_.prepare(sample_rate);
  diode_.prepare(sample_rate);
  sallen_key_.prepare(sample_rate);
}

void SynthFilter::reset() noexcept {
  svf_.reset();
  transistor_.reset();
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
    case SynthFilterModel::kTransistorLadder:
      transistor_.set(cutoff_hz, filter_resonance01_from_q(q));
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
    case SynthFilterModel::kTransistorLadder:
      return transistor_.process(x);
    case SynthFilterModel::kDiodeLadder:
      return diode_.process(x);
    case SynthFilterModel::kSallenKey:
      return sallen_key_.process(x);
  }
  return x;
}

void SynthFilter::settle(float x) noexcept {
  switch (model_) {
    case SynthFilterModel::kSvf:
      svf_.settle(x);
      break;
    case SynthFilterModel::kTransistorLadder:
      transistor_.settle(x);
      break;
    case SynthFilterModel::kDiodeLadder:
      diode_.settle(x);
      break;
    case SynthFilterModel::kSallenKey:
      sallen_key_.settle(x);
      break;
  }
}

}  // namespace sonare::midi::synth
