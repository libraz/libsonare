/// @file string_loop_test.cpp
/// @brief The shared string loop (midi/synth/string_loop): the loss filter
///        solved against decay targets at two named frequencies, and the loop's
///        own stability and tuning.
///
/// The solver is what lets a decay target mean the same thing at every pitch, so
/// the assertions here are on the filter's response rather than on how it
/// sounds: the fundamental must keep exactly the per-traversal gain it was
/// asked for, the reference partial must land on its own target, and no solved
/// filter may reach a gain the delay line would grow on.

#include "midi/synth/string_loop.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

namespace {

using sonare::midi::synth::solve_string_loop_filter;
using sonare::midi::synth::string_loop_gain_for;
using sonare::midi::synth::StringLoop;
using sonare::midi::synth::StringLoopFilter;

constexpr double kSr = 48000.0;

/// The one-pole's magnitude response, gain included: |g (1-a) / (1 - a z^-1)|.
double response(const StringLoopFilter& f, double omega) {
  const double a = f.a;
  return f.g * (1.0 - a) / std::sqrt(1.0 - 2.0 * a * std::cos(omega) + a * a);
}

double note_hz(int note) { return 440.0 * std::pow(2.0, (note - 69) / 12.0); }

}  // namespace

TEST_CASE("solved loss filter gives the fundamental exactly the decay it was asked for",
          "[midi][synth][string_loop]") {
  // Across the compass, because the whole point of solving rather than picking a
  // coefficient is that the answer holds at every pitch. A pole chosen for tone
  // attenuates a treble fundamental on every traversal, and at f''' that
  // uncompensated loss is some 62 dB/s — far more than the decay asked for.
  for (int note : {29, 40, 52, 60, 72, 84, 89}) {
    const double f0 = note_hz(note);
    const double period = kSr / f0;
    const float t60 = 11.6f * std::exp2(0.40f * (69.0f - static_cast<float>(note)) / 12.0f);
    const float g0 = string_loop_gain_for(static_cast<float>(period), kSr, t60);
    const float g_ref = string_loop_gain_for(static_cast<float>(period), kSr, t60 * 0.45f);
    const double omega0 = 2.0 * M_PI / period;
    const double omega_ref = 2.0 * M_PI * std::max(2.0 * f0, 2000.0) / kSr;

    const StringLoopFilter f = solve_string_loop_filter(static_cast<float>(omega0),
                                                        static_cast<float>(omega_ref), g0, g_ref);

    INFO("note " << note << " a=" << f.a << " g=" << f.g);
    // The fundamental keeps its target gain to within float rounding, which over
    // the thousands of traversals in a second is the difference between the
    // requested decay and an arbitrary one.
    REQUIRE(response(f, omega0) == Catch::Approx(static_cast<double>(g0)).epsilon(1e-5));
    // The pole is a lowpass, so the loop's largest response is at DC; it must
    // stay under one or the delay line grows without bound.
    REQUIRE(f.a >= 0.0f);
    REQUIRE(f.g < 1.0f);
    // And it really does tilt: the reference partial decays faster.
    REQUIRE(response(f, omega_ref) < response(f, omega0));
  }
}

TEST_CASE("solved loss filter hits the reference partial's target where one pole can reach it",
          "[midi][synth][string_loop]") {
  // A single pole can only tilt so far, so this asks for a ratio well inside
  // what it can supply and checks the answer is the requested one rather than
  // merely in the right direction.
  const double period = kSr / note_hz(60);
  const float g0 = string_loop_gain_for(static_cast<float>(period), kSr, 8.0f);
  const float g_ref = string_loop_gain_for(static_cast<float>(period), kSr, 8.0f * 0.45f);
  const double omega0 = 2.0 * M_PI / period;
  const double omega_ref = 2.0 * M_PI * 2000.0 / kSr;

  const StringLoopFilter f = solve_string_loop_filter(static_cast<float>(omega0),
                                                      static_cast<float>(omega_ref), g0, g_ref);
  REQUIRE(response(f, omega_ref) == Catch::Approx(static_cast<double>(g_ref)).epsilon(1e-4));
}

TEST_CASE("solved loss filter is transparent when the two targets agree",
          "[midi][synth][string_loop]") {
  // hf_damping == 1 asks for a string whose partials all decay alike. The
  // identity has to be exact: a pole that is nearly-but-not-quite transparent
  // would make a sweep of the damping knob start from the wrong place.
  const double period = kSr / note_hz(60);
  const float g0 = string_loop_gain_for(static_cast<float>(period), kSr, 8.0f);
  const StringLoopFilter f =
      solve_string_loop_filter(static_cast<float>(2.0 * M_PI / period),
                               static_cast<float>(2.0 * M_PI * 2000.0 / kSr), g0, g0);
  REQUIRE(f.a == 0.0f);
  REQUIRE(f.g == g0);
}

TEST_CASE("solved loss filter clamps rather than fails on an unreachable tilt",
          "[midi][synth][string_loop]") {
  // Asking a single pole for a tilt beyond sin(w_ref/2)/sin(w0/2) has no
  // solution. It must come back with the most damping it has, not a pole
  // outside the unit circle.
  const double period = kSr / note_hz(29);
  const double omega0 = 2.0 * M_PI / period;
  const double omega_ref = 1.02 * omega0;  // barely above the fundamental
  const StringLoopFilter f = solve_string_loop_filter(static_cast<float>(omega0),
                                                      static_cast<float>(omega_ref), 0.999f, 0.5f);
  REQUIRE(std::abs(f.a) < 1.0f);
  REQUIRE(f.g <= 1.0f);
  REQUIRE(std::isfinite(f.a));
  REQUIRE(std::isfinite(f.g));
}

TEST_CASE("string loop sounds the pitch it was configured for", "[midi][synth][string_loop]") {
  // The loop compensates its filter's phase delay at the fundamental, so the
  // sounding pitch is the requested one rather than a few percent flat.
  for (int note : {40, 60, 84}) {
    const double f0 = note_hz(note);
    const auto period = static_cast<float>(kSr / f0);
    std::vector<float> buffer(4096, 0.0f);

    StringLoop loop;
    loop.configure(buffer.data(), static_cast<int>(buffer.size()), period, kSr, 0.2f, 4.0f, 0.1f);

    // Excite with a single impulse, then find the period by autocorrelation over
    // a window well clear of it. Counting zero crossings would not do: an
    // impulse-excited string is rich in partials and every one of them crosses
    // zero, so the count reports a high harmonic rather than the fundamental.
    const int total = static_cast<int>(kSr);
    std::vector<float> out(static_cast<size_t>(total));
    for (int i = 0; i < total; ++i) {
      const float in = (i == 0 ? 1.0f : 0.0f) + loop.feedback();
      out[static_cast<size_t>(i)] = loop.process(in, 1.0f);
    }
    const auto from = static_cast<size_t>(0.2 * kSr);
    const auto span = static_cast<size_t>(0.3 * kSr);
    const int lo = static_cast<int>(period * 0.6);
    const int hi = static_cast<int>(period * 1.6);
    int best_lag = lo;
    double best = -1.0e30;
    for (int lag = lo; lag <= hi; ++lag) {
      double sum = 0.0;
      for (size_t i = 0; i < span; ++i) {
        sum += static_cast<double>(out[from + i]) * out[from + i + static_cast<size_t>(lag)];
      }
      if (sum > best) {
        best = sum;
        best_lag = lag;
      }
    }
    const double measured = kSr / best_lag;
    INFO("note " << note << " sounds " << measured << " Hz, wanted " << f0);
    REQUIRE(measured == Catch::Approx(f0).epsilon(0.02));
  }
}
