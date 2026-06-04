/// @file filter_models_test.cpp
/// @brief Selectable VA filter models (midi/synth/filter_models): cutoff /
///        resonance sweep stability (no divergence, no NaN), lowpass transfer
///        sanity, deterministic self-oscillation near the set cutoff for the
///        ladder / Sallen-Key models, and model dispatch through a
///        NativeSynth patch.

#include "midi/synth/filter_models.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "midi/midi_event.h"
#include "midi/synth/native_synth.h"
#include "midi/ump.h"

namespace {

using sonare::midi::MidiEvent;
using sonare::midi::synth::kSelfOscQ;
using sonare::midi::synth::NativeSynth;
using sonare::midi::synth::NativeSynthConfig;
using sonare::midi::synth::SynthFilter;
using sonare::midi::synth::SynthFilterModel;
using sonare::midi::synth::SynthFilterOutput;

constexpr double kRate = 48000.0;

constexpr SynthFilterModel kAllModels[] = {
    SynthFilterModel::kSvf,
    SynthFilterModel::kMoogLadder,
    SynthFilterModel::kDiodeLadder,
    SynthFilterModel::kSallenKey,
};

constexpr SynthFilterModel kSelfOscModels[] = {
    SynthFilterModel::kMoogLadder,
    SynthFilterModel::kDiodeLadder,
    SynthFilterModel::kSallenKey,
};

MidiEvent event(const sonare::midi::Ump& ump) {
  MidiEvent e;
  e.ump = ump;
  return e;
}

/// Naive 110 Hz saw — a harmonically dense, worst-case-ish filter input.
float saw_sample(int i) {
  const double phase = std::fmod(110.0 * i / kRate, 1.0);
  return static_cast<float>(2.0 * phase - 1.0);
}

float rms(const std::vector<float>& buf, size_t from) {
  double acc = 0.0;
  size_t n = 0;
  for (size_t i = from; i < buf.size(); ++i) {
    acc += static_cast<double>(buf[i]) * buf[i];
    ++n;
  }
  return n > 0 ? static_cast<float>(std::sqrt(acc / static_cast<double>(n))) : 0.0f;
}

/// Dominant frequency from rising zero crossings over the analysed window.
double estimate_frequency(const std::vector<float>& buf, size_t from) {
  double first = -1.0;
  double last = -1.0;
  int cycles = -1;
  for (size_t i = from + 1; i < buf.size(); ++i) {
    if (buf[i - 1] < 0.0f && buf[i] >= 0.0f) {
      const double frac =
          static_cast<double>(buf[i - 1]) / (static_cast<double>(buf[i - 1]) - buf[i]);
      const double t = static_cast<double>(i - 1) + frac;
      if (first < 0.0) {
        first = t;
      } else {
        last = t;
      }
      ++cycles;
    }
  }
  if (cycles < 1 || last <= first) return 0.0;
  return kRate * static_cast<double>(cycles) / (last - first);
}

}  // namespace

TEST_CASE("filter models survive full cutoff/resonance sweeps without divergence",
          "[midi][synth][filter]") {
  // 1 second: cutoff sweeps 20 Hz -> 20 kHz -> 20 Hz exponentially while the
  // resonance ramps 0 -> max -> 0, against a dense saw input. Everything must
  // stay finite and bounded.
  constexpr int kN = 48000;
  for (const SynthFilterModel model : kAllModels) {
    SynthFilter filter;
    filter.prepare(kRate);
    filter.set_model(model);
    float peak = 0.0f;
    bool all_finite = true;
    for (int i = 0; i < kN; ++i) {
      const double t = static_cast<double>(i) / kN;
      const double sweep = t < 0.5 ? 2.0 * t : 2.0 - 2.0 * t;  // 0 -> 1 -> 0
      const float cutoff = static_cast<float>(20.0 * std::pow(1000.0, sweep));
      const float q = 0.5f + static_cast<float>(sweep) * 29.5f;
      filter.set(cutoff, q);
      const float y = filter.process(saw_sample(i), SynthFilterOutput::kLowpass);
      all_finite = all_finite && std::isfinite(y);
      peak = std::max(peak, std::fabs(y));
    }
    INFO("model " << static_cast<int>(model));
    REQUIRE(all_finite);
    REQUIRE(peak < 100.0f);
  }
}

TEST_CASE("filter models attenuate above cutoff and pass below it", "[midi][synth][filter]") {
  constexpr int kN = 9600;
  for (const SynthFilterModel model : kAllModels) {
    auto level_at = [&](double freq_hz) {
      SynthFilter filter;
      filter.prepare(kRate);
      filter.set_model(model);
      filter.set(500.0f, 0.707f);
      std::vector<float> out(kN);
      for (int i = 0; i < kN; ++i) {
        const float x = static_cast<float>(std::sin(2.0 * 3.14159265358979 * freq_hz * i / kRate));
        out[static_cast<size_t>(i)] = filter.process(x, SynthFilterOutput::kLowpass);
      }
      return rms(out, kN / 2);
    };
    const float low = level_at(100.0);
    const float high = level_at(8000.0);
    INFO("model " << static_cast<int>(model));
    // The diode ladder's spread poles give a gentler, darker passband
    // (authentic 303/VCS3 character); the others sit near unity.
    const float passband_floor = model == SynthFilterModel::kDiodeLadder ? 0.15f : 0.3f;
    REQUIRE(low > passband_floor);
    REQUIRE(high < 0.05f * low);  // > ~26 dB down at 4 octaves above Fc
  }
}

TEST_CASE("ladder and Sallen-Key models self-oscillate deterministically",
          "[midi][synth][filter]") {
  constexpr int kN = 48000;
  constexpr double kCutoff = 1000.0;
  for (const SynthFilterModel model : kSelfOscModels) {
    auto run = [&]() {
      SynthFilter filter;
      filter.prepare(kRate);
      filter.set_model(model);
      filter.set(static_cast<float>(kCutoff), kSelfOscQ);
      std::vector<float> out(kN);
      for (int i = 0; i < kN; ++i) {
        const float x = i == 0 ? 1.0f : 0.0f;  // single-sample excitation
        out[static_cast<size_t>(i)] = filter.process(x, SynthFilterOutput::kLowpass);
      }
      return out;
    };
    const std::vector<float> first = run();
    INFO("model " << static_cast<int>(model));
    // Still ringing near the end (self-sustained, tanh-bounded; the diode
    // ladder's huge loop gain keeps its bounded amplitude small).
    const float tail_rms = rms(first, kN - 4800);
    REQUIRE(tail_rms > 0.005f);
    REQUIRE(tail_rms < 10.0f);
    // The oscillation sits near the set cutoff.
    const double freq = estimate_frequency(first, kN - 4800);
    REQUIRE(freq > kCutoff * 0.8);
    REQUIRE(freq < kCutoff * 1.25);
    // Bit-identical across runs.
    const std::vector<float> second = run();
    REQUIRE(first == second);
  }
}

TEST_CASE("NativeSynth dispatches every filter model audibly and deterministically",
          "[midi][synth][filter]") {
  for (const SynthFilterModel model : kAllModels) {
    NativeSynthConfig cfg;
    cfg.patch.filter_model = model;
    cfg.patch.cutoff_hz = 1800.0f;
    cfg.patch.resonance_q = 8.0f;
    cfg.patch.drive = 0.4f;
    cfg.patch.env_to_cutoff_cents = 1200.0f;

    auto run = [&]() {
      NativeSynth synth(cfg);
      synth.prepare(kRate, 256);
      synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 48, 110)));
      std::vector<float> left(2048, 0.0f);
      std::vector<float> right(2048, 0.0f);
      float* chans[2] = {left.data(), right.data()};
      synth.process(chans, 2, 2048);
      return left;
    };
    const std::vector<float> first = run();
    float peak = 0.0f;
    bool all_finite = true;
    for (float s : first) {
      peak = std::max(peak, std::fabs(s));
      all_finite = all_finite && std::isfinite(s);
    }
    INFO("model " << static_cast<int>(model));
    REQUIRE(all_finite);
    REQUIRE(peak > 0.01f);
    REQUIRE(first == run());
  }
}
