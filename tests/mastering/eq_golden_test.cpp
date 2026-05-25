#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#include "mastering/eq/equalizer.h"
#include "util/constants.h"

namespace {

using sonare::constants::kPiD;

constexpr int kSampleRate = 48000;
constexpr int kSamples = 4096;

std::vector<float> signal(float phase) {
  std::vector<float> out(kSamples);
  for (int i = 0; i < kSamples; ++i) {
    const double t = static_cast<double>(i) / kSampleRate;
    out[static_cast<size_t>(i)] =
        0.19f * static_cast<float>(std::sin(2.0 * kPiD * 120.0 * t + phase)) +
        0.11f * static_cast<float>(std::sin(2.0 * kPiD * 1000.0 * t)) +
        0.07f * static_cast<float>(std::sin(2.0 * kPiD * 7800.0 * t));
  }
  return out;
}

uint64_t hash_pair(const std::vector<float>& left, const std::vector<float>& right) {
  uint64_t hash = 1469598103934665603ull;
  const auto mix = [&](float sample) {
    const int32_t q =
        static_cast<int32_t>(std::lrint(std::clamp(sample, -2.0f, 2.0f) * 1000000.0f));
    for (int byte = 0; byte < 4; ++byte) {
      hash ^= static_cast<uint8_t>((static_cast<uint32_t>(q) >> (byte * 8)) & 0xffu);
      hash *= 1099511628211ull;
    }
  };
  for (size_t i = 0; i < left.size(); ++i) {
    mix(left[i]);
    mix(right[i]);
  }
  return hash;
}

std::string hex64(uint64_t value) {
  std::ostringstream out;
  out << std::hex;
  out.width(16);
  out.fill('0');
  out << value;
  return out.str();
}

}  // namespace

TEST_CASE("EqualizerProcessor golden hashes stay stable", "[mastering][eq][golden]") {
  using namespace sonare::mastering::eq;
  struct Case {
    const char* name;
    std::array<EqBand, 3> bands;
    const char* expected;
  };

  EqBand mid{EqBandType::Peak, 1000.0f, 4.0f, 1.2f, true};
  mid.placement = StereoPlacement::Mid;
  EqBand side{EqBandType::HighShelf, 6500.0f, -3.0f, 0.8f, true};
  side.placement = StereoPlacement::Side;
  EqBand dyn{EqBandType::Peak, 1000.0f, 0.0f, 2.0f, true};
  dyn.dyn.enabled = true;
  dyn.dyn.threshold_db = -28.0f;
  dyn.dyn.ratio = 3.0f;
  dyn.dyn.range_db = -6.0f;
  dyn.dyn.attack_ms = 0.0f;
  dyn.dyn.release_ms = 15.0f;

  const std::array<Case, 3> cases{{
      {"static-stereo",
       {EqBand{EqBandType::Peak, 1000.0f, 5.0f, 1.0f, true},
        EqBand{EqBandType::LowShelf, 160.0f, -2.5f, 0.707f, true},
        EqBand{EqBandType::HighShelf, 8200.0f, 2.0f, 0.9f, true}},
       "c26d244caee7c908"},
      {"mid-side", {mid, side, EqBand{}}, "bd4feaf3cadb2c40"},
      {"dynamic", {dyn, EqBand{}, EqBand{}}, "0a8c203df9c70c9c"},
  }};

  for (const auto& c : cases) {
    CAPTURE(c.name);
    EqualizerProcessor eq({2});
    eq.prepare(kSampleRate, kSamples);
    for (size_t i = 0; i < c.bands.size(); ++i) {
      if (c.bands[i].enabled) eq.set_band(i, c.bands[i]);
    }
    auto left = signal(0.0f);
    auto right = signal(0.4f);
    float* channels[] = {left.data(), right.data()};
    eq.process(channels, 2, kSamples);
    CHECK(hex64(hash_pair(left, right)) == c.expected);
  }
}
