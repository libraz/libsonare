#include "playback/binaural.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <memory>
#include <random>
#include <vector>

#include "playback/shrf_fixture.h"

using namespace sonare::playback;
using namespace sonare::playback::test;

namespace {

/// Lag (right relative to left, in samples) of the cross-correlation peak,
/// refined by a parabola through the peak and its neighbours.
double xcorr_peak_lag(const std::vector<float>& left, const std::vector<float>& right,
                      int max_lag) {
  const int n = static_cast<int>(left.size());
  std::vector<double> c(static_cast<size_t>(2 * max_lag + 1), 0.0);
  for (int lag = -max_lag; lag <= max_lag; ++lag) {
    double acc = 0.0;
    for (int i = max_lag; i < n - max_lag; ++i) {
      acc +=
          static_cast<double>(left[static_cast<size_t>(i)]) * right[static_cast<size_t>(i + lag)];
    }
    c[static_cast<size_t>(lag + max_lag)] = acc;
  }
  int best = 1;
  for (int k = 1; k < 2 * max_lag; ++k) {
    if (c[static_cast<size_t>(k)] > c[static_cast<size_t>(best)]) best = k;
  }
  const double a = c[static_cast<size_t>(best - 1)];
  const double b = c[static_cast<size_t>(best)];
  const double d = c[static_cast<size_t>(best + 1)];
  const double denom = a - 2.0 * b + d;
  const double frac = denom != 0.0 ? 0.5 * (a - d) / denom : 0.0;
  return static_cast<double>(best - max_lag) + frac;
}

}  // namespace

TEST_CASE("binaural ITD of a grid-point speaker matches the table", "[playback][binaural]") {
  constexpr double kRate = 48000.0;
  constexpr int kBlock = 256;
  constexpr int kFrames = 8192;
  const ShrfFixtureSpec spec;
  const std::vector<uint8_t> bytes = make_shrf_fixture(spec);
  std::unique_ptr<HrtfSet> set;
  REQUIRE_NOTHROW(set =
                      std::make_unique<HrtfSet>(HrtfSet::from_memory(bytes.data(), bytes.size())));

  // 7.1 slot set: Lss90 (-90 degrees, grid index 6 of 8) is bus plane 5.
  const HeadphoneSlotSet slots = headphone_slots(InputLayout::SevenPointOne);
  BinauralRenderer renderer;
  renderer.prepare(kRate, kBlock, *set, slots, RoomPreset::None);

  std::mt19937 rng(3u);
  std::normal_distribution<float> dist(0.0f, 0.1f);
  const int planes = slots.count + 2;
  std::vector<std::vector<float>> bus(static_cast<size_t>(planes), std::vector<float>(kBlock));
  std::vector<const float*> bus_ptrs;
  for (auto& p : bus) bus_ptrs.push_back(p.data());
  std::vector<float> left(kFrames), right(kFrames);
  for (int start = 0; start < kFrames; start += kBlock) {
    for (int i = 0; i < kBlock; ++i) bus[5][static_cast<size_t>(i)] = dist(rng);
    renderer.process(bus_ptrs.data(), left.data() + start, right.data() + start, kBlock);
  }

  double e_l = 0.0, e_r = 0.0;
  for (int i = 0; i < kFrames; ++i) {
    e_l += static_cast<double>(left[static_cast<size_t>(i)]) * left[static_cast<size_t>(i)];
    e_r += static_cast<double>(right[static_cast<size_t>(i)]) * right[static_cast<size_t>(i)];
  }
  REQUIRE(e_l > 0.0);
  REQUIRE(e_r > 0.0);
  // Positive ITD means the left ear lags, i.e. left[i] ~ right[i - itd].
  const double expected = fixture_itd_samples(spec, 0, 6);
  CHECK(std::abs(-xcorr_peak_lag(left, right, 40) - expected) <= 0.1);
}
