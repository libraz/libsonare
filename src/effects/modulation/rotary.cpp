#include "effects/modulation/rotary.h"

#include <algorithm>
#include <cmath>

#include "rt/param_smoother.h"
#include "rt/scoped_no_denormals.h"
#include "rt/tail_budget.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/non_finite_state.h"

namespace sonare::effects::modulation {

using constants::kDefaultDawSampleRate;

namespace {

constexpr float kMaxDepthMs = 20.0f;

/// The fraction of the remaining gap a first-order glide closes per sample.
/// A non-positive time constant is no glide at all: the rotor arrives at once.
double glide_coeff(float tau_s, double sample_rate) noexcept {
  if (!(tau_s > 0.0f)) return 1.0;
  return -std::expm1(-1.0 / (static_cast<double>(tau_s) * sample_rate));
}

}  // namespace

Rotary::Rotary(RotaryConfig config) : config_(config) {
  config_.rate_hz = std::max(0.0f, config_.rate_hz);
  config_.drum_rate_hz = std::max(0.0f, config_.drum_rate_hz);
  config_.depth_ms = std::clamp(config_.depth_ms, 0.0f, kMaxDepthMs);
  config_.tremolo = std::clamp(config_.tremolo, 0.0f, 1.0f);
  if (!std::isfinite(config_.stereo_spread)) config_.stereo_spread = 1.0f;
  config_.stereo_spread = std::clamp(config_.stereo_spread, 0.0f, 1.0f);
  if (!std::isfinite(config_.accel_tau_s)) config_.accel_tau_s = 0.0f;
  if (!std::isfinite(config_.decel_tau_s)) config_.decel_tau_s = 0.0f;
  if (!std::isfinite(config_.undershoot_hz)) config_.undershoot_hz = 0.0f;
  if (!std::isfinite(config_.drum_undershoot_hz)) config_.drum_undershoot_hz = 0.0f;
  config_.accel_tau_s = std::max(0.0f, config_.accel_tau_s);
  config_.decel_tau_s = std::max(0.0f, config_.decel_tau_s);
  config_.undershoot_hz = std::max(0.0f, config_.undershoot_hz);
  config_.drum_undershoot_hz = std::max(0.0f, config_.drum_undershoot_hz);
  config_.horn_slow_hz = std::max(0.0f, config_.horn_slow_hz);
  config_.horn_fast_hz = std::max(0.0f, config_.horn_fast_hz);
  config_.drum_slow_hz = std::max(0.0f, config_.drum_slow_hz);
  config_.drum_fast_hz = std::max(0.0f, config_.drum_fast_hz);
  config_.speed = std::clamp(config_.speed, -1.0f, 1.0f);
}

float Rotary::horn_target_hz() const noexcept {
  if (config_.speed < 0.0f) return config_.rate_hz;
  return config_.speed >= 0.5f ? config_.horn_fast_hz : config_.horn_slow_hz;
}

float Rotary::drum_target_hz() const noexcept {
  if (config_.speed < 0.0f) return config_.drum_rate_hz;
  return config_.speed >= 0.5f ? config_.drum_fast_hz : config_.drum_slow_hz;
}

void Rotary::prepare(double sample_rate, int) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : kDefaultDawSampleRate;
  prepared_ = true;
  // One-pole lowpass crossover coefficient.
  const double x =
      std::exp(-2.0 * ::sonare::constants::kPiD * static_cast<double>(kCrossoverHz) / sample_rate_);
  lp_coeff_ = static_cast<float>(x);
  accel_coeff_ = glide_coeff(config_.accel_tau_s, sample_rate_);
  decel_coeff_ = glide_coeff(config_.decel_tau_s, sample_rate_);
  const int max_delay =
      static_cast<int>(sample_rate_ * (2.0 * static_cast<double>(kMaxDepthMs) + 2.0) * 0.001) + 1;
  for (int ch = 0; ch < 2; ++ch) {
    horn_lfo_[ch].prepare(sample_rate_);
    drum_lfo_[ch].prepare(sample_rate_);
    horn_delay_[ch].prepare(max_delay);
    drum_delay_[ch].prepare(max_delay);
    horn_delay_[ch].set_interpolation(config_.interpolation);
    drum_delay_[ch].set_interpolation(config_.interpolation);
    horn_lp_[ch].prepare(sample_rate_);
  }
  namespace geo = rotary_geometry;
  const float crossover_w0 = rt::frequency_to_w0(geo::kCrossoverHz, sample_rate_);
  const float baffle_w0 = rt::frequency_to_w0(geo::kBaffleSplitHz, sample_rate_);
  for (int stage = 0; stage < 2; ++stage) {
    xover_lp_[stage].set(rt::rbj_lowpass(crossover_w0, ::sonare::constants::kInvSqrt2));
    xover_hp_[stage].set(rt::rbj_highpass(crossover_w0, ::sonare::constants::kInvSqrt2));
    baffle_lp_[stage].set(rt::rbj_lowpass(baffle_w0, ::sonare::constants::kInvSqrt2));
    baffle_hp_[stage].set(rt::rbj_highpass(baffle_w0, ::sonare::constants::kInvSqrt2));
  }
  horn_peak_.set(rt::rbj_peak(rt::frequency_to_w0(geo::kHornPeakHz, sample_rate_), geo::kHornPeakQ,
                              geo::kHornPeakDb));
  reset();
}

float Rotary::advance_rotor(double& rate, float target, float undershoot) const noexcept {
  double aim = target;
  double coeff = decel_coeff_;
  if (target > rate) {
    // Speeding up aims short by the measured offset, and not at all where the target is nearer.
    aim = std::max(rate, static_cast<double>(target) - static_cast<double>(undershoot));
    coeff = accel_coeff_;
  }
  rate = rt::glide_toward(rate, aim, coeff);
  return static_cast<float>(rate);
}

void Rotary::process(float* const* channels, int num_channels, int num_samples) {
  if (channels == nullptr || num_channels <= 0 || num_samples <= 0) {
    return;
  }
  rt::ScopedNoDenormals no_denormals;
  const float wet = std::clamp(config_.dry_wet, 0.0f, 1.0f);
  const float dry = 1.0f - wet;
  const float trem = std::clamp(config_.tremolo, 0.0f, 1.0f);
  const float base = config_.depth_ms;
  const float swing = config_.depth_ms;
  const float ms_to_samp = 0.001f * static_cast<float>(sample_rate_);
  const float horn_target = horn_target_hz();
  const float drum_target = drum_target_hz();
  const float horn_level = db_to_linear(config_.horn_level_db);
  const float drum_level = db_to_linear(config_.drum_level_db);
  // Stereo-pair processor: the horn/drum rotor state exists for two planes only,
  // so planes beyond the pair pass through dry (see the registry's
  // stereoPairOnly classification).
  const int active = std::min(num_channels, 2);
  if (config_.model == RotaryModel::kGeometric) {
    process_geometric(channels, active, num_samples);
    discard_non_finite();
    return;
  }
  for (int i = 0; i < num_samples; ++i) {
    const float horn_rate = advance_rotor(horn_rate_, horn_target, config_.undershoot_hz);
    const float drum_rate = advance_rotor(drum_rate_, drum_target, config_.drum_undershoot_hz);
    for (int ch = 0; ch < 2; ++ch) {
      horn_lfo_[ch].set_rate_hz(horn_rate);
      drum_lfo_[ch].set_rate_hz(drum_rate);
    }
    for (int ch = 0; ch < active; ++ch) {
      if (channels[ch] == nullptr) continue;
      const float in = channels[ch][i];
      // Crossover: one-pole lowpass = drum band, remainder = horn band.
      lp_state_[ch] = in + lp_coeff_ * (lp_state_[ch] - in);
      const float drum_in = lp_state_[ch];
      const float horn_in = in - drum_in;
      const float horn_mod = horn_lfo_[ch].process();
      const float drum_mod = drum_lfo_[ch].process();
      const float horn_delay = (base + swing * horn_mod) * ms_to_samp;
      const float drum_delay = (base + 0.7f * swing * drum_mod) * ms_to_samp;
      // Tremolo attenuates from unity rather than swinging around it. The LFO
      // spans [-1, 1], so a gain of 1 + depth * mod would peak at 1 + depth --
      // at the default depth that is +3.5 dB of makeup nobody asked for, and it
      // scales with the depth control. Mapping the LFO to [1 - depth, 1] keeps
      // the same modulation shape and leaves the loudest point at unity.
      const float horn_gain = 1.0f - trem * 0.5f * (1.0f - horn_mod);
      // The bass rotor's tremolo is shallower (the drum baffle throws less).
      const float drum_gain = 1.0f - 0.6f * trem * 0.5f * (1.0f - drum_mod);
      const float horn = horn_delay_[ch].process(horn_in, horn_delay) * horn_gain * horn_level;
      const float drum = drum_delay_[ch].process(drum_in, drum_delay) * drum_gain * drum_level;
      channels[ch][i] = dry * in + wet * (horn + drum);
    }
  }
  discard_non_finite();
}

void Rotary::process_geometric(float* const* channels, int active, int num_samples) noexcept {
  namespace geo = rotary_geometry;
  using ::sonare::constants::kSoundSpeedMps;
  using ::sonare::constants::kTwoPiD;
  const float wet = std::clamp(config_.dry_wet, 0.0f, 1.0f);
  const float dry = 1.0f - wet;
  const float horn_target = horn_target_hz();
  const float drum_target = drum_target_hz();
  const float horn_level = db_to_linear(config_.horn_level_db);
  const float drum_level = db_to_linear(config_.drum_level_db);
  const float baffle_db = std::clamp(config_.tremolo, 0.0f, 1.0f) * geo::kBaffleDepthDb;
  const float radius_m = config_.depth_ms * 0.001f * kSoundSpeedMps;
  const float mic_m = radius_m + geo::kMicGapM;
  const float samples_per_m = static_cast<float>(sample_rate_) / kSoundSpeedMps;
  for (int i = 0; i < num_samples; ++i) {
    const float horn_rate = advance_rotor(horn_rate_, horn_target, config_.undershoot_hz);
    const float drum_rate = advance_rotor(drum_rate_, drum_target, config_.drum_undershoot_hz);
    const double horn_az = kTwoPiD * horn_lfo_[0].phase();
    const double baffle_az = kTwoPiD * drum_lfo_[0].phase();
    for (int ch = 0; ch < 2; ++ch) {
      horn_lfo_[ch].set_rate_hz(horn_rate);
      drum_lfo_[ch].set_rate_hz(drum_rate);
      horn_lfo_[ch].process();
      drum_lfo_[ch].process();
    }
    float in = 0.0f;
    int fed = 0;
    for (int ch = 0; ch < active; ++ch) {
      if (channels[ch] == nullptr) continue;
      in += channels[ch][i];
      ++fed;
    }
    if (fed > 1) in *= 0.5f;
    const float drum_band = xover_lp_[1].process(xover_lp_[0].process(in));
    const float horn_feed = horn_peak_.process(xover_hp_[1].process(xover_hp_[0].process(in)));
    const float drum_still = baffle_lp_[1].process(baffle_lp_[0].process(drum_band));
    const float drum_moved = baffle_hp_[1].process(baffle_hp_[0].process(drum_band));
    const float face_x = static_cast<float>(std::cos(horn_az));
    const float face_y = static_cast<float>(std::sin(horn_az));
    for (int ch = 0; ch < active; ++ch) {
      if (channels[ch] == nullptr) continue;
      // Horn mouth to microphone: the distance sets delay and gain, its angle to the mouth's
      // facing sets the low-pass corner.
      const float dx = mic_m * mic_cos_[ch] - radius_m * face_x;
      const float dy = mic_m * mic_sin_[ch] - radius_m * face_y;
      const float dist = std::sqrt(dx * dx + dy * dy);
      const float cos_angle = (dx * face_x + dy * face_y) / dist;
      float horn = horn_delay_[ch].process(horn_feed, dist * samples_per_m) * (mic_m / dist);
      horn = horn_lp_[ch].process(horn, geo::horn_corner_hz(cos_angle),
                                  ::sonare::constants::kInvSqrt2, true);
      const float baffle_gain = db_to_linear(
          -0.5f * baffle_db *
          (1.0f - static_cast<float>(std::cos(baffle_az - static_cast<double>(mic_rad_[ch])))));
      const float drum =
          drum_delay_[ch].process(drum_still + drum_moved * baffle_gain, mic_m * samples_per_m);
      channels[ch][i] = dry * channels[ch][i] + wet * (horn * horn_level + drum * drum_level);
    }
  }
}

void Rotary::discard_non_finite() noexcept {
  // The rotor delay lines are fed by the crossover output alone, so a
  // non-finite sample leaves them within one line length.
  bool discarded = discard_run_if_non_finite(lp_state_.begin(), lp_state_.end(), 0.0f);
  for (int stage = 0; stage < 2; ++stage) {
    discarded |= discard_group_if_non_finite(xover_lp_[stage].z1, xover_lp_[stage].z2);
    discarded |= discard_group_if_non_finite(xover_hp_[stage].z1, xover_hp_[stage].z2);
    discarded |= discard_group_if_non_finite(baffle_lp_[stage].z1, baffle_lp_[stage].z2);
    discarded |= discard_group_if_non_finite(baffle_hp_[stage].z1, baffle_hp_[stage].z2);
    discarded |= horn_lp_[stage].discard_non_finite();
  }
  discarded |= discard_group_if_non_finite(horn_peak_.z1, horn_peak_.z2);
  if (discarded) note_non_finite_discard();
}

void Rotary::place_mics() noexcept {
  namespace geo = rotary_geometry;
  const float spread_deg = geo::kMicSpreadDeg * config_.stereo_spread;
  const float deg[2] = {geo::kMicCentreDeg - spread_deg, geo::kMicCentreDeg + spread_deg};
  for (int ch = 0; ch < 2; ++ch) {
    mic_rad_[ch] = deg[ch] * ::sonare::constants::kPi / 180.0f;
    mic_cos_[ch] = std::cos(mic_rad_[ch]);
    mic_sin_[ch] = std::sin(mic_rad_[ch]);
  }
}

void Rotary::reset_geometric() noexcept {
  for (int stage = 0; stage < 2; ++stage) {
    xover_lp_[stage].reset();
    xover_hp_[stage].reset();
    baffle_lp_[stage].reset();
    baffle_hp_[stage].reset();
    horn_lp_[stage].reset();
  }
  horn_peak_.reset();
  place_mics();
}

int Rotary::tail_samples() const noexcept {
  if (!(std::clamp(config_.dry_wet, 0.0f, 1.0f) > 0.0f)) return 0;
  // Longest rotor path (twice the horn radius plus the microphone gap, under 2 ms), then every
  // band filter either model runs, in series.
  rt::TailBudget tail;
  tail.delay((2.0 * config_.depth_ms + 2.0) * 0.001 * sample_rate_ + kDelayReadStencilSamples);
  tail.decay(lp_coeff_);
  for (int stage = 0; stage < 2; ++stage) {
    tail.section(xover_lp_[stage].c).section(xover_hp_[stage].c);
    tail.section(baffle_lp_[stage].c).section(baffle_hp_[stage].c);
  }
  tail.section(horn_peak_.c);
  tail.then(SvfBandpass::ring(rotary_geometry::kOffAxisCornerHz, ::sonare::constants::kInvSqrt2,
                              sample_rate_));
  return tail.samples();
}

void Rotary::reset() {
  lp_state_ = {0.0f, 0.0f};
  // A rotor comes back up to speed rather than spinning from rest, so no glide is in flight here.
  horn_rate_ = horn_target_hz();
  drum_rate_ = drum_target_hz();
  // Anti-phase L/R (scaled by the stereo spread) gives the swirling image.
  const double offset = 0.5 * static_cast<double>(config_.stereo_spread);
  horn_lfo_[0].reset(0.0);
  horn_lfo_[1].reset(offset);
  drum_lfo_[0].reset(0.0);
  drum_lfo_[1].reset(offset);
  for (int ch = 0; ch < 2; ++ch) {
    horn_lfo_[ch].set_rate_hz(static_cast<float>(horn_rate_));
    drum_lfo_[ch].set_rate_hz(static_cast<float>(drum_rate_));
    horn_delay_[ch].reset();
    drum_delay_[ch].reset();
  }
  reset_geometric();
}

bool Rotary::set_parameter_impl(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      config_.rate_hz = std::max(0.0f, value);
      return true;
    case 1:
      config_.depth_ms = std::clamp(value, 0.0f, kMaxDepthMs);
      return true;
    case 2:
      config_.tremolo = std::clamp(value, 0.0f, 1.0f);
      return true;
    case 3:
      config_.dry_wet = value;
      return true;
    case 4:
      config_.drum_rate_hz = std::max(0.0f, value);
      return true;
    case 5:
      config_.horn_slow_hz = std::max(0.0f, value);
      return true;
    case 6:
      config_.horn_fast_hz = std::max(0.0f, value);
      return true;
    case 7:
      config_.drum_slow_hz = std::max(0.0f, value);
      return true;
    case 8:
      config_.drum_fast_hz = std::max(0.0f, value);
      return true;
    case 9:
      config_.speed = std::clamp(value, -1.0f, 1.0f);
      return true;
    case 10:
      config_.horn_level_db = value;
      return true;
    case 11:
      config_.drum_level_db = value;
      return true;
    case 12: {
      if (!delay_interpolation_acceptable(value)) return false;
      config_.interpolation = static_cast<DelayInterpolation>(static_cast<int>(value));
      for (int ch = 0; ch < 2; ++ch) {
        horn_delay_[ch].set_interpolation(config_.interpolation);
        drum_delay_[ch].set_interpolation(config_.interpolation);
      }
      return true;
    }
    case 13: {
      // An unnamed value is refused rather than rounded onto a neighbour.
      if (value < 0.0f || value != std::floor(value) ||
          value >= static_cast<float>(kRotaryModelCount)) {
        return false;
      }
      const auto model = static_cast<RotaryModel>(static_cast<int>(value));
      if (model != config_.model) {
        config_.model = model;
        reset_geometric();
      }
      return true;
    }
    case 14: {
      const float old_spread = config_.stereo_spread;
      config_.stereo_spread = std::clamp(value, 0.0f, 1.0f);
      if (prepared_) {
        // Shift the right LFO by the spread delta rather than resetting it.
        const double phase_delta = 0.5 * static_cast<double>(config_.stereo_spread - old_spread);
        horn_lfo_[1].reset(horn_lfo_[1].phase() + phase_delta);
        drum_lfo_[1].reset(drum_lfo_[1].phase() + phase_delta);
      }
      place_mics();
      return true;
    }
    case 15:
      config_.accel_tau_s = std::max(0.0f, value);
      accel_coeff_ = glide_coeff(config_.accel_tau_s, sample_rate_);
      return true;
    case 16:
      config_.decel_tau_s = std::max(0.0f, value);
      decel_coeff_ = glide_coeff(config_.decel_tau_s, sample_rate_);
      return true;
    case 17:
      config_.undershoot_hz = std::max(0.0f, value);
      return true;
    case 18:
      config_.drum_undershoot_hz = std::max(0.0f, value);
      return true;
    default:
      return false;
  }
}

bool Rotary::parameter_is_realtime_safe(unsigned int param_id) const noexcept {
  return param_id <= 18u;
}

std::vector<rt::ParamDescriptor> Rotary::parameter_descriptors() const {
  return {{"rateHz", 0},         {"depthMs", 1},       {"tremolo", 2},          {"dryWet", 3},
          {"drumRateHz", 4},     {"hornSlowHz", 5},    {"hornFastHz", 6},       {"drumSlowHz", 7},
          {"drumFastHz", 8},     {"speed", 9},         {"hornLevelDb", 10},     {"drumLevelDb", 11},
          {"interpolation", 12}, {"model", 13},        {"stereoSpread", 14},    {"accelTauS", 15},
          {"decelTauS", 16},     {"undershootHz", 17}, {"drumUndershootHz", 18}};
}

}  // namespace sonare::effects::modulation
