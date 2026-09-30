#include "playback/upmix.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <random>
#include <vector>

#include "util/constants.h"

using sonare::ChannelLayout;
using namespace sonare::playback;

namespace {

constexpr double kRate = 48000.0;
constexpr int kBlock = 512;
constexpr int kMaxBlock = 1024;
constexpr int kSeconds = 2;
constexpr float kAmplitude = 0.25f;
/// Output measured after the 100 ms statistics smoothing has settled.
constexpr double kSettleSeconds = 0.5;

using Planes = std::vector<std::vector<float>>;

std::vector<float> sine(int frames, float gain) {
  std::vector<float> x(static_cast<size_t>(frames));
  for (int i = 0; i < frames; ++i) {
    x[static_cast<size_t>(i)] = gain * std::sin(sonare::constants::kTwoPi * 1000.0f *
                                                static_cast<float>(i) / static_cast<float>(kRate));
  }
  return x;
}

std::vector<float> noise(int frames, unsigned seed) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> dist(-kAmplitude, kAmplitude);
  std::vector<float> x(static_cast<size_t>(frames));
  for (auto& v : x) v = dist(rng);
  return x;
}

std::vector<float> scaled(const std::vector<float>& x, float gain) {
  std::vector<float> y(x);
  for (auto& v : y) v *= gain;
  return y;
}

/// Runs a whole stereo signal through @p upmix in blocks of @p block frames.
Planes run(Upmixer& upmix, const std::vector<float>& l, const std::vector<float>& r, int block) {
  const int total = static_cast<int>(l.size());
  const int planes = sonare::channel_count(upmix.output_layout());
  Planes out(static_cast<size_t>(planes), std::vector<float>(static_cast<size_t>(total)));
  std::array<float*, 8> ptrs{};
  for (int start = 0; start < total; start += block) {
    const int count = std::min(block, total - start);
    for (int ch = 0; ch < planes; ++ch) {
      ptrs[static_cast<size_t>(ch)] = out[static_cast<size_t>(ch)].data() + start;
    }
    upmix.process(l.data() + start, r.data() + start, ptrs.data(), count);
  }
  return out;
}

Planes upmix_signal(const std::vector<float>& l, const std::vector<float>& r,
                    ChannelLayout layout = ChannelLayout::FivePointOne) {
  Upmixer upmix;
  upmix.prepare(kRate, kMaxBlock, layout);
  upmix.set_params(UpmixParams{}, true);
  return run(upmix, l, r, kBlock);
}

double energy(const std::vector<float>& x, int begin, int end) {
  double e = 0.0;
  for (int i = begin; i < end; ++i) {
    e += static_cast<double>(x[static_cast<size_t>(i)]) * x[static_cast<size_t>(i)];
  }
  return e;
}

/// Per-plane output energy after settling, and the input energy of the same
/// span (shifted back by the latency).
struct Energies {
  std::vector<double> plane;
  double input = 0.0;
  double total = 0.0;
  double at(int ch) const { return plane[static_cast<size_t>(ch)]; }
  double front() const { return at(0) + at(1) + at(2); }
  double surround() const { return at(4) + at(5); }
};

Energies measure(const Planes& out, const std::vector<float>& l, const std::vector<float>& r) {
  const int latency = upmix_latency_frames(kRate);
  const int total = static_cast<int>(l.size());
  const int begin = static_cast<int>(kSettleSeconds * kRate);
  Energies e;
  for (const auto& p : out) {
    e.plane.push_back(energy(p, begin, total));
    e.total += e.plane.back();
  }
  e.input =
      energy(l, begin - latency, total - latency) + energy(r, begin - latency, total - latency);
  return e;
}

double db(double ratio) { return 10.0 * std::log10(ratio); }

constexpr int kTotal = static_cast<int>(kRate) * kSeconds;

}  // namespace

TEST_CASE("upmix latency is the STFT length", "[playback][upmix]") {
  CHECK(upmix_latency_frames(48000.0) == 1024);
  CHECK(upmix_latency_frames(44100.0) == 1024);
  CHECK(upmix_latency_frames(96000.0) == 2048);
}

TEST_CASE("upmix bypass is the input delayed by exactly N", "[playback][upmix]") {
  for (double rate : {44100.0, 48000.0, 96000.0}) {
    CAPTURE(rate);
    Upmixer upmix;
    upmix.prepare(rate, kMaxBlock, ChannelLayout::FivePointOne);
    UpmixParams off;
    off.enabled = false;
    upmix.set_params(off, true);
    const int n = upmix_latency_frames(rate);
    CHECK(upmix.latency_samples() == n);
    const int total = 4 * n + 333;
    const auto l = noise(total, 1u);
    const auto r = noise(total, 2u);
    const Planes out = run(upmix, l, r, 300);
    bool exact = true;
    for (int i = 0; i < total; ++i) {
      const auto is = static_cast<size_t>(i);
      const float want_l = i >= n ? l[static_cast<size_t>(i - n)] : 0.0f;
      const float want_r = i >= n ? r[static_cast<size_t>(i - n)] : 0.0f;
      exact = exact && out[0][is] == want_l && out[1][is] == want_r;
      for (int ch = 2; ch < 6; ++ch) exact = exact && out[static_cast<size_t>(ch)][is] == 0.0f;
    }
    CHECK(exact);
  }
}

TEST_CASE("upmix STFT is an identity from the first sample", "[playback][upmix]") {
  // L = R is fully coherent with a zero panning index, so the whole input goes
  // to C as (L + R) / sqrt(2) = sqrt(2) * x, delayed by N, from the first
  // frame after reset (zero-history start).
  const int n = upmix_latency_frames(kRate);
  const auto x = noise(8 * n, 3u);
  const Planes out = upmix_signal(x, x);
  float max_err = 0.0f;
  float max_other = 0.0f;
  for (size_t i = 0; i < x.size(); ++i) {
    const float want = i >= static_cast<size_t>(n)
                           ? sonare::constants::kSqrt2 * x[i - static_cast<size_t>(n)]
                           : 0.0f;
    max_err = std::max(max_err, std::abs(out[2][i] - want));
    for (int ch : {0, 1, 3, 4, 5}) {
      max_other = std::max(max_other, std::abs(out[static_cast<size_t>(ch)][i]));
    }
  }
  CHECK(max_err < 1e-5f);
  CHECK(max_other < 1e-5f);
}

TEST_CASE("upmix sends an L=R sine to the centre", "[playback][upmix]") {
  const auto x = sine(kTotal, kAmplitude);
  const Energies e = measure(upmix_signal(x, x), x, x);
  CHECK(e.at(2) / e.front() >= 0.95);
  CHECK(std::abs(db(e.total / e.input)) <= 0.5);
}

TEST_CASE("upmix keeps a +6 dB ICLD sine at least as strong in L as in C", "[playback][upmix]") {
  const auto l = sine(kTotal, kAmplitude);
  const auto r = scaled(l, std::pow(10.0f, -6.0f / 20.0f));
  const Energies e = measure(upmix_signal(l, r), l, r);
  CHECK(e.at(0) >= e.at(2));
  CHECK(std::abs(db(e.total / e.input)) <= 0.5);
}

TEST_CASE("upmix keeps a hard-left sine in L", "[playback][upmix]") {
  const auto l = sine(kTotal, kAmplitude);
  const std::vector<float> r(l.size(), 0.0f);
  const Energies e = measure(upmix_signal(l, r), l, r);
  CHECK(e.at(0) / e.input >= 0.9);
  CHECK(db(e.at(2) / e.input) <= -20.0);
  CHECK(std::abs(db(e.total / e.input)) <= 0.5);
}

TEST_CASE("upmix sends anti-phase L=-R content to the surrounds", "[playback][upmix]") {
  SECTION("sine") {
    const auto l = sine(kTotal, kAmplitude);
    const auto r = scaled(l, -1.0f);
    const Energies e = measure(upmix_signal(l, r), l, r);
    CHECK(e.surround() / e.total >= 0.8);
    CHECK(std::abs(db(e.total / e.input)) <= 0.5);
  }
  SECTION("noise") {
    const auto l = noise(kTotal, 4u);
    const auto r = scaled(l, -1.0f);
    const Energies e = measure(upmix_signal(l, r), l, r);
    CHECK(e.surround() / e.total >= 0.8);
    CHECK(std::abs(db(e.total / e.input)) <= 0.5);
  }
}

TEST_CASE("upmix sends uncorrelated noise to the surrounds", "[playback][upmix]") {
  const auto l = noise(kTotal, 5u);
  const auto r = noise(kTotal, 6u);
  const Energies e = measure(upmix_signal(l, r), l, r);
  CHECK(db(e.surround() / e.front()) >= -6.0);
  CHECK(std::abs(db(e.total / e.input)) <= 0.5);
}

TEST_CASE("upmix 7.1 splits the rear ambience between Ls/Rs and Lss/Rss", "[playback][upmix]") {
  const auto l = noise(kTotal, 7u);
  const auto r = noise(kTotal, 8u);
  const Planes five = upmix_signal(l, r, ChannelLayout::FivePointOne);
  const Planes seven = upmix_signal(l, r, ChannelLayout::SevenPointOne);
  REQUIRE(seven.size() == 8);
  for (size_t ch : {0u, 1u, 2u, 3u}) CHECK(seven[ch] == five[ch]);
  const Energies e5 = measure(five, l, r);
  const Energies e7 = measure(seven, l, r);
  const double side = e7.at(6) + e7.at(7);
  const double rear7 = e7.surround() + side;
  // The 5.1 surround energy is redistributed over four planes.
  CHECK(std::abs(db(rear7 / e5.surround())) <= 0.2);
  // Ambience dominates uncorrelated noise, and half of it goes to the sides.
  CHECK(std::abs(db(side / (0.5 * rear7))) <= 1.0);
  CHECK(std::abs(db(e7.at(6) / e7.at(7))) <= 0.5);

  SECTION("anti-phase content stays on Ls/Rs") {
    const auto a = sine(kTotal, kAmplitude);
    const auto b = scaled(a, -1.0f);
    const Planes out = upmix_signal(a, b, ChannelLayout::SevenPointOne);
    const Energies ea = measure(out, a, b);
    CHECK(ea.at(6) + ea.at(7) == 0.0);
    CHECK(ea.surround() / ea.total >= 0.8);
  }
}

TEST_CASE("upmix on/off cross-fade ends at exact weights", "[playback][upmix]") {
  const auto l = noise(kTotal, 9u);
  const auto r = scaled(noise(kTotal, 10u), 0.5f);
  const int n = upmix_latency_frames(kRate);
  const int ramp = static_cast<int>(std::lround(0.02 * kRate));

  Upmixer on;
  on.prepare(kRate, kMaxBlock, ChannelLayout::FivePointOne);
  on.set_params(UpmixParams{}, true);
  const Planes ref = run(on, l, r, kBlock);

  Upmixer toggled;
  toggled.prepare(kRate, kMaxBlock, ChannelLayout::FivePointOne);
  toggled.set_params(UpmixParams{}, true);
  const int off_at = 40 * kBlock;
  const int on_at = 80 * kBlock;
  Planes out(6, std::vector<float>(static_cast<size_t>(kTotal)));
  std::array<float*, 6> ptrs{};
  for (int start = 0; start < kTotal; start += kBlock) {
    const int count = std::min(kBlock, kTotal - start);
    UpmixParams p;
    p.enabled = start < off_at || start >= on_at;
    toggled.set_params(p, false);
    for (size_t ch = 0; ch < 6; ++ch) ptrs[ch] = out[ch].data() + start;
    toggled.process(l.data() + start, r.data() + start, ptrs.data(), count);
  }

  bool off_exact = true;
  for (int i = off_at + ramp; i < on_at; ++i) {
    const auto is = static_cast<size_t>(i);
    off_exact = off_exact && out[0][is] == l[is - static_cast<size_t>(n)] &&
                out[1][is] == r[is - static_cast<size_t>(n)];
    for (size_t ch = 2; ch < 6; ++ch) off_exact = off_exact && out[ch][is] == 0.0f;
  }
  CHECK(off_exact);
  // Mid-ramp the output is neither end.
  const auto mid = static_cast<size_t>(off_at + ramp / 2);
  CHECK(out[4][mid] != 0.0f);
  CHECK(out[4][mid] != ref[4][mid]);

  bool on_exact = true;
  for (int i = on_at + ramp; i < kTotal; ++i) {
    for (size_t ch = 0; ch < 6; ++ch) {
      on_exact = on_exact && out[ch][static_cast<size_t>(i)] == ref[ch][static_cast<size_t>(i)];
    }
  }
  CHECK(on_exact);
  bool before_exact = true;
  for (int i = 0; i < off_at; ++i) {
    for (size_t ch = 0; ch < 6; ++ch) {
      before_exact =
          before_exact && out[ch][static_cast<size_t>(i)] == ref[ch][static_cast<size_t>(i)];
    }
  }
  CHECK(before_exact);
}

TEST_CASE("upmix output does not depend on the host block size", "[playback][upmix]") {
  const int total = static_cast<int>(kRate);
  const auto l = noise(total, 11u);
  auto r = noise(total, 12u);
  for (size_t i = 0; i < r.size(); ++i) r[i] = 0.5f * (r[i] + l[i]);
  UpmixParams p;
  p.lfe_from_upmix = true;
  std::vector<Planes> outs;
  for (int block : {1, 128, 1000}) {
    Upmixer upmix;
    upmix.prepare(kRate, kMaxBlock, ChannelLayout::SevenPointOne);
    upmix.set_params(p, true);
    outs.push_back(run(upmix, l, r, block));
  }
  CHECK(outs[0] == outs[1]);
  CHECK(outs[0] == outs[2]);
}

TEST_CASE("upmix tail decays below -60 dB within latency, N and decay_frames",
          "[playback][upmix]") {
  Upmixer upmix;
  upmix.prepare(kRate, kMaxBlock, ChannelLayout::FivePointOne);
  UpmixParams p;
  p.lfe_from_upmix = true;
  upmix.set_params(p, true);
  const int n = upmix.latency_samples();
  const int decay = upmix.decay_frames();
  // Four 0.5-gain all-pass stages plus the 10 ms delay: about 185 ms at 48 kHz.
  CHECK(decay > static_cast<int>(0.15 * kRate));
  CHECK(decay < static_cast<int>(0.25 * kRate));

  const int active = kTotal;
  const int total = active + 2 * n + decay;
  auto l = noise(total, 13u);
  auto r = noise(total, 14u);
  std::fill(l.begin() + active, l.end(), 0.0f);
  std::fill(r.begin() + active, r.end(), 0.0f);
  const Planes out = run(upmix, l, r, kBlock);
  const int window = static_cast<int>(0.005 * kRate);
  for (size_t ch = 0; ch < out.size(); ++ch) {
    CAPTURE(ch);
    const double steady = energy(out[ch], active - window, active);
    const double tail = energy(out[ch], total - window, total);
    CHECK(tail <= steady * 1e-6);
  }
}
