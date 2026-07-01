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

TEST_CASE("amp-sim power stage compresses and is off by default", "[mastering][saturation][amp]") {
  // Cab off, a hot preamp so the power section is driven into saturation.
  // power == 0 is bit-identical to a preamp-only amp (every other test in this
  // file runs at the default power of 0).
  AmpSimConfig base;
  base.cab = false;
  base.drive = 0.5f;
  base.power = 0.0f;
  AmpSimConfig powered = base;
  powered.power = 0.8f;

  AmpSim off_amp(base);
  AmpSim on_amp(powered);
  const std::vector<float> off_out = process_mono(off_amp, sine(220.0, 0.6f, kNumSamples));
  const std::vector<float> on_out = process_mono(on_amp, sine(220.0, 0.6f, kNumSamples));

  auto rms_window = [](const std::vector<float>& b, size_t from, size_t to) {
    double acc = 0.0;
    size_t n = 0;
    for (size_t i = from; i < to && i < b.size(); ++i) {
      acc += static_cast<double>(b[i]) * b[i];
      ++n;
    }
    return n > 0 ? std::sqrt(acc / static_cast<double>(n)) : 0.0;
  };
  auto peak = [](const std::vector<float>& buf) {
    float p = 0.0f;
    for (float s : buf) {
      if (std::fabs(s) > p) p = std::fabs(s);
    }
    return p;
  };
  const size_t n = on_out.size();
  // The gain-compensated push-pull saturation reshapes the tone and compresses
  // a hot signal (the RMS drops); it stays finite and non-silent. (RMS is
  // group-delay invariant, unlike an absolute-peak comparison across the
  // 0.5-sample ADAA delay.)
  REQUIRE(on_out != off_out);
  REQUIRE(rms_window(on_out, n - 8192, n) < rms_window(off_out, n - 8192, n));
  REQUIRE(std::isfinite(peak(on_out)));
  REQUIRE(peak(on_out) > 0.0f);
  REQUIRE(peak(on_out) < 4.0f);
}

TEST_CASE("amp-sim power-supply sag compresses the sustain and is off by default",
          "[mastering][saturation][amp]") {
  // Cab off, moderate drive. A sustained tone lets the rail droop. sag == 0 is
  // bit-identical to a stiff supply (every other test runs at the default sag).
  AmpSimConfig base;
  base.cab = false;
  base.drive = 0.3f;
  base.sag = 0.0f;
  AmpSimConfig sagging = base;
  sagging.sag = 0.9f;

  AmpSim off_amp(base);
  AmpSim on_amp(sagging);
  const std::vector<float> off_out = process_mono(off_amp, sine(220.0, 0.6f, kNumSamples));
  const std::vector<float> on_out = process_mono(on_amp, sine(220.0, 0.6f, kNumSamples));

  auto rms_window = [](const std::vector<float>& b, size_t from, size_t to) {
    double acc = 0.0;
    size_t n = 0;
    for (size_t i = from; i < to && i < b.size(); ++i) {
      acc += static_cast<double>(b[i]) * b[i];
      ++n;
    }
    return n > 0 ? std::sqrt(acc / static_cast<double>(n)) : 0.0;
  };
  const size_t n = on_out.size();
  // The rail droops under sustained draw: the settled tail compresses.
  REQUIRE(on_out != off_out);
  REQUIRE(rms_window(on_out, n - 4096, n) < rms_window(off_out, n - 4096, n));
  // Bloom: the tail sags relative to the initial attack more with sag than
  // without (a stiff supply holds a steady level).
  const double on_ratio = rms_window(on_out, n - 4096, n) / rms_window(on_out, 0, 2048);
  const double off_ratio = rms_window(off_out, n - 4096, n) / rms_window(off_out, 0, 2048);
  REQUIRE(on_ratio < off_ratio);
  bool all_finite = true;
  for (float s : on_out) all_finite = all_finite && std::isfinite(s);
  REQUIRE(all_finite);
}

TEST_CASE("amp-sim output transformer saturates the low band only and is off by default",
          "[mastering][saturation][amp]") {
  // Cab off, clean preamp. transformer == 0 is bit-identical to a linear
  // transformer (every other test runs at the default of 0).
  AmpSimConfig base;
  base.cab = false;
  base.drive = 0.2f;
  base.transformer = 0.0f;
  AmpSimConfig xf = base;
  xf.transformer = 0.9f;

  auto rel_change = [](const std::vector<float>& a, const std::vector<float>& b) {
    double num = 0.0;
    double den = 0.0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
      const double d = static_cast<double>(a[i]) - b[i];
      num += d * d;
      den += static_cast<double>(b[i]) * b[i];
    }
    return den > 0.0 ? std::sqrt(num / den) : 0.0;
  };

  // Low tone: the core magnetises with the flux, so the low band is reshaped.
  AmpSim off_lo(base);
  AmpSim on_lo(xf);
  const std::vector<float> lo_off = process_mono(off_lo, sine(60.0, 0.7f, kNumSamples));
  const std::vector<float> lo_on = process_mono(on_lo, sine(60.0, 0.7f, kNumSamples));
  REQUIRE(lo_on != lo_off);

  // High tone: above the transformer corner, so it passes almost unchanged —
  // the defining property is a frequency-dependent nonlinearity (low band only).
  // The low tone must be reshaped far more than the high tone.
  AmpSim off_hi(base);
  AmpSim on_hi(xf);
  const std::vector<float> hi_off = process_mono(off_hi, sine(3000.0, 0.7f, kNumSamples));
  const std::vector<float> hi_on = process_mono(on_hi, sine(3000.0, 0.7f, kNumSamples));
  REQUIRE(rel_change(lo_on, lo_off) > 5.0 * rel_change(hi_on, hi_off));
  bool all_finite = true;
  for (float s : lo_on) all_finite = all_finite && std::isfinite(s);
  REQUIRE(all_finite);
}
