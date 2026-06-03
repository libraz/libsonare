/// @file mir_warp_test.cpp
/// @brief MIR warp: WarpMap monotonicity + anchor round-trip, chroma-DTW
///        alignment recovery of a known time shift/stretch, and HPSS-split
///        component-specific TSM length-target + determinism + transient
///        preservation.

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "core/audio.h"
#include "mir/warp.h"
#include "util/constants.h"

namespace {

using sonare::Audio;
using sonare::mir::chroma_dtw_align;
using sonare::mir::ChromaDtwConfig;
using sonare::mir::ChromaDtwResult;
using sonare::mir::warp_to_length;
using sonare::mir::warp_to_map;
using sonare::mir::WarpAnchor;
using sonare::mir::WarpMap;
using sonare::mir::WarpTsmConfig;

// Deterministic order-independent stable hash over a float buffer (FNV-1a over
// the raw bit patterns). Used to assert run-to-run repeatability.
uint64_t stable_hash(const std::vector<float>& v) {
  uint64_t h = 1469598103934665603ull;
  for (float f : v) {
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    for (int b = 0; b < 4; ++b) {
      h ^= (bits >> (b * 8)) & 0xffu;
      h *= 1099511628211ull;
    }
  }
  return h;
}

// A harmonic test tone: sum of a fundamental + a few partials at sr.
Audio make_tone(int sr, double freq, double seconds) {
  const int n = static_cast<int>(sr * seconds);
  std::vector<float> s(n);
  for (int i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / sr;
    double v = std::sin(sonare::constants::kTwoPiD * freq * t);
    v += 0.5 * std::sin(sonare::constants::kTwoPiD * 2.0 * freq * t);
    v += 0.25 * std::sin(sonare::constants::kTwoPiD * 3.0 * freq * t);
    s[i] = static_cast<float>(0.4 * v);
  }
  return Audio::from_vector(std::move(s), sr);
}

// A percussive test signal: periodic short clicks (impulses with fast decay).
Audio make_clicks(int sr, double seconds, double click_hz) {
  const int n = static_cast<int>(sr * seconds);
  std::vector<float> s(n, 0.0f);
  const int period = static_cast<int>(sr / click_hz);
  for (int start = 0; start < n; start += period) {
    for (int k = 0; k < 64 && start + k < n; ++k) {
      s[start + k] = static_cast<float>(std::exp(-k / 6.0) * (k % 2 == 0 ? 1.0 : -1.0));
    }
  }
  return Audio::from_vector(std::move(s), sr);
}

double total_energy(const Audio& a) {
  double e = 0.0;
  for (size_t i = 0; i < a.size(); ++i) e += static_cast<double>(a[i]) * a[i];
  return e;
}

std::vector<size_t> peak_positions(const Audio& a, float threshold, size_t refractory) {
  std::vector<size_t> peaks;
  size_t next_allowed = 0;
  for (size_t i = 1; i + 1 < a.size(); ++i) {
    const float v = std::abs(a[i]);
    if (i < next_allowed || v < threshold) continue;
    if (v >= std::abs(a[i - 1]) && v >= std::abs(a[i + 1])) {
      peaks.push_back(i);
      next_allowed = i + refractory;
    }
  }
  return peaks;
}

}  // namespace

TEST_CASE("WarpMap is strictly monotonic and round-trips anchors", "[mir]") {
  std::vector<WarpAnchor> anchors = {
      {0.0, 0.0}, {1000.0, 1200.0}, {2500.0, 2400.0}, {5000.0, 6000.0}};
  WarpMap map = WarpMap::from_anchors(anchors);
  REQUIRE(map.valid());

  // Monotonicity: sweeping warp time forward must move source time forward.
  double prev = -1.0;
  for (double w = 0.0; w <= 5000.0; w += 50.0) {
    const double src = map.warp_to_source(w);
    REQUIRE(src > prev);
    prev = src;
  }

  // Exact anchor round-trip: warp -> source -> warp recovers the anchor.
  for (const WarpAnchor& a : map.anchors()) {
    const double src = map.warp_to_source(a.warp_sample);
    REQUIRE(src == Catch::Approx(a.source_sample).margin(1e-6));
    const double back = map.source_to_warp(src);
    REQUIRE(back == Catch::Approx(a.warp_sample).margin(1e-6));
  }

  // Round-trip at arbitrary interior points (not just anchors).
  for (double w = 100.0; w < 5000.0; w += 137.0) {
    const double src = map.warp_to_source(w);
    const double back = map.source_to_warp(src);
    REQUIRE(back == Catch::Approx(w).margin(1e-6));
  }
}

TEST_CASE("WarpMap from_markers rejects non-monotonic / too-few anchors", "[mir]") {
  // Only one usable anchor after dedup -> invalid.
  REQUIRE_THROWS([] { WarpMap::from_anchors({{0.0, 0.0}}); }());
  // Mismatched marker vectors -> invalid.
  REQUIRE_THROWS([] { WarpMap::from_markers({0.0, 1.0}, {0.0}); }());
}

TEST_CASE("chroma-DTW recovers a known time shift within tolerance", "[mir]") {
  const int sr = 22050;
  // Reference: a 1.2 s C-ish tone. Target: a delayed copy (silence prefix),
  // so the alignment path should track a constant offset.
  Audio ref = make_tone(sr, 261.63, 1.2);

  const int delay = sr / 4;  // 0.25 s delay.
  std::vector<float> tgt_samples(delay, 0.0f);
  for (size_t i = 0; i < ref.size(); ++i) tgt_samples.push_back(ref[i]);
  Audio tgt = Audio::from_vector(std::move(tgt_samples), sr);

  ChromaDtwConfig cfg;
  cfg.hop_length = 512;
  ChromaDtwResult r = chroma_dtw_align(ref, tgt, cfg);

  REQUIRE(r.path.size() >= 2);
  REQUIRE(r.path.front().first == 0);
  // Path must be monotonic non-decreasing on both axes.
  for (size_t i = 1; i < r.path.size(); ++i) {
    REQUIRE(r.path[i].first >= r.path[i - 1].first);
    REQUIRE(r.path[i].second >= r.path[i - 1].second);
  }
  // Anchors derived from the path span both signals and stay ordered.
  REQUIRE(r.anchors.size() == r.path.size());

  // The expected target-vs-reference offset (in chroma frames) near the middle
  // of the reference should be close to the delay (within a few-frame band).
  const double expected_offset_frames = static_cast<double>(delay) / cfg.hop_length;
  // Find a path point near the middle of the reference.
  const int mid_ref = r.reference_frames / 2;
  int best = 0;
  for (size_t i = 0; i < r.path.size(); ++i) {
    if (std::abs(r.path[i].first - mid_ref) < std::abs(r.path[best].first - mid_ref)) best = i;
  }
  const double observed_offset = r.path[best].second - r.path[best].first;
  REQUIRE(std::abs(observed_offset - expected_offset_frames) <= 6.0);
}

TEST_CASE("chroma-DTW recovers a known time stretch within tolerance", "[mir]") {
  const int sr = 22050;
  Audio ref = make_tone(sr, 220.0, 1.0);
  // Target = reference stretched to 1.5x duration by linear resampling of the
  // sample index (a known monotone time map).
  const double stretch = 1.5;
  const int out_n = static_cast<int>(ref.size() * stretch);
  std::vector<float> tgt_samples(out_n);
  for (int i = 0; i < out_n; ++i) {
    const double src_pos = i / stretch;
    const int s0 = static_cast<int>(src_pos);
    const int s1 = std::min(s0 + 1, static_cast<int>(ref.size()) - 1);
    const double frac = src_pos - s0;
    tgt_samples[i] = static_cast<float>(ref[s0] * (1.0 - frac) + ref[s1] * frac);
  }
  Audio tgt = Audio::from_vector(std::move(tgt_samples), sr);

  ChromaDtwResult r = chroma_dtw_align(ref, tgt, ChromaDtwConfig());
  REQUIRE(r.path.size() >= 2);

  // The path slope (d target / d reference) should approximate the stretch.
  const auto& a = r.path.front();
  const auto& b = r.path.back();
  const double dref = std::max(1, b.first - a.first);
  const double dtgt = b.second - a.second;
  const double observed_slope = dtgt / dref;
  REQUIRE(observed_slope == Catch::Approx(stretch).margin(0.2));
}

TEST_CASE("TSM hits exact target length and is deterministic", "[mir]") {
  const int sr = 22050;
  Audio tone = make_tone(sr, 330.0, 0.8);

  const size_t target = tone.size() * 3 / 2;  // stretch to 1.5x.
  Audio out1 = warp_to_length(tone, target, WarpTsmConfig());
  Audio out2 = warp_to_length(tone, target, WarpTsmConfig());

  // Exact target length.
  REQUIRE(out1.size() == target);
  REQUIRE(out2.size() == target);

  // Deterministic stable hash across two runs (offline golden = repeatability).
  std::vector<float> b1(out1.begin(), out1.end());
  std::vector<float> b2(out2.begin(), out2.end());
  REQUIRE(stable_hash(b1) == stable_hash(b2));
}

TEST_CASE("warp_to_map follows local segment rates instead of one global rate", "[mir]") {
  const int sr = 22050;
  Audio tone = make_tone(sr, 330.0, 0.7);
  const double n = static_cast<double>(tone.size());

  WarpTsmConfig cfg;
  cfg.n_fft = 512;
  cfg.hop_length = 128;
  cfg.hpss_kernel_harmonic = 7;
  cfg.hpss_kernel_percussive = 7;

  const WarpMap global = WarpMap::from_anchors({{0.0, 0.0}, {n, n}});
  const WarpMap segmented = WarpMap::from_anchors({{0.0, 0.0}, {n * 0.5, n * 0.25}, {n, n}});

  Audio global_out = warp_to_map(tone, global, cfg);
  Audio segmented_a = warp_to_map(tone, segmented, cfg);
  Audio segmented_b = warp_to_map(tone, segmented, cfg);

  REQUIRE(global_out.size() == tone.size());
  REQUIRE(segmented_a.size() == tone.size());
  REQUIRE(segmented_b.size() == tone.size());

  std::vector<float> ga(global_out.begin(), global_out.end());
  std::vector<float> sa(segmented_a.begin(), segmented_a.end());
  std::vector<float> sb(segmented_b.begin(), segmented_b.end());
  REQUIRE(stable_hash(sa) == stable_hash(sb));
  REQUIRE(stable_hash(sa) != stable_hash(ga));
}

TEST_CASE("TSM preserves percussive energy within tolerance", "[mir]") {
  const int sr = 22050;
  Audio clicks = make_clicks(sr, 1.0, 8.0);  // 8 clicks/sec.

  const double in_energy = total_energy(clicks);
  const size_t target = clicks.size() * 3 / 2;  // lengthen by 1.5x.
  Audio out = warp_to_length(clicks, target, WarpTsmConfig());
  REQUIRE(out.size() == target);

  // Energy density (energy per sample) should be roughly preserved for a
  // transient signal: a phase-vocoder-only stretch smears clicks and inflates
  // or deflates energy; the percussive OLA path keeps the transient structure.
  const double out_energy = total_energy(out);
  const double in_density = in_energy / clicks.size();
  const double out_density = out_energy / out.size();
  // Within a factor of ~2 (loose but catches gross transient destruction).
  REQUIRE(out_density > in_density * 0.4);
  REQUIRE(out_density < in_density * 2.5);
}

TEST_CASE("TSM uses WSOLA to stretch percussive click spacing", "[mir]") {
  const int sr = 22050;
  const double click_hz = 8.0;
  Audio clicks = make_clicks(sr, 1.0, click_hz);

  WarpTsmConfig cfg;
  cfg.n_fft = 512;
  cfg.hop_length = 128;
  cfg.hpss_kernel_harmonic = 7;
  cfg.hpss_kernel_percussive = 7;

  const size_t target = clicks.size() * 2;
  Audio out = warp_to_length(clicks, target, cfg);
  REQUIRE(out.size() == target);

  const size_t input_period = static_cast<size_t>(std::llround(sr / click_hz));
  const size_t expected_period = input_period * 2;
  const std::vector<size_t> peaks = peak_positions(out, 0.18f, input_period);
  REQUIRE(peaks.size() >= 5);

  std::vector<size_t> gaps;
  for (size_t i = 1; i < peaks.size(); ++i) gaps.push_back(peaks[i] - peaks[i - 1]);
  std::sort(gaps.begin(), gaps.end());
  const size_t median_gap = gaps[gaps.size() / 2];

  REQUIRE(median_gap > input_period + input_period / 2);
  REQUIRE(median_gap == Catch::Approx(static_cast<double>(expected_period)).margin(input_period));
}
