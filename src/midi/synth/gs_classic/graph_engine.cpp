#include "midi/synth/gs_classic/graph_engine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>

#include "util/constants.h"

namespace sonare::midi::synth::gs_classic {

using sonare::constants::kTwoPiD;

namespace {

constexpr double kMsPerSecond = 1000.0;
/// Control curves are stored as float, so a delay the archive puts on a whole sample can
/// arrive a float rounding under it; within this relative distance it is that sample.
constexpr double kFloatStorageTolerance = 0x1p-22;
constexpr std::size_t kNoHistory = SIZE_MAX;
constexpr std::size_t kKindCount = static_cast<std::size_t>(GsClassicNodeKind::kXNoise) + 1;
constexpr uint8_t kByteMask = 0x7F;

/// numpy's float remainder: the sign of the divisor, and +0 for an exact multiple.
double py_mod(double a, double b) noexcept {
  double m = std::fmod(a, b);
  if (m == 0.0) return std::copysign(0.0, b);
  if ((b < 0.0) != (m < 0.0)) m += b;
  return m;
}

/// numpy's compiled `interp` over n points given by accessors; ends held.
template <typename Xs, typename Ys>
double numpy_interp(double x, std::size_t n, Xs xs, Ys ys) noexcept {
  if (std::isnan(x)) return x;
  if (n == 1) return ys(0);
  if (x < xs(0)) return ys(0);
  if (x >= xs(n - 1)) return ys(n - 1);
  std::size_t lo = 0;
  std::size_t hi = n - 1;
  while (hi - lo > 1) {
    const std::size_t mid = lo + (hi - lo) / 2;
    if (xs(mid) <= x) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  if (xs(lo) == x) return ys(lo);
  const double slope = (ys(lo + 1) - ys(lo)) / (xs(lo + 1) - xs(lo));
  double r = slope * (x - xs(lo)) + ys(lo);
  if (std::isnan(r)) {
    r = slope * (x - xs(lo + 1)) + ys(lo + 1);
    if (std::isnan(r) && ys(lo) == ys(lo + 1)) r = ys(lo);
  }
  return r;
}

double curve_at(const GsClassicCurve& c, double x) noexcept {
  const double lo = c.lo;
  const double hi = c.hi;
  if (!(hi > lo)) return c.v[0];
  const double t = (x - lo) / (hi - lo) * static_cast<double>(kGsClassicCurveSize - 1);
  if (!(t > 0.0)) return c.v[0];
  if (t >= static_cast<double>(kGsClassicCurveSize - 1)) return c.v[kGsClassicCurveSize - 1];
  const auto j = static_cast<std::size_t>(t);
  const double f = t - static_cast<double>(j);
  return c.v[j] + f * (static_cast<double>(c.v[j + 1]) - c.v[j]);
}

/// The value `_from_map` gives a named byte; false where the map names nothing for it.
bool named_value(const GsClassicMapSpec& s, const uint8_t* keys, const double* values, int b,
                 double& out) noexcept {
  for (std::size_t k = 0; k < s.n; ++k) {
    if (keys[s.key_begin + k] == b) {
      out = values[s.value_begin + k];
      return true;
    }
  }
  return false;
}

/// A states map without `*`: the nearest named byte's value, the lower on a tie.
bool nearest_state(const GsClassicMapSpec& s, const uint8_t* keys, const double* values, int b,
                   double& out) noexcept {
  int best = -1;
  for (std::size_t k = 0; k < s.n; ++k) {
    const int key = keys[s.key_begin + k];
    if (key == kGsClassicStateWildcard) continue;
    const int d = std::abs(key - b);
    const int best_d = best < 0 ? INT32_MAX : std::abs(best - b);
    if (d < best_d || (d == best_d && key < best)) best = key;
  }
  return best >= 0 && named_value(s, keys, values, best, out);
}

bool map_value(const GsClassicMapSpec& s, const uint8_t* keys, const double* values, int b,
               double& out) noexcept {
  const double* v = values + s.value_begin;
  switch (s.kind) {
    case GsClassicMapKind::kSteppedTable: {
      if (s.n == 0 || s.per_entry == 0) return false;
      const std::size_t index =
          std::min<std::size_t>(static_cast<std::size_t>(b) / s.per_entry, s.n - 1u);
      out = v[index];
      return true;
    }
    case GsClassicMapKind::kWindow: {
      if (s.n != 2) return false;
      const int low = keys[s.key_begin];
      const int high = keys[s.key_begin + 1];
      if (high == low) return false;
      const int clamped = b < low ? low : (b > high ? high : b);
      const double share = static_cast<double>(clamped - low) / static_cast<double>(high - low);
      out = v[0] + share * (v[1] - v[0]);
      return true;
    }
    case GsClassicMapKind::kStates: {
      if (named_value(s, keys, values, b, out)) return true;
      if (named_value(s, keys, values, kGsClassicStateWildcard, out)) return true;
      if (b >= s.accept_lo && b <= s.accept_hi) return nearest_state(s, keys, values, b, out);
      if (named_value(s, keys, values, s.power_on, out)) return true;
      return nearest_state(s, keys, values, s.power_on, out);
    }
    case GsClassicMapKind::kPoints: {
      if (s.n == 0) return false;
      const uint8_t* k = keys + s.key_begin;
      auto xs = [k](std::size_t i) { return static_cast<double>(k[i]); };
      if (s.log != 0) {
        out = std::exp(numpy_interp(b, s.n, xs, [v](std::size_t i) { return std::log(v[i]); }));
      } else {
        out = numpy_interp(b, s.n, xs, [v](std::size_t i) { return v[i]; });
      }
      return true;
    }
    case GsClassicMapKind::kTable: {
      if (s.out_of_range >= s.n) return false;
      out = v[static_cast<std::size_t>(b) < s.n ? static_cast<std::size_t>(b) : s.out_of_range];
      return true;
    }
  }
  return false;
}

// Per-node state layouts of the kinds drawn here; slot 0 is always "initialized".
enum OscState : std::size_t {
  kInit,
  kAcc,
  kRebaseAt,
  kLastRate,
  kPhase,
  kHoldInit,
  kTick,
  kHeld,
  kOscSize
};
enum NoiseState : std::size_t { kNoiseInit, kRng, kMem0, kMem1, kMem2, kNoiseSize };

/// Phase in cycles of a byte- or constant-rate oscillator at absolute sample `n`.
/// Static rates reproduce `rate * n / fs` exactly; a rate change re-bases so phase stays
/// continuous.
double steady_phase(double* st, double rate, int64_t n) noexcept {
  if (st[kInit] == 0.0) {
    st[kInit] = 1.0;
    st[kAcc] = 0.0;
    st[kRebaseAt] = 0.0;
    st[kLastRate] = rate;
  }
  const double since = static_cast<double>(n) - st[kRebaseAt];
  if (rate != st[kLastRate]) {
    st[kAcc] = st[kLastRate] * since / kGsClassicSampleRateHz + st[kAcc];
    st[kRebaseAt] = static_cast<double>(n);
    st[kLastRate] = rate;
    return st[kAcc];
  }
  return rate * since / kGsClassicSampleRateHz + st[kAcc];
}

double lfo_shape(const GsClassicRenderContext& ctx, const GsClassicNode& node,
                 double phase) noexcept {
  switch (static_cast<GsClassicLfoShape>(node.flags)) {
    case GsClassicLfoShape::kSine:
      return std::sin(kTwoPiD * phase);
    case GsClassicLfoShape::kTriangle:
      return 4.0 * std::fabs(py_mod(phase - 0.25, 1.0) - 0.5) - 1.0;
    case GsClassicLfoShape::kSquare:
      return py_mod(phase, 1.0) < 0.5 ? 1.0 : -1.0;
    case GsClassicLfoShape::kSaw:
      return 2.0 * py_mod(phase + 0.5, 1.0) - 1.0;
    case GsClassicLfoShape::kPoints: {
      const GsClassicPoints& list = ctx.models().point_lists[node.aux];
      const GsClassicPoint* p = ctx.models().points + list.begin;
      const std::size_t n = list.n;
      // np.interp(period=1): the points wrapped once on either side.
      auto xs = [p, n](std::size_t i) {
        return i == 0 ? p[n - 1].x - 1.0 : (i == n + 1 ? p[0].x + 1.0 : p[i - 1].x);
      };
      auto ys = [p, n](std::size_t i) {
        return i == 0 ? p[n - 1].y : (i == n + 1 ? p[0].y : p[i - 1].y);
      };
      return numpy_interp(py_mod(phase, 1.0), n + 2, xs, ys);
    }
  }
  return 0.0;
}

void render_gain(const GsClassicRenderContext& ctx, const GsClassicNode& node, double*,
                 std::size_t a, std::size_t b) {
  const double* x = ctx.input(node, 0);
  double* y = ctx.output(node);
  for (std::size_t i = a; i < b; ++i) y[i] = x[i] * ctx.value(node, 0, i);
}

void render_mix(const GsClassicRenderContext& ctx, const GsClassicNode& node, double*,
                std::size_t a, std::size_t b) {
  double* y = ctx.output(node);
  for (std::size_t i = a; i < b; ++i) y[i] = 0.0;
  for (std::size_t k = 0; k < node.n_inputs; ++k) {
    const double* x = ctx.input(node, k);
    // An input weighted a constant zero is left out, as the renderer leaves it unheard.
    if (!ctx.value_varies(node, k) && ctx.value(node, k, a) == 0.0) continue;
    for (std::size_t i = a; i < b; ++i) y[i] += ctx.value(node, k, i) * x[i];
  }
}

void render_pan(const GsClassicRenderContext& ctx, const GsClassicNode& node, double*,
                std::size_t a, std::size_t b) {
  const GsClassicPanLaw& law = ctx.models().pan_laws[node.aux];
  const double* x = ctx.input(node, 0);
  double* left = ctx.output(node, 0);
  double* right = ctx.output(node, 1);
  auto xs = [](std::size_t i) { return static_cast<double>(i); };
  for (std::size_t i = a; i < b; ++i) {
    const double position = ctx.value(node, 0, i);
    const double l = numpy_interp(position, kGsClassicLutSize, xs, [&law](std::size_t k) {
      return static_cast<double>(law.left[k]);
    });
    const double r = numpy_interp(position, kGsClassicLutSize, xs, [&law](std::size_t k) {
      return static_cast<double>(law.right[k]);
    });
    left[i] = x[i] * l;
    right[i] = x[i] * r;
  }
}

void render_delay(const GsClassicRenderContext& ctx, const GsClassicNode& node, double*,
                  std::size_t a, std::size_t b) {
  double* y = ctx.output(node);
  const bool modulated = node.value_count == 3;
  const bool linear =
      static_cast<GsClassicInterpolation>(node.flags) == GsClassicInterpolation::kLinear;
  // Inside a loop a read-back under one sample has no order; it is drawn as one sample.
  const double shortest = ctx.looped() ? 1.0 : 0.0;
  const auto longest = static_cast<double>(ctx.type().max_delay_samples);
  for (std::size_t i = a; i < b; ++i) {
    double time_ms = ctx.value(node, 0, i);
    if (modulated) time_ms = time_ms + ctx.value(node, 1, i) * ctx.value(node, 2, i);
    double d = time_ms * kGsClassicSampleRateHz / kMsPerSecond;
    if (!(d >= shortest)) d = shortest;
    if (d > longest) d = longest;
    if (std::fabs(d - std::round(d)) <= d * kFloatStorageTolerance) d = std::round(d);
    const double whole = std::floor(d);
    const double f = d - whole;
    const int64_t at = ctx.block_start() + static_cast<int64_t>(i) - static_cast<int64_t>(whole);
    if (linear) {
      y[i] = (1.0 - f) * ctx.history(node, at) + f * ctx.history(node, at - 1);
    } else {
      y[i] = ctx.history(node, at);
    }
  }
}

void render_lfo(const GsClassicRenderContext& ctx, const GsClassicNode& node, double* st,
                std::size_t a, std::size_t b) {
  double* y = ctx.output(node);
  if (ctx.value_varies(node, 0)) {
    if (st[kInit] == 0.0) {
      st[kInit] = 1.0;
      st[kPhase] = ctx.value(node, 1, a);
    }
    for (std::size_t i = a; i < b; ++i) {
      y[i] = lfo_shape(ctx, node, st[kPhase]);
      st[kPhase] = st[kPhase] + ctx.value(node, 0, i) / kGsClassicSampleRateHz;
    }
    return;
  }
  for (std::size_t i = a; i < b; ++i) {
    const int64_t n = ctx.block_start() + static_cast<int64_t>(i);
    const double phase = ctx.value(node, 1, i) + steady_phase(st, ctx.value(node, 0, i), n);
    y[i] = lfo_shape(ctx, node, phase);
  }
}

void render_hold(const GsClassicRenderContext& ctx, const GsClassicNode& node, double* st,
                 std::size_t a, std::size_t b) {
  const double* x = ctx.input(node, 0);
  double* y = ctx.output(node);
  const bool varies = ctx.value_varies(node, 0);
  if (st[kHoldInit] == 0.0) {
    st[kHoldInit] = 1.0;
    st[kTick] = -1.0;
  }
  for (std::size_t i = a; i < b; ++i) {
    double phase = 0.0;
    if (varies) {
      phase = st[kPhase];
      st[kPhase] = phase + ctx.value(node, 0, i) / kGsClassicSampleRateHz;
    } else {
      phase = steady_phase(st, ctx.value(node, 0, i), ctx.block_start() + static_cast<int64_t>(i));
    }
    const double tick = std::floor(phase);
    if (tick != st[kTick]) {
      st[kHeld] = x[i];
      st[kTick] = tick;
    }
    y[i] = st[kHeld];
  }
}

void render_quantize(const GsClassicRenderContext& ctx, const GsClassicNode& node, double*,
                     std::size_t a, std::size_t b) {
  const int bits = node.aux;
  const double step = std::ldexp(1.0, 1 - bits);
  const double lowest = -std::ldexp(1.0, bits - 1);
  const double highest = std::ldexp(1.0, bits - 1) - 1.0;
  const double* x = ctx.input(node, 0);
  double* y = ctx.output(node);
  for (std::size_t i = a; i < b; ++i) {
    double word = std::floor(x[i] / step + 0.5);
    if (word < lowest) {
      word = lowest;
    } else if (word > highest) {
      word = highest;
    }
    y[i] = word * step;
  }
}

uint32_t xorshift32(double* st) noexcept {
  auto s = static_cast<uint32_t>(st[kRng]);
  s ^= s << 13;
  s ^= s >> 17;
  s ^= s << 5;
  st[kRng] = static_cast<double>(s);
  return s;
}

/// The top 16 bits of a draw as a signed word over full scale.
double noise_word(uint32_t u) noexcept {
  return static_cast<double>(static_cast<int16_t>(static_cast<uint16_t>(u >> 16))) / 32768.0;
}

// Kellet's economy pink filter: three poles and their input weights.
constexpr double kPinkPole[3] = {0.99765, 0.96300, 0.57000};
constexpr double kPinkWeight[3] = {0.0990460, 0.2965164, 1.0526913};
constexpr double kPinkDirect = 0.1848;
// Brings the pink sum to white noise's RMS (measured 0.3359 over 10 s).
constexpr double kPinkScale = 0.336;
// Hum as a mains fundamental with its second and third harmonics, normalized to a unit peak sum.
constexpr double kHumSecond = 0.5;
constexpr double kHumThird = 0.25;
constexpr double kHumNorm = 1.75;
constexpr uint32_t kNoiseSeed = 0x9E3779B9u;

void render_noise(const GsClassicRenderContext& ctx, const GsClassicNode& node, double* st,
                  std::size_t a, std::size_t b) {
  double* y = ctx.output(node);
  if (st[kNoiseInit] == 0.0) {
    st[kNoiseInit] = 1.0;
    st[kRng] = static_cast<double>(kNoiseSeed ^ node.aux);
  }
  const auto kind = static_cast<GsClassicNoise>(node.flags);
  for (std::size_t i = a; i < b; ++i) {
    const double param = ctx.value(node, 1, i);
    double v = 0.0;
    switch (kind) {
      case GsClassicNoise::kWhite:
        v = noise_word(xorshift32(st));
        break;
      case GsClassicNoise::kPink: {
        const double w = noise_word(xorshift32(st));
        st[kMem0] = kPinkPole[0] * st[kMem0] + w * kPinkWeight[0];
        st[kMem1] = kPinkPole[1] * st[kMem1] + w * kPinkWeight[1];
        st[kMem2] = kPinkPole[2] * st[kMem2] + w * kPinkWeight[2];
        v = (st[kMem0] + st[kMem1] + st[kMem2] + w * kPinkDirect) * kPinkScale;
        break;
      }
      case GsClassicNoise::kRadio: {
        const double c = std::exp(-kTwoPiD * param / kGsClassicSampleRateHz);
        st[kMem0] = (1.0 - c) * noise_word(xorshift32(st)) + c * st[kMem0];
        v = st[kMem0];
        break;
      }
      case GsClassicNoise::kDisc: {
        const uint32_t u = xorshift32(st);
        const double chance = param * 65536.0 / kGsClassicSampleRateHz;
        v = static_cast<double>(u >> 16) < chance ? noise_word(xorshift32(st)) : 0.0;
        break;
      }
      case GsClassicNoise::kHum: {
        const double t = kTwoPiD * st[kMem0];
        v = (std::sin(t) + kHumSecond * std::sin(2.0 * t) + kHumThird * std::sin(3.0 * t)) /
            kHumNorm;
        st[kMem0] += param / kGsClassicSampleRateHz;
        st[kMem0] -= std::floor(st[kMem0]);
        break;
      }
    }
    y[i] = ctx.value(node, 0, i) * v;
  }
}

std::size_t osc_state(const GsClassicModelSet&, const GsClassicType&, const GsClassicNode&) {
  return kOscSize;
}
std::size_t noise_state(const GsClassicModelSet&, const GsClassicType&, const GsClassicNode&) {
  return kNoiseSize;
}

std::size_t kind_index(GsClassicNodeKind kind) { return static_cast<std::size_t>(kind); }

bool reads_history(GsClassicNodeKind kind) {
  return kind == GsClassicNodeKind::kDelay || kind == GsClassicNodeKind::kPitch;
}

std::size_t port_count(GsClassicNodeKind kind) { return kind == GsClassicNodeKind::kPan ? 2 : 1; }

/// Whether @p n draws through the point list its `aux` names.
bool reads_points(const GsClassicNode& n) {
  return (n.kind == GsClassicNodeKind::kLfo &&
          n.flags == static_cast<uint8_t>(GsClassicLfoShape::kPoints)) ||
         (n.kind == GsClassicNodeKind::kShaper &&
          (n.flags & 0x0F) == static_cast<uint8_t>(GsClassicShaperCurve::kPoints));
}

/// Values and inputs a kind drawn here requires, or false for a malformed node.
bool well_formed(const GsClassicModelSet& m, const GsClassicNode& n) {
  if (reads_points(n) && (n.aux >= m.point_lists.size || m.point_lists[n.aux].n == 0)) {
    return false;
  }
  switch (n.kind) {
    case GsClassicNodeKind::kGain:
    case GsClassicNodeKind::kPan:
    case GsClassicNodeKind::kHold:
      return n.n_inputs == 1 && n.value_count == 1;
    case GsClassicNodeKind::kMix:
      return n.n_inputs >= 1 && n.value_count == n.n_inputs;
    case GsClassicNodeKind::kDelay:
      return n.n_inputs == 1 && (n.value_count == 1 || n.value_count == 3) &&
             n.flags <= static_cast<uint8_t>(GsClassicInterpolation::kLinear);
    case GsClassicNodeKind::kLfo:
      return n.n_inputs == 0 && n.value_count == 2 &&
             n.flags <= static_cast<uint8_t>(GsClassicLfoShape::kPoints);
    case GsClassicNodeKind::kQuantize:
      return n.n_inputs == 1 && n.value_count == 0 && n.aux >= 1 && n.aux <= 52;
    case GsClassicNodeKind::kXNoise:
      return n.n_inputs == 0 && n.value_count == 2 &&
             n.flags <= static_cast<uint8_t>(GsClassicNoise::kHum);
    case GsClassicNodeKind::kShaper:
      return n.n_inputs == 1 && n.value_count == 1 && (n.flags >> 4) == 1 &&
             (n.flags & 0x0F) <= static_cast<uint8_t>(GsClassicShaperCurve::kPoints);
    default:
      return n.n_inputs <= kGsClassicMaxInputs;
  }
}

}  // namespace

double gs_classic_interp(double x, const GsClassicPoint* points, std::size_t n) noexcept {
  if (n == 0) return 0.0;
  return numpy_interp(
      x, n, [points](std::size_t i) { return points[i].x; },
      [points](std::size_t i) { return points[i].y; });
}

bool gs_classic_expand_map(const GsClassicMapSpec& spec, const uint8_t* map_keys,
                           const double* map_values, GsClassicLut& out) noexcept {
  for (int b = 0; b < static_cast<int>(kGsClassicLutSize); ++b) {
    double v = 0.0;
    if (!map_value(spec, map_keys, map_values, b, v)) return false;
    out.v[b] = static_cast<float>(v);
  }
  return true;
}

// ---------------------------------------------------------------- context

const double* GsClassicRenderContext::input(const GsClassicNode& node,
                                            std::size_t k) const noexcept {
  return graph_->signal(graph_->models_->inputs[node.input_begin + k].signal);
}

double* GsClassicRenderContext::output(const GsClassicNode& node, std::size_t port) const noexcept {
  return graph_->signal(static_cast<uint16_t>(graph_->node_out_[graph_->node_index(node)] + port));
}

double GsClassicRenderContext::value(const GsClassicNode& node, std::size_t k,
                                     std::size_t i) const noexcept {
  const GsClassicModelSet& m = *graph_->models_;
  const GsClassicValue& v = m.values[node.value_begin + k];
  switch (v.kind) {
    case GsClassicValueKind::kConst:
      return v.constant;
    case GsClassicValueKind::kByte:
      return m.luts[v.table].v[graph_->bytes_[v.slot]];
    case GsClassicValueKind::kControl: {
      const double x = graph_->signal(v.control_ref)[i];
      return v.table == kGsClassicNone ? x : curve_at(m.curves[v.table], x);
    }
  }
  return 0.0;
}

bool GsClassicRenderContext::value_varies(const GsClassicNode& node, std::size_t k) const noexcept {
  return graph_->models_->values[node.value_begin + k].kind == GsClassicValueKind::kControl;
}

double GsClassicRenderContext::history(const GsClassicNode& node, int64_t at) const noexcept {
  if (at < 0) return 0.0;
  const std::size_t base = graph_->history_offset_[graph_->node_index(node)];
  return graph_->history_[base + (static_cast<std::size_t>(at) & graph_->history_mask_)];
}

int64_t GsClassicRenderContext::block_start() const noexcept { return graph_->block_start_; }
bool GsClassicRenderContext::looped() const noexcept { return looped_; }
uint32_t GsClassicRenderContext::byte_generation() const noexcept {
  return graph_->byte_generation_;
}
const GsClassicModelSet& GsClassicRenderContext::models() const noexcept {
  return *graph_->models_;
}
const GsClassicType& GsClassicRenderContext::type() const noexcept { return *graph_->type_; }

// ---------------------------------------------------------------- graph

const GsClassicNodeKernel* GsClassicGraph::extension(GsClassicNodeKind kind) const noexcept {
  if (kernels_ == nullptr) return nullptr;
  switch (kind) {
    case GsClassicNodeKind::kSection:
      return &kernels_->section;
    case GsClassicNodeKind::kShaper:
      return &kernels_->shaper;
    case GsClassicNodeKind::kEnvelope:
      return &kernels_->envelope;
    case GsClassicNodeKind::kGainComputer:
      return &kernels_->gain_computer;
    case GsClassicNodeKind::kVca:
      return &kernels_->vca;
    case GsClassicNodeKind::kPitch:
      return &kernels_->pitch;
    default:
      return nullptr;
  }
}

bool GsClassicGraph::prepare(const GsClassicModelSet& models, const GsClassicType& type,
                             std::size_t max_block, const GsClassicKernelTable* kernels) {
  static const std::array<GsClassicNodeKernel, kKindCount> kOwn = [] {
    std::array<GsClassicNodeKernel, kKindCount> k{};
    k[kind_index(GsClassicNodeKind::kGain)] = {nullptr, nullptr, render_gain};
    k[kind_index(GsClassicNodeKind::kMix)] = {nullptr, nullptr, render_mix};
    k[kind_index(GsClassicNodeKind::kPan)] = {nullptr, nullptr, render_pan};
    k[kind_index(GsClassicNodeKind::kDelay)] = {nullptr, nullptr, render_delay};
    k[kind_index(GsClassicNodeKind::kLfo)] = {osc_state, nullptr, render_lfo};
    k[kind_index(GsClassicNodeKind::kHold)] = {osc_state, nullptr, render_hold};
    k[kind_index(GsClassicNodeKind::kQuantize)] = {nullptr, nullptr, render_quantize};
    k[kind_index(GsClassicNodeKind::kXNoise)] = {noise_state, nullptr, render_noise};
    return k;
  }();

  if (max_block == 0 || type.node_end < type.node_begin) return false;
  models_ = &models;
  type_ = &type;
  kernels_ = kernels;
  max_block_ = max_block;
  context_.graph_ = this;

  const std::size_t count = type.node_end - type.node_begin;
  node_out_.assign(count, 0);
  kernel_of_.assign(count, GsClassicNodeKernel{});
  std::size_t signals = 2;
  for (std::size_t j = 0; j < count; ++j) {
    const GsClassicNode& node = models.nodes[type.node_begin + j];
    if (kind_index(node.kind) >= kKindCount) return false;
    node_out_[j] = static_cast<uint16_t>(signals);
    signals += port_count(node.kind);
    const GsClassicNodeKernel* ext = extension(node.kind);
    kernel_of_[j] = ext != nullptr ? *ext : kOwn[kind_index(node.kind)];
    if (kernel_of_[j].render == nullptr || !well_formed(models, node)) return false;
  }
  n_signals_ = signals;
  for (std::size_t j = 0; j < count; ++j) {
    const GsClassicNode& node = models.nodes[type.node_begin + j];
    for (std::size_t k = 0; k < node.n_inputs; ++k) {
      if (models.inputs[node.input_begin + k].signal >= signals) return false;
    }
    for (std::size_t k = 0; k < node.value_count; ++k) {
      const GsClassicValue& v = models.values[node.value_begin + k];
      if (v.kind == GsClassicValueKind::kByte &&
          (v.slot >= kGsClassicByteSlots || models.luts == nullptr ||
           v.table >= models.n_map_specs)) {
        return false;
      }
      if (v.kind == GsClassicValueKind::kControl && v.control_ref >= signals) return false;
    }
    if (node.kind == GsClassicNodeKind::kDelay && node.value_count == 3 &&
        models.values[node.value_begin + 2].kind != GsClassicValueKind::kControl) {
      return false;
    }
  }
  if (type.out_l >= signals || type.out_r >= signals) return false;
  for (std::size_t c = type.comp_begin; c < type.comp_end; ++c) {
    const GsClassicComponent& comp = models.components[c];
    if (comp.node_begin < type.node_begin || comp.node_end > type.node_end ||
        comp.node_end < comp.node_begin) {
      return false;
    }
  }

  signals_.assign(n_signals_ * max_block_, 0.0);
  state_offset_.assign(count, 0);
  std::size_t state_size = 0;
  for (std::size_t j = 0; j < count; ++j) {
    state_offset_[j] = state_size;
    if (kernel_of_[j].state_size != nullptr) {
      state_size += kernel_of_[j].state_size(models, type, models.nodes[type.node_begin + j]);
    }
  }
  state_.assign(state_size, 0.0);

  // A block is pushed whole before its reads, so capacity covers the block, the longest
  // read-back and the linear interpolator's extra sample.
  std::size_t capacity = 1;
  while (capacity < max_block_ + static_cast<std::size_t>(type.max_delay_samples) + 3) {
    capacity <<= 1;
  }
  history_mask_ = capacity - 1;
  history_offset_.assign(count, kNoHistory);
  std::size_t history_size = 0;
  for (std::size_t j = 0; j < count; ++j) {
    if (reads_history(models.nodes[type.node_begin + j].kind)) {
      history_offset_[j] = history_size;
      history_size += capacity;
    }
  }
  history_.assign(history_size, 0.0);
  history_written_.assign(count, 0);
  reset();
  return true;
}

void GsClassicGraph::reset() noexcept {
  if (models_ == nullptr) return;
  std::fill(state_.begin(), state_.end(), 0.0);
  std::fill(history_.begin(), history_.end(), 0.0);
  std::fill(history_written_.begin(), history_written_.end(), 0);
  block_start_ = 0;
  for (std::size_t j = 0; j < kernel_of_.size(); ++j) {
    if (kernel_of_[j].reset != nullptr) {
      kernel_of_[j].reset(context_, models_->nodes[type_->node_begin + j],
                          state_.data() + state_offset_[j]);
    }
  }
}

void GsClassicGraph::set_byte(std::size_t slot, uint8_t value) noexcept {
  if (slot >= kGsClassicByteSlots) return;
  const auto masked = static_cast<uint8_t>(value & kByteMask);
  if (bytes_[slot] == masked) return;
  bytes_[slot] = masked;
  ++byte_generation_;
}

uint8_t GsClassicGraph::byte(std::size_t slot) const noexcept {
  return slot < kGsClassicByteSlots ? bytes_[slot] : 0;
}

double* GsClassicGraph::signal(uint16_t ref) const noexcept {
  return signals_.data() + static_cast<std::size_t>(ref) * max_block_;
}

std::size_t GsClassicGraph::node_index(const GsClassicNode& node) const noexcept {
  return static_cast<std::size_t>(&node - (models_->nodes + type_->node_begin));
}

void GsClassicGraph::push_history(std::size_t j, std::size_t a, std::size_t b) noexcept {
  const GsClassicNode& node = models_->nodes[type_->node_begin + j];
  const double* x = signal(models_->inputs[node.input_begin].signal);
  double* ring = history_.data() + history_offset_[j];
  int64_t& written = history_written_[j];
  for (std::size_t i = a; i < b; ++i) {
    ring[static_cast<std::size_t>(written) & history_mask_] = x[i];
    ++written;
  }
}

void GsClassicGraph::render_node(std::size_t j, std::size_t a, std::size_t b,
                                 bool looped) noexcept {
  context_.looped_ = looped;
  kernel_of_[j].render(context_, models_->nodes[type_->node_begin + j],
                       state_.data() + state_offset_[j], a, b);
}

void GsClassicGraph::process_block(const double* in_l, const double* in_r, double* out_l,
                                   double* out_r, std::size_t n) noexcept {
  std::copy(in_l, in_l + n, signal(0));
  std::copy(in_r, in_r + n, signal(1));
  for (std::size_t c = type_->comp_begin; c < type_->comp_end; ++c) {
    const GsClassicComponent& comp = models_->components[c];
    const std::size_t first = comp.node_begin - type_->node_begin;
    const std::size_t last = comp.node_end - type_->node_begin;
    if (comp.looped == 0) {
      for (std::size_t j = first; j < last; ++j) {
        if (history_offset_[j] != kNoHistory) push_history(j, 0, n);
        render_node(j, 0, n, false);
      }
      continue;
    }
    for (std::size_t i = 0; i < n; ++i) {
      for (std::size_t j = first; j < last; ++j) render_node(j, i, i + 1, true);
      for (std::size_t j = first; j < last; ++j) {
        if (history_offset_[j] != kNoHistory) push_history(j, i, i + 1);
      }
    }
  }
  const double* l = signal(type_->out_l);
  const double* r = signal(type_->out_r);
  std::copy(l, l + n, out_l);
  std::copy(r, r + n, out_r);
  block_start_ += static_cast<int64_t>(n);
}

void GsClassicGraph::process(const double* in_l, const double* in_r, double* out_l, double* out_r,
                             std::size_t n) noexcept {
  if (type_ == nullptr) return;
  for (std::size_t done = 0; done < n;) {
    const std::size_t m = std::min(max_block_, n - done);
    process_block(in_l + done, in_r + done, out_l + done, out_r + done, m);
    done += m;
  }
}

}  // namespace sonare::midi::synth::gs_classic
