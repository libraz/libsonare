/// @file modulation_inserts_test.cpp
/// @brief The GS-EFX-driven modulation inserts (wah / auto-wah / rotary /
///        phaser / ring modulator / pitch shifter): each imparts its
///        characteristic transformation, stays finite/stable, renders
///        deterministically, and builds through the insert factory.

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <vector>

#include "core/fft.h"
#include "effects/modulation/auto_wah.h"
#include "effects/modulation/phaser.h"
#include "effects/modulation/pitch_shifter.h"
#include "effects/modulation/ring_modulator.h"
#include "effects/modulation/rotary.h"
#include "effects/modulation/wah.h"
#include "support/audio_fixtures.h"
#ifdef SONARE_WITH_MASTERING
#include "mastering/api/insert_factory.h"
#endif

namespace {

using namespace sonare::effects::modulation;

using sonare::test::kFft;
using sonare::test::kRate;
constexpr int kNumSamples = 24000;

std::vector<float> sine(double freq_hz, float amp, int n) {
  std::vector<float> buf(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    buf[static_cast<size_t>(i)] =
        amp * static_cast<float>(std::sin(2.0 * 3.14159265358979 * freq_hz * i / kRate));
  }
  return buf;
}

std::vector<float> square(double freq_hz, float amp, int n) {
  std::vector<float> buf = sine(freq_hz, 1.0f, n);
  for (float& s : buf) s = s >= 0.0f ? amp : -amp;
  return buf;
}

float rms(const std::vector<float>& buf, size_t from, size_t to) {
  double acc = 0.0;
  size_t k = 0;
  for (size_t i = from; i < to && i < buf.size(); ++i, ++k)
    acc += static_cast<double>(buf[i]) * buf[i];
  return k > 0 ? static_cast<float>(std::sqrt(acc / static_cast<double>(k))) : 0.0f;
}

bool all_finite(const std::vector<float>& buf) {
  for (float s : buf) {
    if (!std::isfinite(s)) return false;
  }
  return true;
}

using sonare::test::spectrum_mag;

int bin_of(double freq_hz) { return static_cast<int>(std::lround(freq_hz / kRate * kFft)); }

/// Summed magnitude in a [flo, fhi] Hz band of the tail spectrum.
double band_energy(const std::vector<float>& buf, double flo, double fhi) {
  const std::vector<float> mag = spectrum_mag(buf, buf.size() - kFft);
  double acc = 0.0;
  for (int b = std::max(1, bin_of(flo)); b <= bin_of(fhi) && b < static_cast<int>(mag.size());
       ++b) {
    acc += mag[static_cast<size_t>(b)];
  }
  return acc;
}

/// Dominant spectral bin frequency (Hz) of the tail.
double dominant_hz(const std::vector<float>& buf) {
  const std::vector<float> mag = spectrum_mag(buf, buf.size() - kFft);
  int peak = 1;
  for (int b = 2; b < static_cast<int>(mag.size()); ++b) {
    if (mag[static_cast<size_t>(b)] > mag[static_cast<size_t>(peak)]) peak = b;
  }
  return static_cast<double>(peak) * kRate / kFft;
}

template <typename Proc>
std::vector<float> run_mono(Proc& proc, std::vector<float> input) {
  proc.prepare(kRate, 512);
  for (size_t off = 0; off + 512 <= input.size(); off += 512) {
    float* block[1] = {input.data() + off};
    proc.process(block, 1, 512);
  }
  return input;
}

}  // namespace

TEST_CASE("the ring modulator replaces the carrier tone with its sidebands",
          "[effects][modulation][ringmod]") {
  // input 440 Hz x carrier 200 Hz (fully wet) -> DSB-SC: energy at 240 and 640,
  // essentially none left at 440.
  RingModulator fx({/*carrier_hz=*/200.0f, /*dry_wet=*/1.0f});
  const std::vector<float> out = run_mono(fx, sine(440.0, 0.4f, kNumSamples));
  REQUIRE(all_finite(out));
  const double at_input = band_energy(out, 430.0, 450.0);
  const double lower = band_energy(out, 230.0, 250.0);
  const double upper = band_energy(out, 630.0, 650.0);
  REQUIRE(lower > 5.0 * at_input);
  REQUIRE(upper > 5.0 * at_input);
}

TEST_CASE("the wah sweeps a resonant band so the level undulates", "[effects][modulation][wah]") {
  Wah fx({/*rate_hz=*/2.0f, /*min_hz=*/400.0f, /*max_hz=*/2000.0f, /*resonance=*/5.0f,
          /*dry_wet=*/1.0f});
  const std::vector<float> out = run_mono(fx, square(150.0, 0.3f, kNumSamples));
  REQUIRE(all_finite(out));
  float lo = 1.0e9f;
  float hi = 0.0f;
  for (size_t from = 2400; from + 2400 <= out.size(); from += 2400) {
    const float level = rms(out, from, from + 2400);
    lo = std::min(lo, level);
    hi = std::max(hi, level);
  }
  REQUIRE(lo > 0.0f);
  REQUIRE(hi > 1.3f * lo);
}

TEST_CASE("the auto-wah opens the filter further for louder input",
          "[effects][modulation][autowah]") {
  // A resonant bandpass tracking the level: a hot input pushes the centre up, so
  // more high-band energy survives than for a quiet input.
  const auto high_fraction = [](float amp) {
    AutoWah fx({/*sensitivity=*/4.0f, /*min_hz=*/300.0f, /*max_hz=*/3000.0f, /*resonance=*/3.0f,
                /*attack_ms=*/5.0f, /*release_ms=*/80.0f, /*dry_wet=*/1.0f});
    const std::vector<float> out = run_mono(fx, square(180.0, amp, kNumSamples));
    REQUIRE(all_finite(out));
    const double high = band_energy(out, 1500.0, 4000.0);
    const double low = band_energy(out, 100.0, 1500.0);
    return high / std::max(1.0e-9, low + high);
  };
  REQUIRE(high_fraction(0.6f) > 1.2 * high_fraction(0.02f));
}

TEST_CASE("wah inserts clamp an over-Nyquist sweep to the filter's stable range",
          "[effects][modulation][wah]") {
  Wah wah({/*rate_hz=*/1.0f, /*min_hz=*/1.0e9f, /*max_hz=*/1.0e9f,
           /*resonance=*/4.0f, /*dry_wet=*/1.0f});
  AutoWah auto_wah({/*sensitivity=*/2.0f, /*min_hz=*/1.0e9f, /*max_hz=*/1.0e9f,
                    /*resonance=*/4.0f, /*attack_ms=*/5.0f, /*release_ms=*/50.0f,
                    /*dry_wet=*/1.0f});
  REQUIRE(all_finite(run_mono(wah, sine(440.0, 0.3f, kNumSamples))));
  REQUIRE(all_finite(run_mono(auto_wah, sine(440.0, 0.3f, kNumSamples))));
}

TEST_CASE("the rotary speaker decorrelates a mono input into a swirling stereo",
          "[effects][modulation][rotary]") {
  Rotary fx({/*rate_hz=*/6.0f, /*depth_ms=*/1.5f, /*tremolo=*/0.6f, /*stereo_spread=*/1.0f,
             /*dry_wet=*/1.0f});
  std::vector<float> left = square(220.0, 0.3f, kNumSamples);
  std::vector<float> right = left;
  fx.prepare(kRate, 512);
  for (size_t off = 0; off + 512 <= left.size(); off += 512) {
    float* block[2] = {left.data() + off, right.data() + off};
    fx.process(block, 2, 512);
  }
  REQUIRE(all_finite(left));
  REQUIRE(all_finite(right));
  std::vector<float> diff(left.size());
  for (size_t i = 0; i < diff.size(); ++i) diff[i] = left[i] - right[i];
  REQUIRE(rms(diff, 2400, diff.size()) > 0.05f * rms(left, 2400, left.size()));
}

TEST_CASE("the phaser sweeps its two channels out of step", "[effects][modulation][phaser]") {
  // The phaser is a stereo effect: its notches must not sit on the same
  // frequencies in both channels. Driving the pair from one oscillator leaves an
  // identical L/R input identical on the way out, which is a mono effect wearing
  // a stereo label -- no width, and the sweep collapses on a mono fold.
  Phaser fx;  // Default sweep: 0.4 Hz over 300..1600 Hz, 4 stages, 50% wet.
  const std::vector<float> input = square(220.0, 0.3f, kNumSamples);
  std::vector<float> left = input;
  std::vector<float> right = input;
  fx.prepare(kRate, 512);
  for (size_t off = 0; off + 512 <= left.size(); off += 512) {
    float* block[2] = {left.data() + off, right.data() + off};
    fx.process(block, 2, 512);
  }
  REQUIRE(all_finite(left));
  REQUIRE(all_finite(right));

  // The notches are deep enough to hear on a single channel...
  std::vector<float> wet(left.size());
  for (size_t i = 0; i < wet.size(); ++i) wet[i] = left[i] - input[i];
  REQUIRE(rms(wet, 2400, wet.size()) > 0.05f * rms(input, 2400, input.size()));

  // ...and the two sweeps are a quarter cycle apart, so the channels come out
  // decorrelated instead of bit-identical.
  std::vector<float> diff(left.size());
  for (size_t i = 0; i < diff.size(); ++i) diff[i] = left[i] - right[i];
  REQUIRE(rms(diff, 2400, diff.size()) > 0.05f * rms(left, 2400, left.size()));
}

TEST_CASE("the rotary speaker's tremolo does not add gain", "[effects][modulation][rotary]") {
  // A tremolo is an amplitude modulator, so its loudest point is the unmodulated
  // signal. Swinging the gain symmetrically around unity instead would make the
  // effect louder the deeper it is set, and the depth control would double as an
  // undocumented makeup gain. Two depths are checked because the defect scales
  // with depth: a single depth cannot tell a fixed offset from a scaling one.
  const auto peak = [](const std::vector<float>& v, size_t begin) {
    float m = 0.0f;
    for (size_t i = begin; i < v.size(); ++i) m = std::max(m, std::abs(v[i]));
    return m;
  };
  const std::vector<float> input = sine(220.0, 0.4f, kNumSamples);
  const float input_peak = peak(input, 2400);

  for (const float tremolo : {0.5f, 1.0f}) {
    Rotary fx({/*rate_hz=*/6.0f, /*depth_ms=*/1.5f, tremolo, /*stereo_spread=*/1.0f,
               /*dry_wet=*/1.0f});
    std::vector<float> left = input;
    std::vector<float> right = input;
    fx.prepare(kRate, 512);
    for (size_t off = 0; off + 512 <= left.size(); off += 512) {
      float* block[2] = {left.data() + off, right.data() + off};
      fx.process(block, 2, 512);
    }
    INFO("tremolo depth " << tremolo);
    // The horn/drum crossover splits and re-sums the signal through two
    // different delays, so the reconstruction alone lifts the peak to about
    // 1.15x regardless of depth -- that is the floor this bound has to clear,
    // not zero. A gain swinging around unity instead measures 1.51x at depth
    // 0.5 and 1.86x at depth 1.0, so the bound sits between the two.
    REQUIRE(peak(left, 2400) < input_peak * 1.25f);
    REQUIRE(peak(right, 2400) < input_peak * 1.25f);
  }
}

TEST_CASE("the pitch shifter raises a tone by an octave", "[effects][modulation][pitchshift]") {
  PitchShifter up({/*semitones=*/12.0f, /*dry_wet=*/1.0f});
  const std::vector<float> shifted = run_mono(up, sine(220.0, 0.4f, kNumSamples));
  REQUIRE(all_finite(shifted));
  REQUIRE(dominant_hz(shifted) > 400.0);
  REQUIRE(dominant_hz(shifted) < 480.0);

  // Unity shift is (nearly) pitch-preserving: the 220 Hz partial stays dominant.
  PitchShifter unity({/*semitones=*/0.0f, /*dry_wet=*/1.0f});
  const std::vector<float> passed = run_mono(unity, sine(220.0, 0.4f, kNumSamples));
  REQUIRE(dominant_hz(passed) > 200.0);
  REQUIRE(dominant_hz(passed) < 240.0);
}

TEST_CASE("the pitch shifter is sample-identical at zero shift",
          "[effects][modulation][pitchshift][latency]") {
  // The grain taps sit half a window apart, so an unshifted signal used to come
  // out delayed by that half window (22.5 ms) while the processor reported zero
  // latency. Zero semitones must now be an exact passthrough, at any mix, so the
  // declared latency is the truth.
  const std::vector<float> input = square(180.0, 0.35f, kNumSamples);
  for (float dry_wet : {1.0f, 0.5f, 0.0f}) {
    PitchShifter unity({/*semitones=*/0.0f, dry_wet});
    const std::vector<float> out = run_mono(unity, input);
    INFO("dryWet " << dry_wet);
    REQUIRE(out == input);
    REQUIRE(unity.latency_samples() == 0);
  }

  // The short circuit must not swallow a real shift: an impulse driven through a
  // shifting instance still comes out delayed by the grain read, so a caller
  // that asked for a shift keeps getting one.
  std::vector<float> impulse(kNumSamples, 0.0f);
  impulse[128] = 0.5f;
  PitchShifter shifted({/*semitones=*/7.0f, /*dry_wet=*/1.0f});
  REQUIRE(run_mono(shifted, impulse) != impulse);
}

TEST_CASE("the new modulation inserts render deterministically", "[effects][modulation]") {
  Wah a;
  Wah b;
  const std::vector<float> in = square(150.0, 0.3f, kNumSamples);
  REQUIRE(run_mono(a, in) == run_mono(b, in));
}

#ifdef SONARE_WITH_MASTERING
TEST_CASE("the GS-EFX modulation inserts build through the insert factory",
          "[effects][modulation][insert_factory]") {
  const auto names = sonare::mastering::api::insert_factory_names();
  for (const char* wanted :
       {"effects.modulation.wah", "effects.modulation.autoWah", "effects.modulation.rotary",
        "effects.modulation.ringModulator", "effects.modulation.pitchShifter"}) {
    bool listed = false;
    for (const auto& name : names) listed |= name == wanted;
    INFO(wanted);
    REQUIRE(listed);
    auto processor = sonare::mastering::api::make_insert(wanted, "{}");
    REQUIRE(processor != nullptr);
  }
  REQUIRE(
      dynamic_cast<Wah*>(
          sonare::mastering::api::make_insert("effects.modulation.wah", R"({"rateHz":3})").get()) !=
      nullptr);
  REQUIRE(dynamic_cast<PitchShifter*>(sonare::mastering::api::make_insert(
                                          "effects.modulation.pitchShifter", R"({"semitones":7})")
                                          .get()) != nullptr);
}
#endif
