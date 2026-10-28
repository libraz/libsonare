/// @file voice_vocoder_test.cpp
/// @brief Channel vocoder: filter bank design, group delay, carrier periodicity, formant map.

#include <algorithm>
#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

#include "editing/voice_changer/channel_vocoder.h"
#include "rt/biquad_design.h"
#include "support/alloc_guard.h"
#include "util/constants.h"
#include "voicelab/synthetic_voice.h"

namespace sonare::editing::voice_changer {

/// Reads the designed filter bank of a ChannelVocoder.
struct ChannelVocoderTestAccess {
  static int bands(const ChannelVocoder& v) { return v.bands_; }
  static float centre_hz(const ChannelVocoder& v, int k) { return v.band_[k].centre_hz; }
  static float low_edge_hz(const ChannelVocoder& v, int k) { return v.band_[k].low_edge_hz; }
  static float high_edge_hz(const ChannelVocoder& v, int k) { return v.band_[k].high_edge_hz; }
  static rt::BiquadCoeffs coeffs(const ChannelVocoder& v, int k, int stage) {
    return v.band_[k].coeffs[stage];
  }
  static float sibilance_corner_hz(const ChannelVocoder& v) { return v.sibilance_corner_hz_; }
};

}  // namespace sonare::editing::voice_changer

namespace {

using namespace sonare::editing::voice_changer;
using sonare::constants::kCentsPerOctave;
using sonare::constants::kTwoPiD;
namespace vl = sonare::voicelab;
using Access = ChannelVocoderTestAccess;

constexpr int kBlock = 256;
constexpr float kNote60Hz = 261.6255653f;
constexpr int kVowelA = 0;

struct Rate {
  double sr;
};
const Rate kRates[] = {{44100.0}, {48000.0}, {22050.0}};
const int kBandCounts[] = {8, 20, 32};

VoiceTargets note_targets(float hz) {
  VoiceTargets t;
  t.count = 1;
  t.voices[0].target_hz = hz;
  t.voices[0].gain = 1.0f;
  t.voices[0].active = true;
  return t;
}

std::vector<float> white_noise(std::size_t n, std::uint32_t seed = 12345u) {
  std::vector<float> x(n);
  std::uint32_t s = seed;
  for (auto& v : x) {
    s = s * 1664525u + 1013904223u;
    v = (static_cast<float>(s >> 8) / 8388608.0f - 1.0f) * 0.5f;
  }
  return x;
}

std::vector<float> sine(std::size_t n, double hz, double sr, float amp = 0.3f) {
  std::vector<float> x(n);
  for (std::size_t i = 0; i < n; ++i) {
    x[i] = amp * static_cast<float>(std::sin(kTwoPiD * hz * static_cast<double>(i) / sr));
  }
  return x;
}

/// Runs the vocoder over the whole signal in `kBlock`-sample blocks.
std::vector<float> run(ChannelVocoder& v, const std::vector<float>& mod, const float* carrier,
                       const VoiceTargets& notes) {
  std::vector<float> out(mod.size());
  for (std::size_t i = 0; i < mod.size(); i += kBlock) {
    const int n = static_cast<int>(std::min<std::size_t>(kBlock, mod.size() - i));
    v.process(mod.data() + i, carrier ? carrier + i : nullptr, notes, out.data() + i, n);
  }
  return out;
}

/// Mean energy per band of `x` seen through the designed bank, skipping the first `skip` samples.
std::vector<double> band_energy(const ChannelVocoder& v, const std::vector<float>& x,
                                std::size_t skip) {
  const int bands = Access::bands(v);
  std::vector<double> energy(static_cast<std::size_t>(bands), 0.0);
  for (int k = 0; k < bands; ++k) {
    sonare::rt::BiquadState s0, s1;
    s0.set(Access::coeffs(v, k, 0));
    s1.set(Access::coeffs(v, k, 1));
    double sum = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i) {
      const float y = s1.process(s0.process(x[i]));
      if (i >= skip) sum += static_cast<double>(y) * y;
    }
    energy[static_cast<std::size_t>(k)] = sum / static_cast<double>(x.size() - skip);
  }
  return energy;
}

int band_containing(const ChannelVocoder& v, double hz) {
  const int bands = Access::bands(v);
  for (int k = 0; k < bands; ++k) {
    if (hz < Access::high_edge_hz(v, k)) return k;
  }
  return bands - 1;
}

int argmax(const std::vector<double>& e) {
  return static_cast<int>(std::max_element(e.begin(), e.end()) - e.begin());
}

double response_db(const sonare::rt::BiquadCoeffs& c, double omega) {
  const auto z1 = std::polar(1.0, -omega);
  const auto z2 = std::polar(1.0, -2.0 * omega);
  const double b0 = c.b0, b1 = c.b1, b2 = c.b2, a1 = c.a1, a2 = c.a2;
  const auto h = (b0 + b1 * z1 + b2 * z2) / (1.0 + a1 * z1 + a2 * z2);
  return 20.0 * std::log10(std::abs(h));
}

/// Group delay in samples of the two-stage band `k`, from its impulse response (not the
/// analytic derivative the vocoder uses): Re(sum n h[n] z^-n / sum h[n] z^-n) at the centre.
double impulse_group_delay(const ChannelVocoder& v, int k, double sr) {
  sonare::rt::BiquadState s0, s1;
  s0.set(Access::coeffs(v, k, 0));
  s1.set(Access::coeffs(v, k, 1));
  const double omega = kTwoPiD * Access::centre_hz(v, k) / sr;
  const std::size_t length = static_cast<std::size_t>(sr);
  std::complex<double> h_sum = 0.0, nh_sum = 0.0;
  for (std::size_t n = 0; n < length; ++n) {
    const float y = s1.process(s0.process(n == 0 ? 1.0f : 0.0f));
    const auto term = static_cast<double>(y) * std::polar(1.0, -omega * static_cast<double>(n));
    h_sum += term;
    nh_sum += term * static_cast<double>(n);
  }
  return (nh_sum / h_sum).real();
}

/// Refines a harmonic's frequency (Hann-windowed DFT magnitude, parabolic peak) within +-0.3%.
double refine_frequency(const float* x, std::size_t n, double sr, double nominal) {
  constexpr int kSteps = 31;
  constexpr double kSpan = 0.003;
  double mag[kSteps];
  for (int g = 0; g < kSteps; ++g) {
    const double f = nominal * (1.0 + kSpan * (2.0 * g / (kSteps - 1) - 1.0));
    std::complex<double> acc = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
      const double w =
          0.5 - 0.5 * std::cos(kTwoPiD * static_cast<double>(i) / static_cast<double>(n));
      acc += w * static_cast<double>(x[i]) *
             std::polar(1.0, -kTwoPiD * f * static_cast<double>(i) / sr);
    }
    mag[g] = std::log(std::abs(acc) + 1e-30);
  }
  int best = static_cast<int>(std::max_element(mag, mag + kSteps) - mag);
  best = std::clamp(best, 1, kSteps - 2);
  const double denom = mag[best - 1] - 2.0 * mag[best] + mag[best + 1];
  const double offset = denom == 0.0 ? 0.0 : 0.5 * (mag[best - 1] - mag[best + 1]) / denom;
  const double step = 2.0 * kSpan / (kSteps - 1);
  return nominal * (1.0 + kSpan * -1.0 + step * (best + offset));
}

ChannelVocoder make(double sr, int bands, VocoderConfig config = {}, float alpha = 1.0f) {
  ChannelVocoder v;
  v.prepare(sr, kBlock, bands);
  config.bands = bands;
  v.set_config(config, alpha);
  return v;
}

}  // namespace

TEST_CASE("Vocoder bank: centres, edges and design corners stay below 0.45 sr",
          "[voice_vocoder][design]") {
  for (const Rate& rate : kRates) {
    for (int bands : kBandCounts) {
      ChannelVocoder v;
      v.prepare(rate.sr, kBlock, bands);
      REQUIRE(Access::bands(v) == bands);
      const double limit = sonare::editing::voice_changer::kMaxDesignFrequencyRatio * rate.sr;
      float previous = 0.0f;
      for (int k = 0; k < bands; ++k) {
        const float fc = Access::centre_hz(v, k);
        CHECK(fc > previous);
        CHECK(fc > 80.0f);
        CHECK(fc < Access::high_edge_hz(v, k));
        CHECK(fc > Access::low_edge_hz(v, k));
        CHECK(Access::high_edge_hz(v, k) <= Catch::Approx(std::min(10000.0, limit)).epsilon(1e-4));
        previous = fc;
      }
      CHECK(Access::sibilance_corner_hz(v) <= limit);
      CHECK(Access::sibilance_corner_hz(v) <= 6000.0f);
    }
  }
}

TEST_CASE("Vocoder bank: band centres in Hz do not depend on 44.1 versus 48 kHz",
          "[voice_vocoder][design]") {
  for (int bands : kBandCounts) {
    ChannelVocoder a, b;
    a.prepare(44100.0, kBlock, bands);
    b.prepare(48000.0, kBlock, bands);
    for (int k = 0; k < bands; ++k) {
      CHECK(Access::centre_hz(a, k) == Catch::Approx(Access::centre_hz(b, k)).epsilon(1e-4));
    }
  }
}

TEST_CASE("Vocoder bank: every stage has unit peak gain at its centre", "[voice_vocoder][design]") {
  for (const Rate& rate : kRates) {
    for (int bands : kBandCounts) {
      ChannelVocoder v;
      v.prepare(rate.sr, kBlock, bands);
      for (int k = 0; k < bands; ++k) {
        const double omega = kTwoPiD * Access::centre_hz(v, k) / rate.sr;
        for (int stage = 0; stage < 2; ++stage) {
          CHECK(std::abs(response_db(Access::coeffs(v, k, stage), omega)) <= 0.1);
        }
      }
    }
  }
}

TEST_CASE("Vocoder group_delay_samples matches the lowest band's impulse response",
          "[voice_vocoder][latency]") {
  for (double sr : {44100.0, 48000.0}) {
    for (int bands : {20, 32}) {
      ChannelVocoder v;
      v.prepare(sr, kBlock, bands);
      const double measured = impulse_group_delay(v, 0, sr);
      INFO("sr " << sr << " bands " << bands << " reported " << v.group_delay_samples()
                 << " measured " << measured);
      CHECK(std::abs(static_cast<double>(v.group_delay_samples()) - measured) <= 1.0);
      CHECK(v.group_delay_samples() > 0);
      // The 20-band bank must fit inside the shifter's delay for f0_min = 70 Hz.
      if (bands == 20) CHECK(v.group_delay_samples() < static_cast<int>(std::ceil(sr / 70.0)));
    }
  }
}

TEST_CASE("Vocoder output periodicity follows the internal carrier", "[voice_vocoder][carrier]") {
  const double sr = 48000.0;
  std::vector<vl::VoiceSegment> seg = {
      {1.0, 120.0f, 120.0f, 0.0f, 0.0f, true, kVowelA, -20.0f, 10.0f}};
  const std::vector<float> mod = vl::synthesize_voice(seg, sr);

  VocoderConfig config;
  config.sibilance = 0.0f;
  config.carrier_wave = VocoderCarrierWave::Saw;
  ChannelVocoder v = make(sr, 20, config);
  const std::vector<float> out = run(v, mod, nullptr, note_targets(kNote60Hz));

  const std::size_t skip = static_cast<std::size_t>(0.25 * sr);
  const std::size_t len = out.size() - skip;
  double worst_cents = 0.0;
  for (int harmonic : {2, 3, 4, 5}) {
    const double f = refine_frequency(out.data() + skip, len, sr, kNote60Hz * harmonic) / harmonic;
    worst_cents = std::max(worst_cents, std::abs(kCentsPerOctave * std::log2(f / kNote60Hz)));
  }
  INFO("worst harmonic-derived deviation " << worst_cents << " cent");
  CHECK(worst_cents <= 2.0);
}

TEST_CASE("Vocoder band energy peaks at the modulator's F1 and F2 bands",
          "[voice_vocoder][formant]") {
  const double sr = 48000.0;
  std::vector<vl::VoiceSegment> seg = {
      {1.0, 120.0f, 120.0f, 0.0f, 0.0f, true, kVowelA, -20.0f, 10.0f}};
  const std::vector<float> mod = vl::synthesize_voice(seg, sr);
  const std::vector<float> noise = white_noise(mod.size());
  const auto formants = vl::vowel_formants_hz(kVowelA);

  VocoderConfig config;
  config.sibilance = 0.0f;
  config.carrier_source = VocoderCarrierSource::External;
  ChannelVocoder v = make(sr, 20, config);
  const std::vector<float> out = run(v, mod, noise.data(), VoiceTargets{});
  const std::vector<double> energy = band_energy(v, out, static_cast<std::size_t>(0.2 * sr));

  std::vector<int> maxima;
  for (int k = 1; k + 1 < static_cast<int>(energy.size()); ++k) {
    if (energy[k] > energy[k - 1] && energy[k] >= energy[k + 1]) maxima.push_back(k);
  }
  for (int f = 0; f < 2; ++f) {
    const int expected = band_containing(v, formants[f]);
    const bool found = std::any_of(maxima.begin(), maxima.end(),
                                   [&](int k) { return std::abs(k - expected) <= 1; });
    INFO("formant " << f + 1 << " at " << formants[f] << " Hz, band " << expected);
    CHECK(found);
  }
  // The spectrum is shaped by the modulator, not flat: the strongest band sits within one band
  // of F1 or F2.
  const int top = argmax(energy);
  const int b1 = band_containing(v, formants[0]);
  const int b2 = band_containing(v, formants[1]);
  CHECK((std::abs(top - b1) <= 1 || std::abs(top - b2) <= 1));
}

TEST_CASE("Vocoder formant_alpha moves the carrier envelope along the band axis",
          "[voice_vocoder][formant]") {
  const double sr = 48000.0;
  const std::size_t n = static_cast<std::size_t>(sr);
  constexpr int kSourceBand = 7;
  ChannelVocoder reference;
  reference.prepare(sr, kBlock, 20);
  const double source_hz = Access::centre_hz(reference, kSourceBand);
  const std::vector<float> mod = sine(n, source_hz, sr);
  const std::vector<float> noise = white_noise(n);

  VocoderConfig config;
  config.sibilance = 0.0f;
  config.carrier_source = VocoderCarrierSource::External;

  int previous = -1;
  for (float alpha : {1.0f, 1.5f, 2.0f}) {
    ChannelVocoder v = make(sr, 20, config, alpha);
    const std::vector<float> out = run(v, mod, noise.data(), VoiceTargets{});
    const std::vector<double> energy = band_energy(v, out, static_cast<std::size_t>(0.2 * sr));
    const int expected = band_containing(v, source_hz * alpha);
    const int peak = argmax(energy);
    INFO("alpha " << alpha << " peak band " << peak << " expected " << expected);
    CHECK(std::abs(peak - expected) <= 1);
    CHECK(peak > previous);
    previous = peak;
  }
  // alpha < 1 moves the envelope down.
  ChannelVocoder v = make(sr, 20, config, 0.6f);
  const std::vector<double> energy =
      band_energy(v, run(v, mod, noise.data(), VoiceTargets{}), static_cast<std::size_t>(0.2 * sr));
  CHECK(argmax(energy) < kSourceBand);
}

TEST_CASE("Vocoder carriers: waves, ensemble and silent voices", "[voice_vocoder][carrier]") {
  const double sr = 48000.0;
  const std::vector<float> mod = white_noise(static_cast<std::size_t>(0.5 * sr));

  auto rms = [](const std::vector<float>& x) {
    double s = 0.0;
    for (float v : x) s += static_cast<double>(v) * v;
    return std::sqrt(s / static_cast<double>(x.size()));
  };

  for (VocoderCarrierWave wave :
       {VocoderCarrierWave::Saw, VocoderCarrierWave::Pulse, VocoderCarrierWave::Ensemble}) {
    VocoderConfig config;
    config.carrier_wave = wave;
    config.sibilance = 0.0f;
    ChannelVocoder v = make(sr, 20, config);
    VoiceTargets four;
    four.count = 4;
    for (int i = 0; i < 4; ++i) {
      four.voices[i].target_hz = 110.0f * static_cast<float>(i + 1);
      four.voices[i].active = true;
    }
    const std::vector<float> out = run(v, mod, nullptr, four);
    CHECK(rms(out) > 1e-3);
    CHECK(std::all_of(out.begin(), out.end(), [](float x) { return std::isfinite(x); }));
  }

  // Inactive voices, zero targets and a count of zero all give silence (sibilance off).
  VocoderConfig config;
  config.sibilance = 0.0f;
  VoiceTargets inactive = note_targets(220.0f);
  inactive.voices[0].active = false;
  VoiceTargets zero_hz = note_targets(0.0f);
  for (const VoiceTargets& t : {inactive, zero_hz, VoiceTargets{}}) {
    ChannelVocoder v = make(sr, 20, config);
    CHECK(rms(run(v, mod, nullptr, t)) == 0.0);
  }

  // level 0 mutes.
  config.level = 0.0f;
  ChannelVocoder muted = make(sr, 20, config);
  CHECK(rms(run(muted, mod, nullptr, note_targets(220.0f))) == 0.0);
}

TEST_CASE("Vocoder sibilance adds the high-passed modulator", "[voice_vocoder][carrier]") {
  const double sr = 48000.0;
  const std::vector<float> hiss = sine(static_cast<std::size_t>(0.3 * sr), 9000.0, sr);
  auto peak_after = [&](float sibilance) {
    VocoderConfig config;
    config.sibilance = sibilance;
    ChannelVocoder v = make(sr, 20, config);
    const std::vector<float> out = run(v, hiss, nullptr, VoiceTargets{});
    float peak = 0.0f;
    for (std::size_t i = out.size() / 2; i < out.size(); ++i)
      peak = std::max(peak, std::abs(out[i]));
    return peak;
  };
  CHECK(peak_after(0.0f) == 0.0f);
  CHECK(peak_after(1.0f) == Catch::Approx(0.3f).margin(0.05f));
}

TEST_CASE("Vocoder at 22.05 kHz stays finite with design corners below 0.45 sr",
          "[voice_vocoder][design]") {
  const double sr = 22050.0;
  VocoderConfig config;
  config.sibilance = 1.0f;
  config.carrier_wave = VocoderCarrierWave::Ensemble;
  ChannelVocoder v = make(sr, 32, config);
  const std::vector<float> mod = white_noise(static_cast<std::size_t>(sr));
  // A target far above Nyquist is clamped, not aliased into NaN.
  const std::vector<float> out = run(v, mod, nullptr, note_targets(30000.0f));
  CHECK(std::all_of(out.begin(), out.end(), [](float x) { return std::isfinite(x); }));
  const double limit = 0.45 * sr;
  for (int k = 0; k < Access::bands(v); ++k) CHECK(Access::centre_hz(v, k) <= limit);
  CHECK(Access::sibilance_corner_hz(v) <= limit);
}

TEST_CASE("Vocoder processing is block-size independent and reset restores the start",
          "[voice_vocoder][carrier]") {
  const double sr = 48000.0;
  const std::vector<float> mod = white_noise(1000);
  VocoderConfig config;
  config.carrier_wave = VocoderCarrierWave::Ensemble;
  const VoiceTargets notes = note_targets(200.0f);

  ChannelVocoder a = make(sr, 20, config);
  std::vector<float> whole(mod.size());
  a.process(mod.data(), nullptr, notes, whole.data(), static_cast<int>(mod.size()));

  ChannelVocoder b = make(sr, 20, config);
  std::vector<float> split(mod.size());
  b.process(mod.data(), nullptr, notes, split.data(), 300);
  b.process(mod.data() + 300, nullptr, notes, split.data() + 300, 700);
  for (std::size_t i = 0; i < mod.size(); ++i)
    CHECK(split[i] == Catch::Approx(whole[i]).margin(1e-6));

  a.reset();
  std::vector<float> again(mod.size());
  a.process(mod.data(), nullptr, notes, again.data(), static_cast<int>(mod.size()));
  CHECK(again == whole);
}

TEST_CASE("Vocoder process performs no heap allocation", "[voice_vocoder][rt]") {
  const double sr = 48000.0;
  const std::vector<float> mod = white_noise(kBlock);
  const std::vector<float> carrier = white_noise(kBlock, 99u);
  std::vector<float> out(kBlock);
  VoiceTargets notes;
  notes.count = 4;
  for (int i = 0; i < 4; ++i) {
    notes.voices[i].target_hz = 150.0f * static_cast<float>(i + 1);
    notes.voices[i].active = true;
  }

  ChannelVocoder v = make(sr, 32);
  v.process(mod.data(), nullptr, notes, out.data(), kBlock);

  std::size_t allocations = 0;
  {
    sonare::test::AllocationGuard guard;
    for (VocoderCarrierWave wave :
         {VocoderCarrierWave::Saw, VocoderCarrierWave::Pulse, VocoderCarrierWave::Ensemble}) {
      VocoderConfig config;
      config.carrier_wave = wave;
      v.set_config(config, 1.3f);
      v.process(mod.data(), nullptr, notes, out.data(), kBlock);
    }
    VocoderConfig external;
    external.carrier_source = VocoderCarrierSource::External;
    v.set_config(external, 0.8f);
    v.process(mod.data(), carrier.data(), notes, out.data(), kBlock);
    v.reset();
    v.process(mod.data(), carrier.data(), notes, out.data(), kBlock);
    allocations = guard.count();
  }
  CHECK(allocations == 0);
}
