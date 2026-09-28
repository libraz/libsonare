#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include "metering/bs1770_weighting.h"
#include "playback/night_mode_drc.h"
#include "rt/biquad_design.h"

using sonare::ChannelLayout;
using namespace sonare::playback;

namespace {

// Seeded pink noise via Paul Kellet's refined filter, normalized to 0.1 RMS
// before the caller rescales it to a target loudness.
std::vector<float> pink_noise(size_t frames, uint32_t seed) {
  std::mt19937 rng(seed);
  std::normal_distribution<float> white(0.0f, 1.0f);
  float b[7] = {};
  std::vector<float> out(frames);
  double power = 0.0;
  for (float& s : out) {
    const float w = white(rng);
    b[0] = 0.99886f * b[0] + w * 0.0555179f;
    b[1] = 0.99332f * b[1] + w * 0.0750759f;
    b[2] = 0.96900f * b[2] + w * 0.1538520f;
    b[3] = 0.86650f * b[3] + w * 0.3104856f;
    b[4] = 0.55000f * b[4] + w * 0.5329522f;
    b[5] = -0.7616f * b[5] - w * 0.0168980f;
    s = b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + w * 0.5362f;
    b[6] = w * 0.115926f;
    power += static_cast<double>(s) * s;
  }
  const double rms = std::sqrt(power / static_cast<double>(frames));
  const float gain = static_cast<float>(0.1 / rms);
  for (float& s : out) s *= gain;
  return out;
}

// BS.1770 K-weighted loudness of `planes[*][start, start+count)`, built from
// the same filter design and channel weights NightModeDrc's own detector uses
// (rt::k_weighting_coefficients + metering::bs1770_channel_weight), but
// as a single windowed mean with no ITU gating -- matching what the detector
// under test actually computes rather than the gated integrated meter.
float k_weighted_lufs(const std::vector<std::vector<float>>& planes, size_t start, size_t count,
                      double sample_rate) {
  const int channels = static_cast<int>(planes.size());
  const auto coeffs = sonare::rt::k_weighting_coefficients(sample_rate);
  const sonare::rt::BiquadCoeffsD pre{coeffs.pre.b0, coeffs.pre.b1, coeffs.pre.b2, coeffs.pre.a1,
                                      coeffs.pre.a2};
  const sonare::rt::BiquadCoeffsD rlb{coeffs.rlb.b0, coeffs.rlb.b1, coeffs.rlb.b2, coeffs.rlb.a1,
                                      coeffs.rlb.a2};
  std::vector<sonare::rt::BiquadStateD> pre_state(static_cast<size_t>(channels));
  std::vector<sonare::rt::BiquadStateD> rlb_state(static_cast<size_t>(channels));
  for (auto& s : pre_state) s.set(pre);
  for (auto& s : rlb_state) s.set(rlb);

  double sum = 0.0;
  for (size_t i = 0; i < count; ++i) {
    double power = 0.0;
    for (int c = 0; c < channels; ++c) {
      const double y0 = pre_state[static_cast<size_t>(c)].process(
          static_cast<double>(planes[static_cast<size_t>(c)][start + i]));
      const double y = rlb_state[static_cast<size_t>(c)].process(y0);
      power += sonare::metering::bs1770_channel_weight(c, channels) * y * y;
    }
    sum += power;
  }
  const double mean_power = sum / static_cast<double>(count);
  return static_cast<float>(sonare::rt::kLoudnessOffset +
                            10.0 * std::log10(std::max(mean_power, 1e-15)));
}

// Scales `mono` (duplicated onto both stereo planes, as every test here feeds
// it) so its measured level equals `target_level_db`.
void calibrate_stereo_level(std::vector<float>& mono, float target_level_db, double sample_rate) {
  const std::vector<std::vector<float>> planes{mono, mono};
  const float current = k_weighted_lufs(planes, 0, mono.size(), sample_rate);
  const float gain = std::pow(10.0f, (target_level_db - current) / 20.0f);
  for (float& s : mono) s *= gain;
}

}  // namespace

TEST_CASE("night-mode static curve matches its analytic values", "[playback][night]") {
  CHECK(std::abs(night_mode_curve_db(15.0f, 1.0f) - 6.0f) < 0.05f);
  CHECK(std::abs(night_mode_curve_db(-25.0f, 1.0f) - (-18.5f)) < 0.05f);
  CHECK(std::abs(night_mode_curve_db(-50.0f, 1.0f) - (-50.0f)) < 0.05f);
}

TEST_CASE("night mode at amount 0 leaves the level unchanged", "[playback][night]") {
  constexpr double kRate = 48000.0;
  constexpr int kBlock = 480;
  NightModeDrc drc;
  drc.prepare(kRate, kBlock, ChannelLayout::Stereo);
  drc.set_target_lufs(-24.0f);
  drc.set_amount(0.0f);

  std::mt19937 rng(7u);
  std::normal_distribution<float> dist(0.0f, 0.1f);
  double e_in = 0.0, e_out = 0.0;
  std::vector<float> l(kBlock), r(kBlock);
  float* planes[2] = {l.data(), r.data()};
  for (int block = 0; block < 200; ++block) {
    for (int i = 0; i < kBlock; ++i) {
      l[static_cast<size_t>(i)] = dist(rng);
      r[static_cast<size_t>(i)] = dist(rng);
      if (block >= 10) {
        e_in += static_cast<double>(l[static_cast<size_t>(i)]) * l[static_cast<size_t>(i)] +
                static_cast<double>(r[static_cast<size_t>(i)]) * r[static_cast<size_t>(i)];
      }
    }
    drc.process(planes, kBlock);
    if (block < 10) continue;
    for (int i = 0; i < kBlock; ++i) {
      e_out += static_cast<double>(l[static_cast<size_t>(i)]) * l[static_cast<size_t>(i)] +
               static_cast<double>(r[static_cast<size_t>(i)]) * r[static_cast<size_t>(i)];
    }
  }
  REQUIRE(e_out > 0.0);
  CHECK(std::abs(10.0 * std::log10(e_out / e_in)) <= 0.2);
}

TEST_CASE("night mode DRC matches the static curve end to end", "[playback][night]") {
  constexpr double kRate = 48000.0;
  constexpr int kBlock = 480;  // 10 ms
  constexpr float kTarget = -24.0f;
  constexpr size_t kSegmentFrames = static_cast<size_t>(kRate) * 4;  // 4 s
  constexpr size_t kSettleFrames = static_cast<size_t>(kRate) * 1;   // skip the first 1 s

  struct Segment {
    float x_db;           // detected level relative to target
    float expected_y_db;  // expected output level relative to target
    float tolerance_db;
  };
  // Alternating +15 / -25 dB segments, plus a segment at
  // the -40 dB no-boost floor's far side to check it is left unlifted.
  const Segment segments[] = {
      {15.0f, 6.0f, 1.0f},
      {-25.0f, -18.5f, 1.0f},
      {-45.0f, -45.0f, 0.2f},
  };

  NightModeDrc drc;
  drc.prepare(kRate, kBlock, ChannelLayout::Stereo);
  drc.set_target_lufs(kTarget);
  drc.set_amount(1.0f);

  std::vector<float> l(static_cast<size_t>(kBlock)), r(static_cast<size_t>(kBlock));
  float* planes[2] = {l.data(), r.data()};

  uint32_t seed = 21u;
  for (const Segment& seg : segments) {
    std::vector<float> noise = pink_noise(kSegmentFrames, seed++);
    calibrate_stereo_level(noise, kTarget + seg.x_db, kRate);

    std::vector<float> out_l(kSegmentFrames), out_r(kSegmentFrames);
    for (size_t start = 0; start < kSegmentFrames; start += static_cast<size_t>(kBlock)) {
      const size_t count = std::min(static_cast<size_t>(kBlock), kSegmentFrames - start);
      std::copy_n(noise.begin() + static_cast<std::ptrdiff_t>(start), count, l.begin());
      std::copy_n(noise.begin() + static_cast<std::ptrdiff_t>(start), count, r.begin());
      drc.process(planes, static_cast<int>(count));
      std::copy_n(l.begin(), count, out_l.begin() + static_cast<std::ptrdiff_t>(start));
      std::copy_n(r.begin(), count, out_r.begin() + static_cast<std::ptrdiff_t>(start));
    }

    // Identical gain on all planes: L and R carry the same content, so a
    // shared gain leaves them bit-identical.
    CHECK(std::equal(out_l.begin() + static_cast<std::ptrdiff_t>(kSettleFrames), out_l.end(),
                     out_r.begin() + static_cast<std::ptrdiff_t>(kSettleFrames)));

    const std::vector<std::vector<float>> out_planes{out_l, out_r};
    const float measured =
        k_weighted_lufs(out_planes, kSettleFrames, kSegmentFrames - kSettleFrames, kRate);
    CHECK(std::abs((measured - kTarget) - seg.expected_y_db) < seg.tolerance_db);
  }
}

TEST_CASE("night-mode handover carries detector state and gain across front ends",
          "[playback][night]") {
  constexpr double kRate = 48000.0;
  constexpr int kBlock = 480;  // 10 ms
  constexpr float kTarget = -24.0f;
  constexpr size_t kSettleFrames = static_cast<size_t>(kRate) * 2;        // 2 s to settle
  constexpr size_t kContinueFrames = static_cast<size_t>(kRate * 0.050);  // 50 ms

  NightModeDrc source;
  source.prepare(kRate, kBlock, ChannelLayout::Stereo);
  source.set_target_lufs(kTarget);
  source.set_amount(1.0f);

  std::vector<float> noise =
      pink_noise(kSettleFrames + kContinueFrames + static_cast<size_t>(kBlock), 33u);
  calibrate_stereo_level(noise, kTarget + 15.0f, kRate);

  std::vector<float> l(static_cast<size_t>(kBlock)), r(static_cast<size_t>(kBlock));
  float* planes[2] = {l.data(), r.data()};

  size_t pos = 0;
  for (; pos < kSettleFrames; pos += static_cast<size_t>(kBlock)) {
    std::copy_n(noise.begin() + static_cast<std::ptrdiff_t>(pos), static_cast<size_t>(kBlock),
                l.begin());
    std::copy_n(noise.begin() + static_cast<std::ptrdiff_t>(pos), static_cast<size_t>(kBlock),
                r.begin());
    source.process(planes, kBlock);
  }

  const NightModeDrcHandover state = source.handover();

  NightModeDrc target;
  target.prepare(kRate, kBlock, ChannelLayout::Stereo);
  target.set_target_lufs(kTarget);
  target.set_amount(1.0f);
  target.accept_handover(state);
  CHECK(std::abs(target.current_gain_db() - state.gain_db) < 0.01f);

  float max_deviation = 0.0f;
  for (size_t done = 0; done < kContinueFrames;
       done += static_cast<size_t>(kBlock), pos += static_cast<size_t>(kBlock)) {
    std::copy_n(noise.begin() + static_cast<std::ptrdiff_t>(pos), static_cast<size_t>(kBlock),
                l.begin());
    std::copy_n(noise.begin() + static_cast<std::ptrdiff_t>(pos), static_cast<size_t>(kBlock),
                r.begin());
    target.process(planes, kBlock);
    max_deviation = std::max(max_deviation, std::abs(target.current_gain_db() - state.gain_db));
  }
  CHECK(max_deviation < 0.5f);
}

TEST_CASE("night-mode look-ahead latency is 5 ms rounded to samples", "[playback][night]") {
  NightModeDrc drc44;
  drc44.prepare(44100.0, 512, ChannelLayout::Stereo);
  CHECK(drc44.latency_samples() == 221);  // round(0.005 * 44100) = round(220.5) = 221

  NightModeDrc drc48;
  drc48.prepare(48000.0, 512, ChannelLayout::Stereo);
  CHECK(drc48.latency_samples() == 240);  // 0.005 * 48000 = 240 exactly
}
