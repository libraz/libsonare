#pragma once

/// @file singing_measure.h
/// @brief Measurements shared by the singing-voice tests: an autocorrelation F0
///        track on fixed 2048-sample windows at a 512 hop, frame RMS, and the
///        renders of the `singing-voice` preset they read.
///
/// Autocorrelation rather than an FFT peak, because a silent window has no
/// autocorrelation peak and reports so, where a spectrum always has a largest bin.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include "midi/synth/native_synth.h"
#include "midi/synth/synth_presets.h"
#include "util/constants.h"

namespace sonare::test::singing {

inline constexpr int kRate = 48000;
inline constexpr std::size_t kF0Window = 2048;
inline constexpr std::size_t kF0Hop = 512;

/// F0 of @p n samples at @p x, 0 when the window has no periodicity: the first
/// autocorrelation peak within 85 % of the strongest, refined by a parabola.
inline double acf_f0(const float* x, std::size_t n, double rate = kRate) {
  const int min_lag = static_cast<int>(rate / 1200.0);
  const int max_lag = static_cast<int>(rate / 50.0);
  std::vector<double> r(static_cast<std::size_t>(max_lag) + 2, 0.0);
  for (int lag = min_lag; lag <= max_lag + 1; ++lag) {
    double sum = 0.0;
    for (std::size_t i = 0; i + static_cast<std::size_t>(lag) < n; ++i) {
      sum += static_cast<double>(x[i]) * x[i + static_cast<std::size_t>(lag)];
    }
    r[static_cast<std::size_t>(lag)] = sum / static_cast<double>(n - static_cast<std::size_t>(lag));
  }
  double strongest = 0.0;
  for (int lag = min_lag + 1; lag <= max_lag; ++lag) strongest = std::max(strongest, r[lag]);
  if (strongest <= 0.0) return 0.0;
  for (int lag = min_lag + 1; lag <= max_lag; ++lag) {
    if (r[lag] > r[lag - 1] && r[lag] >= r[lag + 1] && r[lag] > 0.85 * strongest) {
      const double den = r[lag - 1] - 2.0 * r[lag] + r[lag + 1];
      const double shift = den != 0.0 ? 0.5 * (r[lag - 1] - r[lag + 1]) / den : 0.0;
      return rate / (lag + shift);
    }
  }
  return 0.0;
}

/// Cents of every window's F0 against @p note_hz over [from, to); a window
/// with no F0 reads as -1e9 so a caller's bound fails on it.
inline std::vector<double> cents_track(const std::vector<float>& x, std::size_t from,
                                       std::size_t to, double note_hz) {
  std::vector<double> out;
  for (std::size_t at = from; at + kF0Window <= to; at += kF0Hop) {
    const double f = acf_f0(x.data() + at, kF0Window);
    out.push_back(f > 0.0 ? static_cast<double>(constants::kCentsPerOctave) * std::log2(f / note_hz)
                          : -1.0e9);
  }
  return out;
}

/// RMS of [from, to).
inline double span_rms(const std::vector<float>& x, std::size_t from, std::size_t to) {
  double sum = 0.0;
  for (std::size_t i = from; i < to; ++i) sum += static_cast<double>(x[i]) * x[i];
  return to > from ? std::sqrt(sum / static_cast<double>(to - from)) : 0.0;
}

/// RMS per @p frame samples across the whole signal.
inline std::vector<double> frame_rms(const std::vector<float>& x, std::size_t frame) {
  std::vector<double> out;
  for (std::size_t at = 0; at + frame <= x.size(); at += frame) {
    out.push_back(span_rms(x, at, at + frame));
  }
  return out;
}

/// The catalog's singing preset as a synth config, unity gain and no bus drive.
inline midi::synth::NativeSynthConfig singing_config() {
  const midi::synth::SynthPreset* preset = midi::synth::find_synth_preset("singing-voice");
  midi::synth::NativeSynthConfig cfg =
      preset != nullptr ? preset->config : midi::synth::NativeSynthConfig{};
  cfg.gain = 1.0f;
  cfg.polyphony = 4;
  cfg.bus_drive = 0.0f;
  return cfg;
}

}  // namespace sonare::test::singing
