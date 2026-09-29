#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "midi/synth/gs_classic/graph_engine.h"

namespace sonare::midi::synth::gs_classic {

namespace {

constexpr double kMsPerSecond = 1000.0;
constexpr double kDbPerAmplitudeDecade = 20.0;
constexpr double kDbPerPowerDecade = 10.0;
constexpr uint8_t kShaperCurveMask = 0x0F;

double db_to_amplitude(double db) noexcept { return std::pow(10.0, db / kDbPerAmplitudeDecade); }

/// A one-pole coefficient reaching 1 - 1/e of a step in `ms`.
double pole_of(double ms) noexcept {
  return std::exp(-kMsPerSecond / (ms * kGsClassicSampleRateHz));
}

double sign_of(double x) noexcept { return x > 0.0 ? 1.0 : (x < 0.0 ? -1.0 : x); }

enum EnvelopeState : std::size_t { kInit, kLevel, kHeld, kEnvelopeSize };

}  // namespace

void gs_classic_render_shaper(const GsClassicRenderContext& ctx, const GsClassicNode& node, double*,
                              std::size_t a, std::size_t b) {
  // Every shipped model oversamples by 1, so the curve runs at the model's own rate.
  const auto curve = static_cast<GsClassicShaperCurve>(node.flags & kShaperCurveMask);
  const double* x = ctx.input(node, 0);
  double* y = ctx.output(node);
  for (std::size_t i = a; i < b; ++i) {
    const double v = db_to_amplitude(ctx.value(node, 0, i)) * x[i];
    switch (curve) {
      case GsClassicShaperCurve::kTanh:
        y[i] = std::tanh(v);
        break;
      case GsClassicShaperCurve::kHard:
        y[i] = std::min(std::max(v, -1.0), 1.0);
        break;
      case GsClassicShaperCurve::kCubic:
        y[i] = std::fabs(v) < 1.0 ? 1.5 * v - 0.5 * (v * v * v) : sign_of(v);
        break;
      case GsClassicShaperCurve::kPoints: {
        const GsClassicPoints& list = ctx.models().point_lists[node.aux];
        y[i] = gs_classic_interp(v, ctx.models().points + list.begin, list.n);
        break;
      }
    }
  }
}

std::size_t gs_classic_envelope_state_size(const GsClassicModelSet&, const GsClassicType&,
                                           const GsClassicNode&) {
  return kEnvelopeSize;
}

void gs_classic_envelope_reset(const GsClassicRenderContext&, const GsClassicNode&, double* state) {
  // The starting level depends on the floor, which is read on the first sample drawn.
  state[kInit] = 0.0;
}

void gs_classic_render_envelope(const GsClassicRenderContext& ctx, const GsClassicNode& node,
                                double* st, std::size_t a, std::size_t b) {
  const bool log = (node.flags & kGsClassicEnvelopeLogDomain) != 0;
  const bool rms = (node.flags & kGsClassicEnvelopeRms) != 0;
  const bool decoupled = (node.flags & kGsClassicEnvelopeDecoupled) != 0;
  const double* x = ctx.input(node, 0);
  double* y = ctx.output(node);
  if (st[kInit] == 0.0) {
    double start = 0.0;
    if (log) {
      start = ctx.value(node, 2, a);
      for (std::size_t i = a + 1; i < b; ++i) start = std::min(start, ctx.value(node, 2, i));
    }
    st[kInit] = 1.0;
    st[kLevel] = start;
    st[kHeld] = start;
  }
  double level = st[kLevel];
  double held = st[kHeld];
  for (std::size_t i = a; i < b; ++i) {
    double v = 0.0;
    if (log) {
      v = std::max(kDbPerAmplitudeDecade * std::log10(std::fabs(x[i])), ctx.value(node, 2, i));
    } else {
      v = rms ? x[i] * x[i] : std::fabs(x[i]);
    }
    const double attack = pole_of(ctx.value(node, 0, i));
    const double release = pole_of(ctx.value(node, 1, i));
    if (decoupled) {
      const double decayed = release * held + (1.0 - release) * v;
      held = decayed > v ? decayed : v;
      level = attack * level + (1.0 - attack) * held;
    } else {
      const double k = v > level ? attack : release;
      level = k * level + (1.0 - k) * v;
    }
    if (log) {
      y[i] = level;
    } else {
      y[i] = (rms ? kDbPerPowerDecade : kDbPerAmplitudeDecade) * std::log10(level);
    }
  }
  st[kLevel] = level;
  st[kHeld] = held;
}

void gs_classic_render_gain_computer(const GsClassicRenderContext& ctx, const GsClassicNode& node,
                                     double*, std::size_t a, std::size_t b) {
  const double* level = ctx.input(node, 0);
  double* y = ctx.output(node);
  for (std::size_t i = a; i < b; ++i) {
    const double threshold = ctx.value(node, 0, i);
    const double slope = 1.0 / ctx.value(node, 1, i) - 1.0;
    const double knee = ctx.value(node, 2, i);
    const double over = level[i] - threshold;
    double reduction = 0.0;
    if (2.0 * over <= -knee) {
      reduction = 0.0;
    } else if (2.0 * over >= knee) {
      reduction = slope * over;
    } else {
      const double into = over + knee / 2.0;
      reduction = slope * (into * into) / (2.0 * knee);
    }
    y[i] = db_to_amplitude(reduction);
  }
}

void gs_classic_render_vca(const GsClassicRenderContext& ctx, const GsClassicNode& node, double*,
                           std::size_t a, std::size_t b) {
  const double* x = ctx.input(node, 0);
  double* y = ctx.output(node);
  for (std::size_t i = a; i < b; ++i) y[i] = x[i] * ctx.value(node, 0, i);
}

}  // namespace sonare::midi::synth::gs_classic
