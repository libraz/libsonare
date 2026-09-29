#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "midi/synth/gs_classic/graph_engine.h"
#include "util/constants.h"

namespace sonare::midi::synth::gs_classic {

using sonare::constants::kPiD;

namespace {

constexpr double kMsPerSecond = 1000.0;
constexpr uint8_t kInterpolationMask = 0x03;
constexpr unsigned kCrossfadeShift = 2;
/// The two read pointers sit half a window apart.
constexpr double kPointerOffsets[2] = {0.0, 0.5};

/// numpy's float remainder: the sign of the divisor, and +0 for an exact multiple.
double py_mod(double a, double b) noexcept {
  double m = std::fmod(a, b);
  if (m == 0.0) return std::copysign(0.0, b);
  if ((b < 0.0) != (m < 0.0)) m += b;
  return m;
}

/// A pointer's gain at triangle position `t`: 0 where it wraps, 1 half a window later.
double faded(GsClassicCrossfade shape, double t) noexcept {
  switch (shape) {
    case GsClassicCrossfade::kLinear:
      return t;
    case GsClassicCrossfade::kHann: {
      const double s = std::sin(kPiD * t / 2.0);
      return s * s;
    }
    case GsClassicCrossfade::kSCurve:
      return t * t * (3.0 - 2.0 * t);
  }
  return t;
}

/// The input at absolute sample `at`; the current sample comes from the input itself,
/// since inside a loop the history holds it only after the whole sample is drawn.
double tap(const GsClassicRenderContext& ctx, const GsClassicNode& node, const double* x,
           std::size_t i, int64_t at) noexcept {
  const int64_t now = ctx.block_start() + static_cast<int64_t>(i);
  if (at == now) return x[i];
  if (at > now) return 0.0;
  return ctx.history(node, at);
}

}  // namespace

std::size_t gs_classic_pitch_state_size(const GsClassicModelSet&, const GsClassicType&,
                                        const GsClassicNode&) {
  return 0;
}

void gs_classic_render_pitch(const GsClassicRenderContext& ctx, const GsClassicNode& node, double*,
                             std::size_t a, std::size_t b) {
  const auto interpolation = static_cast<GsClassicInterpolation>(node.flags & kInterpolationMask);
  const auto crossfade = static_cast<GsClassicCrossfade>(node.flags >> kCrossfadeShift);
  const bool linear = interpolation == GsClassicInterpolation::kLinear;
  // How far the interpolator reads towards the present from the delay it was asked for.
  const double reach = linear ? 1.0 : 0.0;
  const auto longest = static_cast<double>(ctx.type().max_delay_samples);
  const double* x = ctx.input(node, 0);
  double* y = ctx.output(node);
  for (std::size_t i = a; i < b; ++i) {
    const double ratio = ctx.value(node, 0, i);
    const double window = ctx.value(node, 1, i) * kGsClassicSampleRateHz / kMsPerSecond;
    const double excursion = (1.0 - ratio) * window;
    const double nearest = reach + std::max(-excursion, 0.0);
    const int64_t n = ctx.block_start() + static_cast<int64_t>(i);
    const double phase = static_cast<double>(n) / window;
    double out = 0.0;
    for (double offset : kPointerOffsets) {
      const double swept = py_mod(phase + offset, 1.0);
      // The sweep is stated in milliseconds and read back in samples, as a delay line reads it.
      const double pointer_ms =
          (nearest + excursion * swept) * kMsPerSecond / kGsClassicSampleRateHz;
      double d = pointer_ms * kGsClassicSampleRateHz / kMsPerSecond;
      if (d > longest) d = longest;
      const double whole = std::floor(d);
      const double f = d - whole;
      const int64_t at = n - static_cast<int64_t>(whole);
      double read = 0.0;
      if (linear) {
        read = (1.0 - f) * tap(ctx, node, x, i, at) + f * tap(ctx, node, x, i, at - 1);
      } else {
        read = tap(ctx, node, x, i, at);
      }
      const double t = 1.0 - std::fabs(2.0 * swept - 1.0);
      out += faded(crossfade, t) * read;
    }
    y[i] = out;
  }
}

}  // namespace sonare::midi::synth::gs_classic
