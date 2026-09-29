/// @file auto_wah_modes_test.cpp
/// @brief The wah's and auto-wah's filter type, sweep law, direction and LFO.
///
/// Expectations are structural: where the centre of a static sweep sits, which
/// tap is heard, which way the sweep runs, and that the LFO moves the output.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <vector>

#include "effects/modulation/auto_wah.h"
#include "effects/modulation/wah.h"
#include "mastering/api/insert_factory.h"
#include "support/audio_fixtures.h"

namespace {

using sonare::effects::modulation::AutoWah;
using sonare::effects::modulation::AutoWahConfig;
using sonare::effects::modulation::AutoWahDirection;
using sonare::effects::modulation::Wah;
using sonare::effects::modulation::WahConfig;
using sonare::effects::modulation::WahFilterType;
using sonare::effects::modulation::WahSweepLaw;

constexpr int kRate = 48000;
constexpr float kMinHz = 400.0f;
constexpr float kMaxHz = 2000.0f;
constexpr float kQ = 8.0f;

double rms_tail(const std::vector<float>& x) {
  double sum = 0.0;
  const std::size_t start = x.size() / 2;
  for (std::size_t i = start; i < x.size(); ++i) sum += static_cast<double>(x[i]) * x[i];
  return std::sqrt(sum / static_cast<double>(x.size() - start));
}

/// Level of a tone through a processor that is static (no LFO motion, no
/// envelope drive), read over the second half so the filter has settled.
template <typename Make>
double level_at(Make make, double hz) {
  auto processor = make();
  processor.prepare(kRate, 4096);
  std::vector<float> left =
      sonare::test::generate_sine(kRate / 5, static_cast<float>(hz), kRate, 0.5f);
  std::vector<float> right = left;
  sonare::test::process_stereo(processor, left, right);
  return rms_tail(left);
}

/// Frequency of the strongest response found on a 48-per-octave grid.
template <typename Make>
double peak_hz(Make make) {
  double best_hz = 0.0;
  double best = -1.0;
  for (double hz = 150.0; hz < 6000.0; hz *= std::pow(2.0, 1.0 / 48.0)) {
    const double level = level_at(make, hz);
    if (level > best) {
      best = level;
      best_hz = hz;
    }
  }
  return best_hz;
}

/// A wah held at mid-sweep: rate 0 leaves the LFO at zero, position 0.5.
WahConfig still_wah(WahSweepLaw law, WahFilterType type = WahFilterType::kBandpass) {
  WahConfig config;
  config.rate_hz = 0.0f;
  config.min_hz = kMinHz;
  config.max_hz = kMaxHz;
  config.resonance = kQ;
  config.sweep_law = law;
  config.filter_type = type;
  return config;
}

/// An auto-wah with no envelope drive whose LFO rests at position `lfo_depth / 2`.
AutoWahConfig still_auto_wah(WahSweepLaw law) {
  AutoWahConfig config;
  config.sensitivity = 0.0f;
  config.min_hz = kMinHz;
  config.max_hz = kMaxHz;
  config.resonance = kQ;
  config.lfo_rate_hz = 0.0f;
  config.lfo_depth = 1.0f;
  config.sweep_law = law;
  return config;
}

bool differ(const std::vector<float>& a, const std::vector<float>& b) {
  double diff = 0.0;
  for (std::size_t i = 0; i < a.size(); ++i) diff += std::fabs(static_cast<double>(a[i]) - b[i]);
  return diff > 1e-3;
}

std::vector<float> render_auto_wah(const AutoWahConfig& config) {
  AutoWah wah(config);
  wah.prepare(kRate, 4096);
  std::vector<float> left = sonare::test::generate_sine(kRate, 700.0f, kRate, 0.5f);
  std::vector<float> right = left;
  sonare::test::process_stereo(wah, left, right);
  return left;
}

}  // namespace

TEST_CASE("wah sweep law places the mid-sweep centre in hertz or in octaves", "[auto-wah-modes]") {
  const double linear = 0.5 * (kMinHz + kMaxHz);
  const double geometric = std::sqrt(static_cast<double>(kMinHz) * kMaxHz);

  const double hz_law = peak_hz([] { return Wah(still_wah(WahSweepLaw::kLinearHz)); });
  const double octave_law = peak_hz([] { return Wah(still_wah(WahSweepLaw::kLinearOctave)); });
  WARN("wah mid-sweep centre: linear law " << hz_law << " Hz, octave law " << octave_law << " Hz");
  REQUIRE(std::fabs(hz_law - linear) / linear < 0.05);
  REQUIRE(std::fabs(octave_law - geometric) / geometric < 0.05);

  const double auto_hz = peak_hz([] { return AutoWah(still_auto_wah(WahSweepLaw::kLinearHz)); });
  const double auto_octave =
      peak_hz([] { return AutoWah(still_auto_wah(WahSweepLaw::kLinearOctave)); });
  WARN("auto-wah mid-sweep centre: linear law " << auto_hz << " Hz, octave law " << auto_octave
                                                << " Hz");
  REQUIRE(std::fabs(auto_hz - 0.5 * (kMinHz + kMaxHz)) / linear < 0.05);
  REQUIRE(std::fabs(auto_octave - geometric) / geometric < 0.05);
}

TEST_CASE("the octave law keeps the sweep's corners", "[auto-wah-modes]") {
  using sonare::effects::modulation::wah_sweep_hz;
  for (const WahSweepLaw law : {WahSweepLaw::kLinearHz, WahSweepLaw::kLinearOctave}) {
    REQUIRE(wah_sweep_hz(law, 300.0f, 2500.0f, 0.0f) == 300.0f);
    REQUIRE(std::fabs(wah_sweep_hz(law, 300.0f, 2500.0f, 1.0f) - 2500.0f) < 0.01f);
  }
  // A quarter of the way up is a quarter of the octaves in the octave law.
  const float quarter = wah_sweep_hz(WahSweepLaw::kLinearOctave, 400.0f, 1600.0f, 0.25f);
  REQUIRE(std::fabs(quarter - 400.0f * std::sqrt(2.0f)) < 0.5f);
}

TEST_CASE("wah and auto-wah filter type chooses the bandpass or the lowpass tap",
          "[auto-wah-modes]") {
  auto band = [] { return Wah(still_wah(WahSweepLaw::kLinearHz)); };
  auto low = [] { return Wah(still_wah(WahSweepLaw::kLinearHz, WahFilterType::kLowpass)); };
  // Well below the 1200 Hz centre: a lowpass passes it, a bandpass does not.
  const double band_low = level_at(band, 100.0);
  const double lowpass_low = level_at(low, 100.0);
  WARN("100 Hz level: bandpass " << band_low << ", lowpass " << lowpass_low);
  REQUIRE(band_low < 0.1);
  REQUIRE(lowpass_low > 0.25);
  // Well above it: both attenuate.
  REQUIRE(level_at(low, 6000.0) < 0.05);

  AutoWahConfig band_config = still_auto_wah(WahSweepLaw::kLinearHz);
  AutoWahConfig low_config = band_config;
  low_config.filter_type = WahFilterType::kLowpass;
  REQUIRE(level_at([&] { return AutoWah(low_config); }, 100.0) >
          3.0 * level_at([&] { return AutoWah(band_config); }, 100.0));
}

TEST_CASE("auto-wah direction inverts the sweep", "[auto-wah-modes]") {
  AutoWahConfig up;
  up.sensitivity = 0.0f;
  up.min_hz = kMinHz;
  up.max_hz = kMaxHz;
  up.resonance = kQ;
  AutoWahConfig down = up;
  down.direction = AutoWahDirection::kDown;
  const double up_hz = peak_hz([&] { return AutoWah(up); });
  const double down_hz = peak_hz([&] { return AutoWah(down); });
  WARN("resting centre: up " << up_hz << " Hz, down " << down_hz << " Hz");
  REQUIRE(std::fabs(up_hz - kMinHz) / kMinHz < 0.05);
  REQUIRE(std::fabs(down_hz - kMaxHz) / kMaxHz < 0.05);

  // With the envelope driving it, a loud input opens the up sweep and closes the down one.
  AutoWahConfig loud_up;
  loud_up.sensitivity = 2.0f;
  AutoWahConfig loud_down = loud_up;
  loud_down.direction = AutoWahDirection::kDown;
  REQUIRE(differ(render_auto_wah(loud_up), render_auto_wah(loud_down)));
}

TEST_CASE("auto-wah LFO moves the sweep and depth 0 leaves it inert", "[auto-wah-modes]") {
  AutoWahConfig off;
  AutoWahConfig slow = off;
  slow.lfo_rate_hz = 2.0f;
  slow.lfo_depth = 0.0f;
  AutoWahConfig fast = off;
  fast.lfo_rate_hz = 9.0f;
  fast.lfo_depth = 0.0f;
  REQUIRE(render_auto_wah(slow) == render_auto_wah(fast));
  REQUIRE(render_auto_wah(off) == render_auto_wah(slow));

  AutoWahConfig deep = off;
  deep.lfo_rate_hz = 2.0f;
  deep.lfo_depth = 0.8f;
  AutoWahConfig deep_fast = deep;
  deep_fast.lfo_rate_hz = 9.0f;
  REQUIRE(differ(render_auto_wah(off), render_auto_wah(deep)));
  REQUIRE(differ(render_auto_wah(deep), render_auto_wah(deep_fast)));
}

TEST_CASE("wah and auto-wah refuse unnamed or fractional enum values", "[auto-wah-modes]") {
  Wah wah;
  wah.prepare(kRate, 512);
  REQUIRE(wah.set_parameter(5, 1.0f));
  REQUIRE(wah.set_parameter(6, 1.0f));
  for (const unsigned int id : {5u, 6u}) {
    REQUIRE_FALSE(wah.set_parameter(id, 0.5f));
    REQUIRE_FALSE(wah.set_parameter(id, -1.0f));
    REQUIRE_FALSE(wah.set_parameter(id, 2.0f));
    REQUIRE(wah.parameter_is_realtime_safe(id));
  }
  REQUIRE_FALSE(wah.set_parameter(7, 0.0f));

  AutoWah auto_wah;
  auto_wah.prepare(kRate, 512);
  for (const unsigned int id : {7u, 8u, 9u}) {
    REQUIRE(auto_wah.set_parameter(id, 1.0f));
    REQUIRE_FALSE(auto_wah.set_parameter(id, 0.5f));
    REQUIRE_FALSE(auto_wah.set_parameter(id, -1.0f));
    REQUIRE_FALSE(auto_wah.set_parameter(id, 2.0f));
    REQUIRE(auto_wah.parameter_is_realtime_safe(id));
  }
  REQUIRE(auto_wah.set_parameter(5, 3.0f));
  REQUIRE(auto_wah.set_parameter(6, 0.5f));
  REQUIRE_FALSE(auto_wah.set_parameter(10, 0.0f));

  const auto descriptors = auto_wah.parameter_descriptors();
  const auto has = [&](const char* key, unsigned int id) {
    return std::any_of(descriptors.begin(), descriptors.end(),
                       [&](const auto& d) { return d.key == key && d.id == id; });
  };
  REQUIRE(has("lfoRateHz", 5));
  REQUIRE(has("lfoDepth", 6));
  REQUIRE(has("filterType", 7));
  REQUIRE(has("direction", 8));
  REQUIRE(has("sweepLaw", 9));
}

TEST_CASE("wah and auto-wah enum keys reach the insert through the factory", "[auto-wah-modes]") {
  using sonare::mastering::api::make_insert;
  auto render = [](const char* name, const char* json) {
    auto processor = make_insert(name, json);
    REQUIRE(processor != nullptr);
    processor->prepare(kRate, 4096);
    std::vector<float> left = sonare::test::generate_sine(kRate / 4, 500.0f, kRate, 0.5f);
    std::vector<float> right = left;
    sonare::test::process_stereo(*processor, left, right);
    return left;
  };
  REQUIRE(differ(render("effects.modulation.wah", "{}"),
                 render("effects.modulation.wah", R"({"filterType":1})")));
  REQUIRE(differ(render("effects.modulation.wah", "{}"),
                 render("effects.modulation.wah", R"({"sweepLaw":1})")));
  REQUIRE(differ(render("effects.modulation.autoWah", "{}"),
                 render("effects.modulation.autoWah", R"({"direction":1})")));
  REQUIRE(differ(render("effects.modulation.autoWah", "{}"),
                 render("effects.modulation.autoWah", R"({"lfoDepth":0.7})")));
  REQUIRE_THROWS(make_insert("effects.modulation.autoWah", R"({"direction":2})"));
}
