#pragma once

/// @file tail_budget.h
/// @brief How long a processor keeps sounding after its input goes silent, built
///        from the memory that produces the sound.
/// @details Every tail_samples() is derived from one of these, so a processor
///   states its delay lines, recirculating loops and decays instead of returning
///   a hand-picked number, and an owner composes its children's tails through the
///   same type. A tail ends where the residual of a unit impulse falls below
///   @ref kTailFloor.

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <limits>

namespace sonare::rt {

/// @brief Residual, relative to a unit input, below which output counts as silent.
inline constexpr double kTailFloor = 1.0e-5;

class TailBudget {
 public:
  /// @brief A tail no finite length covers: a loop whose gain does not decay.
  static constexpr int kUnbounded = std::numeric_limits<int>::max();

  constexpr TailBudget() = default;

  /// @brief A tail some other processor already reported, for composing children.
  static TailBudget reported(int samples) noexcept {
    TailBudget budget;
    budget.samples_ = std::max<std::int64_t>(0, samples);
    return budget;
  }

  /// @brief A line (or FIR) that emits its input @p samples later.
  TailBudget& delay(double samples) noexcept { return add(std::ceil(std::max(0.0, samples))); }

  /// @brief A loop of @p loop_samples that feeds back @p gain per pass: the passes
  ///        it takes for |gain|^n to fall below the floor, each one loop long.
  TailBudget& recirculation(double loop_samples, double gain) noexcept {
    const double magnitude = std::abs(gain);
    if (!(magnitude > 0.0)) return *this;
    if (!(magnitude < 1.0)) return unbounded();
    const double passes = std::ceil(std::log(kTailFloor) / std::log(magnitude));
    return add(passes * std::max(1.0, loop_samples));
  }

  /// @brief A one-pole decay whose state is multiplied by @p radius each sample.
  TailBudget& decay(double radius) noexcept { return recirculation(1.0, radius); }

  /// @brief A two-pole section with denominator 1 + a1 z^-1 + a2 z^-2 and numerator taps
  ///        summing to @p numerator_sum in magnitude. Its impulse response is bounded by
  ///        numerator_sum * (n + 1) * r^n for the larger pole radius r.
  TailBudget& biquad(double numerator_sum, double a1, double a2) noexcept {
    const double discriminant = a1 * a1 - 4.0 * a2;
    const double radius = discriminant < 0.0 ? std::sqrt(std::max(0.0, a2))
                                             : 0.5 * (std::abs(a1) + std::sqrt(discriminant));
    return ringing(numerator_sum, radius, 2);
  }

  /// @brief A three-pole section with denominator 1 + a1 z^-1 + a2 z^-2 + a3 z^-3, bounded as
  ///        numerator_sum * (n + 1)^2 * r^n for its largest pole radius r.
  TailBudget& cubic(double numerator_sum, double a1, double a2, double a3) noexcept {
    // Durand-Kerner on z^3 + a1 z^2 + a2 z + a3.
    using Complex = std::complex<double>;
    Complex roots[3] = {Complex(0.4, 0.9), Complex(0.4, 0.9) * Complex(0.4, 0.9),
                        Complex(0.4, 0.9) * Complex(0.4, 0.9) * Complex(0.4, 0.9)};
    const auto polynomial = [&](Complex z) { return ((z + a1) * z + a2) * z + a3; };
    for (int iteration = 0; iteration < 200; ++iteration) {
      for (int k = 0; k < 3; ++k) {
        Complex denominator = 1.0;
        for (int other = 0; other < 3; ++other) {
          if (other != k) denominator *= roots[k] - roots[other];
        }
        if (std::abs(denominator) > 0.0) roots[k] -= polynomial(roots[k]) / denominator;
      }
    }
    double radius = 0.0;
    for (const Complex& root : roots) radius = std::max(radius, std::abs(root));
    return ringing(numerator_sum, radius, 3);
  }

  /// @brief A biquad section given by any coefficient set with b0..b2, a1, a2.
  template <typename Coefficients>
  TailBudget& section(const Coefficients& c) noexcept {
    return biquad(std::abs(static_cast<double>(c.b0)) + std::abs(static_cast<double>(c.b1)) +
                      std::abs(static_cast<double>(c.b2)),
                  static_cast<double>(c.a1), static_cast<double>(c.a2));
  }

  /// @brief A decay stated by its 60 dB time, carried down to the floor.
  TailBudget& t60(double seconds, double sample_rate) noexcept {
    return this->seconds(seconds * std::log(kTailFloor) / std::log(1.0e-3), sample_rate);
  }

  /// @brief A tail stated as time: @p seconds at @p sample_rate.
  TailBudget& seconds(double seconds, double sample_rate) noexcept {
    return add(std::ceil(std::max(0.0, seconds) * std::max(0.0, sample_rate)));
  }

  /// @brief A stage after this one: its tail rings on after this one's.
  TailBudget& then(const TailBudget& later) noexcept {
    return add(static_cast<double>(later.samples_));
  }

  /// @brief A branch in parallel with this one: the longer of the two.
  TailBudget& alongside(const TailBudget& other) noexcept {
    samples_ = std::max(samples_, other.samples_);
    return *this;
  }

  /// @brief The tail in samples, saturating at @ref kUnbounded.
  int samples() const noexcept { return static_cast<int>(samples_); }

 private:
  TailBudget& add(double samples) noexcept {
    const double total = static_cast<double>(samples_) + samples;
    samples_ =
        total >= static_cast<double>(kUnbounded) ? kUnbounded : static_cast<std::int64_t>(total);
    return *this;
  }
  // Passes for numerator_sum * (n + 1)^(order - 1) * radius^n to reach the floor, solved by a
  // fixed point that only ever lengthens n.
  TailBudget& ringing(double numerator_sum, double radius, int order) noexcept {
    const double scale = std::abs(numerator_sum);
    if (!(radius > 0.0) || !(scale > 0.0)) return *this;
    if (!(radius < 1.0)) return unbounded();
    const double log_radius = std::log(radius);
    double n = 0.0;
    for (int step = 0; step < 12; ++step) {
      n = std::max(n, std::ceil((std::log(kTailFloor / scale) - (order - 1) * std::log(n + 1.0)) /
                                log_radius));
    }
    return add(n);
  }
  TailBudget& unbounded() noexcept {
    samples_ = kUnbounded;
    return *this;
  }

  std::int64_t samples_ = 0;
};

}  // namespace sonare::rt
