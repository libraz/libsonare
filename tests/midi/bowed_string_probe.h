#pragma once

/// @file bowed_string_probe.h
/// @brief Measurement helpers for the bowed-string engine: how often the string
///        slips per period, when Helmholtz motion establishes, the octave-band
///        tilt of a spectrum, and the bridge force the engine's output stands
///        in for.
///
/// A slip is a discontinuity rather than a zero crossing, so every count here
/// runs off the first difference against a hysteretic threshold set from that
/// difference's own RMS — a smooth waveform's crest factor is sqrt(2) and never
/// arms it, which is what separates a slipping string from a sinusoidal one.
/// Helmholtz motion is exactly one slip per period, so the onset predicate is
/// two-sided: no slip at all is not an established Helmholtz regime either.
///
/// Every result carries the number of comparisons it reached alongside its
/// verdict, so a measurement that never ran reads as zero reach rather than as
/// a clean answer.

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

#include "core/fft.h"
#include "util/constants.h"

namespace sonare::test::bowed {

/// Slip-detector arm threshold as a multiple of the first difference's RMS.
/// Above a smooth waveform's crest factor (sqrt(2)), below the crest a
/// stick-slip step reaches.
inline constexpr double kSlipArmFactor = 2.5;
/// Re-arm threshold in the same RMS units: the difference must fall back this
/// far before the next slip counts, so one transition is one event.
inline constexpr double kSlipDisarmFactor = 1.2;

/// Helmholtz motion is one slip per period. The upper bound separates it from
/// multiple slip; the lower separates it from a string that never slips at all,
/// which is the case a one-sided "below 1.5" test would call established.
inline constexpr double kHelmholtzLo = 0.5;
inline constexpr double kHelmholtzHi = 1.5;

/// Onset scan: the slip rate is read over a window this many periods long,
/// hopped one period at a time, and must hold the verdict this many hops.
inline constexpr int kOnsetWindowPeriods = 2;
inline constexpr int kOnsetHoldPeriods = 8;

/// Longest analysis transform; a longer span is read over its trailing window so
/// the measurement sits in the steady region rather than across the attack.
inline constexpr int kProbeFftMax = 32768;
inline constexpr int kProbeFftMin = 1024;

/// Octave bands further below the loudest than this carry no usable energy and
/// are dropped before the slope is fitted.
inline constexpr double kBandFloorDb = 120.0;

/// Largest power of two within [kProbeFftMin, kProbeFftMax] that fits @p count.
inline int probe_fft_size(std::size_t count) noexcept {
  int fft = kProbeFftMin;
  while (fft * 2 <= kProbeFftMax && static_cast<std::size_t>(fft * 2) <= count) fft *= 2;
  return fft;
}

/// Hann-windowed power spectrum of @p span from @p from over @p fft samples,
/// zero-padded past the end.
inline std::vector<double> probe_power_spectrum(const std::vector<float>& span, std::size_t from,
                                                int fft) {
  std::vector<float> windowed(static_cast<std::size_t>(fft), 0.0f);
  for (int i = 0; i < fft; ++i) {
    const double w =
        0.5 - 0.5 * std::cos(sonare::constants::kTwoPiD * i / static_cast<double>(fft - 1));
    const std::size_t idx = from + static_cast<std::size_t>(i);
    windowed[static_cast<std::size_t>(i)] =
        idx < span.size() ? static_cast<float>(span[idx] * w) : 0.0f;
  }
  sonare::FFT plan(fft);
  std::vector<std::complex<float>> spectrum(static_cast<std::size_t>(plan.n_bins()));
  plan.forward(windowed.data(), spectrum.data());
  std::vector<double> power(spectrum.size());
  for (std::size_t i = 0; i < spectrum.size(); ++i) power[i] = std::norm(spectrum[i]);
  return power;
}

/// Trailing analysis window offset for a span of @p count samples at @p fft.
inline std::size_t probe_window_start(std::size_t count, int fft) noexcept {
  return count > static_cast<std::size_t>(fft) ? count - static_cast<std::size_t>(fft) : 0;
}

/// Slips in @p count samples from @p first: a slip is armed when the first
/// difference rises above kSlipArmFactor RMS and re-arms below kSlipDisarmFactor.
inline int slip_count(const float* first, std::size_t count) noexcept {
  if (first == nullptr || count < 3) return 0;
  double acc = 0.0;
  for (std::size_t i = 1; i < count; ++i) {
    const double d = static_cast<double>(first[i]) - static_cast<double>(first[i - 1]);
    acc += d * d;
  }
  const double rms = std::sqrt(acc / static_cast<double>(count - 1));
  if (!(rms > 0.0)) return 0;
  const double arm = kSlipArmFactor * rms;
  const double disarm = kSlipDisarmFactor * rms;
  int slips = 0;
  bool armed = true;
  for (std::size_t i = 1; i < count; ++i) {
    const double d = std::fabs(static_cast<double>(first[i]) - static_cast<double>(first[i - 1]));
    if (armed) {
      if (d >= arm) {
        ++slips;
        armed = false;
      }
    } else if (d <= disarm) {
      armed = true;
    }
  }
  return slips;
}

/// Slip rate over a measured region. `periods` is the reach: zero means nothing
/// was counted and `per_period` carries no verdict.
struct SlipRate {
  double per_period = 0.0;
  int slips = 0;
  double periods = 0.0;
};

/// Mean slips per period of @p span over the steady region (its trailing half)
/// for a fundamental of @p f0 at @p sr. 1.0 is Helmholtz; above kHelmholtzHi is
/// multiple slip.
inline SlipRate slips_per_period(const std::vector<float>& span, double f0, double sr) noexcept {
  SlipRate out;
  if (span.size() < 8 || f0 <= 0.0 || sr <= 0.0) return out;
  const std::size_t from = span.size() / 2;
  const std::size_t count = span.size() - from;
  out.periods = static_cast<double>(count) * f0 / sr;
  if (!(out.periods > 0.0)) return out;
  out.slips = slip_count(span.data() + from, count);
  out.per_period = static_cast<double>(out.slips) / out.periods;
  return out;
}

/// Helmholtz onset. `windows` is the reach: zero means the span was too short
/// for a single scan and `established` carries no verdict.
struct HelmholtzOnset {
  bool established = false;
  double seconds = 0.0;
  int windows = 0;
};

/// First time the slip rate of @p span enters [kHelmholtzLo, kHelmholtzHi) and
/// holds it for kOnsetHoldPeriods consecutive one-period hops.
inline HelmholtzOnset time_to_helmholtz(const std::vector<float>& span, double f0,
                                        double sr) noexcept {
  HelmholtzOnset out;
  if (f0 <= 0.0 || sr <= 0.0) return out;
  const double period = sr / f0;
  const std::size_t hop = static_cast<std::size_t>(period);
  const std::size_t win = static_cast<std::size_t>(period * kOnsetWindowPeriods);
  if (hop < 2 || win < 8 || span.size() <= win) return out;
  const std::size_t last = span.size() - win;
  out.windows = static_cast<int>(last / hop) + 1;
  const double periods = static_cast<double>(win) * f0 / sr;
  int held = 0;
  std::size_t run_start = 0;
  for (std::size_t at = 0; at <= last; at += hop) {
    const double rate = static_cast<double>(slip_count(span.data() + at, win)) / periods;
    if (rate >= kHelmholtzLo && rate < kHelmholtzHi) {
      if (held == 0) run_start = at;
      ++held;
      if (held >= kOnsetHoldPeriods) {
        out.established = true;
        out.seconds = static_cast<double>(run_start) / sr;
        return out;
      }
    } else {
      held = 0;
    }
  }
  return out;
}

/// Octave-band slope. `bands` is the reach: fewer than two bands fits no slope.
struct BandTilt {
  double db_per_octave = 0.0;
  int bands = 0;
};

/// Least-squares dB/octave over the RMS of the octave bands spanning
/// [@p lo_hz, @p hi_hz) in @p span, read over its trailing analysis window.
inline BandTilt octave_band_tilt(const std::vector<float>& span, double lo_hz, double hi_hz,
                                 double sr) {
  BandTilt out;
  if (span.size() < static_cast<std::size_t>(kProbeFftMin) || lo_hz <= 0.0 ||
      hi_hz <= 2.0 * lo_hz || sr <= 0.0) {
    return out;
  }
  const int fft = probe_fft_size(span.size());
  const std::vector<double> power =
      probe_power_spectrum(span, probe_window_start(span.size(), fft), fft);
  const double bin_hz = sr / static_cast<double>(fft);
  std::vector<double> x;
  std::vector<double> y;
  for (double edge = lo_hz; edge * 2.0 <= hi_hz; edge *= 2.0) {
    const int b0 = std::max(1, static_cast<int>(std::ceil(edge / bin_hz)));
    const int b1 =
        std::min(static_cast<int>(power.size()), static_cast<int>(std::ceil(edge * 2.0 / bin_hz)));
    double acc = 0.0;
    for (int b = b0; b < b1; ++b) acc += power[static_cast<std::size_t>(b)];
    if (!(acc > 0.0)) continue;
    // Octave centre on a log2 axis anchored at lo_hz, so the slope is per octave.
    x.push_back(std::log2(edge / lo_hz) + 0.5);
    y.push_back(10.0 * std::log10(acc));
  }
  if (y.size() < 2) return out;
  const double top = *std::max_element(y.begin(), y.end());
  std::vector<double> fx;
  std::vector<double> fy;
  for (std::size_t i = 0; i < y.size(); ++i) {
    if (y[i] >= top - kBandFloorDb) {
      fx.push_back(x[i]);
      fy.push_back(y[i]);
    }
  }
  if (fy.size() < 2) return out;
  double mx = 0.0;
  double my = 0.0;
  for (std::size_t i = 0; i < fx.size(); ++i) {
    mx += fx[i];
    my += fy[i];
  }
  mx /= static_cast<double>(fx.size());
  my /= static_cast<double>(fx.size());
  double num = 0.0;
  double den = 0.0;
  for (std::size_t i = 0; i < fx.size(); ++i) {
    num += (fx[i] - mx) * (fy[i] - my);
    den += (fx[i] - mx) * (fx[i] - mx);
  }
  if (!(den > 0.0)) return out;
  out.db_per_octave = num / den;
  out.bands = static_cast<int>(fx.size());
  return out;
}

/// |1 + g H(w)| in dB at @p hz for a bridge loss gain @p loss_gain and a one-pole
/// reflection filter y += @p lp_alpha * (x - y) — the factor the design's first
/// premise bounds, and the analytic answer the measured ratio is read against.
inline double bridge_force_ratio_db(double loss_gain, double lp_alpha, double hz,
                                    double sr) noexcept {
  const double pole = 1.0 - lp_alpha;
  const double w = sonare::constants::kTwoPiD * hz / sr;
  const double c = std::cos(w);
  const double s = std::sin(w);
  const double a = 1.0 + loss_gain * lp_alpha;
  const double num = (a - pole * c) * (a - pole * c) + (pole * s) * (pole * s);
  const double den = (1.0 - pole * c) * (1.0 - pole * c) + (pole * s) * (pole * s);
  return 10.0 * std::log10(num / den);
}

/// Full-band variation in dB of |1 + g H(w)|: the magnitude is monotone in w, so
/// DC minus Nyquist is the whole span.
inline double bridge_force_variation_db(double loss_gain, double lp_alpha, double sr) noexcept {
  return bridge_force_ratio_db(loss_gain, lp_alpha, 0.0, sr) -
         bridge_force_ratio_db(loss_gain, lp_alpha, 0.5 * sr, sr);
}

/// Measured bridge-force ratio. `bins` is the reach: zero means no bin carried
/// enough energy to compare and `variation_db` carries no verdict.
struct BridgeForce {
  double variation_db = 0.0;
  double lo_hz = 0.0;
  double hi_hz = 0.0;
  int bins = 0;
};

/// Spectral ratio |Z0(v+ - v-)| / |Z0 v+| over the bins of @p v_plus within
/// @p floor_db of its peak. @p v_plus is the wave arriving at the bridge — the
/// engine's dry output up to output_scale_, which cancels in a ratio — and the
/// wave leaving is reconstructed as v- = -@p loss_gain * lowpass(v+), the
/// reflection bowed_string_voice.cpp:257-258 forms from the same sample. The two
/// coefficients mirror the patch mapping at bowed_string_voice.cpp:141 and :145.
inline BridgeForce bridge_force_ratio(const std::vector<float>& v_plus, double loss_gain,
                                      double lp_alpha, double floor_db, double sr) {
  BridgeForce out;
  if (v_plus.size() < static_cast<std::size_t>(kProbeFftMin) || sr <= 0.0) return out;
  std::vector<float> force(v_plus.size(), 0.0f);
  double lp = 0.0;
  for (std::size_t i = 0; i < v_plus.size(); ++i) {
    lp += lp_alpha * (static_cast<double>(v_plus[i]) - lp);
    force[i] = static_cast<float>(static_cast<double>(v_plus[i]) + loss_gain * lp);
  }
  const int fft = probe_fft_size(v_plus.size());
  const std::size_t from = probe_window_start(v_plus.size(), fft);
  const std::vector<double> pv = probe_power_spectrum(v_plus, from, fft);
  const std::vector<double> pf = probe_power_spectrum(force, from, fft);
  double top = 0.0;
  for (std::size_t b = 1; b < pv.size(); ++b) top = std::max(top, pv[b]);
  if (!(top > 0.0)) return out;
  const double floor_power = top * std::pow(10.0, floor_db / 10.0);
  const double bin_hz = sr / static_cast<double>(fft);
  double lo_db = 0.0;
  double hi_db = 0.0;
  for (std::size_t b = 1; b < pv.size(); ++b) {
    if (pv[b] < floor_power) continue;
    const double db = 10.0 * std::log10(pf[b] / pv[b]);
    if (out.bins == 0) {
      lo_db = db;
      hi_db = db;
      out.lo_hz = static_cast<double>(b) * bin_hz;
    }
    lo_db = std::min(lo_db, db);
    hi_db = std::max(hi_db, db);
    out.hi_hz = static_cast<double>(b) * bin_hz;
    ++out.bins;
  }
  out.variation_db = hi_db - lo_db;
  return out;
}

}  // namespace sonare::test::bowed
