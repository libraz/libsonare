/// @file pedal_test.cpp
/// @brief Overdrive and distortion pedal inserts, measured through the insert
///        factory: drive against harmonic distortion, the near-linear floor,
///        alias rejection, recovery from a non-finite sample, and the
///        difference in clipping hardness between the two circuits.

#include <algorithm>
#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "mastering/api/insert_factory.h"
#include "midi/part_rig.h"
#include "rt/adaa.h"
#include "rt/oversampler.h"
#include "rt/processor_base.h"
#include "util/constants.h"

namespace {

using sonare::constants::kTwoPiD;
using sonare::mastering::api::insert_factory_names;
using sonare::mastering::api::make_insert;

constexpr const char* kOverdrive = "saturation.overdrive";
constexpr const char* kDistortion = "saturation.distortion";

constexpr int kBlockSize = 512;
constexpr double kProbeHz = 1000.0;
// A DI-level guitar signal, -20 dBFS.
constexpr float kProbeAmplitude = 0.1f;
// Past the 16 Hz input high-pass settling, so every window below is steady state.
constexpr double kSettleSeconds = 0.5;
// 0.1 s holds a whole number of 1 kHz periods at both rates and of the 100 Hz
// grid every alias product of a 1 kHz tone lands on at 44.1 kHz.
constexpr double kWindowSeconds = 0.1;

std::vector<float> sine(double hz, float amplitude, double rate, int num_samples) {
  std::vector<float> out(static_cast<size_t>(num_samples));
  for (int i = 0; i < num_samples; ++i) {
    out[static_cast<size_t>(i)] =
        amplitude * static_cast<float>(std::sin(kTwoPiD * hz * static_cast<double>(i) / rate));
  }
  return out;
}

std::unique_ptr<sonare::rt::ProcessorBase> build(const std::string& name, const std::string& json,
                                                 double rate) {
  auto processor = make_insert(name, json);
  REQUIRE(processor != nullptr);
  processor->prepare(rate, kBlockSize);
  return processor;
}

void run_blocks(sonare::rt::ProcessorBase& processor, std::vector<float>& signal) {
  for (size_t offset = 0; offset < signal.size(); offset += kBlockSize) {
    const int count = static_cast<int>(std::min<size_t>(kBlockSize, signal.size() - offset));
    float* channels[] = {signal.data() + offset};
    processor.process(channels, 1, count);
  }
}

/// The steady-state window of a 1 kHz probe through @p name configured by @p json.
std::vector<float> render_probe(const std::string& name, const std::string& json, double rate,
                                float amplitude = kProbeAmplitude) {
  const int settle = static_cast<int>(kSettleSeconds * rate);
  const int window = static_cast<int>(std::lround(kWindowSeconds * rate));
  auto processor = build(name, json, rate);
  std::vector<float> signal = sine(kProbeHz, amplitude, rate, settle + window);
  run_blocks(*processor, signal);
  return std::vector<float>(signal.begin() + settle, signal.end());
}

/// Power of the component at @p hz, projected over a window that holds a whole
/// number of its periods.
double power_at(const std::vector<float>& window, double hz, double rate) {
  double re = 0.0;
  double im = 0.0;
  for (size_t n = 0; n < window.size(); ++n) {
    const double phase = kTwoPiD * hz * static_cast<double>(n) / rate;
    re += static_cast<double>(window[n]) * std::cos(phase);
    im += static_cast<double>(window[n]) * std::sin(phase);
  }
  const double scale = 2.0 / static_cast<double>(window.size());
  return (re * re + im * im) * scale * scale * 0.5;
}

/// Harmonic power from @p first_harmonic up to Nyquist, against the fundamental's.
double harmonic_ratio(const std::vector<float>& window, double rate, int first_harmonic) {
  const double fundamental = power_at(window, kProbeHz, rate);
  double harmonics = 0.0;
  for (int k = first_harmonic; k * kProbeHz < rate * 0.5; ++k) {
    harmonics += power_at(window, k * kProbeHz, rate);
  }
  REQUIRE(fundamental > 0.0);
  return harmonics / fundamental;
}

double thd_db(const std::vector<float>& window, double rate) {
  return 10.0 * std::log10(harmonic_ratio(window, rate, 2) + 1e-30);
}

std::string params(double gain_db, double tone_hz) {
  return R"({"gainDb":)" + std::to_string(gain_db) + R"(,"toneHz":)" + std::to_string(tone_hz) +
         "}";
}

struct Circuit {
  const char* name;
  double max_gain_db;
  double max_tone_hz;
};

constexpr Circuit kCircuits[] = {
    {kOverdrive, 41.0, 8000.0},
    {kDistortion, 60.0, 20000.0},
};

struct IdentityNonlinearity {
  float apply(float x) const noexcept { return x; }
  float antiderivative(float x) const noexcept { return 0.5f * x * x; }
};

template <size_t N>
double impulse_centroid(const std::array<float, N>& impulse) {
  double weight = 0.0;
  double moment = 0.0;
  for (size_t i = 0; i < impulse.size(); ++i) {
    weight += impulse[i];
    moment += static_cast<double>(i) * impulse[i];
  }
  REQUIRE(weight > 0.0);
  return moment / weight;
}

template <size_t N>
std::array<float, N> identity_adaa_impulse_response() {
  const std::array<float, N> impulse = [] {
    std::array<float, N> value{};
    value[0] = 1.0f;
    return value;
  }();
  sonare::rt::Adaa1<IdentityNonlinearity> adaa;
  std::array<float, N> response{};
  for (size_t i = 0; i < impulse.size(); ++i) response[i] = adaa.process(impulse[i]);
  return response;
}

}  // namespace

TEST_CASE("both pedals are factory inserts with a bounded fixed latency",
          "[mastering][saturation][pedal]") {
  const auto names = insert_factory_names();
  for (const Circuit& circuit : kCircuits) {
    CAPTURE(circuit.name);
    REQUIRE(std::find(names.begin(), names.end(), circuit.name) != names.end());
    for (const double rate : {44100.0, 48000.0}) {
      auto processor = build(circuit.name, "{}", rate);
      CAPTURE(rate, processor->latency_samples());
      CHECK(processor->latency_samples() > 0);
      CHECK(processor->latency_samples() <= sonare::midi::kMaxPartRigLatencySamples);
    }
  }
}

TEST_CASE("factory pedals report oversampled ADAA latency in Q8",
          "[mastering][saturation][pedal][latency]") {
  const sonare::rt::Oversampler reference_oversampler(2);
  const int oversampler_latency_q8 = reference_oversampler.streaming_round_trip_latency_samples()
                                     << 8;
  const auto overdrive_adaa_impulse = identity_adaa_impulse_response<3>();
  const auto distortion_input = overdrive_adaa_impulse;
  sonare::rt::Adaa1<IdentityNonlinearity> second_adaa;
  std::array<float, 3> distortion_adaa_impulse{};
  for (size_t i = 0; i < distortion_input.size(); ++i) {
    distortion_adaa_impulse[i] = second_adaa.process(distortion_input[i]);
  }
  CHECK(overdrive_adaa_impulse[0] == Catch::Approx(0.5f));
  CHECK(overdrive_adaa_impulse[1] == Catch::Approx(0.5f));
  CHECK(overdrive_adaa_impulse[2] == Catch::Approx(0.0f));
  CHECK(distortion_adaa_impulse[0] == Catch::Approx(0.25f));
  CHECK(distortion_adaa_impulse[1] == Catch::Approx(0.5f));
  CHECK(distortion_adaa_impulse[2] == Catch::Approx(0.25f));
  const int overdrive_core_latency_q8 = static_cast<int>(std::lround(
      impulse_centroid(overdrive_adaa_impulse) * 256.0 / reference_oversampler.factor()));
  const int distortion_core_latency_q8 = static_cast<int>(std::lround(
      impulse_centroid(distortion_adaa_impulse) * 256.0 / reference_oversampler.factor()));
  REQUIRE(overdrive_core_latency_q8 == 64);
  REQUIRE(distortion_core_latency_q8 == 128);

  for (const double rate : {44100.0, 48000.0}) {
    CAPTURE(rate);
    auto overdrive = build(kOverdrive, "{}", rate);
    auto distortion = build(kDistortion, "{}", rate);
    const int overdrive_expected_q8 = oversampler_latency_q8 + overdrive_core_latency_q8;
    const int distortion_expected_q8 = oversampler_latency_q8 + distortion_core_latency_q8;
    CAPTURE(overdrive_expected_q8, distortion_expected_q8);
    CHECK(overdrive->latency_samples_q8() == overdrive_expected_q8);
    CHECK(overdrive->latency_samples() == (overdrive_expected_q8 >> 8));
    CHECK(distortion->latency_samples_q8() == distortion_expected_q8);
    CHECK(distortion->latency_samples() == (distortion_expected_q8 >> 8));
  }
}

TEST_CASE("pedal gain raises harmonic distortion monotonically", "[mastering][saturation][pedal]") {
  // A hot pickup, -10 dBFS, so every step past zero gain reaches the clipper.
  // A hard clip driven far past its limit is a square wave whatever the gain,
  // so the top steps may plateau; they may not fall back.
  constexpr double kRate = 48000.0;
  constexpr float kHotAmplitude = 0.3f;
  constexpr int kSteps = 5;
  constexpr double kPlateauDb = 0.1;
  constexpr double kMinimumRiseDb = 30.0;
  for (const Circuit& circuit : kCircuits) {
    double first = 0.0;
    double previous = -std::numeric_limits<double>::infinity();
    for (int step = 0; step < kSteps; ++step) {
      const double gain_db = circuit.max_gain_db * step / (kSteps - 1);
      const double thd =
          thd_db(render_probe(circuit.name, params(gain_db, 4000.0), kRate, kHotAmplitude), kRate);
      CAPTURE(circuit.name, gain_db, thd, previous);
      CHECK(thd > previous - kPlateauDb);
      if (step == 0) first = thd;
      previous = thd;
    }
    CAPTURE(circuit.name, first, previous);
    CHECK(previous - first >= kMinimumRiseDb);
  }
}

TEST_CASE("pedals are near-linear at zero gain with the tone fully open",
          "[mastering][saturation][pedal]") {
  constexpr double kRate = 48000.0;
  for (const Circuit& circuit : kCircuits) {
    const double thd =
        thd_db(render_probe(circuit.name, params(0.0, circuit.max_tone_hz), kRate), kRate);
    CAPTURE(circuit.name, thd);
    CHECK(thd < -60.0);
  }
}

TEST_CASE("pedals keep alias products of a fully driven tone below -50 dBc",
          "[mastering][saturation][pedal]") {
  // 44.1 kHz, where a 1 kHz tone's folded harmonics land off the harmonic
  // series: every one sits on the 100 Hz grid, so the grid accounts for the
  // whole window and anything off it would show up as unaccounted power.
  constexpr double kRate = 44100.0;
  constexpr double kGridHz = 100.0;
  constexpr double kBandHz = 20000.0;
  for (const Circuit& circuit : kCircuits) {
    const std::vector<float> window = render_probe(
        circuit.name, R"({"gainDb":)" + std::to_string(circuit.max_gain_db) + "}", kRate, 0.5f);
    const double fundamental = power_at(window, kProbeHz, kRate);
    double total = 0.0;
    for (const float sample : window) total += static_cast<double>(sample) * sample;
    total /= static_cast<double>(window.size());
    double on_grid = 0.0;
    double inharmonic = 0.0;
    for (int m = 1; m * kGridHz < kRate * 0.5; ++m) {
      const double power = power_at(window, m * kGridHz, kRate);
      on_grid += power;
      if (m % 10 != 0 && m * kGridHz <= kBandHz) inharmonic += power;
    }
    const double inharmonic_dbc = 10.0 * std::log10(inharmonic / fundamental + 1e-30);
    CAPTURE(circuit.name, fundamental, total, on_grid, inharmonic_dbc);
    CHECK(on_grid >= total * 0.999);
    CHECK(inharmonic_dbc <= -50.0);
  }
}

TEST_CASE("a non-finite input sample does not outlive its block",
          "[mastering][saturation][pedal]") {
  constexpr double kRate = 48000.0;
  constexpr int kBlocks = 40;
  constexpr int kPoisonBlock = 2;
  constexpr int kPoisonIndex = 100;
  const float poisons[] = {std::numeric_limits<float>::quiet_NaN(),
                           std::numeric_limits<float>::infinity(),
                           -std::numeric_limits<float>::infinity()};
  for (const Circuit& circuit : kCircuits) {
    for (const float poison : poisons) {
      CAPTURE(circuit.name, poison);
      const std::vector<float> input = sine(kProbeHz, 0.3f, kRate, kBlocks * kBlockSize);
      std::vector<float> clean = input;
      run_blocks(*build(circuit.name, "{}", kRate), clean);

      std::vector<float> poisoned = input;
      poisoned[static_cast<size_t>(kPoisonBlock * kBlockSize + kPoisonIndex)] = poison;
      auto processor = build(circuit.name, "{}", kRate);
      run_blocks(*processor, poisoned);
      CHECK(processor->non_finite_discard_count() > 0);

      const auto recovered = poisoned.begin() + (kPoisonBlock + 1) * kBlockSize;
      CHECK(std::all_of(recovered, poisoned.end(), [](float s) { return std::isfinite(s); }));
      // The final block has forgotten the restart: the trajectories converge.
      float deviation = 0.0f;
      for (size_t i = (kBlocks - 1) * kBlockSize; i < poisoned.size(); ++i) {
        deviation = std::max(deviation, std::abs(poisoned[i] - clean[i]));
      }
      CAPTURE(deviation);
      CHECK(deviation < 1.0e-3f);
    }
  }
}

TEST_CASE("distortion clips harder than overdrive at the same gain",
          "[mastering][saturation][pedal]") {
  // One shared tone inside both ranges, so the comparison is the clipping
  // stage and not the tone control; gains where both stages clip the probe.
  constexpr double kRate = 48000.0;
  for (const double gain_db : {30.0, 36.0, 41.0}) {
    const std::string json = params(gain_db, 8000.0);
    const double overdrive = harmonic_ratio(render_probe(kOverdrive, json, kRate), kRate, 3);
    const double distortion = harmonic_ratio(render_probe(kDistortion, json, kRate), kRate, 3);
    CAPTURE(gain_db, overdrive, distortion);
    CHECK(distortion > overdrive);
  }
}

TEST_CASE("pedal controls are realtime parameters and refuse values outside their range",
          "[mastering][saturation][pedal]") {
  for (const Circuit& circuit : kCircuits) {
    CAPTURE(circuit.name);
    auto processor = build(circuit.name, "{}", 48000.0);
    const auto descriptors = processor->parameter_descriptors();
    for (const char* key : {"gainDb", "toneHz", "levelDb"}) {
      CAPTURE(key);
      const auto it = std::find_if(descriptors.begin(), descriptors.end(),
                                   [&](const auto& d) { return d.key == key; });
      REQUIRE(it != descriptors.end());
      CHECK(processor->parameter_is_realtime_safe(it->id));
      CHECK(processor->set_parameter(it->id, 6.0f));
    }
    // Gain past its range is clamped into it rather than refused.
    CHECK(processor->set_parameter(descriptors.front().id, 1000.0f));
    CHECK_THROWS(make_insert(circuit.name, R"({"gainDb":-1})"));
    CHECK_THROWS(make_insert(circuit.name,
                             R"({"gainDb":)" + std::to_string(circuit.max_gain_db + 1.0) + "}"));
    CHECK_THROWS(make_insert(circuit.name, R"({"toneHz":100})"));
    CHECK_THROWS(make_insert(circuit.name,
                             R"({"toneHz":)" + std::to_string(circuit.max_tone_hz * 2.0) + "}"));
  }
}
