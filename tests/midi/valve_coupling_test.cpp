/// @file valve_coupling_test.cpp
/// @brief The valves and their implicit tube junction (midi/synth/pressure_valve,
///        fold_valve, passive_coupling): the fold oscillates on a steady lung
///        pressure, the coupled fold-tube system loses energy once the pressure
///        is cut -- through a near-closure of the tract as well -- a reed at a
///        threshold-hugging operating point oscillates only because the junction
///        is implicit, and the solver converges over the whole clamped domain.
///
/// Passivity is not the non-vacuity instrument for the valve: a fold reduced to
/// a linear spring passes it more easily, and stops oscillating. Both are needed.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "midi/synth/acoustic_tube.h"
#include "midi/synth/fold_valve.h"
#include "midi/synth/passive_coupling.h"
#include "midi/synth/pressure_valve.h"
#include "rt/open_end_radiation.h"
#include "util/constants.h"

namespace {

using sonare::constants::kTwoPiD;
using sonare::midi::synth::AcousticTube;
using sonare::midi::synth::CouplingStats;
using sonare::midi::synth::FoldValve;
using sonare::midi::synth::PassiveCoupling;
using sonare::midi::synth::PressureValve;
using sonare::midi::synth::ValveLaw;
using sonare::midi::synth::ValveOrientation;

constexpr double kSr = 48000.0;
constexpr float kLungPressure = 0.8f;

/// Fold, junction, tract and lips at the configuration every coupled case uses:
/// a 130 Hz natural fold behind a 17.5 cm tract.
struct Phonation {
  FoldValve fold;
  PassiveCoupling junction;
  AcousticTube<100> tract;
  sonare::rt::OpenEndRadiation lips;
  float lips_reflected = 0.0f;

  Phonation() {
    const float omega = static_cast<float>(kTwoPiD * 130.0);
    fold.configure(kSr, {1.0f / (omega * omega), 0.5f, 0.3f, 1.0f, 0.1f, 0.05f, 3.0f});
    junction.configure(1.0f, 0.3f);
    tract.configure(kSr, 0.5f);
    tract.set_length(0.175f);
    lips.configure(kSr, 4000.0f, 0.0f);
  }

  /// One sample; returns the glottal flow.
  float step(float lung) {
    const auto j = junction.solve(lung, tract.source_incoming(), 0.0f, fold.law());
    const float flow = fold.process(lung, lung - j.drop);
    lips_reflected = lips.process(tract.process(j.injection, lips_reflected)).reflected;
    return flow;
  }

  double energy() const {
    const double lips_energy = lips.stored_energy() + lips_reflected * lips_reflected;
    return tract.stored_energy() + tract.mouth_area() * lips_energy + fold.stored_energy();
  }
};

void ac_dc(const std::vector<float>& x, std::size_t from, std::size_t to, double& ac, double& dc) {
  dc = 0.0;
  for (std::size_t i = from; i < to; ++i) dc += x[i];
  dc /= static_cast<double>(to - from);
  ac = 0.0;
  for (std::size_t i = from; i < to; ++i) ac += (x[i] - dc) * (x[i] - dc);
  ac = std::sqrt(ac / static_cast<double>(to - from));
}

double rms(const std::vector<float>& x, std::size_t from, std::size_t to) {
  double sum = 0.0;
  for (std::size_t i = from; i < to; ++i) sum += static_cast<double>(x[i]) * x[i];
  return std::sqrt(sum / static_cast<double>(to - from));
}

/// 512-sample block means never rise and no sample exceeds the start by more than 1e-6.
void require_passive(const std::vector<double>& energy) {
  const double start = energy.front();
  REQUIRE(start > 0.0);
  double previous = start * 2.0;
  for (std::size_t b = 0; (b + 1) * 512 <= energy.size(); ++b) {
    double sum = 0.0;
    for (std::size_t i = 0; i < 512; ++i) {
      const double e = energy[b * 512 + i];
      REQUIRE(e <= start * (1.0 + 1.0e-6));
      sum += e;
    }
    const double mean = sum / 512.0;
    INFO("block " << b << " mean " << mean << " previous " << previous);
    REQUIRE(mean <= previous);
    previous = mean;
  }
}

/// The junction map in double, inverted by bisection: the reference the float solver is held to.
double reference_root(double target, double zeta, const ValveLaw& law) {
  const auto map = [&](double u) {
    const double x = u * std::fabs(u);
    const double o = std::clamp(static_cast<double>(law.alpha) - law.beta * x, 0.0,
                                static_cast<double>(law.ceiling));
    return x + zeta * o * u;
  };
  double lo = target >= 0.0 ? 0.0 : -std::sqrt(-target);
  double hi = target >= 0.0 ? std::sqrt(target) : 0.0;
  for (int it = 0; it < 64; ++it) {
    const double mid = 0.5 * (lo + hi);
    if (map(mid) > target) {
      hi = mid;
    } else {
      lo = mid;
    }
  }
  return 0.5 * (lo + hi);
}

/// Error of one solve against the double reference, in units of the tolerance the solver stops on.
double solve_error(PassiveCoupling& c, float upstream, float incoming, float steady,
                   const ValveLaw& law, float pressure_scale) {
  const auto j = c.solve(upstream, incoming, steady, law);
  const double target =
      (static_cast<double>(upstream) - 2.0 * incoming) / pressure_scale +
      static_cast<double>(c.zeta()) * steady / std::sqrt(static_cast<double>(pressure_scale));
  const double u_ref = reference_root(target, c.zeta(), law);
  const double x = j.drop / pressure_scale;
  const double u = std::copysign(std::sqrt(std::fabs(x)), x);
  return std::fabs(u - u_ref) / (PassiveCoupling::kTol * std::max(1.0, std::fabs(u_ref)));
}

}  // namespace

TEST_CASE("fold valve self-oscillates on a steady lung pressure behind a tract",
          "[midi][synth][passive_coupling]") {
  Phonation voice;
  std::vector<float> flow;
  for (int n = 0; n < static_cast<int>(kSr); ++n) flow.push_back(voice.step(kLungPressure));
  double early_ac = 0.0;
  double early_dc = 0.0;
  double late_ac = 0.0;
  double late_dc = 0.0;
  ac_dc(flow, 9600, 19200, early_ac, early_dc);
  ac_dc(flow, 38400, 48000, late_ac, late_dc);
  const float closed = *std::min_element(flow.begin() + 38400, flow.end());
  INFO("flow AC/DC 0.2-0.4 s " << early_ac << "/" << early_dc << ", 0.8-1.0 s " << late_ac << "/"
                               << late_dc);
  // Started: a swing comparable to the mean flow, closing the glottis every cycle.
  REQUIRE(late_ac > 0.3 * late_dc);
  REQUIRE(closed == 0.0f);
  // Sustained: the late swing has not decayed from the early one.
  REQUIRE(late_ac > 0.5 * early_ac);
  REQUIRE(voice.junction.stats().cap_hits == 0);
}

TEST_CASE("coupled fold and tract lose energy once the lung pressure is cut",
          "[midi][synth][passive_coupling]") {
  constexpr int kSounding = 24000;
  constexpr int kDecay = 9600;
  SECTION("open tract") {
    Phonation voice;
    for (int n = 0; n < kSounding; ++n) voice.step(kLungPressure);
    std::vector<double> energy;
    for (int n = 0; n < kDecay; ++n) {
      energy.push_back(voice.energy());
      voice.step(0.0f);
    }
    require_passive(energy);
  }

  SECTION("through a near-closure of the tract") {
    // A tap's transit: the front of the tract driven to the area floor and back,
    // where the junction's reflection approaches 1.
    Phonation voice;
    for (int n = 0; n < kSounding; ++n) voice.step(kLungPressure);
    std::vector<double> energy;
    constexpr int kBlock = 64;
    constexpr int kRamp = 480;
    for (int n = 0; n < kDecay; ++n) {
      if (n % kBlock == 0) {
        const float t = static_cast<float>(n) / kRamp;
        const float degree = t < 1.0f ? t : t < 2.0f ? 1.0f : std::max(0.0f, 3.0f - t);
        voice.tract.set_constriction(0.8f, degree);
      }
      energy.push_back(voice.energy());
      voice.step(0.0f);
    }
    require_passive(energy);
  }
}

TEST_CASE("pressure valve at a threshold-hugging reed oscillates through the implicit junction",
          "[midi][synth][passive_coupling]") {
  // The quasi-static reed of GM 65 as voiced at velocity 127: closing pressure 2.4,
  // zeta 0.387, mouth 0.8872 (gamma 0.3697), behind a cylinder losing 4.06 % per
  // round trip. The implicit junction's loop gain is 1.028 there and an explicit
  // one, reading the drop off the returning wave alone, is 0.993.
  PressureValve<ValveOrientation::kInward> reed;
  reed.configure(kSr, {0.0f, 2.4f, 0.0f, 1.0f, 0.0f});
  PassiveCoupling junction;
  junction.configure(reed.closing_pressure(), 0.387f);
  constexpr float kMouth = 0.8872f;
  constexpr float kRoundTripGain = 0.9594f;
  constexpr int kBore = 100;
  std::vector<float> bore(kBore);
  for (int i = 0; i < kBore; ++i) bore[i] = 1.0e-3f * std::sin(0.37f * i);
  const float steady =
      PassiveCoupling::opening(kMouth / reed.closing_pressure(), reed.law()) * std::sqrt(kMouth);
  std::vector<float> injected;
  int write = 0;
  for (int n = 0; n < static_cast<int>(kSr); ++n) {
    const float incoming = -kRoundTripGain * bore[write];
    const auto j = junction.solve(kMouth, incoming, steady, reed.law());
    reed.process(kMouth, kMouth - j.drop);
    bore[write] = j.injection;
    write = (write + 1) % kBore;
    injected.push_back(j.injection);
  }
  const double seed = rms(injected, 0, 480);
  const double middle = rms(injected, 19200, 24000);
  const double late = rms(injected, 43200, 48000);
  INFO("injection RMS: seed " << seed << ", 0.4-0.5 s " << middle << ", last 0.1 s " << late);
  REQUIRE(late > 30.0 * seed);
  REQUIRE(late > 0.5 * middle);
  REQUIRE(junction.stats().cap_hits == 0);
}

TEST_CASE("passive coupling map keeps its minimum slope at the ends of the open branch",
          "[midi][synth][passive_coupling]") {
  constexpr ValveLaw kReed{1.0f, 1.0f, 1.0f};
  for (int z = 1; z <= 18; ++z) {
    const float zeta = 0.05f * static_cast<float>(z);
    INFO("zeta " << zeta);
    REQUIRE(std::fabs(PassiveCoupling::slope(0.0f, zeta, kReed) - zeta) < 1.0e-6f);
    REQUIRE(std::fabs(PassiveCoupling::slope(1.0f - 1.0e-4f, zeta, kReed) - (2.0f - 2.0f * zeta)) <
            1.0e-3f);
    REQUIRE(PassiveCoupling::slope(1.0f + 1.0e-4f, zeta, kReed) > 1.99f);
    float minimum = 10.0f;
    for (int i = 0; i <= 1000; ++i)
      minimum = std::min(minimum, PassiveCoupling::slope(0.001f * i, zeta, kReed));
    REQUIRE(minimum >= std::min(zeta, 2.0f - 2.0f * zeta) * (1.0f - 1.0e-5f));
  }
}

TEST_CASE("passive coupling converges over the clamped domain", "[midi][synth][passive_coupling]") {
  // The convergence instrument on the solver alone, over the domain the
  // clamps allow (zeta <= 0.9, gamma < 1) and a swept X range, no voice needed.
  constexpr ValveLaw kReed{1.0f, 1.0f, 1.0f};
  constexpr float kMouth = 0.86f;
  CouplingStats total;
  int smooth_iterations = 0;
  double worst_error = 0.0;
  const auto accumulate = [&](const CouplingStats& s) {
    total.solves += s.solves;
    total.cap_hits += s.cap_hits;
    total.iterations += s.iterations;
    total.max_iterations = std::max(total.max_iterations, s.max_iterations);
    total.max_exit_step = std::max(total.max_exit_step, s.max_exit_step);
    total.max_accepted_u = std::max(total.max_accepted_u, s.max_accepted_u);
    total.max_warm_start_gap = std::max(total.max_warm_start_gap, s.max_warm_start_gap);
  };

  {  // Swept X range with warm starts at two step sizes.
    // X is a state, not a clamp: it is swept over [-0.5, 1.5] and the range is recorded as swept.
    for (int z = 1; z <= 18; ++z) {
      const float zeta = 0.05f * static_cast<float>(z);
      for (const float gamma :
           {0.1f, 0.2f, 0.3f, 0.34f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 0.99f}) {
        const float pc = kMouth / gamma;
        PassiveCoupling c;
        c.configure(pc, zeta);
        const float steady = PassiveCoupling::opening(gamma, kReed) * std::sqrt(kMouth);
        const double t_lo = PassiveCoupling::map(-std::sqrt(0.5f), zeta, kReed);
        const double t_hi = PassiveCoupling::map(std::sqrt(1.5f), zeta, kReed);
        for (const int period : {200, 20}) {
          for (int n = 0; n < 2000; ++n) {
            const double target =
                0.5 * (t_lo + t_hi) + 0.5 * (t_hi - t_lo) * std::sin(kTwoPiD * n / period);
            // Invert target = (mouth - 2 incoming) / pc + zeta * steady / sqrt(pc) for the incoming
            // wave.
            const double incoming = 0.5 * (kMouth - pc * (target - zeta * steady / std::sqrt(pc)));
            worst_error = std::max(worst_error, solve_error(c, kMouth, static_cast<float>(incoming),
                                                            steady, kReed, pc));
          }
          if (period == 200)
            smooth_iterations = std::max(smooth_iterations, c.stats().max_iterations);
        }
        accumulate(c.stats());
      }
    }
  }

  {  // Crossing X = 1, where the slope jumps from 2 - 2 zeta to 2.
    // With no returning wave the fixed point is X = gamma, so gamma 0.9 -> 1.1 -> 0.9 walks it
    // across the kink.
    for (int z = 1; z <= 18; ++z) {
      const float zeta = 0.05f * static_cast<float>(z);
      constexpr float kPc = 1.0f;
      PassiveCoupling c;
      c.configure(kPc, zeta);
      for (int n = 0; n <= 4000; ++n) {
        const float gamma = 0.9f + 0.2f * (n <= 2000 ? n / 2000.0f : (4000 - n) / 2000.0f);
        const float steady = PassiveCoupling::opening(gamma, kReed) * std::sqrt(gamma);
        const auto j = c.solve(gamma, 0.0f, steady, kReed);
        REQUIRE(std::fabs(j.drop - gamma) <= 4.0f * PassiveCoupling::kTol * std::max(1.0f, gamma));
        worst_error = std::max(worst_error, solve_error(c, gamma, 0.0f, steady, kReed, kPc));
      }
      accumulate(c.stats());
    }
  }

  {  // Crossing X = 0, the low-zeta end where the slope is zeta.
    for (int z = 1; z <= 18; ++z) {
      const float zeta = 0.05f * static_cast<float>(z);
      constexpr float kPc = 1.0f;
      PassiveCoupling c;
      c.configure(kPc, zeta);
      for (int n = 0; n <= 4000; ++n) {
        const float incoming = 0.1f * static_cast<float>(std::sin(kTwoPiD * n / 400.0));
        worst_error = std::max(worst_error, solve_error(c, 0.0f, incoming, 0.0f, kReed, kPc));
      }
      accumulate(c.stats());
    }
  }

  {  // A fixed opening, shut included, is solved exactly.
    for (const float alpha : {0.0f, 0.05f, 0.5f, 2.0f}) {
      PassiveCoupling c;
      c.configure(1.0f, 0.3f);
      for (int n = 0; n < 400; ++n) {
        const float incoming = 0.5f * static_cast<float>(std::sin(kTwoPiD * n / 40.0));
        worst_error =
            std::max(worst_error, solve_error(c, 0.8f, incoming, 0.0f, {alpha, 0.0f}, 1.0f));
      }
      REQUIRE(c.stats().iterations == 0);
    }
  }

  INFO("solves " << total.solves << ", max iterations " << total.max_iterations
                 << ", max exit |du| " << total.max_exit_step << ", max accepted |u| "
                 << total.max_accepted_u << ", max warm-start gap " << total.max_warm_start_gap
                 << ", worst error " << worst_error << " x tolerance");
  REQUIRE(total.cap_hits == 0);
  REQUIRE(worst_error <= 4.0);
  WARN("passive coupling domain sweep: solves "
       << total.solves << ", max iterations " << total.max_iterations << " of "
       << PassiveCoupling::kNewtonCap << " (" << smooth_iterations
       << " on the 200-sample trajectories)"
       << ", max exit |du| " << total.max_exit_step << ", max accepted |u| " << total.max_accepted_u
       << ", max warm-start gap " << total.max_warm_start_gap << ", worst error " << worst_error
       << " x kTol. NOT measured here, since it needs a sounding reed: signed min/max X, the "
          "share below zeta^2/4 and max incoming/mouth.");
}
