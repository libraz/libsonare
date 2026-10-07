#pragma once

/// @file far_event.h
/// @brief Audio for far-event invariance tests: a weak change inside a target region, with a
///        dominant change inserted far from it. Every material has RMS 0.1.
///
/// The target region is `2 * (window_sec + 2)` long: the first half is the "before" material and
/// the second half the "after" material of the weak change. The inserted variant extends the
/// target's edge material by `distance_factor * window_sec` and then switches to the dominant
/// material for 4 s. Synthesis uses one time axis anchored at the target start, so the target
/// samples are identical in both variants and no junction switches material.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <random>
#include <vector>

#include "util/constants.h"

namespace sonare::test {

/// @brief Material the edge material switches to at the dominant change.
enum class DominantKind { ToNoise, ToClickTrain, ToSaw };
/// @brief Kind of the weak change in the middle of the target region.
enum class WeakKind { TimbreSmall, KeyChange };
/// @brief Which side of the target region the inserted span is placed on.
enum class Side { Before, After };

struct FarEventRow {
  DominantKind dominant;
  Side side;
  float distance_factor;
  WeakKind weak;
};

inline constexpr FarEventRow kFarEventRows[8] = {
    {DominantKind::ToSaw, Side::After, 3.0f, WeakKind::TimbreSmall},
    {DominantKind::ToNoise, Side::Before, 1.5f, WeakKind::TimbreSmall},
    {DominantKind::ToNoise, Side::Before, 3.0f, WeakKind::KeyChange},
    {DominantKind::ToClickTrain, Side::Before, 1.5f, WeakKind::TimbreSmall},
    {DominantKind::ToSaw, Side::Before, 1.5f, WeakKind::KeyChange},
    {DominantKind::ToClickTrain, Side::After, 3.0f, WeakKind::KeyChange},
    {DominantKind::ToSaw, Side::After, 1.5f, WeakKind::KeyChange},
    {DominantKind::ToNoise, Side::After, 1.5f, WeakKind::TimbreSmall},
};

struct FarEventCase {
  std::vector<float> base;      ///< Target region only.
  std::vector<float> inserted;  ///< Target region plus the inserted span on `side`.
  float target_offset_sec;      ///< Start of the target region in `inserted`.
  float weak_change_sec;        ///< Weak change time from the target start.
  float target_length_sec;
};

namespace far_event_detail {

enum class Material { ToneA, ToneB, TriadC, TriadG, Noise, Click, Saw };

inline constexpr double kMaterialRms = 0.1;
inline constexpr double kBaseHz = 220.0;
inline constexpr double kClickRateHz = 8.0;
inline constexpr double kClickHz = 1000.0;
inline constexpr double kClickDecaySec = 0.005;
inline constexpr int kSawHarmonics = 20;
inline constexpr unsigned kNoiseSeed = 7u;
inline constexpr double kDominantSec = 4.0;

struct Partial {
  double hz;
  double amp;
};

/// @brief Partials (frequency, amplitude) of a tonal material; empty for the others.
inline std::vector<Partial> partials_of(Material m) {
  auto harmonics = [](double f0, double a1, double a2, double a3, std::vector<Partial>& out) {
    out.push_back({f0, a1});
    out.push_back({2.0 * f0, a2});
    out.push_back({3.0 * f0, a3});
  };
  std::vector<Partial> p;
  switch (m) {
    case Material::ToneA:
      harmonics(kBaseHz, 1.0, 0.5, 0.25, p);
      break;
    case Material::ToneB:
      harmonics(kBaseHz, 1.0, 0.25, 0.5, p);
      break;
    case Material::TriadC:
      for (double f : {261.6, 329.6, 392.0}) harmonics(f, 1.0, 0.5, 0.25, p);
      break;
    case Material::TriadG:
      for (double f : {196.0, 246.9, 293.7}) harmonics(f, 1.0, 0.5, 0.25, p);
      break;
    case Material::Saw:
      for (int k = 1; k <= kSawHarmonics; ++k) p.push_back({kBaseHz * k, 1.0 / k});
      break;
    default:
      break;
  }
  return p;
}

/// @brief Unit click-train sample at time t (seconds on the shared axis).
inline double click_at(double t) {
  const double since = t - std::floor(t * kClickRateHz) / kClickRateHz;
  return std::exp(-since / kClickDecaySec) * std::sin(constants::kTwoPiD * kClickHz * since);
}

/// @brief Gain that brings the unit click train to RMS 0.1 (mean square over one period).
inline double click_gain(int sr) {
  const int period = static_cast<int>(std::lround(sr / kClickRateHz));
  double sum = 0.0;
  for (int n = 0; n < period; ++n) {
    const double v = click_at(static_cast<double>(n) / sr);
    sum += v * v;
  }
  return kMaterialRms / std::sqrt(sum / period);
}

/// @brief Append `count` samples of `m`; sample `n` of the output sits at t = (n - origin) / sr.
inline void render(std::vector<float>& out, Material m, std::size_t count, long origin, int sr,
                   std::mt19937& rng) {
  const long begin = static_cast<long>(out.size());
  if (m == Material::Noise) {
    std::normal_distribution<double> dist(0.0, 1.0);
    std::vector<double> v(count);
    double ss = 0.0;
    for (auto& s : v) {
      s = dist(rng);
      ss += s * s;
    }
    const double gain = kMaterialRms / std::sqrt(ss / std::max<std::size_t>(count, 1));
    for (double s : v) out.push_back(static_cast<float>(s * gain));
    return;
  }
  if (m == Material::Click) {
    const double gain = click_gain(sr);
    for (std::size_t i = 0; i < count; ++i) {
      const double t = static_cast<double>(begin + static_cast<long>(i) - origin) / sr;
      out.push_back(static_cast<float>(gain * click_at(t)));
    }
    return;
  }
  const std::vector<Partial> parts = partials_of(m);
  double power = 0.0;
  for (const auto& p : parts) power += p.amp * p.amp * 0.5;
  const double gain = kMaterialRms / std::sqrt(power);
  for (std::size_t i = 0; i < count; ++i) {
    const double t = static_cast<double>(begin + static_cast<long>(i) - origin) / sr;
    double v = 0.0;
    for (const auto& p : parts) v += p.amp * std::sin(constants::kTwoPiD * p.hz * t);
    out.push_back(static_cast<float>(gain * v));
  }
}

inline std::size_t samples_of(double sec, int sr) {
  return static_cast<std::size_t>(std::lround(sec * sr));
}

}  // namespace far_event_detail

/// @brief Build the target-only and target-plus-insertion signals for one row.
/// @param row Row of kFarEventRows
/// @param window_sec Reference window the dominant change must lie beyond (seconds)
/// @param sr Sample rate in Hz
inline FarEventCase make_far_event_case(const FarEventRow& row, float window_sec, int sr) {
  using namespace far_event_detail;
  const double w = window_sec;
  const double target_sec = 2.0 * (w + 2.0);
  const double ext_sec = static_cast<double>(row.distance_factor) * w;
  const double insert_sec = ext_sec + kDominantSec;

  const Material first = (row.weak == WeakKind::TimbreSmall) ? Material::ToneA : Material::TriadC;
  const Material second = (row.weak == WeakKind::TimbreSmall) ? Material::ToneB : Material::TriadG;
  Material dominant = Material::Noise;
  if (row.dominant == DominantKind::ToClickTrain) dominant = Material::Click;
  if (row.dominant == DominantKind::ToSaw) dominant = Material::Saw;

  const std::size_t half = samples_of(target_sec / 2.0, sr);
  const std::size_t ext = samples_of(ext_sec, sr);
  const std::size_t dom = samples_of(kDominantSec, sr);

  std::mt19937 rng(kNoiseSeed);
  FarEventCase c;
  render(c.base, first, half, 0, sr, rng);
  render(c.base, second, half, 0, sr, rng);

  if (row.side == Side::After) {
    c.inserted = c.base;
    render(c.inserted, second, ext, 0, sr, rng);
    render(c.inserted, dominant, dom, 0, sr, rng);
    c.target_offset_sec = 0.0f;
  } else {
    const long origin = static_cast<long>(dom + ext);
    render(c.inserted, dominant, dom, origin, sr, rng);
    render(c.inserted, first, ext, origin, sr, rng);
    c.inserted.insert(c.inserted.end(), c.base.begin(), c.base.end());
    c.target_offset_sec = static_cast<float>(insert_sec);
  }
  c.weak_change_sec = static_cast<float>(target_sec / 2.0);
  c.target_length_sec = static_cast<float>(target_sec);
  return c;
}

}  // namespace sonare::test
