#include "mastering/saturation/cab_ir.h"

#include <algorithm>
#include <cmath>

#include "util/constants.h"
#include "util/exception.h"

namespace sonare::mastering::saturation {
namespace {

using constants::kPi;
using constants::kSoundSpeedMps;
using constants::kTwoPi;

// Standard cabinet dimensions. A 12" guitar driver radiates through about
// 530 cm^2 and sits on a 2x2 grid at roughly 36 cm centres inside a 30" cube; a
// 10" bass driver radiates through about 330 cm^2 on a 2x4 grid at 29 cm
// centres. Only the cone area and the spacing matter here — the generator never
// needs the box volume, because the cabinet's own low-frequency alignment is
// already in the voicing it borrows.
constexpr CabGeometry kGeomGuitar4x12{0.0530f, 1500.0f, 2, 2, 0.360f, 0.360f};
constexpr CabGeometry kGeomBass8x10{0.0330f, 1800.0f, 2, 4, 0.290f, 0.290f};

/// Bessel function of the first kind, order 1, for 0 <= x <= 3.
/// Abramowitz & Stegun 9.4.4; the polynomial is accurate to about 1e-8 over that
/// range, which is the whole range the directivity root lives in.
float bessel_j1_small(float x) noexcept {
  const float t = x / 3.0f;
  const float t2 = t * t;
  const float series =
      0.5f + t2 * (-0.56249985f +
                   t2 * (0.21093573f +
                         t2 * (-0.03954289f +
                               t2 * (0.00443319f + t2 * (-0.00031761f + t2 * 0.00001109f)))));
  return x * series;
}

/// The piston directivity function 2*J1(x)/x, which is 1 at x == 0.
float piston_directivity(float x) noexcept {
  if (x < 1e-6f) return 1.0f;
  return 2.0f * bessel_j1_small(x) / x;
}

/// Fourth-order Lagrange weights for a fractional delay, strictly causal so the
/// generated IR never reaches backwards in time.
void lagrange4_weights(float mu, float* out) noexcept {
  out[0] = -(mu - 1.0f) * (mu - 2.0f) * (mu - 3.0f) / 6.0f;
  out[1] = mu * (mu - 2.0f) * (mu - 3.0f) / 2.0f;
  out[2] = -mu * (mu - 1.0f) * (mu - 3.0f) / 2.0f;
  out[3] = mu * (mu - 1.0f) * (mu - 2.0f) / 6.0f;
}

}  // namespace

CabGeometry cab_geometry(CabModel model) noexcept {
  return model == CabModel::kBass8x10 ? kGeomBass8x10 : kGeomGuitar4x12;
}

float piston_minus3db_argument() noexcept {
  // 2*J1(x)/x falls monotonically from 1 to its first zero at x ~ 3.83, so the
  // 3 dB point is bracketed by [0, 3] and bisection is exact enough at float
  // precision in 40 steps.
  const float target = 1.0f / std::sqrt(2.0f);
  float lo = 0.0f;
  float hi = 3.0f;
  for (int i = 0; i < 40; ++i) {
    const float mid = 0.5f * (lo + hi);
    if (piston_directivity(mid) > target) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return 0.5f * (lo + hi);
}

std::vector<float> generate_cab_ir(const CabIrSpec& spec, double sample_rate) {
  if (!(sample_rate > 0.0)) {
    throw SonareException(ErrorCode::InvalidParameter, "generate_cab_ir: invalid sample rate");
  }
  const CabGeometry geometry = cab_geometry(spec.cab_model);
  const float distance_cm = std::clamp(spec.mic_distance_cm, 0.0f, kMaxMicDistanceCm);
  const float axis = std::clamp(spec.mic_axis, 0.0f, 1.0f);
  const float presence_db = std::isfinite(spec.presence_db) ? spec.presence_db : 0.0f;

  const int budget =
      std::min(kMaxCabIrSamples,
               std::max(1, static_cast<int>(std::lround(kMaxCabIrMs * 0.001 * sample_rate))));
  const float length_ms = std::isfinite(spec.length_ms) ? spec.length_ms : kMaxCabIrMs;
  const int length =
      std::clamp(static_cast<int>(std::lround(length_ms * 0.001 * sample_rate)), 1, budget);
  const size_t n = static_cast<size_t>(length);

  // The miked driver's own response is the analytic chain, unchanged.
  const CabDesign design =
      design_cab_stage(spec.cab_model, spec.mic_model, axis, distance_cm, presence_db, sample_rate);

  // Geometry. Driver (0,0) is the one being miked — a corner speaker, which is
  // where a 4x12 is actually miked — and the capsule's off-axis offset moves it
  // across that cone toward the adjacent column, as it does on a real baffle.
  const float cone_radius_m = std::sqrt(std::max(1e-6f, geometry.cone_area_m2) / kPi);
  const float offset_m = axis * cone_radius_m;
  const float depth_m = 0.01f * distance_cm;
  const float r0 = std::sqrt(offset_m * offset_m + depth_m * depth_m);

  struct Neighbour {
    float gain;        // spherical spreading, relative to the miked driver
    float delay;       // path-difference delay in samples
    float rolloff_hz;  // directivity corner, or <= 0 for none in band
  };
  std::vector<Neighbour> neighbours;
  if (spec.multi_driver) {
    const float x3 = piston_minus3db_argument();
    const float nyquist = static_cast<float>(0.5 * sample_rate);
    for (int row = 0; row < geometry.rows; ++row) {
      for (int column = 0; column < geometry.columns; ++column) {
        if (row == 0 && column == 0) continue;
        const float dx = static_cast<float>(column) * geometry.pitch_x_m - offset_m;
        const float dy = static_cast<float>(row) * geometry.pitch_y_m;
        const float lateral = std::sqrt(dx * dx + dy * dy);
        const float r = std::sqrt(lateral * lateral + depth_m * depth_m);
        if (!(r > 0.0f)) continue;
        Neighbour out{};
        out.gain = r0 / r;
        out.delay = std::max(0.0f, (r - r0) / kSoundSpeedMps * static_cast<float>(sample_rate));
        // Rigid-piston directivity, expressed as the first-order roll-off that
        // is 3 dB down where the piston is. Above breakup the radiating area
        // shrinks with frequency, so the argument grows as sqrt(f) rather than
        // f and the corner moves up accordingly.
        const float sin_theta = lateral / r;
        if (sin_theta > 1e-4f) {
          const float linear = x3 * kSoundSpeedMps / (kTwoPi * cone_radius_m * sin_theta);
          out.rolloff_hz =
              linear <= geometry.breakup_hz ? linear : linear * linear / geometry.breakup_hz;
        }
        if (out.rolloff_hz >= 0.45f * nyquist) out.rolloff_hz = 0.0f;
        neighbours.push_back(out);
      }
    }
  }

  // Every driver runs the same cabinet design, so one render per source and one
  // accumulation is the whole convolution.
  std::vector<float> input(n, 0.0f);
  input[0] = 1.0f;
  const std::vector<float> primary = render_cab_design(design, input);
  std::vector<float> ir = primary;

  for (const Neighbour& neighbour : neighbours) {
    const int base = static_cast<int>(std::floor(neighbour.delay));
    if (base >= length) continue;
    float weights[4];
    lagrange4_weights(neighbour.delay - static_cast<float>(base), weights);
    std::fill(input.begin(), input.end(), 0.0f);
    for (int tap = 0; tap < 4; ++tap) {
      const int index = base + tap;
      if (index >= 0 && index < length) {
        input[static_cast<size_t>(index)] += neighbour.gain * weights[tap];
      }
    }
    std::vector<float> path = render_cab_design(design, input);
    if (neighbour.rolloff_hz > 0.0f) {
      rt::BiquadState lp;
      lp.set(rt::first_order_lowpass(rt::frequency_to_w0(neighbour.rolloff_hz, sample_rate)));
      for (float& v : path) v = lp.process(v);
    }
    for (size_t i = 0; i < n; ++i) ir[i] += path[i];
  }

  // Level. The whole cabinet is matched to the single driver's ENERGY, not to
  // its low-frequency level. The difference matters: drivers a fraction of a
  // wavelength apart couple and sum coherently at the bottom of the range — the
  // documented reason a big multi-driver cabinet sounds darker than one of its
  // own drivers — so normalizing the low end to unity would delete that lift and
  // re-express it as a midrange hole that no cabinet has. An energy match moves
  // the level and leaves every part of the shape, coupling gain and comb alike,
  // where the geometry put it.
  if (!neighbours.empty()) {
    double summed = 0.0;
    double single = 0.0;
    for (size_t i = 0; i < n; ++i) {
      summed += static_cast<double>(ir[i]) * ir[i];
      single += static_cast<double>(primary[i]) * primary[i];
    }
    if (summed > 0.0 && single > 0.0) {
      const float scale = static_cast<float>(std::sqrt(single / summed));
      for (float& v : ir) v *= scale;
    }
  }

  // The cabinet's low resonance outlives the budget, so the truncation is faded
  // rather than cut — a step at the end of an IR is a click on every transient.
  const int fade = std::max(1, length / 10);
  for (int i = 0; i < fade; ++i) {
    const float phase = static_cast<float>(i + 1) / static_cast<float>(fade + 1);
    ir[n - static_cast<size_t>(fade) + static_cast<size_t>(i)] *=
        0.5f * (1.0f + std::cos(kPi * phase));
  }

  for (float v : ir) {
    if (!std::isfinite(v)) {
      throw SonareException(ErrorCode::InvalidParameter, "generate_cab_ir: non-finite response");
    }
  }
  return ir;
}

}  // namespace sonare::mastering::saturation
