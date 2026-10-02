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

#include <algorithm>
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

TEST_CASE("configured string loop kill is immediate and restart is exact",
          "[midi][synth][string_loop]") {
  constexpr int kCapacity = 512;
  constexpr float kPeriod = 64.0f;
  constexpr int kWarmup = 1024;
  constexpr int kRestartFrames = 384;

  for (const bool two_pole : {false, true}) {
    for (const float ratio : {0.5f, 1.0f, 2.0f}) {
      auto configure = [&](StringLoop& loop, float* slab) {
        if (!two_pole) {
          loop.configure(slab, kCapacity, kPeriod, kSr, 0.18f, 2.0f, 0.25f);
        } else {
          loop.configure_filter(slab, kCapacity, kPeriod, 0.18f, 0.98f, 0.94f, true, 0.25f, 0.995f);
        }
      };

      std::vector<float> slab(static_cast<size_t>(kCapacity), 0.0f);
      StringLoop loop;
      configure(loop, slab.data());

      float prekill_peak = 0.0f;
      float feedback_peak = 0.0f;
      for (int i = 0; i < kWarmup; ++i) {
        const float input = (i == 0 ? 1.0f : 0.0f) + loop.feedback();
        feedback_peak = std::max(feedback_peak, std::fabs(loop.feedback()));
        const float output = loop.advance(input, ratio);
        loop.commit(output);
        prekill_peak = std::max(prekill_peak, std::fabs(output));
      }

      INFO("two pole=" << two_pole << ", ratio=" << ratio << ", prekill peak=" << prekill_peak
                       << ", feedback peak=" << feedback_peak);
      CHECK(prekill_peak > 1.0e-4f);
      CHECK(feedback_peak > 1.0e-6f);

      // kill() must silence both the fused process() API and the split
      // advance()/commit() API, even when callers continue sending input.
      const std::vector<float> slab_before_kill = slab;
      loop.kill();
      loop.kill();
      bool process_silent = true;
      bool split_silent = true;
      bool feedback_zero = true;
      for (int i = 0; i < 2 * kCapacity; ++i) {
        const float process_output = loop.process(0.37f, ratio);
        process_silent = process_silent && process_output == 0.0f;
        feedback_zero = feedback_zero && loop.feedback() == 0.0f;

        loop.commit(0.61f);
        feedback_zero = feedback_zero && loop.lp_state == 0.0f && loop.lp_state2 == 0.0f;

        const float split_output = loop.advance(0.19f, ratio);
        loop.commit(split_output);
        split_silent = split_silent && split_output == 0.0f;
        feedback_zero = feedback_zero && loop.feedback() == 0.0f;
      }
      CHECK(process_silent);
      CHECK(split_silent);
      CHECK(feedback_zero);
      const bool slab_unchanged_after_kill = slab == slab_before_kill;
      CHECK(slab_unchanged_after_kill);

      // Reconfiguring the killed object must clear its lifecycle state as well
      // as its slab, so its impulse response is exactly a fresh loop's response.
      configure(loop, slab.data());
      std::vector<float> fresh_slab(static_cast<size_t>(kCapacity), 0.0f);
      StringLoop fresh;
      configure(fresh, fresh_slab.data());
      std::vector<float> restarted(static_cast<size_t>(kRestartFrames), 0.0f);
      std::vector<float> fresh_output(static_cast<size_t>(kRestartFrames), 0.0f);
      float restart_peak = 0.0f;
      for (int i = 0; i < kRestartFrames; ++i) {
        const float restart_input = (i == 0 ? 1.0f : 0.0f) + loop.feedback();
        const float fresh_input = (i == 0 ? 1.0f : 0.0f) + fresh.feedback();
        restarted[static_cast<size_t>(i)] = loop.process(restart_input, ratio);
        fresh_output[static_cast<size_t>(i)] = fresh.process(fresh_input, ratio);
        restart_peak = std::max(restart_peak, std::fabs(restarted[static_cast<size_t>(i)]));
      }
      CHECK(restart_peak > 1.0e-4f);
      const bool restart_matches_fresh = restarted == fresh_output;
      CHECK(restart_matches_fresh);
    }
  }
}

TEST_CASE("disabled string loop is inert without clearing its slab and can restart",
          "[midi][synth][string_loop]") {
  constexpr int kCapacity = 512;
  constexpr float kPeriod = 64.0f;
  constexpr int kWarmup = 1024;

  std::vector<float> slab(static_cast<size_t>(kCapacity), 0.0f);
  StringLoop loop;
  loop.configure(slab.data(), kCapacity, kPeriod, kSr, 0.18f, 2.0f, 0.25f);

  float pre_disable_peak = 0.0f;
  for (int i = 0; i < kWarmup; ++i) {
    const float input = (i == 0 ? 1.0f : 0.0f) + loop.feedback();
    pre_disable_peak = std::max(pre_disable_peak, std::fabs(loop.process(input, 1.0f)));
  }
  CHECK(pre_disable_peak > 1.0e-4f);

  const std::vector<float> slab_before_disable = slab;
  loop.disable();
  bool process_silent = true;
  bool split_silent = true;
  bool feedback_zero = true;
  for (int i = 0; i < kCapacity; ++i) {
    process_silent = process_silent && loop.process(0.37f, 1.0f) == 0.0f;
    feedback_zero = feedback_zero && loop.feedback() == 0.0f;
    const float split_output = loop.advance(0.19f, 1.0f);
    loop.commit(0.61f);
    split_silent = split_silent && split_output == 0.0f;
    feedback_zero = feedback_zero && loop.feedback() == 0.0f;
  }
  CHECK(process_silent);
  CHECK(split_silent);
  CHECK(feedback_zero);
  CHECK(loop.lp_state == 0.0f);
  CHECK(loop.lp_state2 == 0.0f);
  const bool slab_unchanged_after_disable = slab == slab_before_disable;
  CHECK(slab_unchanged_after_disable);

  loop.configure(slab.data(), kCapacity, kPeriod, kSr, 0.18f, 2.0f, 0.25f);
  float restart_peak = 0.0f;
  for (int i = 0; i < kWarmup; ++i) {
    const float input = (i == 0 ? 1.0f : 0.0f) + loop.feedback();
    restart_peak = std::max(restart_peak, std::fabs(loop.process(input, 1.0f)));
  }
  CHECK(restart_peak > 1.0e-4f);
}

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

TEST_CASE("an unreachable reference target costs the tilt, never the fundamental's decay",
          "[midi][synth][string_loop]") {
  // The case the two above do not reach: a reference frequency close to the
  // fundamental, or a reference decay far shorter than it, asks for more tilt
  // than one pole has. The answer has to be the darkest pole the compensating
  // gain can still pay for — past that the gain clamps, and the pole's own loss
  // at the fundamental becomes a second decay nothing asked for. It is silent:
  // the filter is still stable, still tuned, and the note is simply gone.
  for (int note : {29, 40, 52, 60, 72, 84, 96, 105}) {
    const double f0 = note_hz(note);
    const double period = kSr / f0;
    const double omega0 = 2.0 * M_PI / period;
    for (float t60 : {0.35f, 3.5f, 12.0f, 55.0f}) {
      const float g0 = string_loop_gain_for(static_cast<float>(period), kSr, t60);
      for (double quote_hz : {1.02 * f0, 1.5 * f0, 4000.0}) {
        for (float hf_t60 : {0.005f, 0.07f, t60 * 0.45f}) {
          const float g_ref = string_loop_gain_for(static_cast<float>(period), kSr, hf_t60);
          const double omega_ref = 2.0 * M_PI * quote_hz / kSr;
          if (omega_ref >= M_PI) continue;
          const StringLoopFilter f = solve_string_loop_filter(
              static_cast<float>(omega0), static_cast<float>(omega_ref), g0, g_ref);

          INFO("note " << note << " t60 " << t60 << " quote " << quote_hz << " hf " << hf_t60
                       << " -> a=" << f.a << " g=" << f.g);
          REQUIRE(std::isfinite(f.a));
          REQUIRE(f.g < 1.0f);
          // The contract, and the whole of the defect: the per-traversal gain the
          // fundamental keeps is the one its own t60 asked for, whatever became of
          // the reference partial's target.
          REQUIRE(response(f, omega0) == Catch::Approx(static_cast<double>(g0)).epsilon(1e-4));
          // And nothing under the fundamental may ring away past it. A lowpass in
          // the loop peaks at DC, so some excess is inherent; unbounded it reached
          // 837 s beneath a 6.8 s note.
          const double traversals = kSr / period;
          const double ring_dc = -6.907755279 / (std::log(static_cast<double>(f.g)) * traversals);
          const double ring_f0 = -6.907755279 / (std::log(response(f, omega0)) * traversals);
          REQUIRE(ring_dc <= 10.0 * ring_f0);
        }
      }
    }
  }
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
