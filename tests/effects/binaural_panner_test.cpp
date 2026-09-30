/// @file binaural_panner_test.cpp
/// @brief stereo.binaural: placement cues from the measured ring, the
///        interpolation between ring points, the speakers canceller and the
///        automatic turn.

#include "mastering/stereo/binaural_panner.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

#include "core/fft.h"
#include "mastering/api/insert_factory.h"
#include "support/audio_fixtures.h"
#include "support/excess_delay.h"
#include "util/constants.h"

namespace {

using sonare::constants::kTwoPiD;
using sonare::mastering::stereo::BinauralOutput;
using sonare::mastering::stereo::BinauralPanner;
using sonare::mastering::stereo::BinauralPannerConfig;
using sonare::test::excess_delay_samples;
using sonare::test::kRate;
using sonare::test::process_stereo;

constexpr std::size_t kIrLength = 4096;
constexpr int kSpectrumSize = 8192;

struct Pair {
  std::vector<float> left;
  std::vector<float> right;
};

/// Impulse response of the panner held at @p config, both inputs fed the impulse.
Pair impulse_response(const BinauralPannerConfig& config, double rate = kRate) {
  BinauralPanner panner(config);
  panner.prepare(rate, static_cast<int>(kIrLength));
  Pair out{std::vector<float>(kIrLength, 0.0f), std::vector<float>(kIrLength, 0.0f)};
  out.left[0] = 1.0f;
  out.right[0] = 1.0f;
  process_stereo(panner, out.left, out.right);
  return out;
}

Pair response_at(float azimuth_deg, BinauralOutput output = BinauralOutput::kPhones) {
  BinauralPannerConfig config;
  config.azimuth_deg = azimuth_deg;
  config.output = output;
  return impulse_response(config);
}

std::vector<std::complex<double>> spectrum(const std::vector<float>& ir) {
  sonare::FFT fft(kSpectrumSize);
  std::vector<float> buffer(kSpectrumSize, 0.0f);
  std::copy(ir.begin(), ir.end(), buffer.begin());
  std::vector<std::complex<float>> out(kSpectrumSize / 2 + 1);
  fft.forward(buffer.data(), out.data());
  return {out.begin(), out.end()};
}

double hz_of(int bin) { return bin * kRate / kSpectrumSize; }

/// Right-over-left level in dB, energy between @p low_hz and @p high_hz.
double ild_db(const Pair& pair, double low_hz, double high_hz) {
  const auto l = spectrum(pair.left);
  const auto r = spectrum(pair.right);
  double el = 0.0;
  double er = 0.0;
  for (int k = 1; k < kSpectrumSize / 2; ++k) {
    if (hz_of(k) < low_hz || hz_of(k) > high_hz) continue;
    el += std::norm(l[static_cast<std::size_t>(k)]);
    er += std::norm(r[static_cast<std::size_t>(k)]);
  }
  return 10.0 * std::log10(er / el);
}

/// Left-minus-right excess delay: positive when the left ear lags.
double itd_samples(const Pair& pair) {
  return excess_delay_samples(pair.left, kRate) - excess_delay_samples(pair.right, kRate);
}

double sum(const std::vector<float>& ir) {
  double total = 0.0;
  for (float v : ir) total += v;
  return total;
}

double distance(const std::vector<float>& a, const std::vector<float>& b) {
  double total = 0.0;
  for (std::size_t i = 0; i < a.size(); ++i) total += (a[i] - b[i]) * (a[i] - b[i]);
  return std::sqrt(total);
}

}  // namespace

TEST_CASE("binaural ITD grows to 90 degrees and ILD grows off the front", "[binaural]") {
  double previous_itd = -1.0;
  double previous_ild = -1.0;
  double ild_30 = 0.0;
  for (int az = 0; az <= 90; az += 15) {
    INFO("azimuth " << az);
    const Pair pair = response_at(static_cast<float>(az));
    const double itd = itd_samples(pair);
    const double ild = ild_db(pair, 1000.0, 8000.0);
    CHECK(itd > previous_itd);
    // The head's bright spot lifts the far ear again near 90 degrees, so the ILD
    // climbs only to 60; at 90 it is still well above the 30-degree value.
    if (az <= 60) CHECK(ild > previous_ild);
    if (az == 30) ild_30 = ild;
    if (az == 90) CHECK(ild > ild_30);
    previous_itd = itd;
    previous_ild = ild;
  }
  // 90 degrees: ITD near 0.8 ms (the ring's 38.2 samples), source on the right.
  const double itd_90 = itd_samples(response_at(90.0f));
  CHECK(itd_90 > 36.0);
  CHECK(itd_90 < 40.0);
}

TEST_CASE("binaural front is left/right symmetric and mirrored azimuths swap the ears",
          "[binaural]") {
  const Pair front = response_at(0.0f);
  CHECK(front.left == front.right);
  CHECK(std::fabs(sum(front.left)) > 0.5);
  for (float az : {30.0f, 72.5f, 135.0f}) {
    INFO("azimuth " << az);
    const Pair right = response_at(az);
    const Pair left = response_at(-az);
    CHECK(right.left == left.right);
    CHECK(right.right == left.left);
  }
}

TEST_CASE("binaural interpolation between ring points is continuous", "[binaural]") {
  // Across a ring point the response moves by as little as the angle does.
  const Pair below = response_at(9.99f);
  const Pair above = response_at(10.01f);
  const Pair at = response_at(10.0f);
  const Pair neighbour = response_at(15.0f);
  const double step = distance(at.right, neighbour.right) + distance(at.left, neighbour.left);
  const double crossing = distance(below.right, above.right) + distance(below.left, above.left);
  CHECK(crossing < 0.01 * step);
  // Weights sum to one: halfway, the DC gain (which no delay changes) is the mean.
  const Pair a = response_at(20.0f);
  const Pair b = response_at(25.0f);
  const Pair mid = response_at(22.5f);
  CHECK(std::fabs(sum(mid.right) - 0.5 * (sum(a.right) + sum(b.right))) < 1e-4);
  CHECK(std::fabs(sum(mid.left) - 0.5 * (sum(a.left) + sum(b.left))) < 1e-4);
}

TEST_CASE("binaural speakers output cancels crosstalk above 200 Hz and not below", "[binaural]") {
  // The mid/side canceller is linear and symmetric, so one render gives its mid
  // and side gains; the plant is the panner's own phones render at +30 degrees.
  const Pair phones = response_at(90.0f);
  const Pair speakers = response_at(90.0f, BinauralOutput::kSpeakers);
  const Pair plant = response_at(30.0f);
  const auto pl = spectrum(phones.left);
  const auto pr = spectrum(phones.right);
  const auto sl = spectrum(speakers.left);
  const auto sr = spectrum(speakers.right);
  const auto hi = spectrum(plant.right);
  const auto hc = spectrum(plant.left);

  double cancelled_db = 0.0;
  double plain_db = 0.0;
  double weight = 0.0;
  double worst_low_db = 0.0;
  for (int k = 1; k < kSpectrumSize / 2; ++k) {
    const auto i = static_cast<std::size_t>(k);
    const double hz = hz_of(k);
    if (hz <= 100.0) {
      // Below the crossover the speakers pair is the phones pair.
      worst_low_db =
          std::max(worst_low_db, std::fabs(20.0 * std::log10(std::abs(sl[i]) / std::abs(pl[i]))));
      worst_low_db =
          std::max(worst_low_db, std::fabs(20.0 * std::log10(std::abs(sr[i]) / std::abs(pr[i]))));
    }
    if (hz < 200.0 || hz > 8000.0) continue;
    const std::complex<double> mid_gain = (sl[i] + sr[i]) / (pl[i] + pr[i]);
    const std::complex<double> side_gain = (sl[i] - sr[i]) / (pl[i] - pr[i]);
    // A left-only binaural signal through the canceller and the plant.
    const std::complex<double> m = hi[i] + hc[i];
    const std::complex<double> s = hi[i] - hc[i];
    const std::complex<double> ear_near = 0.5 * (m * mid_gain + s * side_gain);
    const std::complex<double> ear_far = 0.5 * (m * mid_gain - s * side_gain);
    const double w = 1.0 / hz;
    cancelled_db += w * 20.0 * std::log10(std::abs(ear_far) / std::abs(ear_near));
    plain_db += w * 20.0 * std::log10(std::abs(hc[i]) / std::abs(hi[i]));
    weight += w;
  }
  cancelled_db /= weight;
  plain_db /= weight;
  WARN("crosstalk 200 Hz - 8 kHz: " << plain_db << " dB plain, " << cancelled_db
                                    << " dB cancelled; worst deviation below 100 Hz "
                                    << worst_low_db << " dB");
  CHECK(cancelled_db < plain_db - 12.0);
  CHECK(worst_low_db < 1.0);
  // Both outputs carry the same latency, so switching does not move the stream.
  BinauralPannerConfig config;
  BinauralPanner a(config);
  config.output = BinauralOutput::kSpeakers;
  BinauralPanner b(config);
  a.prepare(kRate, 512);
  b.prepare(kRate, 512);
  CHECK(a.latency_samples() == b.latency_samples());
  CHECK(a.latency_samples() > 0);
}

TEST_CASE("binaural autoTurn rotates at turnRateHz in the direction asked", "[binaural]") {
  constexpr float kTurnHz = 2.0f;
  constexpr int kSamples = 96000;
  constexpr int kFrame = 240;
  for (bool clockwise : {true, false}) {
    INFO("clockwise " << clockwise);
    BinauralPannerConfig config;
    config.auto_turn = true;
    config.turn_rate_hz = kTurnHz;
    config.clockwise = clockwise;
    BinauralPanner panner(config);
    panner.prepare(kRate, kSamples);
    std::vector<float> left =
        sonare::test::generate_sine(kSamples, 3000.0f, static_cast<int>(kRate), 0.5f);
    std::vector<float> right = left;
    process_stereo(panner, left, right);
    // Right-minus-left frame energy: positive while the source is on the right.
    std::vector<double> balance;
    for (int start = 0; start + kFrame <= kSamples; start += kFrame) {
      double el = 0.0;
      double er = 0.0;
      for (int n = start; n < start + kFrame; ++n) {
        el += left[static_cast<std::size_t>(n)] * left[static_cast<std::size_t>(n)];
        er += right[static_cast<std::size_t>(n)] * right[static_cast<std::size_t>(n)];
      }
      balance.push_back(er - el);
    }
    // The first quarter turn is on the right for clockwise, the left otherwise.
    const std::size_t quarter = static_cast<std::size_t>(kRate / kTurnHz / 4.0 / kFrame);
    CHECK((balance[quarter] > 0.0) == clockwise);
    // Each revolution crosses from left to right once.
    std::vector<double> rising;
    for (std::size_t f = 1; f < balance.size(); ++f) {
      const bool up = clockwise ? balance[f - 1] < 0.0 && balance[f] >= 0.0
                                : balance[f - 1] > 0.0 && balance[f] <= 0.0;
      if (up) rising.push_back(static_cast<double>(f * kFrame) / kRate);
    }
    REQUIRE(rising.size() >= 3);
    const double rate = static_cast<double>(rising.size() - 1) / (rising.back() - rising.front());
    WARN("turn rate " << rate << " Hz, asked " << kTurnHz);
    CHECK(std::fabs(rate - kTurnHz) < 0.02 * kTurnHz);
  }
}

TEST_CASE("binaural is built by the insert factory with every key", "[binaural]") {
  auto insert = sonare::mastering::api::make_insert(
      "stereo.binaural",
      R"({"azimuthDeg":-45,"autoTurn":true,"turnRateHz":0.5,"clockwise":false,"output":0,"dryWet":0.8})");
  REQUIRE(insert != nullptr);
  // The ordinals are the GS 3D `Out` byte: 0 Speaker, 1 Phones.
  CHECK(static_cast<int>(BinauralOutput::kSpeakers) == 0);
  CHECK(static_cast<int>(BinauralOutput::kPhones) == 1);
  CHECK(BinauralPannerConfig{}.output == BinauralOutput::kPhones);
  CHECK_THROWS(sonare::mastering::api::make_insert("stereo.binaural", R"({"turnRateHz":11})"));
  CHECK_THROWS(sonare::mastering::api::make_insert("stereo.binaural", R"({"output":2})"));
  insert->prepare(kRate, 256);
  CHECK(insert->set_parameter(0, 190.0f));
  CHECK_FALSE(insert->set_parameter(4, 0.5f));
  CHECK_FALSE(insert->set_parameter(0, std::nanf("")));
  // Dry at the latency: dryWet 0 is the input delayed by the reported latency.
  auto dry = sonare::mastering::api::make_insert("stereo.binaural", R"({"dryWet":0})");
  dry->prepare(kRate, 256);
  std::vector<float> left(256, 0.0f);
  std::vector<float> right(256, 0.0f);
  left[0] = 1.0f;
  right[0] = -0.5f;
  process_stereo(*dry, left, right);
  const auto lag = static_cast<std::size_t>(dry->latency_samples());
  CHECK(left[lag] == 1.0f);
  CHECK(right[lag] == -0.5f);
}
