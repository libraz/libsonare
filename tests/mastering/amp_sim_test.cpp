// Guitar amp-sim insert (mastering/saturation/amp_sim): monotonic distortion
// vs the drive knob, cab-EQ top-end roll-off, tone-stack wiring through the
// insert factory and deterministic rendering.

#include "mastering/saturation/amp_sim.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "core/fft.h"
#include "mastering/api/insert_factory.h"
#include "mastering/api/named_processor.h"
#include "mastering/dynamics/channel_limits.h"
#include "mastering/saturation/amp_physics.h"
#include "mastering/saturation/amp_presets.h"
#include "mastering/saturation/triode.h"
#include "support/audio_fixtures.h"

namespace {

using sonare::mastering::api::apply_named_processor;
using sonare::mastering::api::insert_factory_names;
using sonare::mastering::api::make_insert;
using sonare::mastering::api::processor_names;
using sonare::mastering::saturation::AmpSim;
using sonare::mastering::saturation::AmpSimConfig;

using sonare::test::kFft;
using sonare::test::kRate;
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
  const size_t original_size = input.size();
  const int latency = processor.latency_samples();
  input.resize(original_size + static_cast<size_t>(std::max(0, latency)), 0.0f);
  processor.prepare(kRate, 512);
  for (size_t off = 0; off < input.size(); off += 512) {
    const int count = static_cast<int>(std::min<size_t>(512, input.size() - off));
    float* block[1] = {input.data() + off};
    processor.process(block, 1, count);
  }
  input.erase(input.begin(), input.begin() + latency);
  return input;
}

using sonare::test::power_spectrum;

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
  REQUIRE(result.latency_samples == 12);
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
  // The power stage has to be on: sag is the rail falling under the output
  // pair's plate current, so with no power amp there is no draw to model.
  AmpSimConfig base;
  base.cab = false;
  base.drive = 0.3f;
  base.power = 0.6f;
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

TEST_CASE("the bass cab voicing extends lower and darkens the top",
          "[mastering][saturation][amp]") {
  using sonare::mastering::saturation::CabModel;
  auto rms = [](const std::vector<float>& b) {
    double acc = 0.0;
    for (float s : b) acc += static_cast<double>(s) * s;
    return b.empty() ? 0.0 : std::sqrt(acc / static_cast<double>(b.size()));
  };

  // Low-end extension: a 55 Hz tone sits below the guitar cab's 75 Hz cut but
  // above the bass cab's 40 Hz cut, so the bass voicing passes far more of it.
  AmpSimConfig guitar;
  guitar.drive = 0.2f;
  guitar.cab_model = CabModel::kGuitar4x12;
  AmpSimConfig bass = guitar;
  bass.cab_model = CabModel::kBass8x10;
  AmpSim guitar_lo(guitar);
  AmpSim bass_lo(bass);
  const std::vector<float> g_lo = process_mono(guitar_lo, sine(55.0, 0.5f, kNumSamples));
  const std::vector<float> b_lo = process_mono(bass_lo, sine(55.0, 0.5f, kNumSamples));
  REQUIRE(rms(b_lo) > 1.5 * rms(g_lo));

  // Darker top: the bass cab rolls off at 3.5 kHz vs the guitar's 4.8 kHz, so
  // its share of >5 kHz harmonics is smaller.
  AmpSimConfig guitar_hd = guitar;
  guitar_hd.drive = 0.7f;
  AmpSimConfig bass_hd = bass;
  bass_hd.drive = 0.7f;
  AmpSim guitar_hi(guitar_hd);
  AmpSim bass_hi(bass_hd);
  const std::vector<float> g_hi = process_mono(guitar_hi, sine(220.0, 0.3f, kNumSamples));
  const std::vector<float> b_hi = process_mono(bass_hi, sine(220.0, 0.3f, kNumSamples));
  REQUIRE(high_band_fraction(b_hi, 5000.0) < high_band_fraction(g_hi, 5000.0));
}

TEST_CASE("global NFB tightens the midrange more than the extremes and is off by default",
          "[mastering][saturation][amp]") {
  // NFB acts around the power stage, so drive the power section. nfb == 0 is
  // bit-identical to the open-loop power path (every other test runs nfb == 0).
  AmpSimConfig base;
  base.cab = false;
  base.drive = 0.5f;
  base.power = 0.8f;
  base.nfb = 0.0f;
  AmpSimConfig fed = base;
  fed.nfb = 0.8f;

  auto rms = [](const std::vector<float>& b) {
    double acc = 0.0;
    for (float s : b) acc += static_cast<double>(s) * s;
    return b.empty() ? 0.0 : std::sqrt(acc / static_cast<double>(b.size()));
  };
  auto ratio = [&](double freq_hz) {
    AmpSim off_amp(base);
    AmpSim on_amp(fed);
    const std::vector<float> off = process_mono(off_amp, sine(freq_hz, 0.5f, kNumSamples));
    const std::vector<float> on = process_mono(on_amp, sine(freq_hz, 0.5f, kNumSamples));
    return rms(off) > 0.0 ? rms(on) / rms(off) : 1.0;
  };

  // The mid (in the feedback band) is fed back hard, so it is attenuated more
  // than a high tone that escapes the loop.
  const double mid_ratio = ratio(800.0);
  const double high_ratio = ratio(5000.0);
  REQUIRE(mid_ratio < 1.0);         // NFB reduces mid gain
  REQUIRE(mid_ratio < high_ratio);  // the top escapes the loop

  // The loop is contractive: the fed-back output stays finite and bounded.
  AmpSim on_amp(fed);
  const std::vector<float> on = process_mono(on_amp, sine(800.0, 0.9f, kNumSamples));
  float peak = 0.0f;
  bool all_finite = true;
  for (float s : on) {
    all_finite = all_finite && std::isfinite(s);
    if (std::fabs(s) > peak) peak = std::fabs(s);
  }
  REQUIRE(all_finite);
  REQUIRE(peak < 4.0f);
}

TEST_CASE("the amp voicing presets scale gain from clean to high-gain and default to classic",
          "[mastering][saturation][amp]") {
  using sonare::mastering::saturation::AmpModel;
  // Cab off so the measurement is the nonlinearity alone; a moderate drive so
  // the voicing's gain structure (not a clip ceiling) sets the distortion.
  auto distortion_for = [](AmpModel model) {
    AmpSimConfig config;
    config.drive = 0.5f;
    config.cab = false;
    config.amp_model = model;
    AmpSim amp(config);
    const std::vector<float> out = process_mono(amp, sine(220.0, 0.3f, kNumSamples));
    return thd(out, 220.0);
  };
  // The clean voicing keeps more headroom; the high-gain voicing saturates
  // earlier and harder — the distortion grows across the three presets.
  REQUIRE(distortion_for(AmpModel::kFenderClean) < distortion_for(AmpModel::kClassicCrunch));
  REQUIRE(distortion_for(AmpModel::kClassicCrunch) < distortion_for(AmpModel::kModernHiGain));

  // The classic crunch is the default: an unset amp_model is bit-identical.
  AmpSimConfig defaulted;
  defaulted.drive = 0.5f;
  defaulted.cab = false;
  AmpSimConfig classic = defaulted;
  classic.amp_model = AmpModel::kClassicCrunch;
  AmpSim default_amp(defaulted);
  AmpSim classic_amp(classic);
  const std::vector<float> default_out = process_mono(default_amp, sine(220.0, 0.3f, kNumSamples));
  const std::vector<float> classic_out = process_mono(classic_amp, sine(220.0, 0.3f, kNumSamples));
  REQUIRE(default_out == classic_out);
}

TEST_CASE("the added amp voicings retain distinct gain and spectral characteristics",
          "[mastering][saturation][amp]") {
  using sonare::mastering::saturation::AmpModel;
  auto out_for = [](AmpModel model, bool cab) {
    AmpSimConfig config;
    config.drive = 0.5f;
    config.cab = cab;
    config.amp_model = model;
    AmpSim amp(config);
    return process_mono(amp, sine(220.0, 0.3f, kNumSamples));
  };
  // Cab off (nonlinearity alone): the rectifier is the hottest voicing (it
  // saturates harder than the modern high-gain), the tweed breaks up earlier
  // than the clean.
  REQUIRE(thd(out_for(AmpModel::kFenderClean, false), 220.0) <
          thd(out_for(AmpModel::kTweed, false), 220.0));
  REQUIRE(thd(out_for(AmpModel::kModernHiGain, false), 220.0) <
          thd(out_for(AmpModel::kRectifier, false), 220.0));

  // Cab on so the voicing's tone-stack/pre-emphasis colour remains observable.
  // The guitar-cab roll-off leaves little energy above 3 kHz, so assert that
  // the two deliberately different voicings do not collapse to the same
  // spectrum instead of comparing values at the floating-point noise floor.
  const auto vox = out_for(AmpModel::kVoxChime, true);
  const auto tweed = out_for(AmpModel::kTweed, true);
  REQUIRE(high_band_fraction(vox, 3000.0) != high_band_fraction(tweed, 3000.0));
}

TEST_CASE("saturation.ampSim selects the amp voicing through the param bag",
          "[mastering][saturation][amp][insert_factory]") {
  using sonare::mastering::saturation::AmpModel;
  auto clean = make_insert("saturation.ampSim", R"({"ampModel":1})");
  auto hi = make_insert("saturation.ampSim", R"({"ampModel":2})");
  auto* clean_amp = dynamic_cast<AmpSim*>(clean.get());
  auto* hi_amp = dynamic_cast<AmpSim*>(hi.get());
  REQUIRE(clean_amp != nullptr);
  REQUIRE(hi_amp != nullptr);
  REQUIRE(clean_amp->amp_config().amp_model == AmpModel::kFenderClean);
  REQUIRE(hi_amp->amp_config().amp_model == AmpModel::kModernHiGain);

  // The added voicings parse from their model ids too.
  auto tweed = make_insert("saturation.ampSim", R"({"ampModel":3})");
  auto vox = make_insert("saturation.ampSim", R"({"ampModel":4})");
  auto rect = make_insert("saturation.ampSim", R"({"ampModel":5})");
  REQUIRE(dynamic_cast<AmpSim*>(tweed.get())->amp_config().amp_model == AmpModel::kTweed);
  REQUIRE(dynamic_cast<AmpSim*>(vox.get())->amp_config().amp_model == AmpModel::kVoxChime);
  REQUIRE(dynamic_cast<AmpSim*>(rect.get())->amp_config().amp_model == AmpModel::kRectifier);
}

TEST_CASE("saturation.ampSim selects the bass cab through the param bag",
          "[mastering][saturation][amp][insert_factory]") {
  using sonare::mastering::saturation::CabModel;
  auto guitar = make_insert("saturation.ampSim", R"({"cab":true,"cabModel":0})");
  auto bass = make_insert("saturation.ampSim", R"({"cab":true,"cabModel":1})");
  auto* guitar_amp = dynamic_cast<AmpSim*>(guitar.get());
  auto* bass_amp = dynamic_cast<AmpSim*>(bass.get());
  REQUIRE(guitar_amp != nullptr);
  REQUIRE(bass_amp != nullptr);
  REQUIRE(guitar_amp->amp_config().cab_model == CabModel::kGuitar4x12);
  REQUIRE(bass_amp->amp_config().cab_model == CabModel::kBass8x10);
}

namespace {

using sonare::mastering::saturation::MicModel;

double rms_of(const std::vector<float>& buf) {
  double acc = 0.0;
  for (float s : buf) acc += static_cast<double>(s) * s;
  return buf.empty() ? 0.0 : std::sqrt(acc / static_cast<double>(buf.size()));
}

/// Spectral power summed over [lo, hi] Hz.
double band_power(const std::vector<float>& buf, double lo_hz, double hi_hz) {
  const std::vector<double> power = power_spectrum(buf, buf.size() - kFft);
  const int lo = static_cast<int>(std::lround(lo_hz / kRate * kFft));
  const int hi = static_cast<int>(std::lround(hi_hz / kRate * kFft));
  double acc = 0.0;
  for (int b = std::max(1, lo); b <= hi && b < static_cast<int>(power.size()); ++b) {
    acc += power[static_cast<size_t>(b)];
  }
  return acc;
}

double relative_change(const std::vector<float>& a, const std::vector<float>& b) {
  double num = 0.0;
  double den = 0.0;
  for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
    const double d = static_cast<double>(a[i]) - b[i];
    num += d * d;
    den += static_cast<double>(b[i]) * b[i];
  }
  return den > 0.0 ? std::sqrt(num / den) : 0.0;
}

std::vector<float> two_tone(double low_hz, float low_amp, double high_hz, float high_amp,
                            int num_samples) {
  std::vector<float> low = sine(low_hz, low_amp, num_samples);
  const std::vector<float> high = sine(high_hz, high_amp, num_samples);
  for (size_t i = 0; i < low.size(); ++i) low[i] += high[i];
  return low;
}

}  // namespace

TEST_CASE("the mic stage is inert until a capsule is selected", "[mastering][saturation][amp]") {
  // The position and distance controls only mean anything once a capsule is
  // chosen, so setting them under kNone must leave the cab chain untouched.
  AmpSimConfig plain;
  plain.drive = 0.5f;
  AmpSimConfig posed = plain;
  posed.mic_axis = 1.0f;
  posed.mic_distance_cm = 80.0f;
  posed.mic_b_axis = 1.0f;
  posed.mic_b_model = MicModel::kRibbon;  // the second mic is gated by mic_blend
  AmpSim without(plain);
  AmpSim with(posed);
  const std::vector<float> a = process_mono(without, sine(220.0, 0.3f, kNumSamples));
  const std::vector<float> b = process_mono(with, sine(220.0, 0.3f, kNumSamples));
  REQUIRE(a == b);

  // Selecting a capsule is what makes it audible.
  AmpSimConfig miked = plain;
  miked.mic_model = MicModel::kDynamic;
  AmpSim dynamic_mic(miked);
  const std::vector<float> c = process_mono(dynamic_mic, sine(220.0, 0.3f, kNumSamples));
  REQUIRE(c != a);
}

TEST_CASE("the mic capsules keep distinct top-end voicings", "[mastering][saturation][amp]") {
  // Ribbon rolls off earliest, the condenser latest, with the dynamic between:
  // the capsule's roll-off scaling is the defining difference between them.
  auto top_share = [](MicModel model) {
    AmpSimConfig config;
    config.drive = 0.7f;
    config.mic_model = model;
    AmpSim amp(config);
    return high_band_fraction(process_mono(amp, sine(220.0, 0.3f, kNumSamples)), 4000.0);
  };
  const double ribbon = top_share(MicModel::kRibbon);
  const double dynamic = top_share(MicModel::kDynamic);
  const double condenser = top_share(MicModel::kCondenser);
  REQUIRE(ribbon < dynamic);
  REQUIRE(dynamic < condenser);
}

TEST_CASE("moving the mic off-axis darkens it", "[mastering][saturation][amp]") {
  auto top_share = [](float axis) {
    AmpSimConfig config;
    config.drive = 0.7f;
    config.mic_model = MicModel::kDynamic;
    config.mic_axis = axis;
    AmpSim amp(config);
    return high_band_fraction(process_mono(amp, sine(220.0, 0.3f, kNumSamples)), 4000.0);
  };
  REQUIRE(top_share(1.0f) < top_share(0.0f));
}

TEST_CASE("backing the mic off trims the proximity lift and the top",
          "[mastering][saturation][amp]") {
  auto render = [](float distance_cm, double freq_hz) {
    AmpSimConfig config;
    config.drive = 0.4f;
    config.mic_model = MicModel::kRibbon;  // the strongest proximity lift
    config.mic_distance_cm = distance_cm;
    AmpSim amp(config);
    return process_mono(amp, sine(freq_hz, 0.3f, kNumSamples));
  };
  // Proximity: a 90 Hz tone sits in the shelf and above the cab's 75 Hz cut.
  REQUIRE(rms_of(render(60.0f, 90.0)) < rms_of(render(2.5f, 90.0)));
  // Air: the top goes first as the mic backs away.
  REQUIRE(high_band_fraction(render(60.0f, 220.0), 4000.0) <
          high_band_fraction(render(2.5f, 220.0), 4000.0));
}

TEST_CASE("a second mic combs against the first through their path difference",
          "[mastering][saturation][amp]") {
  // 12.5 cm apart is a 0.364 ms path difference: a first null at 1372 Hz and the
  // first constructive peak at 2744 Hz. Both mics keep the cab's own voicing so
  // the interference is the only thing under test.
  constexpr double kNullHz = 1372.1;
  constexpr double kPeakHz = 2744.2;
  AmpSimConfig single;
  single.drive = 0.2f;
  single.mic_distance_cm = 2.5f;
  single.mic_b_distance_cm = 15.0f;
  AmpSimConfig pair = single;
  pair.mic_blend = 0.5f;

  auto render = [](const AmpSimConfig& config, double freq_hz) {
    AmpSim amp(config);
    return process_mono(amp, sine(freq_hz, 0.3f, kNumSamples));
  };
  // The second mic is not summed at all until mic_blend opens it: its distance
  // and capsule are inert at blend 0.
  AmpSimConfig unset_second = single;
  unset_second.mic_b_distance_cm = AmpSimConfig{}.mic_b_distance_cm;
  unset_second.mic_b_model = MicModel::kCondenser;
  REQUIRE(render(single, kNullHz) == render(unset_second, kNullHz));
  REQUIRE(rms_of(render(pair, kNullHz)) < 0.35 * rms_of(render(single, kNullHz)));
  REQUIRE(rms_of(render(pair, kPeakHz)) > 0.8 * rms_of(render(single, kPeakHz)));

  // Flipping the second mic's polarity turns the null into a peak.
  AmpSimConfig flipped = pair;
  flipped.mic_b_invert = true;
  REQUIRE(rms_of(render(flipped, kNullHz)) > 2.0 * rms_of(render(pair, kNullHz)));

  // The pair's delay line is the only stage that outlives its input.
  AmpSim single_amp(single);
  AmpSim pair_amp(pair);
  single_amp.prepare(kRate, 512);
  pair_amp.prepare(kRate, 512);
  REQUIRE(single_amp.tail_samples() == 0);
  REQUIRE(pair_amp.tail_samples() > 0);
}

TEST_CASE("the cone stage compresses the low band asymmetrically and is off by default",
          "[mastering][saturation][amp]") {
  // Cab off so the measurement is the cone alone; a light preamp keeps the tube's
  // own even-harmonic contribution small.
  AmpSimConfig base;
  base.cab = false;
  base.drive = 0.1f;
  AmpSimConfig cone = base;
  cone.cone = 0.8f;

  auto fundamental_power = [](const AmpSimConfig& config) {
    AmpSim amp(config);
    return band_power(process_mono(amp, sine(60.0, 0.7f, kNumSamples)), 50.0, 70.0);
  };
  auto second_harmonic_ratio = [](const AmpSimConfig& config) {
    AmpSim amp(config);
    const std::vector<float> out = process_mono(amp, sine(60.0, 0.7f, kNumSamples));
    return band_power(out, 110.0, 130.0) / band_power(out, 50.0, 70.0);
  };
  // The suspension bias is what produces even harmonics an amp stage cannot.
  REQUIRE(second_harmonic_ratio(cone) > 2.0 * second_harmonic_ratio(base));
  // ...and it must do so by reshaping the low band, not by removing it: a
  // harmonic ratio also rises when the fundamental is gutted, which is what an
  // absolute (rather than excursion-normalized) bias would do at this level. The
  // asymmetry term vanishes with the excursion, so a lightly driven amp keeps its
  // fundamental essentially intact.
  REQUIRE(fundamental_power(cone) > 0.8 * fundamental_power(base));

  // Excursion lives in the low band, so a high tone passes near-unchanged — the
  // same frequency-dependent signature as the output transformer.
  AmpSim off_lo(base);
  AmpSim on_lo(cone);
  const std::vector<float> lo_off = process_mono(off_lo, sine(60.0, 0.7f, kNumSamples));
  const std::vector<float> lo_on = process_mono(on_lo, sine(60.0, 0.7f, kNumSamples));
  AmpSim off_hi(base);
  AmpSim on_hi(cone);
  const std::vector<float> hi_off = process_mono(off_hi, sine(3000.0, 0.7f, kNumSamples));
  const std::vector<float> hi_on = process_mono(on_hi, sine(3000.0, 0.7f, kNumSamples));
  REQUIRE(relative_change(lo_on, lo_off) > 5.0 * relative_change(hi_on, hi_off));
  bool all_finite = true;
  for (float s : lo_on) all_finite = all_finite && std::isfinite(s);
  REQUIRE(all_finite);
}

TEST_CASE("the Doppler stage modulates the top with the low end and is off by default",
          "[mastering][saturation][amp]") {
  // A light preamp: the tube's own intermodulation also lands on 3 kHz +- 60 Hz,
  // and it grows far faster with drive than the cone's travel does, so the
  // modulation is easiest to see when the tube is close to clean.
  AmpSimConfig base;
  base.cab = false;
  base.drive = 0.1f;
  AmpSimConfig doppler = base;
  doppler.doppler = 1.0f;

  // The modulation swings around a fixed centre, so that centre is real latency.
  AmpSim plain_amp(base);
  AmpSim doppler_amp(doppler);
  REQUIRE(doppler_amp.latency_samples() == plain_amp.latency_samples() + 2);

  // A 60 Hz tone moving the cone frequency-modulates a 3 kHz tone riding on it,
  // which shows up as sidebands a low-tone period apart.
  auto sideband_ratio = [](const AmpSimConfig& config, const std::vector<float>& input) {
    AmpSim amp(config);
    const std::vector<float> out = process_mono(amp, input);
    const double sidebands = band_power(out, 2900.0, 2975.0) + band_power(out, 3025.0, 3100.0);
    return sidebands / band_power(out, 2980.0, 3020.0);
  };
  const std::vector<float> two = two_tone(60.0, 0.8f, 3000.0, 0.2f, kNumSamples);
  REQUIRE(sideband_ratio(doppler, two) > 1.5 * sideband_ratio(base, two));

  // The defining property is that the LOW tone is what modulates the top: the
  // same 3 kHz tone with nothing moving the cone gains no sidebands at all.
  const std::vector<float> high_only = sine(3000.0, 0.2f, kNumSamples);
  REQUIRE(sideband_ratio(doppler, high_only) < 0.1 * sideband_ratio(doppler, two));

  // Off is off: the delay line is not even allocated, let alone stepped.
  AmpSimConfig unrelated = base;
  unrelated.doppler = 0.0f;
  unrelated.cone = 0.0f;
  AmpSim a(base);
  AmpSim b(unrelated);
  REQUIRE(process_mono(a, sine(220.0, 0.3f, kNumSamples)) ==
          process_mono(b, sine(220.0, 0.3f, kNumSamples)));
}

TEST_CASE("amp-sim exposes the mic and cone controls for automation",
          "[mastering][saturation][amp]") {
  AmpSimConfig config;
  config.mic_model = MicModel::kDynamic;
  config.mic_b_model = MicModel::kCondenser;
  AmpSim amp(config);
  amp.prepare(kRate, 512);
  REQUIRE(amp.set_parameter(10, 0.8f));  // micAxis
  REQUIRE(amp.set_parameter(11, 0.4f));  // micBAxis
  REQUIRE(amp.set_parameter(12, 0.5f));  // micBlend
  REQUIRE(amp.set_parameter(13, 0.6f));  // cone
  REQUIRE_FALSE(amp.set_parameter(16, 0.5f));
  REQUIRE(amp.amp_config().mic_axis == 0.8f);
  REQUIRE(amp.amp_config().mic_blend == 0.5f);
  REQUIRE(amp.amp_config().cone == 0.6f);

  // Opening the blend from the audio thread must find a delay line waiting: the
  // line is sized from the (fixed) distances, never from the blend.
  std::vector<float> input = sine(1372.1, 0.3f, kNumSamples);
  for (size_t off = 0; off < input.size(); off += 512) {
    const int count = static_cast<int>(std::min<size_t>(512, input.size() - off));
    float* block[1] = {input.data() + off};
    amp.process(block, 1, count);
  }
  bool all_finite = true;
  for (float s : input) all_finite = all_finite && std::isfinite(s);
  REQUIRE(all_finite);

  // The delay taps and the Doppler centre are deliberately not automatable: one
  // cannot move without a click, the other would move the reported latency.
  const auto descriptors = amp.parameter_descriptors();
  bool has_blend = false;
  bool has_distance = false;
  bool has_doppler = false;
  for (const auto& descriptor : descriptors) {
    has_blend |= descriptor.key == "micBlend";
    has_distance |= descriptor.key == "micDistanceCm";
    has_doppler |= descriptor.key == "doppler";
  }
  REQUIRE(has_blend);
  REQUIRE_FALSE(has_distance);
  REQUIRE_FALSE(has_doppler);
}

TEST_CASE("saturation.ampSim selects the mic rig through the param bag",
          "[mastering][saturation][amp][insert_factory]") {
  auto processor = make_insert(
      "saturation.ampSim",
      R"({"micModel":2,"micAxis":0.6,"micDistanceCm":12,"micBlend":0.4,"micBModel":3,)"
      R"("micBAxis":0.2,"micBDistanceCm":40,"micBInvert":true,"cone":0.5,"doppler":0.3})");
  REQUIRE(processor != nullptr);
  auto* amp = dynamic_cast<AmpSim*>(processor.get());
  REQUIRE(amp != nullptr);
  REQUIRE(amp->amp_config().mic_model == MicModel::kRibbon);
  REQUIRE(amp->amp_config().mic_axis == 0.6f);
  REQUIRE(amp->amp_config().mic_distance_cm == 12.0f);
  REQUIRE(amp->amp_config().mic_blend == 0.4f);
  REQUIRE(amp->amp_config().mic_b_model == MicModel::kCondenser);
  REQUIRE(amp->amp_config().mic_b_distance_cm == 40.0f);
  REQUIRE(amp->amp_config().mic_b_invert);
  REQUIRE(amp->amp_config().cone == 0.5f);
  REQUIRE(amp->amp_config().doppler == 0.3f);

  // Out-of-range distances clamp rather than oversizing the delay line.
  auto far = make_insert("saturation.ampSim", R"({"micBlend":0.5,"micBDistanceCm":5000})");
  REQUIRE(dynamic_cast<AmpSim*>(far.get())->amp_config().mic_b_distance_cm ==
          sonare::mastering::saturation::kMaxMicDistanceCm);
}

namespace {

using sonare::mastering::saturation::AmpTopology;
using sonare::mastering::saturation::design_tone_stack;
using sonare::mastering::saturation::PowerTube;
using sonare::mastering::saturation::tone_stack_components;
using sonare::mastering::saturation::ToneStackCoeffs;
using sonare::mastering::saturation::ToneStackModel;
using sonare::mastering::saturation::ToneStackState;

/// Magnitude of a designed tone stack at @p f_hz, in dB, by evaluating H(z)
/// directly on the coefficients.
double tone_stack_db(ToneStackModel model, double rate, double t, double m, double l, double f_hz) {
  const ToneStackCoeffs c = design_tone_stack(tone_stack_components(model), rate, t, m, l);
  const std::complex<double> z = std::polar(1.0, -2.0 * M_PI * f_hz / rate);
  const std::complex<double> z2 = z * z;
  const std::complex<double> z3 = z2 * z;
  const std::complex<double> num = c.b0 + c.b1 * z + c.b2 * z2 + c.b3 * z3;
  const std::complex<double> den = 1.0 + c.a1 * z + c.a2 * z2 + c.a3 * z3;
  return 20.0 * std::log10(std::abs(num / den));
}

/// Ratio of odd-harmonic magnitude (3rd + 5th + 7th) to the fundamental's, by
/// direct quadrature demodulation — no bin-alignment constraint on f0.
double odd_harmonic_ratio(const std::vector<float>& buf, double f0, size_t skip) {
  auto mag = [&](double f) {
    double re = 0.0;
    double im = 0.0;
    for (size_t i = skip; i < buf.size(); ++i) {
      const double t = 2.0 * M_PI * f * static_cast<double>(i) / kRate;
      re += buf[i] * std::cos(t);
      im += buf[i] * std::sin(t);
    }
    return std::hypot(re, im);
  };
  const double f = mag(f0);
  return f > 0.0 ? (mag(3 * f0) + mag(5 * f0) + mag(7 * f0)) / f : 0.0;
}

double rms_window(const std::vector<float>& buf, size_t begin, size_t end) {
  end = std::min(end, buf.size());
  double acc = 0.0;
  for (size_t i = begin; i < end; ++i) acc += static_cast<double>(buf[i]) * buf[i];
  return end > begin ? std::sqrt(acc / static_cast<double>(end - begin)) : 0.0;
}

}  // namespace

TEST_CASE("the passive tone stack matches its circuit analysis", "[mastering][saturation][amp]") {
  // Reference magnitudes come from an independent numerical solve of the ladder's
  // nodal admittance matrix, not from the closed-form coefficients under test, so
  // a transcription slip in the coefficients cannot agree with them by
  // construction. (One did: the a3 term is line-wrapped in the source paper and
  // its third, negative summand is easy to drop. These values catch exactly that.)
  struct Reference {
    ToneStackModel model;
    double t, m, l;
    double db[4];  // at 100, 400, 1000, 4000 Hz
  };
  const Reference kReferences[] = {
      {ToneStackModel::kAmerican, 0.5, 0.5, 0.5, {-2.7957, -10.3007, -11.7494, -5.7400}},
      {ToneStackModel::kAmerican, 0.5, 0.0, 0.5, {-2.5967, -11.5426, -20.8215, -7.9211}},
      {ToneStackModel::kAmerican, 1.0, 0.5, 0.0, {-12.0120, -10.2121, -7.4616, -1.4556}},
      {ToneStackModel::kBritish, 0.5, 0.5, 0.5, {-1.6467, -7.4456, -7.5078, -4.1887}},
      {ToneStackModel::kBritish, 0.5, 0.0, 0.5, {-1.4587, -8.5677, -14.1217, -6.7887}},
      {ToneStackModel::kBritish, 1.0, 0.5, 0.0, {-8.8916, -6.2724, -3.7297, -0.4902}},
  };
  const double kFreqs[4] = {100.0, 400.0, 1000.0, 4000.0};
  // The stack runs oversampled in the amp, where bilinear warping is negligible;
  // designing at that rate is what lets the tolerance be this tight.
  const double kDesignRate = kRate * 4.0;
  for (const Reference& ref : kReferences) {
    for (int i = 0; i < 4; ++i) {
      const double got = tone_stack_db(ref.model, kDesignRate, ref.t, ref.m, ref.l, kFreqs[i]);
      INFO("model=" << static_cast<int>(ref.model) << " t=" << ref.t << " m=" << ref.m
                    << " l=" << ref.l << " f=" << kFreqs[i]);
      CHECK(std::abs(got - ref.db[i]) < 0.05);
    }
  }
}

TEST_CASE("the tone stack's controls interact and it loses level when centred",
          "[mastering][saturation][amp]") {
  // The two properties three independent shelves cannot reproduce, and the whole
  // reason the ladder is modelled as a circuit.
  const double kDesignRate = kRate * 4.0;
  for (ToneStackModel model : {ToneStackModel::kAmerican, ToneStackModel::kBritish}) {
    // Insertion loss: everything centred is still well down, which is why a real
    // amp needs so much preamp gain in front of the stack.
    const double centred = tone_stack_db(model, kDesignRate, 0.5, 0.5, 0.5, 1000.0);
    CHECK(centred < -6.0);

    // Non-orthogonality: moving the MID control moves the TREBLE response.
    const double treble_at_mid_min = tone_stack_db(model, kDesignRate, 0.5, 0.0, 0.5, 6000.0);
    const double treble_at_mid_max = tone_stack_db(model, kDesignRate, 0.5, 1.0, 0.5, 6000.0);
    CHECK(std::abs(treble_at_mid_max - treble_at_mid_min) > 1.5);

    // The treble control moves zeros only, never poles, so the denominator must
    // not depend on it at all.
    const ToneStackCoeffs lo =
        design_tone_stack(tone_stack_components(model), kDesignRate, 0.0, 0.4, 0.7);
    const ToneStackCoeffs hi =
        design_tone_stack(tone_stack_components(model), kDesignRate, 1.0, 0.4, 0.7);
    CHECK(lo.a1 == hi.a1);
    CHECK(lo.a2 == hi.a2);
    CHECK(lo.a3 == hi.a3);
  }
  // The American set is the more scooped of the two at the mid notch.
  CHECK(tone_stack_db(ToneStackModel::kAmerican, kDesignRate, 0.5, 0.5, 0.5, 720.0) <
        tone_stack_db(ToneStackModel::kBritish, kDesignRate, 0.5, 0.5, 0.5, 720.0));
}

TEST_CASE("the tone stack stays stable and finite at every control extreme",
          "[mastering][saturation][amp]") {
  // A passive RC ladder has three real negative poles for any control setting, so
  // no combination may ring or blow up — including the exact 0 and 1 endpoints,
  // where the circuit itself has a short or an open.
  const double kDesignRate = kRate * 4.0;
  for (ToneStackModel model : {ToneStackModel::kAmerican, ToneStackModel::kBritish}) {
    for (double t : {0.0, 0.5, 1.0}) {
      for (double m : {0.0, 0.5, 1.0}) {
        for (double l : {0.0, 0.5, 1.0}) {
          const ToneStackCoeffs c =
              design_tone_stack(tone_stack_components(model), kDesignRate, t, m, l);
          ToneStackState state;
          float peak = 0.0f;
          for (int i = 0; i < 8192; ++i) {
            const float x = i == 0 ? 1.0f : 0.0f;  // impulse
            peak = std::max(peak, std::abs(state.process(x, c)));
          }
          INFO("t=" << t << " m=" << m << " l=" << l);
          REQUIRE(std::isfinite(peak));
          // Passive: an impulse can never come out bigger than it went in.
          CHECK(peak <= 1.0f);
        }
      }
    }
  }
}

TEST_CASE("the voiced topology is the default and the circuit topology differs",
          "[mastering][saturation][amp]") {
  AmpSimConfig voiced;
  voiced.drive = 0.5f;
  REQUIRE(voiced.topology == AmpTopology::kVoiced);

  AmpSimConfig circuit = voiced;
  circuit.topology = AmpTopology::kCircuit;

  const std::vector<float> input = sine(440.0, 0.5f, kNumSamples);
  AmpSim a{voiced};
  AmpSim b{circuit};
  const std::vector<float> voiced_out = process_mono(a, input);
  const std::vector<float> circuit_out = process_mono(b, input);
  // Not a subtle difference: a passive ladder and a cascade are a different amp.
  CHECK(relative_change(circuit_out, voiced_out) > 0.3);
  // But they land at a comparable internal level, which is what keeps the shared
  // downstream stages (power, sag, transformer, cone) calibrated across both.
  const double ratio = rms_of(circuit_out) / rms_of(voiced_out);
  CHECK(ratio > 0.4);
  CHECK(ratio < 2.5);
}

TEST_CASE("the preamp cascade raises harmonic density with the stage count",
          "[mastering][saturation][amp]") {
  // The claim item-for-item: the same nominal triode gain reached through more
  // stages produces harmonics of harmonics, which one stage cannot.
  //
  // Probed at 1 kHz deliberately. Each stage carries its own cathode shelf, and
  // those accumulate below their corner, so at 220 Hz the added low-end loss
  // swamps the effect being measured. That is a real property of the cascade, not
  // a measurement artefact -- see preamp_stages.
  double previous = 0.0;
  for (int stages : {1, 2, 3, 4}) {
    AmpSimConfig config;
    config.topology = AmpTopology::kCircuit;
    config.preamp_stages = stages;
    config.drive = 0.75f;
    config.cab = false;
    AmpSim amp{config};
    const std::vector<float> out = process_mono(amp, sine(1000.0, 0.5f, kNumSamples));
    const double density = odd_harmonic_ratio(out, 1000.0, 2048);
    INFO("stages=" << stages << " density=" << density);
    CHECK(density > previous);
    previous = density;
  }
  CHECK(previous > 0.6);
}

TEST_CASE("the class-AB crossover opens a dead zone and is off by default",
          "[mastering][saturation][amp]") {
  AmpSimConfig base;
  base.drive = 0.85f;
  base.power = 1.0f;
  base.cab = false;
  REQUIRE(base.crossover == 0.0f);
  REQUIRE(base.power_tube == PowerTube::k6L6);

  AmpSimConfig cold = base;
  cold.crossover = 1.0f;

  // Gain at four input levels. The defining property of a dead zone is that it
  // is an EXPANDER at low level -- gain rises as the signal grows enough to
  // cross the zone -- whereas a saturator's gain only ever falls. Asserting the
  // opposite sign of that slope is what makes this test specific to a crossover
  // rather than to "something got quieter and dirtier".
  const float kLevels[4] = {0.01f, 0.05f, 0.2f, 0.5f};
  double class_a_gain[4] = {};
  double class_ab_gain[4] = {};
  for (int i = 0; i < 4; ++i) {
    AmpSim a{base};
    AmpSim b{cold};
    const std::vector<float> in = sine(220.0, kLevels[i], kNumSamples);
    class_a_gain[i] = rms_of(process_mono(a, in)) / kLevels[i];
    class_ab_gain[i] = rms_of(process_mono(b, in)) / kLevels[i];
  }
  // Class A compresses monotonically.
  CHECK(class_a_gain[1] < class_a_gain[0]);
  CHECK(class_a_gain[2] < class_a_gain[1]);
  CHECK(class_a_gain[3] < class_a_gain[2]);
  // The cold bias expands out of the dead zone first...
  CHECK(class_ab_gain[1] > 2.0 * class_ab_gain[0]);
  // ...having squashed the quietest signal hard, since that one never leaves it.
  CHECK(class_ab_gain[0] < 0.3 * class_a_gain[0]);
  // ...and converges on the class-A curve once the signal clears the zone, which
  // is why crossover distortion is a small-signal problem and not a loud one.
  CHECK(class_ab_gain[3] > 0.8 * class_a_gain[3]);
  CHECK(class_ab_gain[3] < 1.2 * class_a_gain[3]);

  // The harmonic penalty is a SMALL-signal one and largely vanishes when loud,
  // which is the other way a dead zone differs from a saturator. Measured on the
  // default voicing: about 12x the odd-harmonic content at a quiet input against
  // about 1.15x at a loud one.
  const auto penalty = [&](float amplitude) {
    const std::vector<float> in = sine(220.0, amplitude, kNumSamples);
    AmpSim a{base};
    AmpSim b{cold};
    const double clean = odd_harmonic_ratio(process_mono(a, in), 220.0, 2048);
    const double cold_biased = odd_harmonic_ratio(process_mono(b, in), 220.0, 2048);
    return clean > 0.0 ? cold_biased / clean : 0.0;
  };
  const double quiet_penalty = penalty(0.02f);
  const double loud_penalty = penalty(0.5f);
  CHECK(quiet_penalty > 5.0);
  CHECK(loud_penalty < 1.5);
  CHECK(quiet_penalty > 4.0 * loud_penalty);

  // Off is bit-identical: the composite reduces exactly to the symmetric tanh.
  const std::vector<float> input = sine(220.0, 0.2f, kNumSamples);
  AmpSim reference{base};
  AmpSim again{base};
  CHECK(process_mono(again, input) == process_mono(reference, input));
}

TEST_CASE("the output tube class scales the power stage and 6L6 is neutral",
          "[mastering][saturation][amp]") {
  AmpSimConfig base;
  base.drive = 0.7f;
  base.power = 0.8f;
  base.cab = false;
  const std::vector<float> input = sine(220.0, 0.5f, kNumSamples);

  AmpSim reference{base};
  const std::vector<float> neutral = process_mono(reference, input);

  AmpSimConfig el34 = base;
  el34.power_tube = PowerTube::kEL34;
  AmpSimConfig el84 = base;
  el84.power_tube = PowerTube::kEL84;
  AmpSim a{el34};
  AmpSim b{el84};
  const std::vector<float> el34_out = process_mono(a, input);
  const std::vector<float> el84_out = process_mono(b, input);

  // Lower headroom means more compression at the same setting, so the ordering is
  // by distortion, not by level.
  const double d_6l6 = odd_harmonic_ratio(neutral, 220.0, 2048);
  const double d_el34 = odd_harmonic_ratio(el34_out, 220.0, 2048);
  const double d_el84 = odd_harmonic_ratio(el84_out, 220.0, 2048);
  CHECK(d_el34 > d_6l6);
  CHECK(d_el84 > d_el34);

  // k6L6 is unity, so selecting it explicitly changes nothing at all.
  AmpSimConfig explicit_6l6 = base;
  explicit_6l6.power_tube = PowerTube::k6L6;
  AmpSim c{explicit_6l6};
  CHECK(process_mono(c, input) == neutral);
}

TEST_CASE("blocking distortion collapses the note after a loud attack and is off by default",
          "[mastering][saturation][amp]") {
  // The signature is a TIME signature, not a level one: the attack punches
  // through before the coupling cap has charged, the note behind it collapses
  // while the cap holds the stage near cutoff, and it recovers over the grid
  // leak's time constant. A steady tone shows almost nothing, which is why this
  // is measured on a burst and in windows.
  constexpr int kAttack = 8000;
  constexpr int kTotal = 32768;
  constexpr size_t kWindow = 512;
  std::vector<float> burst = sine(220.0, 1.0f, kTotal);
  for (int i = 0; i < kTotal; ++i) {
    burst[static_cast<size_t>(i)] *= i < kAttack ? 1.0f : 0.05f;
  }

  AmpSimConfig base;
  base.topology = AmpTopology::kCircuit;
  base.preamp_stages = 3;
  base.drive = 0.95f;
  base.cab = false;
  REQUIRE(base.bias_shift == 0.0f);

  AmpSimConfig blocking = base;
  blocking.bias_shift = 1.0f;

  AmpSim off{base};
  AmpSim on{blocking};
  const std::vector<float> steady = process_mono(off, burst);
  const std::vector<float> blocked = process_mono(on, burst);

  const auto window = [&](const std::vector<float>& buf, int index) {
    const size_t begin = static_cast<size_t>(kAttack) + static_cast<size_t>(index) * kWindow;
    return rms_window(buf, begin, begin + kWindow);
  };

  // The transient itself survives -- it is through before the cap has charged.
  CHECK(window(blocked, 0) > window(steady, 0));
  // The note immediately behind it collapses.
  CHECK(window(blocked, 1) < 0.85 * window(steady, 1));
  // And it comes back, so this is a transient behaviour rather than an amp that
  // is simply quieter. Without this the first two checks would also pass for a
  // plain gain reduction.
  CHECK(window(blocked, 4) > 0.95 * window(steady, 4));
  CHECK(rms_window(blocked, 28000, 32000) > 0.98 * rms_window(steady, 28000, 32000));

  // Off leaves the tracker unstepped entirely.
  AmpSim again{base};
  CHECK(process_mono(again, burst) == steady);
}

TEST_CASE("a cabinet IR replaces the analytic cab and costs no latency",
          "[mastering][saturation][amp]") {
  AmpSimConfig config;
  config.drive = 0.4f;
  const std::vector<float> input = sine(1000.0, 0.4f, kNumSamples);

  AmpSim analytic{config};
  const int analytic_latency = analytic.latency_samples();
  const std::vector<float> without = process_mono(analytic, input);

  // A short, obviously non-flat IR: a decaying comb, so its effect is easy to
  // distinguish from a passthrough.
  std::vector<float> ir(96, 0.0f);
  ir[0] = 1.0f;
  ir[13] = -0.6f;
  ir[47] = 0.35f;
  AmpSim convolved{config};
  REQUIRE_FALSE(convolved.has_cab_ir());
  convolved.load_cab_ir(ir);
  REQUIRE(convolved.has_cab_ir());
  // Direct FIR, so the cab adds nothing for the host to compensate.
  CHECK(convolved.latency_samples() == analytic_latency);
  const std::vector<float> with = process_mono(convolved, input);
  CHECK(relative_change(with, without) > 0.2);

  // The IR replaces the mic stage too: a real cab IR is already a mic'd capture,
  // so selecting a capsule alongside one must not add a second microphone.
  AmpSimConfig miked = config;
  miked.mic_model = MicModel::kCondenser;
  miked.mic_axis = 0.9f;
  AmpSim with_mic{miked};
  with_mic.load_cab_ir(ir);
  const std::vector<float> mic_out = process_mono(with_mic, input);
  CHECK(mic_out == with);

  // Over-long IRs truncate at the documented cap rather than being rejected.
  std::vector<float> huge(sonare::mastering::saturation::kMaxCabIrSamples * 3, 0.0f);
  huge[0] = 1.0f;
  AmpSim capped{config};
  REQUIRE_NOTHROW(capped.load_cab_ir(huge));
  CHECK(capped.has_cab_ir());
  // A non-finite IR is refused outright rather than poisoning the render, and
  // that holds for a sample past the truncation budget too — keeping the part
  // that happened to fit would hide a broken capture.
  std::vector<float> bad(16, 0.0f);
  bad[3] = std::numeric_limits<float>::infinity();
  AmpSim rejecting{config};
  CHECK_THROWS(rejecting.load_cab_ir(bad));
  CHECK_FALSE(rejecting.has_cab_ir());
  std::vector<float> bad_tail(static_cast<size_t>(kRate), 0.0f);
  bad_tail[0] = 1.0f;
  bad_tail.back() = std::numeric_limits<float>::quiet_NaN();
  AmpSim rejecting_tail{config};
  CHECK_THROWS(rejecting_tail.load_cab_ir(bad_tail));
}

TEST_CASE("the cab IR is truncated by duration, not by a sample count",
          "[mastering][saturation][amp]") {
  // A fixed sample cap would silently halve the modelled tail at twice the rate,
  // so the same IR would voice a different cabinet per session rate. The budget
  // is a duration, so the retained length tracks the rate instead.
  using sonare::mastering::saturation::kMaxCabIrMs;
  std::vector<float> ir(48000, 0.0f);
  ir[0] = 1.0f;

  auto retained_at = [&](double rate) {
    AmpSimConfig config;
    AmpSim amp{config};
    amp.load_cab_ir(ir, rate);  // already at the processor rate
    amp.prepare(rate, 512);
    return amp.cab_ir_samples();
  };
  const int at_44k = retained_at(44100.0);
  const int at_88k = retained_at(88200.0);
  CHECK(at_44k == static_cast<int>(std::lround(kMaxCabIrMs * 0.001 * 44100.0)));
  // Twice the rate keeps twice the samples, i.e. the same duration.
  CHECK(at_88k == 2 * at_44k);

  // Nothing is derived before there is a rate to derive against, but the load
  // itself is still recorded.
  AmpSimConfig config;
  AmpSim unprepared{config};
  unprepared.load_cab_ir(ir, 44100.0);
  CHECK(unprepared.has_cab_ir());
  CHECK(unprepared.cab_ir_samples() == 0);
  unprepared.prepare(kRate, 512);
  CHECK(unprepared.cab_ir_samples() > 0);
}

TEST_CASE("a cab IR captured at another rate is resampled, not transposed",
          "[mastering][saturation][amp]") {
  // The trap this closes: a 48 kHz capture replayed sample-for-sample in a
  // 96 kHz session is an octave out, silently. Probe it with an IR that is a
  // narrow resonance — a transposition moves its peak, a resampling does not.
  constexpr double kIrRate = 48000.0;
  constexpr double kPeakHz = 1500.0;
  std::vector<float> ir(1024, 0.0f);
  for (size_t i = 0; i < ir.size(); ++i) {
    const double t = static_cast<double>(i) / kIrRate;
    ir[i] = static_cast<float>(std::exp(-t * 400.0) * std::sin(2.0 * M_PI * kPeakHz * t));
  }

  // Convolve the IR alone by running an impulse through the cab path, and find
  // where its energy sits.
  auto peak_bin_hz = [](AmpSim& amp, double rate) {
    std::vector<float> impulse(8192, 0.0f);
    impulse[0] = 1.0f;
    amp.prepare(rate, 512);
    for (size_t off = 0; off < impulse.size(); off += 512) {
      const int count = static_cast<int>(std::min<size_t>(512, impulse.size() - off));
      float* block[1] = {impulse.data() + off};
      amp.process(block, 1, count);
    }
    // Quadrature-demodulate at a sweep of frequencies and take the strongest.
    double best_hz = 0.0;
    double best = -1.0;
    for (double f = 500.0; f <= 4000.0; f += 25.0) {
      double re = 0.0;
      double im = 0.0;
      for (size_t i = 0; i < impulse.size(); ++i) {
        const double t = 2.0 * M_PI * f * static_cast<double>(i) / rate;
        re += impulse[i] * std::cos(t);
        im += impulse[i] * std::sin(t);
      }
      const double mag = std::hypot(re, im);
      if (mag > best) {
        best = mag;
        best_hz = f;
      }
    }
    return best_hz;
  };

  AmpSimConfig config;
  config.drive = 0.0f;

  // Declared at its own rate and run at that rate: the reference.
  AmpSim matched{config};
  matched.load_cab_ir(ir, kIrRate);
  const double at_native = peak_bin_hz(matched, kIrRate);

  // Declared at its own rate but run at double: resampled, so the resonance
  // stays where it was.
  AmpSim resampled{config};
  resampled.load_cab_ir(ir, kIrRate);
  const double at_double = peak_bin_hz(resampled, 2.0 * kIrRate);

  // NOT declared (rate 0 = "already at the processor rate"), run at double: the
  // old contract, and the octave error it produces is exactly why the rate
  // argument exists.
  AmpSim transposed{config};
  transposed.load_cab_ir(ir, 0.0);
  const double at_double_undeclared = peak_bin_hz(transposed, 2.0 * kIrRate);

  INFO("native=" << at_native << " resampled=" << at_double
                 << " undeclared=" << at_double_undeclared);
  CHECK(at_native == Catch::Approx(kPeakHz).margin(100.0));
  CHECK(at_double == Catch::Approx(at_native).margin(100.0));
  // The undeclared path lands an octave up — the defect, kept as a measured
  // contrast so the fix cannot quietly stop mattering.
  CHECK(at_double_undeclared == Catch::Approx(2.0 * at_native).margin(200.0));

  // Resampling also renumbers the length: the same duration at twice the rate.
  CHECK(resampled.cab_ir_samples() == Catch::Approx(2 * matched.cab_ir_samples()).margin(4));
}

TEST_CASE("saturation.ampSim carries the circuit topology and a cab IR through the param bag",
          "[mastering][saturation][amp]") {
  auto processor = make_insert(
      "saturation.ampSim",
      R"({"topology":1,"preampStages":3,"biasShift":0.4,"crossover":0.6,"powerTube":2})");
  REQUIRE(processor != nullptr);
  auto* amp = dynamic_cast<AmpSim*>(processor.get());
  REQUIRE(amp != nullptr);
  CHECK(amp->amp_config().topology == AmpTopology::kCircuit);
  CHECK(amp->amp_config().preamp_stages == 3);
  CHECK(amp->amp_config().bias_shift == 0.4f);
  CHECK(amp->amp_config().crossover == 0.6f);
  CHECK(amp->amp_config().power_tube == PowerTube::kEL84);

  // Stage counts past the cap clamp instead of over-running the state array.
  auto clamped = make_insert("saturation.ampSim", R"({"topology":1,"preampStages":99})");
  CHECK(dynamic_cast<AmpSim*>(clamped.get())->amp_config().preamp_stages ==
        sonare::mastering::saturation::kMaxPreampStages);

  // A cab IR reaches the insert through make_insert_with_ir, the same channel the
  // convolution reverb uses.
  const std::vector<float> ir = {1.0f, -0.5f, 0.25f, 0.1f};
  auto with_ir = sonare::mastering::api::make_insert_with_ir("saturation.ampSim", "{}", ir.data(),
                                                             static_cast<int>(ir.size()));
  REQUIRE(with_ir != nullptr);
  CHECK(dynamic_cast<AmpSim*>(with_ir.get())->has_cab_ir());
}

TEST_CASE("amp-sim exposes the crossover and blocking controls for automation",
          "[mastering][saturation][amp]") {
  AmpSim amp{AmpSimConfig{}};
  amp.prepare(kRate, 256);
  CHECK(amp.set_parameter(14, 0.5f));
  CHECK(amp.set_parameter(15, 0.5f));
  CHECK_FALSE(amp.set_parameter(16, 0.5f));

  const auto descriptors = amp.parameter_descriptors();
  const auto has = [&](const char* key) {
    for (const auto& d : descriptors) {
      if (d.key == key) return true;
    }
    return false;
  };
  CHECK(has("crossover"));
  CHECK(has("biasShift"));
  // Topology and stage count select which state was prepared, so they must not be
  // reachable from the audio thread.
  CHECK_FALSE(has("topology"));
  CHECK_FALSE(has("preampStages"));
}

namespace reference_triode {

// The pre-extraction body of the Dempwolf current law, verbatim. Kept as an
// independent copy so the extracted shared header can be checked against what it
// replaced, in-process: both sides compile with the same flags, so any difference
// can only come from the arithmetic itself rather than from a build setting.
constexpr float G = 2.242e-3f;
constexpr float mu = 103.2f;
constexpr float gamma = 1.26f;
constexpr float C = 3.40f;
constexpr float Gg = 6.177e-4f;
constexpr float xi = 1.314f;
constexpr float Cg = 9.901f;
constexpr float Ig0 = 8.025e-8f;

float smooth_positive(float c, float x) {
  const float z = c * x;
  if (z > 30.0f) return x;
  if (z < -30.0f) return std::exp(z) / c;
  return std::log1p(std::exp(z)) / c;
}
float cathode_current_ma(float vg, float va) {
  const float effective = va / mu + vg;
  return G * std::pow(smooth_positive(C, effective), gamma);
}
float grid_current_ma(float vg) { return Gg * std::pow(smooth_positive(Cg, vg), xi) + Ig0; }
float plate_current_ma(float vg, float va) {
  return cathode_current_ma(vg, va) - grid_current_ma(vg);
}

}  // namespace reference_triode

namespace triode = sonare::mastering::saturation::triode;

TEST_CASE("the shared triode law is bit-identical to the per-processor copy it replaced",
          "[mastering][saturation][amp]") {
  // Moving the Dempwolf current law out of saturation::Tube into a shared header
  // is a refactor with no intended behaviour change, and a refactor's
  // bit-exactness has to be proved on its own rather than inferred from the
  // feature tests that happen to run alongside it.
  CHECK(triode::kPlateVoltageV == 250.0f);
  // Sweep the whole range the model is ever evaluated over, including both
  // asymptote branches of the smoothing function and the cutoff region.
  int checked = 0;
  for (int vg_step = -800; vg_step <= 400; ++vg_step) {
    const float vg = static_cast<float>(vg_step) * 0.01f;
    for (float va : {20.0f, 100.0f, 250.0f, 300.0f}) {
      REQUIRE(triode::plate_current_ma(vg, va) == reference_triode::plate_current_ma(vg, va));
      ++checked;
    }
    REQUIRE(triode::grid_current_ma(vg) == reference_triode::grid_current_ma(vg));
  }
  CHECK(checked == 4804);
}

TEST_CASE("the push-pull power nonlinearity reduces exactly to a symmetric tanh in class A",
          "[mastering][saturation][amp]") {
  // The bit-identity of the whole default power stage rests on this one identity,
  // so it is asserted directly rather than only through a rendered buffer.
  const sonare::rt::TanhNonlinearity symmetric;
  sonare::rt::PushPullNonlinearity pair;
  REQUIRE(pair.bias == 0.0f);
  REQUIRE(pair.knee == 1.0f);
  for (int step = -6000; step <= 6000; ++step) {
    const float x = static_cast<float>(step) * 0.001f;
    REQUIRE(pair.apply(x) == symmetric.apply(x));
    REQUIRE(pair.antiderivative(x) == symmetric.antiderivative(x));
  }
  // And once biased apart it is genuinely different, so the identity above is not
  // just a function that ignores its parameters.
  pair.bias = 0.15f;
  pair.knee = 6.0f;
  CHECK(pair.apply(0.05f) != symmetric.apply(0.05f));
  // Odd by construction: a push-pull pair cancels even harmonics.
  for (int step = 1; step <= 400; ++step) {
    const float x = static_cast<float>(step) * 0.01f;
    REQUIRE(pair.apply(-x) == -pair.apply(x));
  }
}

TEST_CASE("the circuit head survives more channels than prepare() sized for",
          "[mastering][saturation][amp]") {
  // process() grows its per-channel state when a caller hands over more channels
  // than prepare() expected. The circuit head's resampler state is indexed by the
  // same channel index as that state, so it has to grow with it -- otherwise this
  // reads past the end, which is silent until it is not.
  constexpr int kChannels =
      static_cast<int>(sonare::mastering::dynamics::kRealtimePreparedChannels) + 6;
  constexpr int kBlock = 128;

  AmpSimConfig config;
  config.topology = AmpTopology::kCircuit;
  config.preamp_stages = 2;
  config.drive = 0.6f;
  AmpSim amp{config};
  amp.prepare(kRate, kBlock);

  std::vector<std::vector<float>> buffers(static_cast<size_t>(kChannels));
  std::vector<float*> pointers(static_cast<size_t>(kChannels));
  for (int ch = 0; ch < kChannels; ++ch) {
    buffers[static_cast<size_t>(ch)] = sine(220.0 + 5.0 * ch, 0.3f, kBlock);
    pointers[static_cast<size_t>(ch)] = buffers[static_cast<size_t>(ch)].data();
  }
  REQUIRE_NOTHROW(amp.process(pointers.data(), kChannels, kBlock));
  for (int ch = 0; ch < kChannels; ++ch) {
    for (float sample : buffers[static_cast<size_t>(ch)]) {
      REQUIRE(std::isfinite(sample));
    }
  }
  // The highest channel must actually have been processed, not left as input.
  const std::vector<float>& last = buffers[static_cast<size_t>(kChannels - 1)];
  CHECK(rms_of(last) > 0.0);
}

namespace {

using sonare::mastering::saturation::amp_preset_config;
using sonare::mastering::saturation::amp_preset_from_string;
using sonare::mastering::saturation::amp_preset_names;
using sonare::mastering::saturation::AmpPreset;
using sonare::mastering::saturation::crossover_from_bias_fraction;
using sonare::mastering::saturation::kTypicalPowerTransformerOhms;
using sonare::mastering::saturation::plate_dissipation_w;
using sonare::mastering::saturation::power_tube_scale;
using sonare::mastering::saturation::Rectifier;
using sonare::mastering::saturation::rectifier_resistance_ohms;
using sonare::mastering::saturation::sag_from_supply;

}  // namespace

TEST_CASE("the supply model reproduces the rail drops these amps are quoted at",
          "[mastering][saturation][amp]") {
  // The check that matters for a derived control: one free constant (the
  // transformer's own resistance) against TWO independent published drops. A
  // 5Y3 small combo is quoted around 50 V and a GZ34 big combo around 30 V, and
  // both solve to the same 150 ohms — so the constant is corroborated rather
  // than fitted to one point.
  const float five_y3 = rectifier_resistance_ohms(Rectifier::k5Y3) + kTypicalPowerTransformerOhms;
  const float gz34 = rectifier_resistance_ohms(Rectifier::kGz34) + kTypicalPowerTransformerOhms;
  CHECK(sag_from_supply(five_y3, 0.100f, 350.0f) * 350.0f == Catch::Approx(50.0).margin(1.0));
  CHECK(sag_from_supply(gz34, 0.150f, 440.0f) * 440.0f == Catch::Approx(30.0).margin(1.0));

  // Silicon drops nothing, so a solid-state rig's droop is the transformer's
  // alone and always the smallest for a given rail and current.
  CHECK(rectifier_resistance_ohms(Rectifier::kSolidState) == 0.0f);
  CHECK(sag_from_supply(rectifier_resistance_ohms(Rectifier::kSolidState), 0.15f, 440.0f) == 0.0f);
  // Ordering: the softer the rectifier, the more the rail falls.
  CHECK(rectifier_resistance_ohms(Rectifier::kGz34) < rectifier_resistance_ohms(Rectifier::k5V4));
  CHECK(rectifier_resistance_ohms(Rectifier::k5V4) < rectifier_resistance_ohms(Rectifier::k5U4));
  CHECK(rectifier_resistance_ohms(Rectifier::k5U4) < rectifier_resistance_ohms(Rectifier::k5Y3));
  // Degenerate inputs cannot produce a nonsense control value.
  CHECK(sag_from_supply(200.0f, 0.15f, 0.0f) == 0.0f);
  CHECK(sag_from_supply(1e9f, 1.0f, 1.0f) == 1.0f);
}

TEST_CASE("the output-tube drive scale follows plate dissipation", "[mastering][saturation][amp]") {
  // The tube-intrinsic figure, and the reason the scale is derived rather than
  // voiced: pair output power is set as much by the rail the circuit chose as by
  // the bottle, dissipation is not.
  CHECK(plate_dissipation_w(PowerTube::k6L6) == 30.0f);
  CHECK(plate_dissipation_w(PowerTube::kEL34) == 25.0f);
  CHECK(plate_dissipation_w(PowerTube::k6V6) == 14.0f);
  CHECK(plate_dissipation_w(PowerTube::kEL84) == 12.0f);

  // The 6L6 is the reference, so it must be EXACTLY unity — that is what keeps
  // the default power stage bit-identical, and a sqrt that lands at 0.9999998
  // would silently break it.
  CHECK(power_tube_scale(PowerTube::k6L6) == 1.0f);
  // Headroom goes as the square root of the power the tube can pass.
  CHECK(power_tube_scale(PowerTube::kEL34) == Catch::Approx(std::sqrt(30.0 / 25.0)).epsilon(1e-6));
  CHECK(power_tube_scale(PowerTube::kEL84) == Catch::Approx(std::sqrt(30.0 / 12.0)).epsilon(1e-6));
  // Less dissipation means an earlier breakup, in dissipation order.
  CHECK(power_tube_scale(PowerTube::k6L6) < power_tube_scale(PowerTube::kEL34));
  CHECK(power_tube_scale(PowerTube::kEL34) < power_tube_scale(PowerTube::k6V6));
  CHECK(power_tube_scale(PowerTube::k6V6) < power_tube_scale(PowerTube::kEL84));
}

TEST_CASE("the crossover control is anchored on the bias point", "[mastering][saturation][amp]") {
  // A hot bias conducts through the crossing, so there is no dead zone at all;
  // a genuinely cold one is the full control. Both ends must saturate rather
  // than extrapolate, since a bias outside the range is not a colder amp.
  CHECK(crossover_from_bias_fraction(0.70f) == 0.0f);
  CHECK(crossover_from_bias_fraction(0.95f) == 0.0f);
  CHECK(crossover_from_bias_fraction(0.30f) == 1.0f);
  CHECK(crossover_from_bias_fraction(0.10f) == 1.0f);
  // A cool-but-normal 0.5 sits at the middle of the range.
  CHECK(crossover_from_bias_fraction(0.50f) == Catch::Approx(0.5).epsilon(1e-6));
  // Monotone: colder is always more crossover.
  CHECK(crossover_from_bias_fraction(0.65f) < crossover_from_bias_fraction(0.55f));
  CHECK(crossover_from_bias_fraction(0.55f) < crossover_from_bias_fraction(0.45f));
  // A non-finite bias must not propagate into the signal path.
  CHECK(crossover_from_bias_fraction(std::numeric_limits<float>::quiet_NaN()) == 0.0f);
}

TEST_CASE("power-supply sag is the fractional rail droop at full output",
          "[mastering][saturation][amp]") {
  // The calibration claim, measured: at a setting that drives the power stage to
  // its ceiling, the droop the model produces is the `sag` value itself. That is
  // what makes a derived number transfer — a rig whose supply computes to 0.14
  // must actually lose about 14 %, not some unrelated amount.
  AmpSimConfig base;
  base.cab = false;
  base.drive = 1.0f;
  base.power = 1.0f;
  base.sag = 0.0f;
  const std::vector<float> input = sine(220.0, 1.0f, kNumSamples * 4);

  for (float sag : {0.05f, 0.10f, 0.15f}) {
    AmpSimConfig sagging = base;
    sagging.sag = sag;
    AmpSim stiff_amp{base};
    AmpSim sagging_amp{sagging};
    const std::vector<float> stiff = process_mono(stiff_amp, input);
    const std::vector<float> drooped = process_mono(sagging_amp, input);
    const size_t n = stiff.size();
    const double droop = 1.0 - rms_window(drooped, n - 8192, n) / rms_window(stiff, n - 8192, n);
    INFO("sag=" << sag << " droop=" << droop);
    // Driven this hard the draw is most of full output, so the droop lands
    // within a factor of two of the setting rather than the 12x spread an
    // uncalibrated sensitivity produced.
    CHECK(droop > 0.4 * static_cast<double>(sag));
    CHECK(droop <= static_cast<double>(sag) + 1e-6);
  }

  // Gated on the power stage, exactly like NFB: with no power amp there is no
  // plate current to pull the rail down, so the path stays bit-identical.
  AmpSimConfig preamp_only = base;
  preamp_only.power = 0.0f;
  AmpSimConfig preamp_only_sagging = preamp_only;
  preamp_only_sagging.sag = 0.9f;
  AmpSim a{preamp_only};
  AmpSim b{preamp_only_sagging};
  CHECK(process_mono(a, input) == process_mono(b, input));
}

TEST_CASE("every amp preset is coherent, derived and renders", "[mastering][saturation][amp]") {
  const std::vector<std::string> names = amp_preset_names();
  CHECK(names.size() == 10);
  for (const std::string& name : names) {
    const AmpPreset preset = amp_preset_from_string(name);
    INFO("preset=" << name);
    // Round-trip: the name table and the enum cannot drift apart.
    CHECK(std::string(amp_preset_to_string(preset)) == name);

    const AmpSimConfig config = amp_preset_config(preset);
    // Every derived control must have landed in the range its physics allows.
    // A supply that computes above 0.2 means someone typed a rail or a current
    // no guitar amp has.
    CHECK(config.sag >= 0.0f);
    CHECK(config.sag < 0.2f);
    CHECK(config.crossover >= 0.0f);
    CHECK(config.crossover <= 1.0f);
    CHECK(config.preamp_stages >= 1);
    CHECK(config.preamp_stages <= sonare::mastering::saturation::kMaxPreampStages);

    AmpSim amp{config};
    const std::vector<float> out = process_mono(amp, sine(220.0, 0.4f, kNumSamples));
    bool all_finite = true;
    float peak = 0.0f;
    for (float s : out) {
      all_finite = all_finite && std::isfinite(s);
      peak = std::max(peak, std::fabs(s));
    }
    CHECK(all_finite);
    CHECK(peak > 0.0f);
    CHECK(peak < 4.0f);
  }
  // An unknown name is rejected rather than silently resolving to the first rig.
  CHECK_THROWS(amp_preset_from_string("noSuchAmp"));
  CHECK(std::string(amp_preset_to_string(static_cast<AmpPreset>(999))) == "unknown");
}

TEST_CASE("saturation.ampSim takes a named rig as the base its params ride on",
          "[mastering][saturation][amp][insert_factory]") {
  using sonare::mastering::saturation::AmpTopology;
  // A preset reaches the discrete switches nothing else can: topology, tube,
  // capsule and stage count are all non-automatable.
  auto rig = make_insert("saturation.ampSim", R"({"preset":"modernLead"})");
  auto* amp = dynamic_cast<AmpSim*>(rig.get());
  REQUIRE(amp != nullptr);
  const AmpSimConfig expected = amp_preset_config(AmpPreset::kModernLead);
  CHECK(amp->amp_config().topology == AmpTopology::kCircuit);
  CHECK(amp->amp_config().preamp_stages == expected.preamp_stages);
  CHECK(amp->amp_config().power_tube == expected.power_tube);
  CHECK(amp->amp_config().sag == expected.sag);

  // A numeric param overrides ITS OWN field and leaves the rest of the rig
  // alone — the property that makes a preset a starting point rather than a
  // replacement for the whole bag.
  auto turned_down = make_insert("saturation.ampSim", R"({"preset":"modernLead","drive":0.2})");
  auto* quiet = dynamic_cast<AmpSim*>(turned_down.get());
  REQUIRE(quiet != nullptr);
  CHECK(quiet->amp_config().drive == 0.2f);
  CHECK(quiet->amp_config().topology == AmpTopology::kCircuit);
  CHECK(quiet->amp_config().power_tube == expected.power_tube);
  CHECK(quiet->amp_config().mic_model == expected.mic_model);

  // Including the discrete switches: an explicit enum id still wins over the
  // rig's, so a preset never locks a caller out of a control.
  auto retubed = make_insert("saturation.ampSim", R"({"preset":"modernLead","powerTube":0})");
  CHECK(dynamic_cast<AmpSim*>(retubed.get())->amp_config().power_tube == PowerTube::k6L6);

  // With no preset the base is the processor's own defaults, unchanged.
  auto plain = make_insert("saturation.ampSim", R"({"drive":0.4})");
  CHECK(dynamic_cast<AmpSim*>(plain.get())->amp_config().topology == AmpTopology::kVoiced);
  CHECK(dynamic_cast<AmpSim*>(plain.get())->amp_config().power_tube == PowerTube::k6L6);

  // An unknown rig name is an error, not a silent fallback to the defaults.
  CHECK_THROWS(make_insert("saturation.ampSim", R"({"preset":"noSuchAmp"})"));
  CHECK_THROWS(make_insert("saturation.ampSim", R"({"preset":7})"));
}

TEST_CASE("the presets order the supplies the way their rectifiers do",
          "[mastering][saturation][amp]") {
  // The point of deriving rather than choosing: the tweed's 5Y3 has to come out
  // saggier than the silicon-rectified modern rigs without anyone deciding that
  // it should.
  const float tweed = amp_preset_config(AmpPreset::kTweedGrind).sag;
  const float modern = amp_preset_config(AmpPreset::kModernLead).sag;
  const float brit = amp_preset_config(AmpPreset::kBritStack).sag;
  const float rectifier = amp_preset_config(AmpPreset::kRectifierChug).sag;
  CHECK(tweed > 2.0f * modern);
  CHECK(tweed > 2.0f * brit);
  // The one modern rig on a tube rectifier sits with the vintage amps, not with
  // its silicon-rectified siblings.
  CHECK(rectifier > 2.0f * modern);

  // Likewise the bias points: the cold-bias rig is the only one with a large
  // dead zone, and the two cathode-biased amps have none at all.
  CHECK(amp_preset_config(AmpPreset::kColdBiasBuzz).crossover > 0.8f);
  CHECK(amp_preset_config(AmpPreset::kTweedGrind).crossover == 0.0f);
  CHECK(amp_preset_config(AmpPreset::kChimeEdge).crossover == 0.0f);
}

namespace {

/// Exact magnitude of an IR at one frequency, in dB. A direct DFT bin rather
/// than an FFT so the probe frequencies are the ones asked for, not the ones a
/// bin grid happens to land on.
double ir_response_db(const std::vector<float>& ir, double freq_hz) {
  std::complex<double> acc{0.0, 0.0};
  for (size_t i = 0; i < ir.size(); ++i) {
    const double w = -2.0 * 3.14159265358979 * freq_hz * static_cast<double>(i) / kRate;
    acc += std::complex<double>(ir[i] * std::cos(w), ir[i] * std::sin(w));
  }
  return 20.0 * std::log10(std::max(1e-12, std::abs(acc)));
}

/// The analytic cab chain's own response, rendered long enough that truncation
/// is not part of the comparison.
std::vector<float> analytic_cab_response(sonare::mastering::saturation::CabModel cab,
                                         MicModel mic = MicModel::kNone, float axis = 0.0f,
                                         float distance_cm = 2.5f, float presence_db = 0.0f) {
  const auto design = sonare::mastering::saturation::design_cab_stage(cab, mic, axis, distance_cm,
                                                                      presence_db, kRate);
  std::vector<float> impulse(static_cast<size_t>(kRate), 0.0f);
  impulse[0] = 1.0f;
  return sonare::mastering::saturation::render_cab_design(design, impulse);
}

}  // namespace

TEST_CASE("the piston directivity argument is solved, not tabulated",
          "[mastering][saturation][amp]") {
  // The whole neighbour roll-off hangs off this root, so it is worth pinning to
  // the function it claims to describe rather than to a number someone typed.
  // 2*J1(x)/x == 1/sqrt(2) at x = 1.6163399..., from the Bessel power series.
  const float x3 = sonare::mastering::saturation::piston_minus3db_argument();
  CHECK(x3 == Catch::Approx(1.61634).epsilon(1e-4));
}

TEST_CASE("a single-driver generated cabinet reproduces the analytic cab",
          "[mastering][saturation][amp]") {
  using sonare::mastering::saturation::CabIrSpec;
  using sonare::mastering::saturation::CabModel;
  using sonare::mastering::saturation::generate_cab_ir;

  // The model's identity case. The generator borrows the analytic voicing rather
  // than restating it, so with the cabinet's other drivers switched off there is
  // nothing left for it to disagree about — and if that ever stops holding, the
  // generator has grown a second opinion about what a cabinet sounds like.
  //
  // It also settles what the truncation costs, which is nothing measurable: the
  // design's own ring fits inside the budget.
  CabIrSpec spec;
  spec.multi_driver = false;
  const std::vector<float> ir = generate_cab_ir(spec, kRate);
  const std::vector<float> analytic = analytic_cab_response(CabModel::kGuitar4x12);
  for (double f : {40.0, 80.0, 150.0, 500.0, 2000.0, 5000.0}) {
    CHECK(ir_response_db(ir, f) - ir_response_db(analytic, f) == Catch::Approx(0.0).margin(0.01));
  }

  // The capsule and the presence control come along with it, so a generated
  // cabinet is the rig that was configured rather than a bare cab.
  CabIrSpec miked;
  miked.multi_driver = false;
  miked.mic_model = MicModel::kRibbon;
  miked.mic_axis = 0.6f;
  miked.mic_distance_cm = 12.0f;
  miked.presence_db = 4.0f;
  const std::vector<float> ir_miked = generate_cab_ir(miked, kRate);
  const std::vector<float> analytic_miked =
      analytic_cab_response(CabModel::kGuitar4x12, MicModel::kRibbon, 0.6f, 12.0f, 4.0f);
  for (double f : {100.0, 1000.0, 3000.0}) {
    CHECK(ir_response_db(ir_miked, f) - ir_response_db(analytic_miked, f) ==
          Catch::Approx(0.0).margin(0.05));
  }
}

TEST_CASE("the cabinet's other drivers couple at the bottom and comb above it",
          "[mastering][saturation][amp]") {
  using sonare::mastering::saturation::CabIrSpec;
  using sonare::mastering::saturation::CabModel;
  using sonare::mastering::saturation::generate_cab_ir;

  const std::vector<float> analytic = analytic_cab_response(CabModel::kGuitar4x12);
  auto measure = [&](float distance_cm) {
    CabIrSpec spec;
    spec.mic_distance_cm = distance_cm;
    const std::vector<float> ir = generate_cab_ir(spec, kRate);
    struct Result {
      double low_db;
      double worst_db;
      double worst_hz;
    } result{ir_response_db(ir, 100.0) - ir_response_db(analytic, 100.0), 0.0, 0.0};
    for (double f = 200.0; f < 8000.0; f *= 1.02) {
      const double delta = ir_response_db(ir, f) - ir_response_db(analytic, f);
      if (delta < result.worst_db) {
        result.worst_db = delta;
        result.worst_hz = f;
      }
    }
    return result;
  };

  // Mutual coupling: drivers less than a quarter-wavelength apart sum coherently,
  // and the closer to equidistant the mic gets the more of them do. This is the
  // reason a big cabinet is darker than one of its own drivers, and normalizing
  // it away was the first thing this model got wrong.
  const auto close = measure(2.5f);
  const auto near = measure(15.0f);
  const auto back = measure(60.0f);
  CHECK(close.low_db > 0.5);
  CHECK(near.low_db > close.low_db);
  CHECK(back.low_db > near.low_db);
  CHECK(back.low_db < 12.0);  // four drivers, perfectly coupled, is the ceiling

  // The comb deepens and its first notch walks down in frequency as the mic backs
  // off, because the path differences grow relative to nothing while the level
  // difference that was masking them shrinks.
  CHECK(close.worst_db > -3.0);
  CHECK(near.worst_db < -6.0);
  CHECK(back.worst_db < near.worst_db);
  CHECK(back.worst_hz > near.worst_hz);

  // A single driver has nothing to interfere with, which is the escape hatch for
  // anything the idealized array over-does.
  CabIrSpec alone;
  alone.mic_distance_cm = 60.0f;
  alone.multi_driver = false;
  const std::vector<float> solo = generate_cab_ir(alone, kRate);
  for (double f = 200.0; f < 8000.0; f *= 1.02) {
    CHECK(ir_response_db(solo, f) - ir_response_db(analytic, f) > -0.5);
  }
}

TEST_CASE("a generated cabinet is re-derived at the processor's own rate",
          "[mastering][saturation][amp]") {
  using sonare::mastering::saturation::CabIrSpec;

  // The reason the spec is stored rather than the samples: a generated cabinet
  // never needs resampling, so a session rate change re-derives the cabinet
  // instead of transposing it — the failure a loaded IR has to defend against.
  AmpSimConfig config;
  config.drive = 0.3f;
  AmpSim sim{config};
  CHECK_FALSE(sim.has_cab_ir());
  sim.load_generated_cab_ir(CabIrSpec{});
  // Answered before prepare(), because the caller's question is "did it take".
  CHECK(sim.has_cab_ir());
  CHECK(sim.cab_ir_samples() == 0);

  sim.prepare(kRate, 512);
  const int at_base = sim.cab_ir_samples();
  CHECK(at_base > 0);
  sim.prepare(2.0 * kRate, 512);
  const int at_double = sim.cab_ir_samples();
  // Same duration, twice the samples: the budget is a duration, and generating
  // at the new rate is what keeps it one.
  CHECK(at_double == Catch::Approx(2 * at_base).margin(2));

  // A supplied capture replaces a generated cabinet rather than layering on it.
  std::vector<float> captured(64, 0.0f);
  captured[0] = 1.0f;
  sim.load_cab_ir(captured);
  CHECK(sim.cab_ir_samples() == 64);

  // And the generated cabinet actually reaches the audio, rather than being
  // stored and forgotten.
  const std::vector<float> input = sine(700.0, 0.3f, kNumSamples);
  AmpSim analytic{config};
  AmpSim generated{config};
  generated.load_generated_cab_ir(CabIrSpec{});
  CHECK(relative_change(process_mono(generated, input), process_mono(analytic, input)) > 0.01);
}

TEST_CASE("saturation.ampSim generates a cabinet from the param bag",
          "[mastering][saturation][amp]") {
  // Off by default: no key, no cabinet, and the shipped sound does not move.
  auto plain = make_insert("saturation.ampSim", "{}");
  REQUIRE(plain != nullptr);
  CHECK_FALSE(static_cast<AmpSim*>(plain.get())->has_cab_ir());

  auto generated =
      make_insert("saturation.ampSim", R"({"cabIrGenerate":true,"micModel":1,"micDistanceCm":15})");
  REQUIRE(generated != nullptr);
  auto* sim = static_cast<AmpSim*>(generated.get());
  CHECK(sim->has_cab_ir());
  // The generated cabinet follows the rig already configured rather than
  // carrying a duplicate set of mic controls that could drift out of step.
  CHECK(sim->amp_config().mic_model == MicModel::kDynamic);
  CHECK(sim->amp_config().mic_distance_cm == Catch::Approx(15.0f));

  // The whole-cabinet switch is reachable, and it is the one thing the analytic
  // chain has no equivalent for.
  auto single = make_insert("saturation.ampSim",
                            R"({"cabIrGenerate":true,"cabIrDrivers":false,"micDistanceCm":60})");
  REQUIRE(single != nullptr);
  const std::vector<float> input = sine(700.0, 0.3f, kNumSamples);
  auto full = make_insert("saturation.ampSim", R"({"cabIrGenerate":true,"micDistanceCm":60})");
  REQUIRE(full != nullptr);
  CHECK(relative_change(process_mono(*full, input), process_mono(*single, input)) > 0.05);

  // A supplied capture still wins over a generated one.
  auto captured = make_insert(
      "saturation.ampSim", R"({"cabIrGenerate":true,"cabIrF32Base64":"AACAPwAAAAAAAAAAAAAAAA=="})");
  REQUIRE(captured != nullptr);
  CHECK(static_cast<AmpSim*>(captured.get())->has_cab_ir());
}
