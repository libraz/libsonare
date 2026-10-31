#include "singing/pitch_fit.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <vector>

#include "midi/synth/singing_voice.h"
#include "util/constants.h"

namespace sonare::tools::singing {

namespace {

using sonare::constants::kCentsPerOctave;
using sonare::midi::synth::SingingPatchParams;
using sonare::midi::synth::SingingPhonation;

// Settling time before the window, and the window itself (samples at 48 kHz).
constexpr int kSettle = 14400;
constexpr int kWindow = 9600;
// Scan span around the guess and its step, about 3.5 cents of F0 per step; the
// largest step one branch may take before two points count as different oscillations.
constexpr double kScanLow = 0.8;
constexpr double kScanHigh = 1.25;
constexpr double kScanRatio = 1.004;
constexpr double kBranchCents = 40.0;
// Median window half-width, and how far from the median a point may sit.
constexpr size_t kMedianHalf = 3;
constexpr double kOffBranchCents = 20.0;
constexpr double kNoPitch = -1.0e9;

/// Autocorrelation F0 with parabolic peak refinement: the first lag within 85 %
/// of the strongest, which keeps a strong second partial from reading as the period.
double acf_f0(const std::vector<float>& x, double sample_rate) {
  const int min_lag = static_cast<int>(sample_rate / 1200.0);
  const int max_lag = static_cast<int>(sample_rate / 50.0);
  const int n = static_cast<int>(x.size());
  std::vector<double> r(static_cast<size_t>(max_lag) + 2, 0.0);
  for (int lag = min_lag; lag <= max_lag + 1; ++lag) {
    double sum = 0.0;
    for (int i = 0; i + lag < n; ++i) sum += static_cast<double>(x[i]) * x[i + lag];
    r[static_cast<size_t>(lag)] = sum / (n - lag);
  }
  double strongest = 0.0;
  for (int lag = min_lag + 1; lag <= max_lag; ++lag) strongest = std::max(strongest, r[lag]);
  if (strongest <= 0.0) return 0.0;
  for (int lag = min_lag + 1; lag <= max_lag; ++lag) {
    if (r[lag] > r[lag - 1] && r[lag] >= r[lag + 1] && r[lag] > 0.85 * strongest) {
      const double y0 = r[lag - 1];
      const double y1 = r[lag];
      const double y2 = r[lag + 1];
      const double den = y0 - 2.0 * y1 + y2;
      const double shift = den != 0.0 ? 0.5 * (y0 - y2) / den : 0.0;
      return sample_rate / (lag + shift);
    }
  }
  return 0.0;
}

}  // namespace

double assembly_f0(float note_hz, float force, float tension) {
  SingingPhonation voice;
  voice.configure_patch(kFitSampleRate, note_hz, SingingPatchParams{});
  std::vector<float> window;
  window.reserve(kWindow);
  double energy = 0.0;
  for (int n = 0; n < kSettle + kWindow; ++n) {
    const float y = voice.process(force, 0.0f, tension, 0.0f);
    if (n >= kSettle) {
      window.push_back(y);
      energy += static_cast<double>(y) * y;
    }
  }
  if (!(energy / kWindow > 1.0e-10)) return 0.0;
  return acf_f0(window, kFitSampleRate);
}

float fit_tension(float note_hz, float force, float guess) {
  // Sample the F0 curve finely around the guess. A tension can settle into a
  // neighbouring oscillation, so a point off both its neighbours' branch is
  // dropped before the crossing is read off the branch that remains.
  std::vector<double> t;
  std::vector<double> c;
  for (double x = guess * kScanLow; x <= guess * kScanHigh; x *= kScanRatio) {
    const double f = assembly_f0(note_hz, force, static_cast<float>(x));
    t.push_back(x);
    c.push_back(f > 0.0 ? kCentsPerOctave * std::log2(f / note_hz) : kNoPitch);
  }
  const size_t n = t.size();
  std::vector<bool> keep(n, false);
  for (size_t i = 0; i < n; ++i) {
    if (c[i] == kNoPitch) continue;
    std::vector<double> around;
    for (size_t j = i >= kMedianHalf ? i - kMedianHalf : 0; j < n && j <= i + kMedianHalf; ++j) {
      if (c[j] != kNoPitch) around.push_back(c[j]);
    }
    std::nth_element(around.begin(), around.begin() + around.size() / 2, around.end());
    keep[i] = std::fabs(c[i] - around[around.size() / 2]) <= kOffBranchCents;
  }
  double best = -1.0;
  size_t prev = n;
  for (size_t i = 0; i < n; ++i) {
    if (!keep[i]) continue;
    if (prev < n && std::fabs(c[i] - c[prev]) <= kBranchCents && c[prev] <= 0.0 && c[i] > 0.0) {
      const double frac = -c[prev] / (c[i] - c[prev]);
      const double root = std::exp(std::log(t[prev]) + frac * (std::log(t[i]) - std::log(t[prev])));
      if (best < 0.0 || std::fabs(std::log(root / guess)) < std::fabs(std::log(best / guess))) {
        best = root;
      }
    }
    prev = i;
  }
  return static_cast<float>(best);
}

}  // namespace sonare::tools::singing
