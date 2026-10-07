/// @file lofi_extras_test.cpp
/// @brief The bitcrusher's four noise sources, its filters around the hold, the
///        mono sum and the type ladder.
///
/// Expectations are structural (energy present or exactly absent, a click
/// density, a spectral tilt, a fundamental) rather than recorded waveforms.

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <vector>

#include "mastering/saturation/bitcrusher.h"
#include "util/constants.h"

namespace {

using sonare::constants::kTwoPiD;
using sonare::mastering::saturation::BitCrusher;
using sonare::mastering::saturation::BitCrusherConfig;
using sonare::mastering::saturation::BitCrusherDiscType;
using sonare::mastering::saturation::BitCrusherFilterType;
using sonare::mastering::saturation::kDiscClickRateHz;
using sonare::mastering::saturation::kTypeLadderHoldHz;
using sonare::mastering::saturation::QuantizerMode;

constexpr double kRate = 48000.0;
constexpr std::size_t kSeconds = 4;
constexpr float kLevel = 0.5f;

/// A crusher with the quantizer out of the way, so only what the case sets acts.
BitCrusherConfig bare() {
  BitCrusherConfig config;
  config.quantizer_mode = QuantizerMode::kOff;
  return config;
}

std::vector<float> render(const BitCrusherConfig& config, const std::vector<float>& input,
                          double rate = kRate, int prepared_block = 0) {
  BitCrusher crusher(config);
  crusher.prepare(rate, prepared_block > 0 ? prepared_block : static_cast<int>(input.size()));
  std::vector<float> out = input;
  float* channels[] = {out.data()};
  crusher.process(channels, 1, static_cast<int>(out.size()));
  return out;
}

std::vector<float> silence(double rate = kRate, std::size_t seconds = kSeconds) {
  return std::vector<float>(static_cast<std::size_t>(rate) * seconds, 0.0f);
}

std::vector<float> tone(double hz, double rate = kRate, std::size_t seconds = 1) {
  std::vector<float> x(static_cast<std::size_t>(rate) * seconds);
  for (std::size_t i = 0; i < x.size(); ++i) {
    x[i] = static_cast<float>(std::sin(kTwoPiD * hz * static_cast<double>(i) / rate));
  }
  return x;
}

double energy(const std::vector<float>& x) {
  double sum = 0.0;
  for (const float v : x) sum += static_cast<double>(v) * v;
  return sum / static_cast<double>(x.size());
}

/// Mean squared first difference over mean square: two for white noise, far less for a dull one.
double roughness(const std::vector<float>& x) {
  double diff = 0.0;
  for (std::size_t i = 1; i < x.size(); ++i) {
    const double d = static_cast<double>(x[i]) - x[i - 1];
    diff += d * d;
  }
  return diff / static_cast<double>(x.size() - 1) / energy(x);
}

/// Amplitude of the component at @p hz.
double component(const std::vector<float>& x, double hz, double rate = kRate) {
  double re = 0.0;
  double im = 0.0;
  for (std::size_t i = 0; i < x.size(); ++i) {
    const double a = kTwoPiD * hz * static_cast<double>(i) / rate;
    re += x[i] * std::cos(a);
    im += x[i] * std::sin(a);
  }
  return 2.0 * std::hypot(re, im) / static_cast<double>(x.size());
}

std::size_t nonzero(const std::vector<float>& x) {
  return static_cast<std::size_t>(
      std::count_if(x.begin(), x.end(), [](float v) { return v != 0.0f; }));
}

}  // namespace

TEST_CASE("every noise level at zero adds nothing, whatever the sources are set to",
          "[lofi-extras]") {
  BitCrusherConfig set = bare();
  set.noise_detune = 0.3f;
  set.wp_noise_pink = true;
  set.disc_type = BitCrusherDiscType::kRnd;
  set.hum_hz = 60.0f;
  set.noise_lpf_hz = 2000.0f;
  set.wp_noise_lpf_hz = 2000.0f;
  set.disc_noise_lpf_hz = 2000.0f;
  set.hum_lpf_hz = 2000.0f;
  const std::vector<float> input = tone(220.0);
  CHECK(render(set, input) == render(bare(), input));
  CHECK(energy(render(set, silence())) == 0.0);
}

TEST_CASE("each noise source sounds alone and two renders agree", "[lofi-extras]") {
  const std::vector<float> quiet = silence();
  for (int source = 0; source < 4; ++source) {
    BitCrusherConfig config = bare();
    (source == 0   ? config.radio_noise_level
     : source == 1 ? config.wp_noise_level
     : source == 2 ? config.disc_noise_level
                   : config.hum_level) = kLevel;
    const std::vector<float> a = render(config, quiet);
    INFO("source " << source);
    CHECK(energy(a) > 1e-6);
    CHECK(a == render(config, quiet));
  }
}

TEST_CASE("the four sources sound together at the sum of their energies", "[lofi-extras]") {
  const std::vector<float> quiet = silence();
  BitCrusherConfig all = bare();
  double sum = 0.0;
  for (int source = 0; source < 4; ++source) {
    BitCrusherConfig one = bare();
    float& level = source == 0   ? one.radio_noise_level
                   : source == 1 ? one.wp_noise_level
                   : source == 2 ? one.disc_noise_level
                                 : one.hum_level;
    level = kLevel;
    sum += energy(render(one, quiet));
    (source == 0   ? all.radio_noise_level
     : source == 1 ? all.wp_noise_level
     : source == 2 ? all.disc_noise_level
                   : all.hum_level) = kLevel;
  }
  CHECK(energy(render(all, quiet)) == Catch::Approx(sum).epsilon(0.1));
}

TEST_CASE("the detune scales the radio source only", "[lofi-extras]") {
  const std::vector<float> quiet = silence();
  BitCrusherConfig config = bare();
  config.radio_noise_level = kLevel;
  config.noise_detune = 0.0f;
  CHECK(energy(render(config, quiet)) == 0.0);
  config.noise_detune = 1.0f;
  const double full = energy(render(config, quiet));
  config.noise_detune = 0.5f;
  CHECK(energy(render(config, quiet)) == Catch::Approx(0.25 * full).epsilon(1e-3));
  BitCrusherConfig hum = bare();
  hum.hum_level = kLevel;
  hum.noise_detune = 0.0f;
  CHECK(energy(render(hum, quiet)) > 0.0);
}

TEST_CASE("pink noise is duller than white noise", "[lofi-extras]") {
  BitCrusherConfig config = bare();
  config.wp_noise_level = kLevel;
  const double white = roughness(render(config, silence()));
  config.wp_noise_pink = true;
  const double pink = roughness(render(config, silence()));
  CHECK(white == Catch::Approx(2.0).epsilon(0.05));
  CHECK(pink < 0.5 * white);
}

TEST_CASE("each source low-pass dulls its own source and zero bypasses it", "[lofi-extras]") {
  for (int source = 0; source < 4; ++source) {
    BitCrusherConfig config = bare();
    float& level = source == 0   ? config.radio_noise_level
                   : source == 1 ? config.wp_noise_level
                   : source == 2 ? config.disc_noise_level
                                 : config.hum_level;
    level = kLevel;
    float& lpf = source == 0   ? config.noise_lpf_hz
                 : source == 1 ? config.wp_noise_lpf_hz
                 : source == 2 ? config.disc_noise_lpf_hz
                               : config.hum_lpf_hz;
    const std::vector<float> open = render(config, silence());
    lpf = 300.0f;
    const std::vector<float> closed = render(config, silence());
    INFO("source " << source);
    CHECK(closed != open);
    CHECK(energy(closed) < energy(open));
    lpf = 0.0f;
    CHECK(render(config, silence()) == open);
  }
}

TEST_CASE("the disc types click at 4, 8, 16 and 32 a second at either rate", "[lofi-extras]") {
  constexpr std::size_t seconds = 20;
  for (const double rate : {44100.0, 48000.0}) {
    for (int type = 0; type < 4; ++type) {
      BitCrusherConfig config = bare();
      config.disc_noise_level = kLevel;
      config.disc_type = static_cast<BitCrusherDiscType>(type);
      const double expected = static_cast<double>(kDiscClickRateHz[static_cast<size_t>(type)]) *
                              static_cast<double>(seconds);
      INFO("type " << type << " at " << rate);
      CHECK(static_cast<double>(nonzero(render(config, silence(rate, seconds), rate))) ==
            Catch::Approx(expected).epsilon(0.25));
    }
  }
  CHECK(kDiscClickRateHz[0] == 4.0f);
  CHECK(kDiscClickRateHz[1] == 8.0f);
  CHECK(kDiscClickRateHz[2] == 16.0f);
  CHECK(kDiscClickRateHz[3] == 32.0f);
}

TEST_CASE("the hum sits at the mains frequency it names", "[lofi-extras]") {
  for (const float mains : {50.0f, 60.0f}) {
    BitCrusherConfig config = bare();
    config.hum_level = 1.0f;
    config.hum_hz = mains;
    const std::vector<float> hum = render(config, silence());
    const double other = mains == 50.0f ? 60.0 : 50.0;
    INFO("mains " << mains);
    CHECK(component(hum, static_cast<double>(mains)) == Catch::Approx(1.0 / 1.75).epsilon(0.02));
    CHECK(component(hum, other) < 0.02);
  }
}

TEST_CASE("the post filter is a low-pass, a high-pass or nothing by its type", "[lofi-extras]") {
  const std::vector<float> low = tone(100.0);
  const std::vector<float> high = tone(10000.0);
  BitCrusherConfig config = bare();
  config.post_filter_hz = 1500.0f;
  config.filter_type = BitCrusherFilterType::kLowpass;
  CHECK(component(render(config, low), 100.0) > 0.95);
  CHECK(component(render(config, high), 10000.0) < 0.2);
  config.filter_type = BitCrusherFilterType::kHighpass;
  CHECK(component(render(config, low), 100.0) < 0.2);
  CHECK(component(render(config, high), 10000.0) > 0.95);
  config.filter_type = BitCrusherFilterType::kOff;
  CHECK(render(config, low) == render(bare(), low));
}

TEST_CASE("the pre filter low-passes what the hold latches", "[lofi-extras]") {
  const std::vector<float> high = tone(6000.0);
  BitCrusherConfig config = bare();
  CHECK(component(render(config, high), 6000.0) > 0.99);
  config.pre_filter_hz = 1500.0f;
  // Four times the corner sits near -12 dB for one pole.
  CHECK(component(render(config, high), 6000.0) == Catch::Approx(0.24).margin(0.03));
}

TEST_CASE("mono writes the channel mean to every channel", "[lofi-extras]") {
  BitCrusherConfig config = bare();
  config.mono = true;
  BitCrusher crusher(config);
  crusher.prepare(kRate, 256);
  std::vector<float> l = tone(300.0, kRate, 1);
  std::vector<float> r = tone(500.0, kRate, 1);
  l.resize(256);
  r.resize(256);
  const std::vector<float> l0 = l;
  const std::vector<float> r0 = r;
  float* channels[] = {l.data(), r.data()};
  crusher.process(channels, 2, 256);
  for (std::size_t i = 0; i < 256; ++i) {
    CHECK(l[i] == r[i]);
    CHECK(l[i] == Catch::Approx(0.5f * (l0[i] + r0[i])).margin(1e-6));
  }
}

TEST_CASE("the type ladder selects the classic hold rates in place of the hold", "[lofi-extras]") {
  CHECK(kTypeLadderHoldHz[0] == 16000.0f);
  CHECK(kTypeLadderHoldHz[1] == Catch::Approx(10666.666666666668f));
  CHECK(kTypeLadderHoldHz[2] == 8000.0f);
  for (std::size_t i = 0; i < kTypeLadderHoldHz.size(); ++i) {
    CHECK(kTypeLadderHoldHz[i] == kTypeLadderHoldHz[i % 3]);
  }
  const std::vector<float> input = tone(313.0);
  for (int state = 1; state <= 9; ++state) {
    BitCrusherConfig ladder = bare();
    ladder.type_ladder = state;
    ladder.hold_hz = 1000.0f;  // the ladder replaces it
    BitCrusherConfig rate = bare();
    rate.hold_hz = kTypeLadderHoldHz[static_cast<size_t>(state - 1)];
    INFO("state " << state);
    CHECK(render(ladder, input) == render(rate, input));
    CHECK(render(ladder, input) != render(bare(), input));
  }
}

TEST_CASE("a block longer than the prepared size renders as one block", "[lofi-extras]") {
  BitCrusherConfig config = bare();
  config.radio_noise_level = 0.3f;
  config.hum_level = 0.2f;
  config.wp_noise_level = 0.2f;
  config.post_filter_hz = 3000.0f;
  const std::vector<float> input = tone(200.0, kRate, 1);
  CHECK(render(config, input, kRate, 64) == render(config, input));
}

TEST_CASE("the automation keys include the type ladder and refuse unknown values",
          "[lofi-extras]") {
  BitCrusher crusher;
  crusher.prepare(kRate, 64);
  for (unsigned id = 0; id <= 20; ++id) {
    INFO("id " << id);
    CHECK(crusher.parameter_is_realtime_safe(id));
  }
  CHECK_FALSE(crusher.parameter_is_realtime_safe(21));
  CHECK_FALSE(crusher.set_parameter(21, 1.0f));
  // The mix law takes a named law and refuses a fraction or one past the list.
  CHECK(crusher.set_parameter(20, 1.0f));
  CHECK_FALSE(crusher.set_parameter(20, 0.5f));
  CHECK_FALSE(crusher.set_parameter(20, 2.0f));
  const auto descriptors = crusher.parameter_descriptors();
  CHECK(descriptors.size() == 21);
  CHECK(std::any_of(descriptors.begin(), descriptors.end(), [](const auto& descriptor) {
    return descriptor.id == 19 && descriptor.key == "typeLadder";
  }));
  CHECK(crusher.set_parameter(19, 4.0f));
  CHECK(crusher.config().type_ladder == 4);
  CHECK_FALSE(crusher.set_parameter(19, 20.0f));
  CHECK(crusher.set_parameter(3, 0.4f));
  CHECK(crusher.config().radio_noise_level == 0.4f);
  CHECK(crusher.set_parameter(6, 3.0f));
  CHECK(crusher.config().hum_level == 1.0f);
  CHECK(crusher.set_parameter(12, 900.0f));
  CHECK(crusher.config().pre_filter_hz == 900.0f);
  CHECK_FALSE(crusher.set_parameter(12, -1.0f));
  CHECK_FALSE(crusher.set_parameter(13, std::nanf("")));
  CHECK(crusher.set_parameter(16, 3.0f));
  CHECK(crusher.config().disc_type == BitCrusherDiscType::kRnd);
  CHECK_FALSE(crusher.set_parameter(16, 4.0f));
  CHECK_FALSE(crusher.set_parameter(17, 1.5f));
  CHECK(crusher.set_parameter(17, 2.0f));
  CHECK(crusher.config().filter_type == BitCrusherFilterType::kHighpass);
  CHECK(crusher.set_parameter(18, 1.0f));
  CHECK(crusher.config().mono);
}
