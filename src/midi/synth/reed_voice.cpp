#include "midi/synth/reed_voice.h"

#include <algorithm>
#include <cmath>

#include "rt/fractional_delay.h"

namespace sonare::midi::synth {

namespace {

constexpr float kTwoPi = 6.28318530717958647692f;

// Mouth-pressure calibration: a reed self-oscillates only inside a narrow
// pressure BAND, and the oscillation is STRONGEST right at the edge of the
// beating threshold — the reed table's operating point (roughly offset +
// |slope|*breath) sits just below the +1 clamp, where the reed nearly closes on
// each cycle. Too little and the tone is a whisper; too much clamps the reed
// shut and it goes silent. The breath and reed-table spans are calibrated
// together so the WHOLE exposed knob range lands in that strong band ([~0.88,
// ~0.99]) — the knobs colour the tone, they do not gate it on and off. Dynamic
// LOUDNESS is not taken from pushing the breath toward shutoff (a real reed's
// usable pressure range is narrow); it comes from the voice's velocity / amp
// VCA around this core, so breath varies only mildly with the note. breath =
// base + span*level.
constexpr float kBreathBase = 0.82f;
constexpr float kBreathSpan = 0.08f;

// Reed table from the stiffness / opening knobs (STK Clarinet: offset ~0.7 rest
// opening, slope ~-0.3 stiffness). A stiffer reed = a steeper (more negative)
// slope = a brighter, buzzier tone; a smaller opening = a larger offset. The
// spans are deliberately small so no knob combination pushes the operating point
// past the clamp (silent) or far below the threshold (a whisper).
constexpr float kReedOffsetMin = 0.68f;
constexpr float kReedOffsetSpan = 0.04f;  // offset = min + span*(1 - opening)
constexpr float kReedSlopeBase = 0.25f;
constexpr float kReedSlopeSpan = 0.05f;  // slope = -(base + span*stiffness)

// Bell reflection loss from damping: < 1 so the loop is stable (the breath
// replenishes it). Low damping = a purer, more sustained bore.
constexpr float kLossBase = 0.99f;
constexpr float kLossSpan = 0.09f;
constexpr float kLossFloor = 0.80f;
constexpr float kLossCeil = 0.999f;

// Bell loop-lowpass depth: brightness maps to the one-pole pole (a brighter bell
// reflects more upper partials).
constexpr float kBellPoleSpan = 0.7f;

// Live-control smoothing time (ms): the per-sample ramp of breath / brightness
// toward their CC targets — fast enough to feel immediate, slow enough to never
// zipper.
constexpr float kControlSmoothMs = 8.0f;

// Breath turbulence depth (a light seeded jitter on the mouth pressure).
constexpr float kBreathNoiseDepth = 0.08f;

// Onset chiff depth (the reed's initial "speak" noise burst) and the level of
// the seeded burst pre-filled into the bore so the note speaks promptly.
constexpr float kChiffDepth = 0.5f;
constexpr float kBorePrefill = 0.02f;

// In-loop DC-blocker corner (~10 Hz): the conical (positive-feedback) comb has a
// DC mode that does not radiate, and the breath DC drives the reed, so the loop
// must shed the accumulated offset without touching the tone.
constexpr float kDcCornerHz = 10.0f;

// Output trim: the raw bore pressure sits near unity already (the reed table is
// bounded to [-1,1]), so only a gentle scale brings a forte note into the other
// engines' range.
constexpr float kOutputScale = 0.9f;

// --- 4a dynamic (mass-spring) reed (only when params.dynamic_reed) ---
// Reed natural frequency (Hz) = base + span*reed_resonance: a cane reed's own
// resonance sits well above the played fundamental (clarinet reeds ~2-3 kHz), so
// it boosts the partials near it rather than the fundamental.
constexpr float kReedResBaseHz = 1500.0f;
constexpr float kReedResSpanHz = 2000.0f;
// Reed resonator pole radius (Q) and how strongly its displacement biases the
// (sharp) reed table's operating point. Kept small so the loop stays bounded —
// the bias only nudges the already-clamped table, never adds unbounded gain.
constexpr float kReedResR = 0.985f;
constexpr float kReedCouple = 0.15f;

// --- 4b register vent (only when params.register_vent > 0) ---
// Low-band follower corner (Hz): the follower tracks the loop's low content so
// it can be subtracted out, damping the fundamental toward the register break.
constexpr float kRegVentCornerHz = 700.0f;
// Maximum fraction of the low band subtracted from the reflection at full vent.
// Capped below 1 so even a fully-open register stays audible (venting loses
// energy, so a register note is quieter and thinner, but never silent).
constexpr float kRegVentMax = 0.7f;

// --- 4c growl (only when params.growl > 0) ---
// Growl LFO rate (Hz): the sub-audio flutter/hum that sidebands the tone.
constexpr float kGrowlRateHz = 28.0f;
// Maximum breath modulation depth at full growl.
constexpr float kGrowlDepthMax = 0.5f;

// --- 4d growth cone (only when params.conical && params.cone_growth > 0) ---
// Throat integrator corner as a multiple of the fundamental: the apex's 1/r
// amplification is a low-frequency term, so the integrator low-passes the bore
// output just above the fundamental (blooming the fundamental and the lowest
// couple of partials, note-independently, rather than only the sub-bass of a
// fixed-corner filter). The pole is < 1 so the integrator is unconditionally
// bounded (the practical bounded form of the lossless pole-on-circle apex
// integrator Smith's TIIR filters would otherwise be needed to tame).
constexpr float kConeThroatMult = 1.6f;
// Radiation-side throat gain at full growth: how strongly the recovered
// low-frequency pressure is added back at the output. Output-side (not in the
// loop), so it colours the radiated tone without touching the resonance — the
// tuning and stability are unchanged by construction.
constexpr float kConeGrowthGain = 1.4f;

// --- 4e tonehole scattering (only when params.tonehole > 0) ---
// Hole position as a fraction of the one-way bore delay (the reed->hole
// distance), so the reed<->hole round trip resonates the register above the
// fundamental — the surviving mode when the hole imposes a node there. The
// value is topology-specific: a cylinder overblows to the TWELFTH (3*f0), a
// cone to the OCTAVE (2*f0). For a cone, half-way (0.5) is degenerate — the
// sub-loop coincides with the full-bore mode and the loop either quenches or
// runs away — so the cone hole sits a quarter of the way down (sub-loop at
// 2*f0), while the cylinder hole sits half-way.
constexpr float kToneholeFracCylinder = 0.5f;
constexpr float kToneholeFracCone = 0.25f;
// Maximum scattering-reflection strength at a fully open hole (the pressure
// release is partial — a real hole is a finite, radiating branch, not a perfect
// short). Kept < 1 so the reed<->hole sub-loop stays bounded alongside the main
// reed<->bell loop.
constexpr float kToneholeGainMax = 0.5f;

float note_to_hz(uint8_t note) noexcept {
  return 440.0f * std::exp2((static_cast<float>(note & 0x7Fu) - 69.0f) / 12.0f);
}

/// One-pole ramp coefficient reaching ~95% of the target in @p ms.
float ramp_coeff(float ms, double sample_rate) noexcept {
  const double t = std::max(0.5f, ms) * 0.001 * sample_rate;
  return static_cast<float>(1.0 - std::exp(-3.0 / std::max(1.0, t)));
}

}  // namespace

void ReedVoiceCore::start(const ReedPatchParams& params, double sample_rate, uint8_t note,
                          uint8_t velocity, uint64_t seed) noexcept {
  const double sr = sample_rate > 0.0 ? sample_rate : 48000.0;
  noise_ = VoiceRandomSequence(seed);
  drive_index_ = 0;
  releasing_ = false;
  breath_level_ = 0.0f;
  lp_state_ = 0.0f;
  bore_out_ = 0.0f;
  dc_x1_ = 0.0f;
  dc_y1_ = 0.0f;

  const float f0 = note_to_hz(note);
  const float period = static_cast<float>(sr) / std::max(1.0f, f0);
  // Bore topology: a cylinder (clarinet) is a negative-feedback comb of half the
  // period -> odd harmonics; a cone (sax/oboe/bassoon), approximated as an open
  // pipe, is a positive-feedback comb of the full period -> full harmonics.
  if (params.conical) {
    bore_period_ = period;
    sign_ = 1.0f;
  } else {
    bore_period_ = 0.5f * period;
    sign_ = -1.0f;
  }

  const float vel01 = static_cast<float>(velocity & 0x7Fu) / 127.0f;
  // Mouth pressure = the dynamic level: the patch breath blended with the struck
  // velocity by vel_to_breath.
  const float vel_to_breath = std::clamp(params.vel_to_breath, 0.0f, 1.0f);
  const float level = std::clamp(
      (1.0f - vel_to_breath) * params.breath_pressure + vel_to_breath * vel01, 0.0f, 1.0f);
  breath_target_ = kBreathBase + kBreathSpan * level;
  breath_ctrl_target_ = breath_target_;
  ctrl_coeff_ = ramp_coeff(kControlSmoothMs, sr);

  // Reed table from stiffness / opening.
  const float stiffness = std::clamp(params.reed_stiffness, 0.0f, 1.0f);
  const float opening = std::clamp(params.reed_opening, 0.0f, 1.0f);
  reed_offset_ = kReedOffsetMin + kReedOffsetSpan * (1.0f - opening);
  reed_slope_ = -(kReedSlopeBase + kReedSlopeSpan * stiffness);

  // Bell loop lowpass: brightness -> pole a (y += (1-a)(x - y)).
  const float a = (1.0f - std::clamp(params.brightness, 0.0f, 1.0f)) * kBellPoleSpan;
  lp_alpha_ = 1.0f - a;
  lp_alpha_target_ = lp_alpha_;
  loss_gain_ = std::clamp(kLossBase - kLossSpan * std::clamp(params.damping, 0.0f, 1.0f),
                          kLossFloor, kLossCeil);

  // In-loop DC blocker pole.
  dc_r_ = 1.0f - static_cast<float>(kTwoPi * kDcCornerHz / sr);

  // Tuning compensation: one feedback register (bore_out_ is consumed one sample
  // after it is produced) plus the bell lowpass's phase delay at the resonant
  // frequency. Subtract from the loop delay.
  const float omega = kTwoPi / std::max(1.0f, bore_period_);
  const float tau_lp =
      std::atan2(a * std::sin(omega), 1.0f - a * std::cos(omega)) / std::max(omega, 1.0e-6f);
  comp_ = 1.0f + tau_lp;

  // Circular span sized for the whole loop period plus bend-down headroom and the
  // interpolator stencil margin.
  const float eff = std::max(2.0f, bore_period_ - comp_);
  const int span = static_cast<int>(eff * 1.3f) + 8;
  bore_size_ = std::min(capacity_, std::max(16, span));
  bore_write_ = 0;

  // Contour + textures.
  attack_coeff_ = ramp_coeff(params.attack_ms, sr);
  release_coeff_ = ramp_coeff(params.release_ms, sr);
  breath_noise_ = std::clamp(params.breath_noise, 0.0f, 1.0f) * kBreathNoiseDepth;
  chiff_level_ = std::clamp(params.chiff, 0.0f, 1.0f) * kChiffDepth;
  chiff_coeff_ = ramp_coeff(params.chiff_ms, sr);
  output_scale_ = kOutputScale;

  // Prompt speech: pre-fill the bore with a low-level seeded noise burst (the
  // Karplus-Strong trick) so the reed locks onto a resonating column quickly
  // rather than swelling up from silence.
  const float prefill = kBorePrefill * breath_target_;
  if (bore_ != nullptr) {
    for (int i = 0; i < bore_size_; ++i) {
      bore_[static_cast<size_t>(i)] = prefill * noise_.bipolar_at(static_cast<uint64_t>(i));
    }
  }
  drive_index_ = static_cast<uint64_t>(bore_size_);

  // --- off-by-default advanced physics (Phase 4). When off, render() takes the
  // memoryless branch untouched (bit-identical). ---
  const float srf = static_cast<float>(sr);

  // 4a: dynamic (mass-spring) reed — a biquad bandpass resonator tuned to the
  // reed's natural frequency, driven by the pressure difference, biasing the
  // table. Off -> render skips the resonator entirely.
  reed_dyn_ = params.dynamic_reed;
  reed_z1_ = 0.0f;
  reed_z2_ = 0.0f;
  if (reed_dyn_) {
    const float res01 = std::clamp(params.reed_resonance, 0.0f, 1.0f);
    float f_reed = kReedResBaseHz + kReedResSpanHz * res01;
    f_reed = std::min(f_reed, 0.45f * srf);
    const float w = kTwoPi * f_reed / srf;
    reed_a1_ = 2.0f * kReedResR * std::cos(w);
    reed_a2_ = -kReedResR * kReedResR;
    reed_b0_ = 1.0f - kReedResR;  // unity-ish peak so the bias stays bounded
    reed_couple_ = kReedCouple;
  }

  // 4b: register vent — a low-band follower whose output is subtracted from the
  // loop reflection. Off (0) -> skipped.
  reg_vent_ = std::clamp(params.register_vent, 0.0f, 1.0f) * kRegVentMax;
  reg_lp_state_ = 0.0f;
  if (reg_vent_ > 0.0f) {
    reg_lp_alpha_ = 1.0f - std::exp(-kTwoPi * kRegVentCornerHz / srf);
  }

  // 4c: growl — a deterministic sub-audio LFO amplitude-modulating the breath.
  // Off (0) -> skipped.
  growl_depth_ = std::clamp(params.growl, 0.0f, 1.0f) * kGrowlDepthMax;
  growl_phase_ = 0.0f;
  if (growl_depth_ > 0.0f) {
    growl_inc_ = kTwoPi * kGrowlRateHz / srf;
  }

  // 4d: growth cone — the truncated-cone throat integrator (conical bores only).
  // Off (cylinder, or cone_growth 0) -> render skips the throat entirely
  // (bit-identical). The leaky pole realises the apex 1/r growth in a bounded
  // form; the tap gain folds the throat's low band back into the injection.
  throat_gain_ = 0.0f;
  throat_state_ = 0.0f;
  if (params.conical) {
    const float grow = std::clamp(params.cone_growth, 0.0f, 1.0f);
    if (grow > 0.0f) {
      throat_gain_ = kConeGrowthGain * grow;
      const float corner = std::min(kConeThroatMult * f0, 0.45f * srf);
      throat_pole_ = std::exp(-kTwoPi * corner / srf);
    }
  }

  // 4e: tonehole scattering — an inline reflection tapped from the bore at the
  // reed<->hole round trip. Off (0) -> the tap is skipped (bit-identical). The
  // hole sits kToneholeFrac of the way down the bore, so its round trip is twice
  // that; the tap depth is clamped inside the used span.
  hole_gain_ = 0.0f;
  hole_delay_samples_ = 0;
  hole_refl_ = 0.0f;
  const float hole = std::clamp(params.tonehole, 0.0f, 1.0f);
  if (hole > 0.0f) {
    hole_gain_ = kToneholeGainMax * hole;
    const float frac = params.conical ? kToneholeFracCone : kToneholeFracCylinder;
    const int round_trip = static_cast<int>(2.0f * frac * bore_period_);
    hole_delay_samples_ = std::clamp(round_trip, 1, bore_size_ - 1);
  }
}

float ReedVoiceCore::render(float pitch_ratio) noexcept {
  if (bore_ == nullptr || bore_size_ < 8) return 0.0f;
  const float ratio = pitch_ratio > 0.01f ? pitch_ratio : 0.01f;

  // Live control: ramp the steady breath / bell brightness toward their CC
  // targets (control-rate host updates, audio-rate smoothing -> no zipper).
  // Initialised equal at note-on, so an untouched note is bit-identical.
  breath_target_ += ctrl_coeff_ * (breath_ctrl_target_ - breath_target_);
  lp_alpha_ += ctrl_coeff_ * (lp_alpha_target_ - lp_alpha_);

  // Mouth pressure contour: ramp toward the target (1 while blowing, 0 once the
  // player tongues off), then the steady breath plus its turbulence.
  const float target = releasing_ ? 0.0f : 1.0f;
  const float coeff = releasing_ ? release_coeff_ : attack_coeff_;
  breath_level_ += coeff * (target - breath_level_);
  float breath = breath_target_ * breath_level_;
  if (breath_noise_ > 0.0f) {
    breath += breath * breath_noise_ * noise_.bipolar_at(drive_index_);
  }
  // Onset chiff: a short decaying noise burst folded into the breath, the reed's
  // articulated "speak" before the pitch settles.
  if (chiff_level_ > 1.0e-4f) {
    breath += chiff_level_ * breath_target_ * noise_.bipolar_at(drive_index_ + 1u);
    chiff_level_ -= chiff_coeff_ * chiff_level_;
  }
  // Growl (gated): a sub-audio LFO amplitude-modulating the breath, sidebanding
  // the tone with the rough vocal edge of a sax growl.
  if (growl_depth_ > 0.0f) {
    breath *= 1.0f + growl_depth_ * std::sin(growl_phase_);
    growl_phase_ += growl_inc_;
    if (growl_phase_ >= kTwoPi) growl_phase_ -= kTwoPi;
  }

  // Bell reflection from the previous bore output: one-pole loss lowpass, the
  // loss gain and the topology sign, then the in-loop DC blocker so the driven
  // loop sheds the breath's DC without colouring the tone.
  lp_state_ += lp_alpha_ * (bore_out_ - lp_state_);
  float refl_raw = sign_ * loss_gain_ * lp_state_;
  // Register vent (gated): subtract the tracked low band from the reflection,
  // damping the fundamental so the dominant mode rises toward the register break.
  if (reg_vent_ > 0.0f) {
    reg_lp_state_ += reg_lp_alpha_ * (refl_raw - reg_lp_state_);
    refl_raw -= reg_vent_ * reg_lp_state_;
  }
  // Tonehole scattering (gated): fold in the reflection scattered off the open
  // side hole one round trip ago, imposing a pressure node at the hole so the
  // bore's fundamental gives way to the register above.
  if (hole_delay_samples_ > 0) refl_raw += hole_refl_;
  const float dc = refl_raw - dc_x1_ + dc_r_ * dc_y1_;
  dc_x1_ = refl_raw;
  dc_y1_ = dc;
  const float refl = dc;

  // Reed valve (memoryless reed table): the pressure difference across the reed
  // drives its opening. coeff = clamp(offset + slope*dp, -1, 1). As the bore
  // pressure rises the reed pinches shut, gating the breath into the pressure
  // pulses that sustain the oscillation.
  const float dp = refl - breath;
  float reed = reed_offset_ + reed_slope_ * dp;
  // Dynamic (mass-spring) reed (gated): bias the sharp table's operating point by
  // the reed resonator's displacement, so the reed rings at its natural frequency
  // (a live beating and a "reed formant" edge) while the table stays sharp.
  if (reed_dyn_) reed += reed_couple_ * reed_resonator(dp);
  if (reed > 1.0f) reed = 1.0f;
  if (reed < -1.0f) reed = -1.0f;
  const float inj = breath + dp * reed;

  // Advance the bore delay line: write the reed injection, read the delayed
  // pressure returning from the bell.
  const float delay =
      std::clamp(bore_period_ / ratio - comp_, 1.0f, static_cast<float>(bore_size_ - 4));
  bore_out_ = rt::lagrange3_fractional_delay(bore_, static_cast<size_t>(bore_size_), bore_write_,
                                             static_cast<int>(delay * 256.0f), inj);
  ++drive_index_;

  // Tonehole scattering (gated): read the bore (read-only) at the reed<->hole
  // round trip and store the open hole's inverting partial reflection for the
  // next sample's loop reflection. bore_write_ now points past the just-written
  // injection, so the tap sits hole_delay_samples_ behind it.
  if (hole_delay_samples_ > 0) {
    const int d = hole_delay_samples_;
    const size_t idx =
        (bore_write_ + static_cast<size_t>(bore_size_ - 1 - d)) % static_cast<size_t>(bore_size_);
    hole_refl_ = -hole_gain_ * bore_[idx];
  }

  // Growth cone (gated, conical only): the bore delay line carries the
  // travelling-wave variable u = r*p (which propagates cylindrically); the
  // radiated pressure of a truncated cone recovers p = u/r, and near the apex
  // the small radius amplifies the low frequencies — the cone's "growing" apex
  // term. Realised as a stable leaky one-pole integrator on the bore output
  // (tuned near the fundamental) added back at radiation: it blooms the
  // fundamental / low partials the way a saxophone's or oboe's strong low end
  // does, WITHOUT feeding the loop (so it cannot detune or destabilise the
  // resonance — the practical bounded form of the apex integrator, in place of
  // the lossless pole-on-circle case Smith's TIIR filters would bound).
  if (throat_gain_ > 0.0f) {
    throat_state_ += (1.0f - throat_pole_) * (bore_out_ - throat_state_);
    return output_scale_ * (bore_out_ + throat_gain_ * throat_state_);
  }
  return output_scale_ * bore_out_;
}

float ReedVoiceCore::reed_resonator(float dp) noexcept {
  // Biquad bandpass (direct-form II transposed core): a damped mass-spring reed
  // driven by the pressure difference. The output is the reed's displacement,
  // which rings at its natural frequency and biases the table's operating point.
  const float y = reed_b0_ * dp + reed_a1_ * reed_z1_ + reed_a2_ * reed_z2_;
  reed_z2_ = reed_z1_;
  reed_z1_ = y;
  return y;
}

void ReedVoiceCore::set_breath(float breath01) noexcept {
  const float b = breath01 < 0.0f ? 0.0f : (breath01 > 1.0f ? 1.0f : breath01);
  // Map across the reed's stable band only, so CC2 colours the tone toward the
  // beating edge without ever silencing the reed.
  breath_ctrl_target_ = kBreathBase + kBreathSpan * b;
}

void ReedVoiceCore::set_brightness(float bright01) noexcept {
  const float br = bright01 < 0.0f ? 0.0f : (bright01 > 1.0f ? 1.0f : bright01);
  lp_alpha_target_ = 1.0f - (1.0f - br) * kBellPoleSpan;
}

void ReedVoiceCore::snap_reed_control() noexcept {
  breath_target_ = breath_ctrl_target_;
  lp_alpha_ = lp_alpha_target_;
}

void ReedVoiceCore::release() noexcept { releasing_ = true; }

void ReedVoiceCore::kill() noexcept {
  breath_level_ = 0.0f;
  lp_state_ = 0.0f;
  bore_out_ = 0.0f;
  dc_x1_ = 0.0f;
  dc_y1_ = 0.0f;
  chiff_level_ = 0.0f;
  reed_z1_ = 0.0f;
  reed_z2_ = 0.0f;
  reg_lp_state_ = 0.0f;
  throat_state_ = 0.0f;
  hole_refl_ = 0.0f;
  releasing_ = true;
}

}  // namespace sonare::midi::synth
