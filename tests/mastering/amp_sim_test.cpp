// Guitar amp-sim insert (mastering/saturation/amp_sim): monotonic distortion
// vs the drive knob, cab-EQ top-end roll-off, tone-stack wiring through the
// insert factory and deterministic rendering.

#include "mastering/saturation/amp_sim.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <memory>
#include <vector>

#include "core/fft.h"
#include "mastering/api/insert_factory.h"
#include "mastering/api/named_processor.h"

namespace {

using sonare::mastering::api::apply_named_processor;
using sonare::mastering::api::insert_factory_names;
using sonare::mastering::api::make_insert;
using sonare::mastering::api::processor_names;
using sonare::mastering::saturation::AmpSim;
using sonare::mastering::saturation::AmpSimConfig;

constexpr double kRate = 48000.0;
constexpr int kFft = 8192;
constexpr int kNumSamples = 16384;

std::vector<float> sine(double freq_hz, float amplitude, int num_samples) {
  std::vector<float> buf(static_cast<size_t>(num_samples));
  for (int i = 0; i < num_samples; ++i) {
    buf[static_cast<size_t>(i)] =
        amplitude * static_cast<float>(std::sin(2.0 * 3.14159265358979 * freq_hz * i / kRate));
  }
  return buf;
}

std::vector<float> process_mono(sonare::rt::ProcessorBase& processor, std::vector<float> input) {
  processor.prepare(kRate, 512);
  for (size_t off = 0; off + 512 <= input.size(); off += 512) {
    float* block[1] = {input.data() + off};
    processor.process(block, 1, 512);
  }
  return input;
}

std::vector<double> power_spectrum(const std::vector<float>& buf, size_t from) {
  std::vector<float> windowed(kFft);
  for (int i = 0; i < kFft; ++i) {
    const double w = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979 * i / (kFft - 1));
    windowed[static_cast<size_t>(i)] = buf[from + static_cast<size_t>(i)] * static_cast<float>(w);
  }
  sonare::FFT fft(kFft);
  std::vector<std::complex<float>> spectrum(static_cast<size_t>(fft.n_bins()));
  fft.forward(windowed.data(), spectrum.data());
  std::vector<double> power(spectrum.size());
  for (size_t i = 0; i < spectrum.size(); ++i) power[i] = std::norm(spectrum[i]);
  return power;
}

/// Total harmonic distortion proxy: power outside +-3 bins of the fundamental
/// over total power (level-invariant).
double thd(const std::vector<float>& buf, double f0) {
  const std::vector<double> power = power_spectrum(buf, buf.size() - kFft);
  const int centre = static_cast<int>(std::lround(f0 / kRate * kFft));
  double fundamental = 0.0;
  double other = 0.0;
  for (int b = 2; b < static_cast<int>(power.size()); ++b) {
    if (std::abs(b - centre) <= 3) {
      fundamental += power[static_cast<size_t>(b)];
    } else {
      other += power[static_cast<size_t>(b)];
    }
  }
  const double total = fundamental + other;
  return total > 0.0 ? other / total : 0.0;
}

/// Fraction of spectral power above @p freq_hz.
double high_band_fraction(const std::vector<float>& buf, double freq_hz) {
  const std::vector<double> power = power_spectrum(buf, buf.size() - kFft);
  const int split = static_cast<int>(std::lround(freq_hz / kRate * kFft));
  double low = 0.0;
  double high = 0.0;
  for (int b = 1; b < static_cast<int>(power.size()); ++b) {
    (b >= split ? high : low) += power[static_cast<size_t>(b)];
  }
  const double total = low + high;
  return total > 0.0 ? high / total : 0.0;
}

}  // namespace

TEST_CASE("amp-sim distortion grows monotonically with drive", "[mastering][saturation][amp]") {
  // Cab off so the measurement is the nonlinearity alone.
  double previous = -1.0;
  for (float drive : {0.0f, 0.3f, 0.6f, 0.9f}) {
    AmpSimConfig config;
    config.drive = drive;
    config.cab = false;
    AmpSim amp(config);
    const std::vector<float> out = process_mono(amp, sine(220.0, 0.3f, kNumSamples));
    const double distortion = thd(out, 220.0);
    REQUIRE(distortion > previous);
    previous = distortion;
  }
  // The top of the range must be genuinely saturated, not just warm.
  REQUIRE(previous > 0.05);
}

TEST_CASE("the cab-EQ rolls off the top end", "[mastering][saturation][amp]") {
  AmpSimConfig with_cab;
  with_cab.drive = 0.7f;
  AmpSimConfig di = with_cab;
  di.cab = false;

  AmpSim cab_amp(with_cab);
  AmpSim di_amp(di);
  const std::vector<float> cab_out = process_mono(cab_amp, sine(220.0, 0.3f, kNumSamples));
  const std::vector<float> di_out = process_mono(di_amp, sine(220.0, 0.3f, kNumSamples));
  // The 4th-order 4.8 kHz roll-off must strip most of the >6 kHz harmonics.
  REQUIRE(high_band_fraction(cab_out, 6000.0) < 0.2 * high_band_fraction(di_out, 6000.0));
}

TEST_CASE("the tone stack shapes the spectrum", "[mastering][saturation][amp]") {
  AmpSimConfig dark;
  dark.drive = 0.4f;
  dark.treble_db = -12.0f;
  AmpSimConfig bright = dark;
  bright.treble_db = 12.0f;

  AmpSim dark_amp(dark);
  AmpSim bright_amp(bright);
  const std::vector<float> dark_out = process_mono(dark_amp, sine(220.0, 0.3f, kNumSamples));
  const std::vector<float> bright_out = process_mono(bright_amp, sine(220.0, 0.3f, kNumSamples));
  REQUIRE(high_band_fraction(bright_out, 3000.0) > 2.0 * high_band_fraction(dark_out, 3000.0));
}

TEST_CASE("amp-sim renders deterministically", "[mastering][saturation][amp]") {
  AmpSimConfig config;
  config.drive = 0.6f;
  AmpSim first(config);
  AmpSim second(config);
  const std::vector<float> a = process_mono(first, sine(330.0, 0.25f, kNumSamples));
  const std::vector<float> b = process_mono(second, sine(330.0, 0.25f, kNumSamples));
  REQUIRE(a == b);
}

TEST_CASE("saturation.ampSim builds through the insert factory",
          "[mastering][saturation][amp][insert_factory]") {
  const auto names = insert_factory_names();
  bool listed = false;
  for (const auto& name : names) listed |= name == "saturation.ampSim";
  REQUIRE(listed);

  auto processor = make_insert(
      "saturation.ampSim",
      R"({"drive":0.7,"bassDb":2,"midDb":-3,"trebleDb":1.5,"presenceDb":3,"cab":true,"levelDb":-6})");
  REQUIRE(processor != nullptr);
  auto* amp = dynamic_cast<AmpSim*>(processor.get());
  REQUIRE(amp != nullptr);
  REQUIRE(amp->amp_config().drive == 0.7f);
  REQUIRE(amp->amp_config().bass_db == 2.0f);
  REQUIRE(amp->amp_config().mid_db == -3.0f);
  REQUIRE(amp->amp_config().treble_db == 1.5f);
  REQUIRE(amp->amp_config().presence_db == 3.0f);
  REQUIRE(amp->amp_config().cab);
  REQUIRE(amp->amp_config().level_db == -6.0f);
}

TEST_CASE("saturation.ampSim is reachable through offline named processing",
          "[mastering][saturation][amp][named_processor]") {
  const auto names = processor_names();
  bool listed = false;
  for (const auto& name : names) listed |= name == "saturation.ampSim";
  REQUIRE(listed);

  const std::vector<float> input = sine(220.0, 0.3f, kNumSamples);
  const auto result = apply_named_processor("saturation.ampSim", input.data(), input.size(),
                                            static_cast<int>(kRate),
                                            {{"drive", 0.8},
                                             {"bassDb", 2.0},
                                             {"midDb", -3.0},
                                             {"trebleDb", 1.5},
                                             {"presenceDb", 3.0},
                                             {"cab", 1.0},
                                             {"levelDb", -6.0}});

  REQUIRE(result.sample_rate == static_cast<int>(kRate));
  REQUIRE(result.samples.size() == input.size());
  REQUIRE(result.latency_samples == 0);
  REQUIRE(result.samples != input);
  for (const float sample : result.samples) {
    REQUIRE(std::isfinite(sample));
  }
}
