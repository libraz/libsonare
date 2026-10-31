#include "midi/synth/singing_voice.h"

#include <algorithm>
#include <cmath>

#include "midi/synth/pitch.h"
#include "midi/synth/string_loop.h"
#include "util/constants.h"

namespace sonare::midi::synth {

#include "midi/synth/singing_pitch_compensation.inc"

namespace {

using sonare::constants::kDefaultDawSampleRate;
using sonare::constants::kPi;
using sonare::constants::kTwoPi;

constexpr float kTubeMinArea = AcousticTube<kSingingTractSections>::kMinArea;

// Fold and junction operating point, in the pressure unit of the fold's base
// stiffness. Measured on the reference tract from A2 to C#4: phonation starts
// below force 0.15 and F0 then rises 130-160 cents to full breath without a jump.
constexpr float kLungPressure = 0.21f;
constexpr float kRestBreathy = 0.067f;
constexpr float kRestPressed = -0.12f;
constexpr float kFoldDamping = 0.051f;
constexpr float kFoldCoupling = 0.23f;
constexpr float kFoldCollision = 2.08f;
constexpr float kFoldStiffening = 0.15f;
constexpr float kJunctionZeta = 0.15f;

// Cover fraction the morph axis spans: thin (falsetto) to thick (chest).
constexpr float kCoverThin = 0.35f;
constexpr float kCoverThick = 0.85f;

// Extra rest opening of the breathing position, against the paired adduction.
constexpr float kAbductionOpening = 0.3f;

// Tract wall loss (nepers per metre) and the lips' flare corner.
constexpr float kTractDamping = 0.5f;
constexpr float kLipCutoffHz = 4000.0f;

// Lip rounding: the last two centimetres narrow by up to 80 % and the tract
// lengthens by up to 1.5 cm, which is what lowers every formant of /u/.
constexpr float kLipRegionM = 0.02f;
constexpr float kRoundingNarrowing = 0.8f;
constexpr float kRoundingLengthM = 0.015f;

// Aspiration at breath_noise 1. Turbulence grows as the square of the flow, so
// its share rises with the breath and falls with the closed phase on its own.
constexpr float kAspirationDepth = 0.25f;

// Vibrato depth 1 moves tension by 6 %, about +/-50 cents.
constexpr float kVibratoTensionDepth = 0.06f;

// Radiated pressure to a musical voice level.
constexpr float kOutputScale = 300.0f;

// The fold's tension follows a pitch step at a muscle's speed rather than in one
// sample: a step in tension knocks the fold off its limit cycle for ~10 ms.
constexpr float kTensionSettleMs = 20.0f;

// Live-control glide toward a moved axis, as the other sustained engines.
constexpr float kControlSmoothMs = 8.0f;
// The constriction is redrawn at this sample interval while it moves.
constexpr int kShapeBlock = 32;
// Gap at which a smoothed axis is declared arrived.
constexpr float kCtrlSettle = 1.0e-6f;

/// Bracketing index and fraction of @p x over the ascending @p grid, clamped to its ends.
template <int kCount>
void locate(const float (&grid)[kCount], float x, int& lo, float& frac) noexcept {
  if (x <= grid[0]) {
    lo = 0;
    frac = 0.0f;
    return;
  }
  lo = kCount - 2;
  for (int i = 0; i < kCount - 1; ++i) {
    if (x < grid[i + 1]) {
      lo = i;
      break;
    }
  }
  frac = std::min(1.0f, (x - grid[lo]) / (grid[lo + 1] - grid[lo]));
}

float table_tension(int force_lo, float force_frac, int pitch_lo, float pitch_frac) noexcept {
  const auto row = [&](int f) {
    const float* t = kSingingCompTension[f];
    return t[pitch_lo] + pitch_frac * (t[pitch_lo + 1] - t[pitch_lo]);
  };
  const float a = row(force_lo);
  return a + force_frac * (row(force_lo + 1) - a);
}

}  // namespace

// ---------------------------------------------------------------------------
// SingingPhonation
// ---------------------------------------------------------------------------

void SingingPhonation::configure(double sample_rate, float note_hz, float fold_mass,
                                 float cover_fraction) noexcept {
  const float omega = kTwoPi * std::max(note_hz, 1.0f);
  const float mass = std::max(fold_mass, 1.0e-3f);
  fold_mass_ = mass;
  fold_.configure(sample_rate, {mass / (omega * omega), cover_fraction, kFoldCoupling * mass, mass,
                                kFoldDamping, kRestBreathy, kFoldCollision, kFoldStiffening});
  junction_.configure(1.0f, kJunctionZeta);
  tract_.configure(sample_rate, kTractDamping);
  lips_.configure(sample_rate, kLipCutoffHz, 0.0f);
  lips_reflected_ = 0.0f;
  pitch_ratio_ = 1.0f;
  pitch_scale_ = 1.0f;
}

void SingingPhonation::configure_patch(double sample_rate, float note_hz,
                                       const SingingPatchParams& params) noexcept {
  configure(sample_rate, note_hz, params.fold_mass, singing_cover_fraction(params.morph));
  set_tract(kSingingReferenceTractM * std::clamp(params.tract_length, 0.5f, 2.0f), params.rounding,
            params.position, params.brightness);
}

void SingingPhonation::set_tract(float length_m, float rounding, float position,
                                 float brightness) noexcept {
  const float round = std::clamp(rounding, 0.0f, 1.0f);
  length_m_ = length_m + kRoundingLengthM * round;
  tract_.set_length(length_m_);
  for (int i = 0; i < kSingingTractSections; ++i) {
    const float x_m =
        length_m_ * static_cast<float>(i) / static_cast<float>(kSingingTractSections - 1);
    profile_[i] = length_m_ - x_m < kLipRegionM ? 1.0f - kRoundingNarrowing * round : 1.0f;
  }
  tract_.set_areas(profile_);
  set_constriction(position, brightness);
}

void SingingPhonation::set_tap(float position, float closure) noexcept {
  const float centre_m = std::clamp(position, 0.0f, 1.0f) * length_m_;
  const float depth = std::clamp(closure, 0.0f, 1.0f);
  float areas[kSingingTractSections];
  for (int i = 0; i < kSingingTractSections; ++i) {
    const float x_m =
        length_m_ * static_cast<float>(i) / static_cast<float>(kSingingTractSections - 1);
    const float dist = std::fabs(x_m - centre_m);
    float w = 0.0f;
    if (dist < kTubeConstrictionHalfWidthM) {
      w = 0.5f * (1.0f + std::cos(kPi * dist / kTubeConstrictionHalfWidthM));
    }
    areas[i] = profile_[i] * std::pow(kTubeMinArea, depth * w);
  }
  tract_.set_areas(areas);
}

void SingingPhonation::set_constriction(float position, float brightness) noexcept {
  // Brightness is logarithmic in area: 1 leaves the tract open, 0 closes it to the floor.
  const float area = std::pow(kTubeMinArea, 1.0f - std::clamp(brightness, 0.0f, 1.0f));
  tract_.set_constriction(std::clamp(position, 0.0f, 1.0f), 1.0f - area);
}

void SingingPhonation::set_pitch_scale(float ratio) noexcept {
  pitch_ratio_ = ratio;
  pitch_scale_ = ratio * ratio;
  junction_.set_pressure_scale(pitch_scale_);
}

float SingingPhonation::process(float force, float abduction, float tension, float noise) noexcept {
  const float s = pitch_scale_;
  const float lung = s * kLungPressure * force;
  fold_.set_rest_opening(kRestBreathy + (kRestPressed - kRestBreathy) * force +
                         kAbductionOpening * abduction);
  fold_.set_tension(fold_mass_ * tension * s);
  const PassiveCoupling::Junction j =
      junction_.solve(lung, tract_.source_incoming(), 0.0f, fold_.law());
  fold_.process(lung, lung - j.drop);
  const float unit_flow = j.flow / pitch_ratio_;
  const float injected = j.injection + noise * junction_.flow_gain() * j.flow * std::fabs(unit_flow);
  const rt::OpenEndRadiation::Output out = lips_.process(tract_.process(injected, lips_reflected_));
  lips_reflected_ = out.reflected;
  return out.radiated / s;
}

float SingingPhonation::probe_tract(float input) noexcept {
  const rt::OpenEndRadiation::Output out =
      lips_.process(tract_.process(input + tract_.source_incoming(), lips_reflected_));
  lips_reflected_ = out.reflected;
  return out.radiated;
}

double SingingPhonation::stored_energy() const noexcept {
  const double lips =
      static_cast<double>(lips_.stored_energy()) + lips_reflected_ * lips_reflected_;
  return tract_.stored_energy() + tract_.mouth_area() * lips + fold_.stored_energy();
}

void SingingPhonation::reset() noexcept {
  fold_.reset();
  junction_.reset();
  tract_.reset();
  lips_.reset();
  lips_reflected_ = 0.0f;
}

float singing_cover_fraction(float morph) noexcept {
  return kCoverThin + (kCoverThick - kCoverThin) * std::clamp(morph, 0.0f, 1.0f);
}

float singing_compensated_tension(float note_hz, float force, float weight) noexcept {
  int pitch_lo = 0;
  float pitch_frac = 0.0f;
  locate(kSingingCompPitchHz, note_hz, pitch_lo, pitch_frac);
  int force_lo = 0;
  float force_frac = 0.0f;
  locate(kSingingCompForce, force, force_lo, force_frac);
  const float threshold = table_tension(0, 0.0f, pitch_lo, pitch_frac);
  if (weight == 0.0f) return threshold;
  return threshold +
         weight * (table_tension(force_lo, force_frac, pitch_lo, pitch_frac) - threshold);
}

// ---------------------------------------------------------------------------
// SingingVoiceCore
// ---------------------------------------------------------------------------

void SingingVoiceCore::start(const SingingPatchParams& params, double sample_rate, uint8_t note,
                             Velocity16 velocity, uint64_t seed) noexcept {
  const double sr = sample_rate > 0.0 ? sample_rate : kDefaultDawSampleRate;
  sample_rate_ = sr;
  note_hz_ = note_to_hz(note);
  noise_ = VoiceRandomSequence(seed);
  drive_index_ = 0;

  const float vel01 = velocity.f7() / 127.0f;
  axes_.home.force = std::clamp(params.velocity_to_force, 0.0f, 1.0f) * vel01;
  axes_.home.position = std::clamp(params.position, 0.0f, 1.0f);
  axes_.home.brightness = std::clamp(params.brightness, 0.0f, 1.0f);
  axes_.home.morph = std::clamp(params.morph, 0.0f, 1.0f);
  axes_.base = axes_.home;
  axes_.mod = {};
  target_ = axes_.base;
  live_ = axes_.base;
  ctrl_coeff_ = ramp_coeff(kControlSmoothMs, sr);
  ratio_coeff_ = ramp_coeff(kTensionSettleMs, sr);
  ratio_ = -1.0f;
  moving_ = false;

  compensation_ = std::clamp(params.pitch_compensation, 0.0f, 1.0f);

  tap_depth_ = std::clamp(params.tap_depth, 0.0f, 1.0f);
  tap_position_ = std::clamp(params.tap_position, 0.0f, 1.0f);
  tap_phase_ = tap_depth_ > 0.0f ? 0.0f : 1.0f;
  tap_inc_ =
      1.0f / std::max(1.0f, 2.0f * std::max(params.tap_ms, 1.0f) * 0.001f * static_cast<float>(sr));

  phonation_.configure_patch(sr, note_hz_, params);
  shape_countdown_ = 0;
  apply_shape();

  breath_.start(params.breath_onset_ms, params.release_ms, sr);
  adduction_.start(params.attack_ms, params.release_ms, sr);

  vib_depth_ = std::clamp(params.vibrato_depth, 0.0f, 1.0f) * kVibratoTensionDepth;
  vib_phase_ = 0.0f;
  vib_inc_ = vib_depth_ > 0.0f
                 ? kTwoPi * std::max(0.1f, params.vibrato_rate_hz) / static_cast<float>(sr)
                 : 0.0f;
  breath_noise_ =
      std::clamp(params.breath_noise, 0.0f, 1.0f) * kAspirationDepth * noise_gain_at_rate(sr);
}

void SingingVoiceCore::set_excitation_base(const ExcitationAxes& base, uint32_t present) noexcept {
  if ((present & kAxisForce) != 0u) axes_.base.force = std::clamp(base.force, 0.0f, 1.0f);
  if ((present & kAxisPosition) != 0u) {
    axes_.base.position = std::clamp(base.position, 0.0f, 1.0f);
  }
  if ((present & kAxisBrightness) != 0u) {
    axes_.base.brightness = std::clamp(base.brightness, 0.0f, 1.0f);
  }
  if ((present & kAxisMorph) != 0u) axes_.base.morph = std::clamp(base.morph, 0.0f, 1.0f);
  refresh_targets();
}

void SingingVoiceCore::restore_excitation_base() noexcept {
  axes_.base = axes_.home;
  refresh_targets();
}

void SingingVoiceCore::set_excitation_mod(const ExcitationAxes& offsets) noexcept {
  axes_.mod = offsets;
  refresh_targets();
}

void SingingVoiceCore::refresh_targets() noexcept {
  target_.force = std::clamp(axes_.base.force + axes_.mod.force, 0.0f, 1.0f);
  target_.position = std::clamp(axes_.base.position + axes_.mod.position, 0.0f, 1.0f);
  target_.brightness = std::clamp(axes_.base.brightness + axes_.mod.brightness, 0.0f, 1.0f);
  target_.morph = std::clamp(axes_.base.morph + axes_.mod.morph, 0.0f, 1.0f);
  moving_ = target_.force != live_.force || target_.position != live_.position ||
            target_.brightness != live_.brightness || target_.morph != live_.morph;
}

void SingingVoiceCore::snap_excitation() noexcept {
  if (!moving_) return;
  live_ = target_;
  moving_ = false;
  apply_shape();
}

void SingingVoiceCore::apply_shape() noexcept {
  phonation_.set_constriction(live_.position, live_.brightness);
  phonation_.set_cover_fraction(singing_cover_fraction(live_.morph));
}

void SingingVoiceCore::apply_tap() noexcept {
  // A tongue-tip closure over the held vowel: the vowel's own constriction never moves.
  float closure = 0.0f;
  if (tap_phase_ < 1.0f) {
    const float s = std::sin(kPi * tap_phase_);
    closure = tap_depth_ * s * s;
  }
  phonation_.set_tap(tap_position_, closure);
}

float SingingVoiceCore::render(float pitch_ratio) noexcept {
  const float asked = pitch_ratio > 0.01f ? pitch_ratio : 0.01f;
  if (ratio_ < 0.0f) ratio_ = asked;
  ratio_ += ratio_coeff_ * (asked - ratio_);
  const float ratio = ratio_;

  bool reshape = false;
  bool retap = false;
  if (moving_) {
    const auto glide = [&](float& value, float target) {
      value += ctrl_coeff_ * (target - value);
      if (std::fabs(target - value) < kCtrlSettle) value = target;
    };
    glide(live_.force, target_.force);
    glide(live_.position, target_.position);
    glide(live_.brightness, target_.brightness);
    glide(live_.morph, target_.morph);
    moving_ = live_.force != target_.force || live_.position != target_.position ||
              live_.brightness != target_.brightness || live_.morph != target_.morph;
    reshape = true;
  }
  if (tap_phase_ < 1.0f) {
    tap_phase_ = std::min(1.0f, tap_phase_ + tap_inc_);
    retap = true;
  }
  if ((reshape || retap) && --shape_countdown_ <= 0) {
    shape_countdown_ = kShapeBlock;
    if (reshape) apply_shape();
    if (retap) apply_tap();
  }
  // Open the closure on the sample the tap ends rather than at the next block.
  if (retap && tap_phase_ >= 1.0f) apply_tap();

  const float breath = breath_.advance();
  const float adduction = adduction_.advance();
  const float force = live_.force * breath;

  float tension = singing_compensated_tension(note_hz_ * ratio, force, compensation_);
  if (vib_depth_ > 0.0f) {
    tension *= 1.0f + vib_depth_ * std::sin(vib_phase_);
    vib_phase_ += vib_inc_;
    if (vib_phase_ >= kTwoPi) vib_phase_ -= kTwoPi;
  }
  phonation_.set_pitch_scale(ratio);

  float noise = 0.0f;
  if (breath_noise_ > 0.0f) {
    noise = breath_noise_ * noise_.bipolar_at(drive_index_);
  }
  ++drive_index_;

  return kOutputScale * phonation_.process(force, 1.0f - adduction, tension, noise);
}

void SingingVoiceCore::release() noexcept {
  breath_.release();
  adduction_.release();
}

void SingingVoiceCore::kill() noexcept {
  breath_.kill();
  adduction_.kill();
  phonation_.reset();
  tap_phase_ = 1.0f;
  vib_phase_ = 0.0f;
}

}  // namespace sonare::midi::synth
