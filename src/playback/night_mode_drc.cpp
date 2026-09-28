#include "playback/night_mode_drc.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

#include "metering/bs1770_weighting.h"
#include "rt/biquad_design.h"
#include "rt/delay_line.h"
#include "rt/scoped_no_denormals.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/dsp_primitives.h"

namespace sonare::playback {

using sonare::constants::kEpsilon;
using sonare::constants::kFloorDb;

namespace {

// Static curve breakpoints: 1:1 between the two, soft-kneed on each
// side, with the quiet side additionally capped and tapered to no boost.
constexpr float kUpperBreakpointDb = 3.0f;
constexpr float kLowerBreakpointDb = -12.0f;
constexpr float kKneeWidthDb = 6.0f;
constexpr float kBoostFloorStartDb = -40.0f;    // x <= this: no boost at all
constexpr float kBoostFormulaFloorDb = -32.0f;  // x >= this: the capped formula applies as-is
constexpr float kMaxBoostPerAmountDb = 12.0f;   // boost ceiling is this times amount

/// Quadratic soft-knee gain computer (Giannoulis/Reiss/Zambon "Digital Dynamic
/// Range Compressor Design"): 1:1 below the knee, 1:`ratio` above it, joined by
/// a parabola across `knee_db` centered on `breakpoint_db`.
float knee_transition(float x_db, float breakpoint_db, float ratio, float knee_db) noexcept {
  const float half_knee = knee_db * 0.5f;
  const float over = x_db - breakpoint_db;
  if (over <= -half_knee) return x_db;
  if (over >= half_knee) return breakpoint_db + over / ratio;
  const float t = over + half_knee;
  return x_db + (1.0f / ratio - 1.0f) * t * t / (2.0f * knee_db);
}

/// Upward-expansion lift (>= 0 dB) on the quiet side, soft-kneed at
/// `kLowerBreakpointDb` and capped at `kMaxBoostPerAmountDb * amount`. Computed
/// by mirroring `knee_transition` around zero, since the quiet-side curve is
/// the loud-side one reflected. Ignores the near-silence taper below
/// `kBoostFormulaFloorDb`, which `night_mode_curve_db` applies on top.
float capped_lower_lift_db(float x_db, float amount) noexcept {
  const float ratio = 1.0f + amount;
  const float mirrored = knee_transition(-x_db, -kLowerBreakpointDb, ratio, kKneeWidthDb);
  const float lift = -mirrored - x_db;
  return std::min(lift, kMaxBoostPerAmountDb * amount);
}

}  // namespace

float night_mode_curve_db(float x_db, float amount) noexcept {
  if (amount <= 0.0f) return x_db;

  const float upper_ratio = 1.0f + 3.0f * amount;
  const float y_upper = knee_transition(x_db, kUpperBreakpointDb, upper_ratio, kKneeWidthDb);

  float lift = 0.0f;
  if (x_db > kBoostFloorStartDb) {
    if (x_db >= kBoostFormulaFloorDb) {
      lift = capped_lower_lift_db(x_db, amount);
    } else {
      // Linear taper: 0 at kBoostFloorStartDb, the capped formula's own value
      // at kBoostFormulaFloorDb.
      const float anchor = capped_lower_lift_db(kBoostFormulaFloorDb, amount);
      lift = anchor * (x_db - kBoostFloorStartDb) / (kBoostFormulaFloorDb - kBoostFloorStartDb);
    }
  }
  return y_upper + lift;
}

struct NightModeDrc::Impl {
  ChannelLayout layout = ChannelLayout::Stereo;

  // Detection: per-plane K-weighting (double, same evaluation as
  // mixing/meter.cpp), a BS.1770-weighted power sum, and a 50 ms moving
  // average of that sum held in a ring (own to this class, not the LUFS
  // meter's gated windows).
  std::array<rt::BiquadStateD, 8> k_pre{};
  std::array<rt::BiquadStateD, 8> k_rlb{};
  std::vector<double> power_ring;
  size_t ring_pos = 0;
  double ring_sum = 0.0;

  // Per-plane look-ahead so the smoothed gain is applied ahead of the signal
  // that produced it.
  std::array<rt::DelayLine, 8> lookahead{};
  int latency = 0;

  float attack_coeff = 0.0f;
  float release_coeff = 0.0f;

  float amount = 0.0f;
  float target_lufs = -24.0f;
  bool frozen = false;
  float gain_db = 0.0f;  // smoothed gain applied to every plane
};

NightModeDrc::NightModeDrc() : impl_(std::make_unique<Impl>()) {}
NightModeDrc::~NightModeDrc() = default;

void NightModeDrc::prepare(double sample_rate, int max_block_size, ChannelLayout layout) {
  (void)max_block_size;  // detection and gain run per sample; no block scratch needed.
  Impl& m = *impl_;
  m.layout = layout;

  const auto coeffs = rt::k_weighting_coefficients(sample_rate);
  const rt::BiquadCoeffsD pre{coeffs.pre.b0, coeffs.pre.b1, coeffs.pre.b2, coeffs.pre.a1,
                              coeffs.pre.a2};
  const rt::BiquadCoeffsD rlb{coeffs.rlb.b0, coeffs.rlb.b1, coeffs.rlb.b2, coeffs.rlb.a1,
                              coeffs.rlb.a2};
  for (auto& s : m.k_pre) s.set(pre);
  for (auto& s : m.k_rlb) s.set(rlb);

  const size_t window = std::max<size_t>(
      1, static_cast<size_t>(std::lround(kNightModeDetectorWindowMs * 0.001 * sample_rate)));
  m.power_ring.assign(window, 0.0);

  const size_t lookahead_samples =
      static_cast<size_t>(std::lround(kNightModeLookaheadMs * 0.001 * sample_rate));
  m.latency = static_cast<int>(lookahead_samples);
  for (auto& d : m.lookahead) d.prepare(lookahead_samples);

  m.attack_coeff = time_to_coefficient(sample_rate, kNightModeAttackMs);
  m.release_coeff = time_to_coefficient(sample_rate, kNightModeReleaseMs);

  reset();
}

void NightModeDrc::set_amount(float amount) noexcept { impl_->amount = amount; }

void NightModeDrc::set_target_lufs(float target_lufs) noexcept { impl_->target_lufs = target_lufs; }

void NightModeDrc::set_frozen(bool frozen) noexcept { impl_->frozen = frozen; }

void NightModeDrc::process(float* const* planes, int frames) noexcept {
  if (planes == nullptr || frames <= 0) return;
  Impl& m = *impl_;
  const int channels = channel_count(m.layout);
  const size_t window = m.power_ring.size();
  if (channels <= 0 || window == 0) return;
  rt::ScopedNoDenormals guard;

  const bool evaluate_curve = m.amount > 0.0f;
  for (int i = 0; i < frames; ++i) {
    if (!m.frozen) {
      // K-weighted, channel-weighted instantaneous power for this sample.
      double power = 0.0;
      for (int c = 0; c < channels; ++c) {
        float* plane = planes[static_cast<size_t>(c)];
        if (plane == nullptr) continue;
        const double y0 = m.k_pre[static_cast<size_t>(c)].process(static_cast<double>(plane[i]));
        const double y = m.k_rlb[static_cast<size_t>(c)].process(y0);
        power += metering::bs1770_channel_weight(c, channels) * y * y;
      }
      m.ring_sum += power - m.power_ring[m.ring_pos];
      m.power_ring[m.ring_pos] = power;
      m.ring_pos = (m.ring_pos + 1) % window;

      const double mean_power = m.ring_sum / static_cast<double>(window);
      const float detected_lufs = power_to_offset_db(mean_power, rt::kLoudnessOffset,
                                                     static_cast<double>(kEpsilon), kFloorDb);
      const float x_db = detected_lufs - m.target_lufs;

      if (evaluate_curve) {
        const float target_gain_db = night_mode_curve_db(x_db, m.amount) - x_db;
        const float coeff =
            std::abs(target_gain_db) > std::abs(m.gain_db) ? m.attack_coeff : m.release_coeff;
        m.gain_db = coeff * m.gain_db + (1.0f - coeff) * target_gain_db;
      } else {
        m.gain_db = 0.0f;  // amount == 0: bypass the curve entirely.
      }
    }

    const float linear_gain = db_to_linear(m.gain_db);
    for (int c = 0; c < channels; ++c) {
      float* plane = planes[static_cast<size_t>(c)];
      if (plane == nullptr) continue;
      plane[i] = m.lookahead[static_cast<size_t>(c)].process(plane[i]) * linear_gain;
    }
  }
}

void NightModeDrc::reset() noexcept {
  Impl& m = *impl_;
  for (auto& s : m.k_pre) s.reset();
  for (auto& s : m.k_rlb) s.reset();
  std::fill(m.power_ring.begin(), m.power_ring.end(), 0.0);
  m.ring_pos = 0;
  m.ring_sum = 0.0;
  for (auto& d : m.lookahead) d.reset();
  m.gain_db = 0.0f;
}

int NightModeDrc::latency_samples() const noexcept { return impl_->latency; }

NightModeDrcHandover NightModeDrc::handover() const noexcept {
  const Impl& m = *impl_;
  NightModeDrcHandover state;
  state.mean_power =
      m.power_ring.empty() ? 0.0 : m.ring_sum / static_cast<double>(m.power_ring.size());
  state.gain_db = m.gain_db;
  return state;
}

void NightModeDrc::accept_handover(const NightModeDrcHandover& state) noexcept {
  Impl& m = *impl_;
  if (m.power_ring.empty()) return;
  std::fill(m.power_ring.begin(), m.power_ring.end(), state.mean_power);
  m.ring_sum = state.mean_power * static_cast<double>(m.power_ring.size());
  m.ring_pos = 0;
  m.gain_db = state.gain_db;
}

float NightModeDrc::current_gain_db() const noexcept { return impl_->gain_db; }

}  // namespace sonare::playback
