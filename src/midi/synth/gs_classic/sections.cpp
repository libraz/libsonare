#include "midi/synth/gs_classic/sections.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <initializer_list>

#include "midi/synth/gs_classic/graph_engine.h"
#include "util/constants.h"

namespace sonare::midi::synth::gs_classic {

using sonare::constants::kPiD;
using sonare::constants::kSqrt2D;
using sonare::constants::kTwoPiD;

namespace {

using Complex = std::complex<double>;

/// One designed row, normalized so that a0 = 1.
struct GsClassicBiquad {
  double b0;
  double b1;
  double b2;
  double a1;
  double a2;
};

void put(double* rows, std::size_t r, const GsClassicBiquad& q) noexcept {
  double* p = rows + r * kGsClassicSectionRowSize;
  p[0] = q.b0;
  p[1] = q.b1;
  p[2] = q.b2;
  p[3] = q.a1;
  p[4] = q.a2;
}

/// A gain this close to 0 dB designs the unity row, as the reference designer does.
constexpr double kFlatDb = 1e-9;
constexpr double kDbPerAmplitudeDecade = 20.0;
constexpr double kDbPerShelfAmplitudeDecade = 40.0;

constexpr GsClassicBiquad kUnity{1.0, 0.0, 0.0, 0.0, 0.0};

/// `[b0, b1, b2] / [a0, a1, a2]` with every coefficient divided by a0.
GsClassicBiquad normalized(double b0, double b1, double b2, double a0, double a1,
                           double a2) noexcept {
  return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

double db_to_amplitude(double db) noexcept { return std::pow(10.0, db / kDbPerAmplitudeDecade); }

GsClassicBiquad first_order_shelf(GsClassicSide side, double corner_hz, double gain_db) noexcept {
  const double g = db_to_amplitude(gain_db);
  if (std::fabs(gain_db) < kFlatDb) return kUnity;
  double k = std::tan(kPiD * corner_hz / kGsClassicSampleRateHz);
  k = side == GsClassicSide::kLow ? k / std::sqrt(g) : k * std::sqrt(g);
  const double inv = 1.0 / k;
  double b0 = 0.0;
  double b1 = 0.0;
  if (side == GsClassicSide::kLow) {
    b0 = inv + g;
    b1 = g - inv;
  } else {
    b0 = g * inv + 1.0;
    b1 = 1.0 - g * inv;
  }
  return normalized(b0, b1, 0.0, inv + 1.0, 1.0 - inv, 0.0);
}

GsClassicBiquad second_order_shelf(GsClassicSide side, double corner_hz, double gain_db) noexcept {
  if (std::fabs(gain_db) < kFlatDb) return kUnity;
  const double amp = std::pow(10.0, gain_db / kDbPerShelfAmplitudeDecade);
  const double w0 = kTwoPiD * corner_hz / kGsClassicSampleRateHz;
  const double cos0 = std::cos(w0);
  const double sin0 = std::sin(w0);
  // The cookbook's alpha at shelf slope 1.
  const double alpha = sin0 / 2.0 * kSqrt2D;
  const double root = 2.0 * std::sqrt(amp) * alpha;
  if (side == GsClassicSide::kLow) {
    return normalized(
        amp * ((amp + 1) - (amp - 1) * cos0 + root), 2 * amp * ((amp - 1) - (amp + 1) * cos0),
        amp * ((amp + 1) - (amp - 1) * cos0 - root), (amp + 1) + (amp - 1) * cos0 + root,
        -2 * ((amp - 1) + (amp + 1) * cos0), (amp + 1) + (amp - 1) * cos0 - root);
  }
  return normalized(
      amp * ((amp + 1) + (amp - 1) * cos0 + root), -2 * amp * ((amp - 1) + (amp + 1) * cos0),
      amp * ((amp + 1) + (amp - 1) * cos0 - root), (amp + 1) - (amp - 1) * cos0 + root,
      2 * ((amp - 1) - (amp + 1) * cos0), (amp + 1) - (amp - 1) * cos0 - root);
}

GsClassicBiquad peaking(double centre_hz, double q, double gain_db) noexcept {
  if (std::fabs(gain_db) < kFlatDb) return kUnity;
  const double amp = std::pow(10.0, gain_db / kDbPerShelfAmplitudeDecade);
  const double w0 = kTwoPiD * centre_hz / kGsClassicSampleRateHz;
  const double alpha = std::sin(w0) / (2.0 * q);
  const double cos0 = std::cos(w0);
  return normalized(1 + alpha * amp, -2 * cos0, 1 - alpha * amp, 1 + alpha / amp, -2 * cos0,
                    1 - alpha / amp);
}

/// The stage's own section at `gain_db` (shelf or peaking).
GsClassicBiquad gained(const GsClassicSection& s, const GsClassicSectionValues& v,
                       double gain_db) noexcept {
  if (s.stage == GsClassicStage::kPeaking) return peaking(v.corner_hz, v.q, gain_db);
  if (s.order == 1) return first_order_shelf(s.side, v.corner_hz, gain_db);
  return second_order_shelf(s.side, v.corner_hz, gain_db);
}

/// A section stored at `towards_db` blended with the dry path to stand at `gain_db`.
GsClassicBiquad reached_by_mix(const GsClassicBiquad& at_full, double gain_db, double towards_db,
                               bool mirrored) noexcept {
  if (std::fabs(gain_db) < kFlatDb) return kUnity;
  const double wanted = db_to_amplitude(mirrored ? -gain_db : gain_db);
  const double mix = (wanted - 1.0) / (db_to_amplitude(towards_db) - 1.0);
  const double num[3] = {at_full.b0, at_full.b1, at_full.b2};
  const double den[3] = {1.0, at_full.a1, at_full.a2};
  double blended[3];
  for (int k = 0; k < 3; ++k) blended[k] = (1.0 - mix) * den[k] + mix * num[k];
  const double* top = mirrored ? den : blended;
  const double* bottom = mirrored ? blended : den;
  return normalized(top[0], top[1], top[2], bottom[0], bottom[1], bottom[2]);
}

GsClassicBiquad how_the_gain_reaches(const GsClassicSection& s, const GsClassicReachedBy* reach,
                                     const GsClassicSectionValues& v) noexcept {
  const double gain_db = v.gain_db;
  if (reach == nullptr) return gained(s, v, gain_db);
  const double full_db = reach->full_db;
  const double stored = reach->stored_at == 1 ? -full_db : full_db;
  double towards = stored;
  bool mirrored = gain_db * stored < 0;
  if (reach->other_side == 1) {
    towards = std::copysign(full_db, gain_db);
    mirrored = false;
  }
  return reached_by_mix(gained(s, v, towards), gain_db, towards, mirrored);
}

GsClassicBiquad pole_row(const GsClassicSection& s, const GsClassicSectionValues& v) noexcept {
  const bool low = s.side == GsClassicSide::kLow;
  if (!v.has_q && s.form == GsClassicSectionForm::kOneMultiply) {
    // The pole putting the -3 dB point at the corner: the inner root of
    // a^2 - 2(2 - cos w0)a + 1.
    const double cos0 = std::cos(kTwoPiD * v.corner_hz / kGsClassicSampleRateHz);
    const double root = 2.0 - cos0;
    const double a = root - std::sqrt(root * root - 1.0);
    return low ? normalized(1.0 - a, 0.0, 0.0, 1.0, -a, 0.0) : normalized(a, -a, 0.0, 1.0, -a, 0.0);
  }
  if (!v.has_q) {
    const double t = std::tan(kPiD * v.corner_hz / kGsClassicSampleRateHz);
    return low ? normalized(t, t, 0.0, 1.0 + t, t - 1.0, 0.0)
               : normalized(1.0, -1.0, 0.0, 1.0 + t, t - 1.0, 0.0);
  }
  const double w0 = kTwoPiD * v.corner_hz / kGsClassicSampleRateHz;
  const double cos0 = std::cos(w0);
  const double alpha = std::sin(w0) / (2.0 * v.q);
  if (low) {
    return normalized((1 - cos0) / 2.0, 1 - cos0, (1 - cos0) / 2.0, 1 + alpha, -2 * cos0,
                      1 - alpha);
  }
  return normalized((1 + cos0) / 2.0, -(1 + cos0), (1 + cos0) / 2.0, 1 + alpha, -2 * cos0,
                    1 - alpha);
}

long count_of(double count) noexcept { return count > 0.0 ? static_cast<long>(count) : 0; }

/// A row with numerator `[1, -2 Re z, |z|^2]` over `den`.
GsClassicBiquad zero_pair_row(Complex z, const double* den, std::size_t n_den) noexcept {
  const double r = std::abs(z);
  return normalized(1.0, -2.0 * z.real(), r * r, den[0], n_den > 1 ? den[1] : 0.0,
                    n_den > 2 ? den[2] : 0.0);
}

/// Both roots of p0 z^2 + p1 z + p2 without cancellation.
void quadratic_roots(Complex p0, Complex p1, Complex p2, Complex& r0, Complex& r1) noexcept {
  Complex root = std::sqrt(p1 * p1 - 4.0 * p0 * p2 + Complex(0.0, 0.0));
  if (!((std::conj(p1) * root).real() >= 0)) root = -root;
  const Complex q = -0.5 * (p1 + root);
  r0 = q / p0;
  r1 = p2 / q;
}

/// `1 + mix A^sections` without a loop: the all-pass poles, and zeros where A reaches a
/// root of -1/mix.
std::size_t allpass_chain(const GsClassicSectionValues& v, double* rows,
                          std::size_t capacity) noexcept {
  const long sections = count_of(v.count);
  const double mix = v.mix;
  if (mix == 0 || sections == 0) {
    if (capacity < 1) return 0;
    put(rows, 0, kUnity);
    return 1;
  }
  double den[3];
  std::size_t n_den = 2;
  if (!v.has_q) {
    const double t = std::tan(kPiD * v.corner_hz / kGsClassicSampleRateHz);
    den[0] = 1.0;
    den[1] = (t - 1.0) / (t + 1.0);
  } else {
    const double w0 = kTwoPiD * v.corner_hz / kGsClassicSampleRateHz;
    const double cos0 = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * v.q);
    den[0] = 1 + alpha;
    den[1] = -2 * cos0;
    den[2] = 1 - alpha;
    n_den = 3;
  }
  double num[3];
  const double lead = den[0];
  for (std::size_t k = 0; k < n_den; ++k) num[k] = den[n_den - 1 - k] / lead;
  for (std::size_t k = 0; k < n_den; ++k) den[k] = den[k] / lead;
  const double radius = std::pow(std::fabs(mix), -1.0 / static_cast<double>(sections));
  const auto n_sections = static_cast<double>(sections);

  std::size_t n = 0;
  for (long m = mix > 0 ? 1 : 0; m <= sections; m += 2) {
    const bool real = m == 0 || m == sections;
    if (real) {
      const double s = m == 0 ? radius : -radius;
      double solved[3];
      for (std::size_t k = 0; k < n_den; ++k) solved[k] = num[k] - s * den[k];
      if (n + 1 > capacity) return 0;
      const double s0 = solved[0];
      put(rows, n++,
          normalized(solved[0] / s0, solved[1] / s0, n_den > 2 ? solved[2] / s0 : 0.0, den[0],
                     den[1], n_den > 2 ? den[2] : 0.0));
      continue;
    }
    const Complex s = radius * std::exp(Complex(0.0, kPiD) * static_cast<double>(m) / n_sections);
    Complex solved[3];
    for (std::size_t k = 0; k < n_den; ++k) solved[k] = num[k] - s * den[k];
    if (n_den == 2) {
      if (n + 1 > capacity) return 0;
      const Complex z = -solved[1] / solved[0];
      const double squared[3] = {den[0] * den[0], 2.0 * den[0] * den[1], den[1] * den[1]};
      put(rows, n++, zero_pair_row(z, squared, 3));
    } else {
      if (n + 2 > capacity) return 0;
      Complex z0;
      Complex z1;
      quadratic_roots(solved[0], solved[1], solved[2], z0, z1);
      put(rows, n++, zero_pair_row(z0, den, 3));
      put(rows, n++, zero_pair_row(z1, den, 3));
    }
  }
  const double scale = 1.0 + mix * std::pow(num[0], n_sections);
  for (std::size_t k = 0; k < 3; ++k) rows[k] *= scale;
  return n;
}

// ---------------------------------------------------------------- kernel

// State: flags, then two delay words per row, then the rows' coefficients.
enum SectionState : std::size_t { kInit, kGeneration, kRows, kCapacity, kHeader };
constexpr std::size_t kWordsPerRow = 2;

double largest(const GsClassicModelSet& m, const GsClassicValue& v) noexcept {
  switch (v.kind) {
    case GsClassicValueKind::kConst:
      return v.constant;
    case GsClassicValueKind::kByte: {
      const GsClassicLut& lut = m.luts[v.table];
      return *std::max_element(lut.v, lut.v + kGsClassicLutSize);
    }
    case GsClassicValueKind::kControl: {
      if (v.table == kGsClassicNone) return 0.0;
      const GsClassicCurve& c = m.curves[v.table];
      return *std::max_element(c.v, c.v + kGsClassicCurveSize);
    }
  }
  return 0.0;
}

std::size_t capacity_of(const GsClassicModelSet& m, const GsClassicNode& node) noexcept {
  const GsClassicSection& s = m.sections[node.aux];
  const double count =
      s.count == kGsClassicAbsent ? 0.0 : largest(m, m.values[node.value_begin + s.count]);
  return gs_classic_section_capacity(s, count);
}

bool section_varies(const GsClassicRenderContext& ctx, const GsClassicNode& node,
                    const GsClassicSection& s) noexcept {
  for (uint8_t k : {s.corner, s.gain, s.q, s.count, s.mix}) {
    if (k != kGsClassicAbsent && ctx.value_varies(node, k)) return true;
  }
  return false;
}

GsClassicSectionValues values_at(const GsClassicRenderContext& ctx, const GsClassicNode& node,
                                 const GsClassicSection& s, std::size_t i) noexcept {
  GsClassicSectionValues v;
  if (s.corner != kGsClassicAbsent) v.corner_hz = ctx.value(node, s.corner, i);
  if (s.gain != kGsClassicAbsent) v.gain_db = ctx.value(node, s.gain, i);
  if (s.q != kGsClassicAbsent) {
    v.q = ctx.value(node, s.q, i);
    v.has_q = true;
  }
  if (s.count != kGsClassicAbsent) v.count = ctx.value(node, s.count, i);
  if (s.mix != kGsClassicAbsent) v.mix = ctx.value(node, s.mix, i);
  return v;
}

/// Designs the rows for block sample `i`; delay words of rows new to the cascade start at zero.
void redesign(const GsClassicRenderContext& ctx, const GsClassicNode& node,
              const GsClassicSection& s, double* st, std::size_t i) noexcept {
  const auto capacity = static_cast<std::size_t>(st[kCapacity]);
  double* words = st + kHeader;
  double* rows = words + capacity * kWordsPerRow;
  const GsClassicReachedBy* reach =
      s.reached_by == kGsClassicNone ? nullptr : &ctx.models().reached_by[s.reached_by];
  const auto before = static_cast<std::size_t>(st[kRows]);
  const std::size_t now =
      gs_classic_design_section(s, reach, values_at(ctx, node, s, i), rows, capacity);
  for (std::size_t r = before; r < now; ++r) {
    words[r * kWordsPerRow] = 0.0;
    words[r * kWordsPerRow + 1] = 0.0;
  }
  st[kRows] = static_cast<double>(now);
}

}  // namespace

std::size_t gs_classic_section_capacity(const GsClassicSection& section,
                                        double largest_count) noexcept {
  const auto count = static_cast<std::size_t>(count_of(largest_count));
  switch (section.stage) {
    case GsClassicStage::kShelf:
    case GsClassicStage::kPeaking:
      return 1;
    case GsClassicStage::kPole:
      return std::max<std::size_t>(count, 1);
    case GsClassicStage::kAllpassChain:
      return count + 2;
  }
  return 1;
}

std::size_t gs_classic_design_section(const GsClassicSection& section,
                                      const GsClassicReachedBy* reached,
                                      const GsClassicSectionValues& values, double* rows,
                                      std::size_t capacity) noexcept {
  switch (section.stage) {
    case GsClassicStage::kShelf:
    case GsClassicStage::kPeaking:
      if (capacity < 1) return 0;
      put(rows, 0, how_the_gain_reaches(section, reached, values));
      return 1;
    case GsClassicStage::kPole: {
      const auto count = static_cast<std::size_t>(count_of(values.count));
      if (count == 0) {
        if (capacity < 1) return 0;
        put(rows, 0, kUnity);
        return 1;
      }
      if (count > capacity) return 0;
      const GsClassicBiquad one = pole_row(section, values);
      for (std::size_t r = 0; r < count; ++r) put(rows, r, one);
      return count;
    }
    case GsClassicStage::kAllpassChain:
      return allpass_chain(values, rows, capacity);
  }
  return 0;
}

std::size_t gs_classic_section_state_size(const GsClassicModelSet& models, const GsClassicType&,
                                          const GsClassicNode& node) {
  return kHeader + capacity_of(models, node) * (kWordsPerRow + kGsClassicSectionRowSize);
}

void gs_classic_section_reset(const GsClassicRenderContext& ctx, const GsClassicNode& node,
                              double* state) {
  state[kCapacity] = static_cast<double>(capacity_of(ctx.models(), node));
}

void gs_classic_render_section(const GsClassicRenderContext& ctx, const GsClassicNode& node,
                               double* st, std::size_t a, std::size_t b) {
  const GsClassicSection& s = ctx.models().sections[node.aux];
  const auto capacity = static_cast<std::size_t>(st[kCapacity]);
  double* words = st + kHeader;
  const double* rows = words + capacity * kWordsPerRow;
  const bool varies = section_varies(ctx, node, s);
  const auto generation = static_cast<double>(ctx.byte_generation());
  if (!varies && (st[kInit] == 0.0 || st[kGeneration] != generation)) {
    redesign(ctx, node, s, st, a);
    st[kInit] = 1.0;
    st[kGeneration] = generation;
  }
  const double* x = ctx.input(node, 0);
  double* y = ctx.output(node);
  for (std::size_t i = a; i < b; ++i) {
    if (varies) redesign(ctx, node, s, st, i);
    const auto n_rows = static_cast<std::size_t>(st[kRows]);
    double now = x[i];
    // Transposed direct form II, the recurrence of the reference renderer.
    for (std::size_t r = 0; r < n_rows; ++r) {
      const double* q = rows + r * kGsClassicSectionRowSize;
      double* z = words + r * kWordsPerRow;
      const double next = q[0] * now + z[0];
      z[0] = q[1] * now - q[3] * next + z[1];
      z[1] = q[2] * now - q[4] * next;
      now = next;
    }
    y[i] = now;
  }
}

}  // namespace sonare::midi::synth::gs_classic
