/// @file acoustic_tube_test.cpp
/// @brief The scattering-junction tube (midi/synth/acoustic_tube): resonances of
///        a two-section profile against the closed-open two-tube solution, the
///        same length ringing at the same frequency at two rates, and a length
///        moved every block without allocating, clicking or gaining energy.
///
/// A uniform tube is deliberately not the scattering test: equal areas make
/// every reflection coefficient zero, so a chain with the wrong sign or order of
/// scattering is still a correct delay line. The two-section profile is where a
/// scattering defect moves a resonance.

#include "midi/synth/acoustic_tube.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "rt/open_end_radiation.h"
#include "support/alloc_guard.h"
#include "util/constants.h"

namespace {

using sonare::constants::kPiD;
using sonare::constants::kTwoPiD;
using Tube = sonare::midi::synth::AcousticTube<100>;

constexpr double kSoundSpeed = sonare::constants::kSoundSpeedMps;
constexpr float kTractM = 0.175f;
constexpr double kClickHz = 130.0;

/// Hann-windowed DFT magnitude of @p x at @p hz.
double magnitude(const std::vector<double>& x, double hz, double sr) {
  double re = 0.0;
  double im = 0.0;
  const double n = static_cast<double>(x.size());
  for (std::size_t i = 0; i < x.size(); ++i) {
    const double w = 0.5 - 0.5 * std::cos(kTwoPiD * static_cast<double>(i) / (n - 1.0));
    const double phase = kTwoPiD * hz * static_cast<double>(i) / sr;
    re += w * x[i] * std::cos(phase);
    im -= w * x[i] * std::sin(phase);
  }
  return std::hypot(re, im);
}

/// Strongest peak inside [lo, hi], refined by golden-section search.
double peak_hz(const std::vector<double>& x, double lo, double hi, double sr) {
  constexpr double kGrid = 4.0;
  double best = lo;
  double best_mag = 0.0;
  for (double f = lo; f <= hi; f += kGrid) {
    const double m = magnitude(x, f, sr);
    if (m > best_mag) {
      best_mag = m;
      best = f;
    }
  }
  double a = best - kGrid;
  double b = best + kGrid;
  for (int it = 0; it < 40; ++it) {
    const double c1 = a + (b - a) * 0.382;
    const double c2 = a + (b - a) * 0.618;
    if (magnitude(x, c1, sr) > magnitude(x, c2, sr)) {
      b = c2;
    } else {
      a = c1;
    }
  }
  return 0.5 * (a + b);
}

/// Impulse response of the tube closed at the source (rigid) and open at the
/// mouth (inverting, lossless), with the rear half at area 1 and the front at @p front_area.
std::vector<double> impulse_response(double sr, float front_area) {
  Tube tube;
  tube.configure(sr, 0.05f);
  tube.set_length(kTractM);
  float areas[100];
  for (int i = 0; i < 100; ++i) areas[i] = i < 50 ? 1.0f : front_area;
  tube.set_areas(areas);
  std::vector<double> y;
  float arriving = 0.0f;
  const int n = static_cast<int>(sr / 2);
  for (int i = 0; i < n; ++i) {
    const float input = tube.source_incoming() + (i == 0 ? 1.0f : 0.0f);
    arriving = tube.process(input, -arriving);
    y.push_back(arriving);
  }
  return y;
}

/// First two resonances of the closed-open two-tube, equal halves of @p length_m:
/// tan(k l)^2 = A2 / A1.
void two_tube_resonances(double length_m, double area_ratio, double& f1, double& f2) {
  const double half = 0.5 * length_m;
  const double kl = std::atan(std::sqrt(area_ratio));
  f1 = kl * kSoundSpeed / (kTwoPiD * half);
  f2 = (kPiD - kl) * kSoundSpeed / (kTwoPiD * half);
}

/// Free-decay energy check shared with the coupled test's criterion: 512-sample
/// block means never rise and no sample exceeds the start by more than 1e-6.
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

/// Wave arriving at the mouth of a tube driven at the source by a 130 Hz sine,
/// with the length set per 128-sample block by @p length_at. A sine is the
/// detector: its own second difference is omega^2 times its amplitude, which a
/// wave slipped by one sample exceeds by about 1/omega.
template <class LengthAt>
std::vector<double> sine_render(int blocks, LengthAt length_at) {
  constexpr double kSr = 48000.0;
  Tube tube;
  tube.configure(kSr, 0.5f);
  tube.set_length(length_at(0));
  sonare::rt::OpenEndRadiation mouth;
  mouth.configure(kSr, 4000.0f, 0.0f);
  float reflected = 0.0f;
  std::vector<double> y;
  for (int b = 0; b < blocks; ++b) {
    tube.set_length(length_at(b));
    for (int i = 0; i < 128; ++i) {
      const int n = b * 128 + i;
      const float drive = 0.1f * static_cast<float>(std::sin(kTwoPiD * kClickHz * n / kSr));
      const float arriving = tube.process(tube.source_incoming() + drive, reflected);
      reflected = mouth.process(arriving).reflected;
      y.push_back(arriving);
    }
  }
  return y;
}

/// Largest second difference over omega^2 times the local peak, past the onset.
double curvature_ratio(const std::vector<double>& y) {
  const double omega = kTwoPiD * kClickHz / 48000.0;
  double worst = 0.0;
  for (std::size_t i = 4800; i < y.size(); ++i) {
    double peak = 0.0;
    for (std::size_t k = i - 400; k <= i; ++k) peak = std::max(peak, std::fabs(y[k]));
    worst = std::max(worst, std::fabs(y[i] - 2.0 * y[i - 1] + y[i - 2]) / (omega * omega * peak));
  }
  return worst;
}

float moving_length(int block) {
  return 0.17f + 0.02f * static_cast<float>(std::sin(kTwoPiD * block / 250.0));
}

}  // namespace

TEST_CASE("acoustic tube with a two-section profile rings at the two-tube resonances",
          "[midi][synth][acoustic_tube]") {
  constexpr double kSr = 48000.0;
  constexpr float kFrontArea = 8.0f;
  double f1 = 0.0;
  double f2 = 0.0;
  two_tube_resonances(kTractM, kFrontArea, f1, f2);
  const auto y = impulse_response(kSr, kFrontArea);
  const double m1 = peak_hz(y, 0.8 * f1, 1.2 * f1, kSr);
  const double m2 = peak_hz(y, 0.85 * f2, 1.15 * f2, kSr);
  INFO("measured " << m1 << " / " << m2 << " Hz, two-tube " << f1 << " / " << f2 << " Hz");
  // The step sits inside one 3.6 mm section, which moves the resonances by under 1 %.
  REQUIRE(std::fabs(m1 / f1 - 1.0) < 0.015);
  REQUIRE(std::fabs(m2 / f2 - 1.0) < 0.015);
}

TEST_CASE("acoustic tube rings at the same frequency for the same length at 48 and 44.1 kHz",
          "[midi][synth][acoustic_tube]") {
  const double quarter_wave = kSoundSpeed / (4.0 * kTractM);
  double rate_hz[2][2] = {};
  int r = 0;
  for (const double sr : {48000.0, 44100.0}) {
    const auto y = impulse_response(sr, 1.0f);
    rate_hz[r][0] = peak_hz(y, 0.8 * quarter_wave, 1.2 * quarter_wave, sr);
    rate_hz[r][1] = peak_hz(y, 2.8 * quarter_wave, 3.2 * quarter_wave, sr);
    INFO(sr << " Hz: " << rate_hz[r][0] << " / " << rate_hz[r][1] << ", c/4L " << quarter_wave);
    REQUIRE(std::fabs(rate_hz[r][0] / quarter_wave - 1.0) < 0.001);
    REQUIRE(std::fabs(rate_hz[r][1] / (3.0 * quarter_wave) - 1.0) < 0.001);
    ++r;
  }
  REQUIRE(std::fabs(rate_hz[0][0] / rate_hz[1][0] - 1.0) < 0.0005);
  REQUIRE(std::fabs(rate_hz[0][1] / rate_hz[1][1] - 1.0) < 0.0005);

  // The profile is resampled along the length, so the two-section tract is one tract too.
  double f1 = 0.0;
  double f2 = 0.0;
  two_tube_resonances(kTractM, 8.0, f1, f2);
  const auto y48 = impulse_response(48000.0, 8.0f);
  const auto y44 = impulse_response(44100.0, 8.0f);
  REQUIRE(std::fabs(peak_hz(y48, 0.8 * f1, 1.2 * f1, 48000.0) /
                        peak_hz(y44, 0.8 * f1, 1.2 * f1, 44100.0) -
                    1.0) < 0.005);
}

TEST_CASE("acoustic tube length moves every block without allocating, clicking or gaining energy",
          "[midi][synth][acoustic_tube]") {
  constexpr int kBlocks = 1000;
  SECTION("no allocation") {
    Tube tube;
    tube.configure(48000.0, 0.5f);
    float reflected = 0.0f;
    sonare::test::AllocationGuard guard;
    for (int b = 0; b < kBlocks; ++b) {
      tube.set_length(moving_length(b));
      tube.set_constriction(0.8f, 0.5f);
      for (int i = 0; i < 128; ++i)
        reflected = -0.9f * tube.process(tube.source_incoming() + 0.01f, reflected);
    }
    REQUIRE(guard.count() == 0);
  }

  SECTION("no clicks") {
    // A crossing hands one sample of the wave between the mouth delay line and the
    // sections; an emptied section instead would be a hole the size of the wave.
    const double still = curvature_ratio(sine_render(kBlocks, [](int) { return 0.17f; }));
    const double moving = curvature_ratio(sine_render(kBlocks, moving_length));
    INFO("second difference over omega^2 * peak: fixed " << still << ", moving " << moving);
    REQUIRE(still < 1.1);
    REQUIRE(moving < 30.0);
  }

  SECTION("no energy gained") {
    // Light walls and a fast swing, so the free decay crosses many section boundaries.
    constexpr double kSr = 48000.0;
    Tube tube;
    tube.configure(kSr, 0.05f);
    sonare::rt::OpenEndRadiation mouth;
    mouth.configure(kSr, 4000.0f, 0.0f);
    float reflected = 0.0f;
    std::vector<double> energy;
    constexpr int kDrive = 480;
    int crossings = 0;
    for (int b = 0; b < kBlocks; ++b) {
      const int before = tube.active_sections();
      tube.set_length(0.17f + 0.02f * static_cast<float>(std::sin(kTwoPiD * b / 25.0)));
      if (b > kDrive / 128 && tube.active_sections() != before) ++crossings;
      for (int i = 0; i < 128; ++i) {
        const int n = b * 128 + i;
        const float drive =
            n < kDrive ? 0.1f * static_cast<float>(std::sin(kTwoPiD * 300.0 * n / kSr)) : 0.0f;
        reflected =
            mouth.process(tube.process(tube.source_incoming() + drive, reflected)).reflected;
        // The reflection waits a sample in `reflected` before entering; it is stored energy too.
        if (n >= kDrive) {
          energy.push_back(tube.stored_energy() +
                           tube.mouth_area() * (mouth.stored_energy() + reflected * reflected));
        }
      }
    }
    INFO("section crossings during the decay " << crossings << ", energy start " << energy.front()
                                               << " end " << energy.back());
    REQUIRE(crossings > 100);
    require_passive(energy);
  }
}
