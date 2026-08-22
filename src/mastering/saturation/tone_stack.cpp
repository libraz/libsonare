#include "mastering/saturation/tone_stack.h"

#include <algorithm>

namespace sonare::mastering::saturation {

namespace {

// '59 Bassman values (DAFx-06 Fig. 1).
constexpr ToneStackComponents kAmericanComponents{0.25e-9, 20e-9, 20e-9, 250e3, 1e6, 25e3, 56e3};
// Same ladder with the later British values: a larger treble cap and a smaller
// slope resistor.
constexpr ToneStackComponents kBritishComponents{470e-12, 22e-9, 22e-9, 250e3, 1e6, 25e3, 33e3};

}  // namespace

ToneStackComponents tone_stack_components(ToneStackModel model) noexcept {
  return model == ToneStackModel::kBritish ? kBritishComponents : kAmericanComponents;
}

ToneStackCoeffs design_tone_stack(const ToneStackComponents& k, double sample_rate, double treble,
                                  double mid, double bass) noexcept {
  const double t = std::clamp(treble, 0.0, 1.0);
  const double m = std::clamp(mid, 0.0, 1.0);
  const double l = std::clamp(bass, 0.0, 1.0);

  const double c1 = k.c1, c2 = k.c2, c3 = k.c3;
  const double r1 = k.r1, r2 = k.r2, r3 = k.r3, r4 = k.r4;
  const double r3sq = r3 * r3;

  // Continuous-time coefficients of
  //   H(s) = (b1 s + b2 s^2 + b3 s^3) / (a0 + a1 s + a2 s^2 + a3 s^3).
  // Note b0 == 0: the ladder is capacitively coupled, so it blocks DC.
  const double b1 = t * c1 * r1 + m * c3 * r3 + l * (c1 * r2 + c2 * r2) + (c1 * r3 + c2 * r3);
  const double b2 = t * (c1 * c2 * r1 * r4 + c1 * c3 * r1 * r4) -
                    m * m * (c1 * c3 * r3sq + c2 * c3 * r3sq) +
                    m * (c1 * c3 * r1 * r3 + c1 * c3 * r3sq + c2 * c3 * r3sq) +
                    l * (c1 * c2 * r1 * r2 + c1 * c2 * r2 * r4 + c1 * c3 * r2 * r4) +
                    l * m * (c1 * c3 * r2 * r3 + c2 * c3 * r2 * r3) +
                    (c1 * c2 * r1 * r3 + c1 * c2 * r3 * r4 + c1 * c3 * r3 * r4);
  const double b3 = l * m * (c1 * c2 * c3 * r1 * r2 * r3 + c1 * c2 * c3 * r2 * r3 * r4) -
                    m * m * (c1 * c2 * c3 * r1 * r3sq + c1 * c2 * c3 * r3sq * r4) +
                    m * (c1 * c2 * c3 * r1 * r3sq + c1 * c2 * c3 * r3sq * r4) +
                    t * c1 * c2 * c3 * r1 * r3 * r4 - t * m * c1 * c2 * c3 * r1 * r3 * r4 +
                    t * l * c1 * c2 * c3 * r1 * r2 * r4;

  const double a0 = 1.0;
  // The denominator carries no dependence on t at all: the treble pot places
  // zeros, it does not move the poles.
  const double a1 =
      (c1 * r1 + c1 * r3 + c2 * r3 + c2 * r4 + c3 * r4) + m * c3 * r3 + l * (c1 * r2 + c2 * r2);
  const double a2 =
      m * (c1 * c3 * r1 * r3 - c2 * c3 * r3 * r4 + c1 * c3 * r3sq + c2 * c3 * r3sq) +
      l * m * (c1 * c3 * r2 * r3 + c2 * c3 * r2 * r3) - m * m * (c1 * c3 * r3sq + c2 * c3 * r3sq) +
      l * (c1 * c2 * r2 * r4 + c1 * c2 * r1 * r2 + c1 * c3 * r2 * r4 + c2 * c3 * r2 * r4) +
      (c1 * c2 * r1 * r4 + c1 * c3 * r1 * r4 + c1 * c2 * r3 * r4 + c1 * c2 * r1 * r3 +
       c1 * c3 * r3 * r4 + c2 * c3 * r3 * r4);
  const double a3 =
      l * m * (c1 * c2 * c3 * r1 * r2 * r3 + c1 * c2 * c3 * r2 * r3 * r4) -
      m * m * (c1 * c2 * c3 * r1 * r3sq + c1 * c2 * c3 * r3sq * r4) +
      m * (c1 * c2 * c3 * r3sq * r4 + c1 * c2 * c3 * r1 * r3sq - c1 * c2 * c3 * r1 * r3 * r4) +
      l * (c1 * c2 * c3 * r1 * r2 * r4) + (c1 * c2 * c3 * r1 * r3 * r4);

  // Bilinear transform, s = c (1 - z^-1)/(1 + z^-1), with c = 2/T. Both numerator
  // and denominator are multiplied through by (1 + z^-1)^3 and then by -1, which
  // cancels in the ratio.
  const double c = 2.0 * sample_rate;
  const double c2_ = c * c;
  const double c3_ = c2_ * c;

  const double bb0 = -b1 * c - b2 * c2_ - b3 * c3_;
  const double bb1 = -b1 * c + b2 * c2_ + 3.0 * b3 * c3_;
  const double bb2 = b1 * c + b2 * c2_ - 3.0 * b3 * c3_;
  const double bb3 = b1 * c - b2 * c2_ + b3 * c3_;

  const double aa0 = -a0 - a1 * c - a2 * c2_ - a3 * c3_;
  const double aa1 = -3.0 * a0 - a1 * c + a2 * c2_ + 3.0 * a3 * c3_;
  const double aa2 = -3.0 * a0 + a1 * c + a2 * c2_ - 3.0 * a3 * c3_;
  const double aa3 = -a0 + a1 * c - a2 * c2_ + a3 * c3_;

  ToneStackCoeffs out;
  const double inv = 1.0 / aa0;
  out.b0 = bb0 * inv;
  out.b1 = bb1 * inv;
  out.b2 = bb2 * inv;
  out.b3 = bb3 * inv;
  out.a1 = aa1 * inv;
  out.a2 = aa2 * inv;
  out.a3 = aa3 * inv;
  return out;
}

}  // namespace sonare::mastering::saturation
