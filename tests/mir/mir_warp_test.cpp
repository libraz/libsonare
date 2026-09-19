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
#include <functional>
#include <limits>
#include <utility>
#include <vector>

#include "core/audio.h"
#include "mir/warp.h"
#include "mir/warp_detail.h"
#include "util/constants.h"

namespace {

using sonare::Audio;
using sonare::mir::chroma_dtw_align;
using sonare::mir::ChromaDtwConfig;
using sonare::mir::ChromaDtwResult;
using sonare::mir::strictly_increasing_anchors;
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

// A continuous pitch glide: the chroma moves every frame, so no two frames
// carry the same chroma and one alignment is strictly cheaper than the rest.
// Neither a stationary tone nor a held chord can do this -- a constant-chroma
// stretch is a plateau on the cost surface that a DTW path crosses for free,
// so the recovered offset is unconstrained across its whole length.
// Kept under an octave so no pitch class repeats, and phase-continuous at
// constant amplitude so the front end sees no energy notch to chase.
Audio make_glide(int sr, double f0, double semitones, double seconds) {
  const int n = static_cast<int>(sr * seconds);
  std::vector<float> s(static_cast<size_t>(n));
  double phase[2] = {};
  for (int i = 0; i < n; ++i) {
    const double frac = static_cast<double>(i) / n;
    const double freq = f0 * std::pow(2.0, semitones * frac / 12.0);
    double v = 0.0;
    for (int partial = 1; partial <= 2; ++partial) {
      double& p = phase[partial - 1];
      p += sonare::constants::kTwoPiD * freq * partial / sr;
      v += (partial == 1 ? 1.0 : 0.5) * std::sin(p);
    }
    s[static_cast<size_t>(i)] = static_cast<float>(0.4 * v);
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

// ---------------------------------------------------------------------------
// Banded DTW, both accumulated-cost shapes.
//
// The banded DP has internal linkage and the only route into it is a
// multi-second alignment, so both shapes live here: the full per-row matrix as
// the oracle, and the two-row window the DP runs. They must agree on the PATH
// element for element, not merely on the cost -- the three-way min is a min
// with a tie-break, so a cell can keep its cost while handing the tie to
// another predecessor, and only the path shows it.
// ---------------------------------------------------------------------------

// The DP's unreachable-cell sentinel.
constexpr float kBandInf = 1e30f;

using BandPath = std::vector<std::pair<int, int>>;
using CellCost = std::function<float(int, int)>;

struct BandedDtwOut {
  BandPath path;
  float final_cost = 0.0f;
};

// Oracle: one densely-stored accumulated-cost row per reference frame.
BandedDtwOut banded_dtw_full_matrix(int ref_frames, int tgt_frames, const std::vector<int>& lo,
                                    const std::vector<int>& hi, const CellCost& cell_cost) {
  std::vector<std::vector<float>> acc(ref_frames);
  std::vector<std::vector<int>> back(ref_frames);
  for (int i = 0; i < ref_frames; ++i) {
    const int width = hi[i] - lo[i] + 1;
    acc[i].assign(width, kBandInf);
    back[i].assign(width, -1);
  }
  auto at = [&](int i, int j) -> float {
    if (i < 0 || j < lo[i] || j > hi[i]) return kBandInf;
    return acc[i][j - lo[i]];
  };
  for (int i = 0; i < ref_frames; ++i) {
    for (int j = lo[i]; j <= hi[i]; ++j) {
      const float local = cell_cost(i, j);
      if (i == 0 && j == 0) {
        acc[i][j - lo[i]] = local;
        continue;
      }
      const float d = at(i - 1, j - 1);
      const float u = at(i - 1, j);
      const float l = (j > 0) ? at(i, j - 1) : kBandInf;
      float best = d;
      int bk = 0;
      if (u < best) {
        best = u;
        bk = 1;
      }
      if (l < best) {
        best = l;
        bk = 2;
      }
      if (best >= kBandInf) continue;
      acc[i][j - lo[i]] = best + local;
      back[i][j - lo[i]] = bk;
    }
  }
  int i = ref_frames - 1;
  int j = tgt_frames - 1;
  if (j < lo[i] || j > hi[i] || acc[i][j - lo[i]] >= kBandInf) {
    j = std::clamp(j, lo[i], hi[i]);
  }
  BandedDtwOut out;
  out.final_cost = acc[i][j - lo[i]];
  while (i >= 0 && j >= 0) {
    out.path.emplace_back(i, j);
    if (i == 0 && j == 0) break;
    const int bk = (j >= lo[i] && j <= hi[i]) ? back[i][j - lo[i]] : 0;
    if (bk == 0) {
      --i;
      --j;
    } else if (bk == 1) {
      --i;
    } else {
      --j;
    }
    if (i < 0) i = 0;
    if (j < 0) j = 0;
    if (bk == -1) break;
  }
  std::reverse(out.path.begin(), out.path.end());
  return out;
}

// The shipped shape: rows i-1 and i only, each re-based on its own band.
// `loose_tie` flips the three-way min to `<=`, which leaves every accumulated
// cost untouched and moves the path -- it is what proves the path assertion
// below is not vacuous.
BandedDtwOut banded_dtw_row_window(int ref_frames, int tgt_frames, const std::vector<int>& lo,
                                   const std::vector<int>& hi, const CellCost& cell_cost,
                                   bool loose_tie = false) {
  std::vector<std::vector<int>> back(ref_frames);
  for (int i = 0; i < ref_frames; ++i) {
    back[i].assign(hi[i] - lo[i] + 1, -1);
  }
  std::vector<float> prev_acc;
  std::vector<float> curr_acc;
  int prev_lo = 0;
  int prev_hi = -1;
  int curr_row = 0;
  auto at_prev = [&](int j) -> float {
    if (j < prev_lo || j > prev_hi) return kBandInf;
    return prev_acc[j - prev_lo];
  };
  auto at_curr = [&](int j) -> float {
    if (j < lo[curr_row] || j > hi[curr_row]) return kBandInf;
    return curr_acc[j - lo[curr_row]];
  };
  for (int i = 0; i < ref_frames; ++i) {
    curr_row = i;
    curr_acc.assign(hi[i] - lo[i] + 1, kBandInf);
    for (int j = lo[i]; j <= hi[i]; ++j) {
      const float local = cell_cost(i, j);
      if (i == 0 && j == 0) {
        curr_acc[j - lo[i]] = local;
        continue;
      }
      const float d = at_prev(j - 1);
      const float u = at_prev(j);
      const float l = (j > 0) ? at_curr(j - 1) : kBandInf;
      float best = d;
      int bk = 0;
      if (loose_tie ? (u <= best) : (u < best)) {
        best = u;
        bk = 1;
      }
      if (loose_tie ? (l <= best) : (l < best)) {
        best = l;
        bk = 2;
      }
      if (best >= kBandInf) continue;
      curr_acc[j - lo[i]] = best + local;
      back[i][j - lo[i]] = bk;
    }
    prev_acc.swap(curr_acc);
    prev_lo = lo[i];
    prev_hi = hi[i];
  }
  int i = ref_frames - 1;
  int j = tgt_frames - 1;
  if (j < lo[i] || j > hi[i] || at_prev(j) >= kBandInf) {
    j = std::clamp(j, lo[i], hi[i]);
  }
  BandedDtwOut out;
  out.final_cost = at_prev(j);
  while (i >= 0 && j >= 0) {
    out.path.emplace_back(i, j);
    if (i == 0 && j == 0) break;
    const int bk = (j >= lo[i] && j <= hi[i]) ? back[i][j - lo[i]] : 0;
    if (bk == 0) {
      --i;
      --j;
    } else if (bk == 1) {
      --i;
    } else {
      --j;
    }
    if (i < 0) i = 0;
    if (j < 0) j = 0;
    if (bk == -1) break;
  }
  std::reverse(out.path.begin(), out.path.end());
  return out;
}

struct BandRng {
  uint32_t state;
  uint32_t next() {
    state = state * 1664525u + 1013904223u;
    return state;
  }
  int in(int a, int b) { return a + static_cast<int>(next() % static_cast<uint32_t>(b - a + 1)); }
  float unit() { return static_cast<float>(next() >> 8) / static_cast<float>(1u << 24); }
};

// A band that only ever widens: a non-decreasing projection with a running min
// over lo and a running max over hi, then both corners anchored. Kept as a band
// shape for the two-row window below, not as the shape the DP is handed.
void make_band_widening(int ref_frames, int tgt_frames, int radius, BandRng& rng,
                        std::vector<int>& lo, std::vector<int>& hi) {
  lo.assign(ref_frames, 0);
  hi.assign(ref_frames, 0);
  int center = 0;
  for (int i = 0; i < ref_frames; ++i) {
    center = std::min(tgt_frames - 1, center + rng.in(0, 2));
    lo[i] = std::max(0, center - radius);
    hi[i] = std::min(tgt_frames - 1, center + radius);
    if (hi[i] < lo[i]) hi[i] = lo[i];
  }
  for (int i = 1; i < ref_frames; ++i) {
    lo[i] = std::min(lo[i], lo[i - 1]);
    hi[i] = std::max(hi[i], hi[i - 1]);
  }
  lo[0] = 0;
  hi[ref_frames - 1] = std::max(hi[ref_frames - 1], tgt_frames - 1);
}

// A band whose edges move and narrow per row. The window has to index each
// neighbour by that neighbour's own lo, which a band that never narrows cannot
// distinguish from indexing by a constant width.
void make_band_narrowing(int ref_frames, int tgt_frames, int radius, BandRng& rng,
                         std::vector<int>& lo, std::vector<int>& hi) {
  lo.assign(ref_frames, 0);
  hi.assign(ref_frames, 0);
  for (int i = 0; i < ref_frames; ++i) {
    const int center = std::min(tgt_frames - 1, i * tgt_frames / std::max(1, ref_frames));
    const int r = rng.in(1, std::max(1, radius));
    lo[i] = std::max(0, center - r);
    hi[i] = std::min(tgt_frames - 1, center + r);
    if (hi[i] < lo[i]) hi[i] = lo[i];
  }
  lo[0] = 0;
  hi[ref_frames - 1] = std::max(hi[ref_frames - 1], tgt_frames - 1);
}

// A band with a gap wide enough that no step crosses it, so every row past the
// cut is unreachable and the last row's corner cell stays at infinity.
void make_band_disjoint(int ref_frames, int tgt_frames, int radius, std::vector<int>& lo,
                        std::vector<int>& hi) {
  lo.assign(ref_frames, 0);
  hi.assign(ref_frames, 0);
  const int cut = std::max(1, ref_frames / 2);
  for (int i = 0; i < ref_frames; ++i) {
    if (i < cut) {
      lo[i] = 0;
      hi[i] = std::min(tgt_frames - 1, radius);
    } else {
      lo[i] = std::min(tgt_frames - 1, hi[cut - 1] + 2);
      hi[i] = std::max(tgt_frames - 1, lo[i]);
    }
  }
  lo[0] = 0;
  hi[ref_frames - 1] = std::max(hi[ref_frames - 1], tgt_frames - 1);
}

// The band the DP is handed: the projection's radius neighbourhood, repaired
// only where consecutive rows would not overlap, with both corners anchored.
// Mirrors banded_dtw_path's construction, which has internal linkage.
void make_band_projected(const std::vector<int>& projected_tgt, int tgt_frames, int radius,
                         std::vector<int>& lo, std::vector<int>& hi) {
  const int ref_frames = static_cast<int>(projected_tgt.size());
  lo.assign(ref_frames, 0);
  hi.assign(ref_frames, 0);
  for (int i = 0; i < ref_frames; ++i) {
    const int center = projected_tgt[i];
    lo[i] = std::max(0, center - radius);
    hi[i] = std::min(tgt_frames - 1, center + radius);
    if (hi[i] < lo[i]) hi[i] = lo[i];
  }
  lo[0] = 0;
  int reach_lo = lo[0];
  for (int i = 1; i < ref_frames; ++i) {
    lo[i] = std::min(lo[i], hi[i - 1] + 1);
    hi[i] = std::max(hi[i], reach_lo);
    reach_lo = std::max(reach_lo, lo[i]);
  }
  hi[ref_frames - 1] = std::max(hi[ref_frames - 1], tgt_frames - 1);
}

// Cells the DP evaluates, which is also what it allocates for backpointers.
long long band_cells(const std::vector<int>& lo, const std::vector<int>& hi) {
  long long n = 0;
  for (size_t i = 0; i < lo.size(); ++i) n += hi[i] - lo[i] + 1;
  return n;
}

// A DTW step advances at least one axis and neither by more than one.
bool path_is_continuous(const BandPath& path) {
  if (path.empty()) return false;
  for (size_t k = 1; k < path.size(); ++k) {
    const int di = path[k].first - path[k - 1].first;
    const int dj = path[k].second - path[k - 1].second;
    if (di < 0 || dj < 0 || di > 1 || dj > 1 || (di == 0 && dj == 0)) return false;
  }
  return true;
}

// Small integer costs: every accumulated cost stays an exact integer, so the
// three-way min ties constantly. Random floats essentially never tie, which is
// why a suite built only on them covers the DP and misses how it fails.
float tie_rich_cost(int i, int j) { return static_cast<float>((i * 7 + j * 13) % 3); }

// How many reachable cells have more than one predecessor at the minimum.
int count_tied_cells(int ref_frames, const std::vector<int>& lo, const std::vector<int>& hi,
                     const CellCost& cell_cost) {
  std::vector<std::vector<float>> acc(ref_frames);
  for (int i = 0; i < ref_frames; ++i) acc[i].assign(hi[i] - lo[i] + 1, kBandInf);
  auto at = [&](int i, int j) -> float {
    if (i < 0 || j < lo[i] || j > hi[i]) return kBandInf;
    return acc[i][j - lo[i]];
  };
  int ties = 0;
  for (int i = 0; i < ref_frames; ++i) {
    for (int j = lo[i]; j <= hi[i]; ++j) {
      const float local = cell_cost(i, j);
      if (i == 0 && j == 0) {
        acc[i][j - lo[i]] = local;
        continue;
      }
      const float d = at(i - 1, j - 1);
      const float u = at(i - 1, j);
      const float l = (j > 0) ? at(i, j - 1) : kBandInf;
      const float best = std::min(d, std::min(u, l));
      if (best >= kBandInf) continue;
      if ((d == best ? 1 : 0) + (u == best ? 1 : 0) + (l == best ? 1 : 0) > 1) ++ties;
      acc[i][j - lo[i]] = best + local;
    }
  }
  return ties;
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

TEST_CASE("WarpMap drops non-finite anchors before ordering them", "[mir]") {
  // A NaN coordinate compares false against every other value, so an anchor
  // carrying one is not merely unusable: it breaks the strict weak ordering the
  // anchor sort requires. Both entry points have to discard it before that
  // sort, and neither infinity is a position on a timeline either.
  const double nan_sample = std::numeric_limits<double>::quiet_NaN();
  const double inf_sample = std::numeric_limits<double>::infinity();

  const WarpMap map = WarpMap::from_anchors({{0.0, 0.0},
                                             {nan_sample, 500.0},
                                             {1000.0, 1200.0},
                                             {2000.0, inf_sample},
                                             {3000.0, nan_sample}});
  REQUIRE(map.valid());
  REQUIRE(map.anchors().size() == 2);
  REQUIRE(map.anchors()[0].warp_sample == 0.0);
  REQUIRE(map.anchors()[1].warp_sample == 1000.0);
  REQUIRE(map.warp_to_source(500.0) == Catch::Approx(600.0).margin(1e-6));

  // Dropping them can leave too few anchors to interpolate between, which is
  // the same invalid-map refusal a caller gets for any other unusable input.
  REQUIRE_THROWS([&] { WarpMap::from_anchors({{0.0, 0.0}, {nan_sample, nan_sample}}); }());
  REQUIRE_THROWS(
      [&] { WarpMap::from_markers({0.0, inf_sample, 1000.0}, {0.0, 500.0, nan_sample}); }());
}

TEST_CASE("chroma-DTW builds its chroma grid from the requested resolution", "[mir]") {
  // ChromaDtwConfig::bins_per_octave has to move ChromaCqtConfig's n_bins with
  // it: n_bins is a bin count, not an octave span, so assigning one without the
  // other stretches the CQT grid past Nyquist and the kernel rejects it. This
  // ran at every default and threw, but only the [.]-tagged alignment cases
  // called chroma_dtw_align, so the default run never saw it. Kept fast (short
  // signals, no assertion on the path) so it stays in the default run.
  const int sr = 22050;
  const Audio a = make_tone(sr, 261.63, 0.3);
  const Audio b = make_tone(sr, 261.63, 0.35);

  SECTION("default configuration") {
    ChromaDtwConfig cfg;
    REQUIRE_NOTHROW(chroma_dtw_align(a, b, cfg));
  }

  SECTION("a finer resolution than the default") {
    ChromaDtwConfig cfg;
    cfg.bins_per_octave = 36;
    REQUIRE_NOTHROW(chroma_dtw_align(a, b, cfg));
  }

  SECTION("the lowest sample rate that still carries the grid") {
    // Seven octaves above C1 top out under 4 kHz, so 8 kHz is the floor. The
    // grid does not shrink with the sample rate, so this is a hard boundary
    // rather than a graceful degradation.
    const Audio low_a = make_tone(8000, 261.63, 0.3);
    const Audio low_b = make_tone(8000, 261.63, 0.35);
    ChromaDtwConfig cfg;
    REQUIRE_NOTHROW(chroma_dtw_align(low_a, low_b, cfg));

    const Audio too_low_a = make_tone(4000, 261.63, 0.3);
    const Audio too_low_b = make_tone(4000, 261.63, 0.35);
    REQUIRE_THROWS(chroma_dtw_align(too_low_a, too_low_b, cfg));
  }

  SECTION("rejects a non-positive resolution") {
    ChromaDtwConfig cfg;
    cfg.bins_per_octave = 0;
    REQUIRE_THROWS(chroma_dtw_align(a, b, cfg));
  }
}

TEST_CASE("chroma-DTW puts the target on the warp axis and the reference on the source axis",
          "[mir]") {
  // Which anchor field carries which signal decides whether a WarpMap built
  // from these anchors warps the way the caller asked or the inverse, and a
  // swap is invisible to every other assertion here: the path stays monotonic,
  // the anchor count still matches, and the recovered offset only changes sign.
  // The sibling cases that do assert on the path read `path`, whose two axes are
  // named by position, so none of them reaches this.
  //
  // Asserted by length rather than by alignment accuracy. A DTW path ends at the
  // far corner of its cost matrix whatever it did in between, so the last anchor
  // carries each signal's own extent -- which makes two signals of different
  // lengths enough, and makes the check independent of how well short signals
  // align.
  const int sr = 22050;
  const double reference_seconds = 0.4;
  const double target_seconds = 0.6;
  const Audio reference = make_glide(sr, 261.63, 11.0, reference_seconds);
  const Audio target = make_glide(sr, 261.63, 11.0, target_seconds);

  ChromaDtwConfig cfg;
  cfg.hop_length = 512;
  const ChromaDtwResult r = chroma_dtw_align(reference, target, cfg);

  REQUIRE(r.anchors.size() >= 2);
  REQUIRE(r.reference_frames > 0);
  REQUIRE(r.target_frames > 0);
  // The frame counts follow the same naming, and the two differ here, so a
  // swapped pair fails before the anchors are read.
  REQUIRE(r.target_frames > r.reference_frames);

  const WarpAnchor& last = r.anchors.back();
  const double reference_extent = static_cast<double>(reference.size());
  const double target_extent = static_cast<double>(target.size());
  CAPTURE(last.warp_sample, last.source_sample, reference_extent, target_extent);
  // One hop of slack at each end: the last frame starts within a hop of the
  // signal's end rather than at it.
  const double slack = 2.0 * cfg.hop_length;
  REQUIRE(std::abs(last.warp_sample - target_extent) < slack);
  REQUIRE(std::abs(last.source_sample - reference_extent) < slack);
  // Stated as the separation the swap would invert, so the assertion names the
  // defect rather than two coincidences.
  REQUIRE(last.warp_sample > last.source_sample);

  // What the anchors are for: WarpMap::from_anchors consumes them directly, and
  // `warp_to_source` then takes a position on the TARGET timeline to one on the
  // REFERENCE timeline. A caller placing a take under a reference needs that
  // direction, so it is pinned here rather than left to the header's prose.
  // These anchors are monotonic but NOT strictly increasing, and a consumer that
  // needs strictness has to reduce them rather than forward them. The path takes
  // one step per cell, so when one signal carries more frames than the other,
  // several of its frames necessarily share a frame of the other -- which appears
  // here as repeated values on the shorter signal's axis. Structural, not
  // incidental: it follows from target_frames > reference_frames above.
  //
  // WarpMap::from_anchors absorbs it by de-duplicating on construction, so
  // nothing inside C++ notices. sonare_project_set_warp_map does not: it
  // requires at least two finite, strictly increasing pairs, so the same anchors
  // that satisfy the C++ consumer are refused at the C boundary.
  int warp_flat = 0;
  int source_flat = 0;
  for (size_t i = 1; i < r.anchors.size(); ++i) {
    if (!(r.anchors[i].warp_sample > r.anchors[i - 1].warp_sample)) ++warp_flat;
    if (!(r.anchors[i].source_sample > r.anchors[i - 1].source_sample)) ++source_flat;
  }
  CAPTURE(r.anchors.size(), warp_flat, source_flat);
  // The longer signal advances every step, so its own axis stays strict.
  REQUIRE(warp_flat == 0);
  REQUIRE(source_flat > 0);

  const WarpMap map = WarpMap::from_anchors(r.anchors);
  REQUIRE(map.valid());
  const double mapped = map.warp_to_source(target_extent * 0.5);
  CAPTURE(mapped);
  REQUIRE(mapped > 0.0);
  REQUIRE(mapped < reference_extent);

  // And the reduction that makes them acceptable to a strict consumer actually
  // removes the ties counted above rather than returning the input.
  const std::vector<WarpAnchor> strict = strictly_increasing_anchors(r.anchors);
  REQUIRE(strict.size() >= 2);
  REQUIRE(strict.size() < r.anchors.size());
  for (size_t i = 1; i < strict.size(); ++i) {
    CAPTURE(i, strict[i - 1].warp_sample, strict[i].warp_sample, strict[i - 1].source_sample,
            strict[i].source_sample);
    REQUIRE(strict[i].warp_sample > strict[i - 1].warp_sample);
    REQUIRE(strict[i].source_sample > strict[i - 1].source_sample);
  }
  REQUIRE(WarpMap::from_anchors(strict).valid());
}

TEST_CASE("strictly-increasing reduction represents a tied run by its middle anchor", "[mir]") {
  // Hand-built so the selection rule is asserted against stated positions rather
  // than against whatever a DTW happened to produce.
  SECTION("a tie on the source axis") {
    // Three anchors share source 100; the middle one is warp 20.
    const std::vector<WarpAnchor> reduced = strictly_increasing_anchors(
        {{0.0, 0.0}, {10.0, 100.0}, {20.0, 100.0}, {30.0, 100.0}, {40.0, 200.0}});
    REQUIRE(reduced.size() == 3);
    REQUIRE(reduced[1].warp_sample == Catch::Approx(20.0));
    REQUIRE(reduced[1].source_sample == Catch::Approx(100.0));
  }

  SECTION("a tie on the warp axis, which one source-axis pass cannot reach") {
    // The mirror case: collapsing source alone leaves these three untouched,
    // because every source value here is already distinct.
    const std::vector<WarpAnchor> reduced = strictly_increasing_anchors(
        {{0.0, 0.0}, {100.0, 10.0}, {100.0, 20.0}, {100.0, 30.0}, {200.0, 40.0}});
    REQUIRE(reduced.size() == 3);
    REQUIRE(reduced[1].warp_sample == Catch::Approx(100.0));
    REQUIRE(reduced[1].source_sample == Catch::Approx(20.0));
  }

  SECTION("an even run takes the lower middle, so the choice is reproducible") {
    const std::vector<WarpAnchor> reduced =
        strictly_increasing_anchors({{0.0, 0.0}, {10.0, 100.0}, {20.0, 100.0}, {30.0, 200.0}});
    REQUIRE(reduced.size() == 3);
    REQUIRE(reduced[1].warp_sample == Catch::Approx(10.0));
  }

  SECTION("an input already strict comes back unchanged") {
    const std::vector<WarpAnchor> input = {{0.0, 0.0}, {10.0, 20.0}, {30.0, 40.0}};
    const std::vector<WarpAnchor> reduced = strictly_increasing_anchors(input);
    REQUIRE(reduced.size() == input.size());
    for (size_t i = 0; i < input.size(); ++i) {
      REQUIRE(reduced[i].warp_sample == Catch::Approx(input[i].warp_sample));
      REQUIRE(reduced[i].source_sample == Catch::Approx(input[i].source_sample));
    }
  }

  SECTION("a run that collapses below two anchors is handed back rather than invented into one") {
    // Every anchor shares both coordinates, so the reduction has nothing to
    // return; the caller refuses it, since a map needs two.
    const std::vector<WarpAnchor> input = {{5.0, 7.0}, {5.0, 7.0}, {5.0, 7.0}};
    const std::vector<WarpAnchor> reduced = strictly_increasing_anchors(input);
    REQUIRE(reduced.size() == input.size());
    REQUIRE_THROWS([&] { WarpMap::from_anchors(reduced); }());
  }
}

TEST_CASE("chroma-DTW recovers a known time shift within tolerance", "[.][slow][mir]") {
  const int sr = 22050;
  // Reference: a continuous glide. Target: a delayed copy (silence prefix), so
  // the alignment path should track a constant offset once past the corner.
  Audio ref = make_glide(sr, 261.63, 11.0, 1.5);

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

  const double expected_offset_frames = static_cast<double>(delay) / cfg.hop_length;
  // DTW anchors the path at (0, 0), so the offset necessarily starts at zero and
  // climbs; one step moves each axis by at most one frame, so the climb cannot
  // finish before reference frame ceil(expected). Everything after it is steady
  // state and is where the shift is actually recoverable.
  const int settle_ref = static_cast<int>(std::ceil(expected_offset_frames));
  double worst = 0.0;
  int worst_ref = -1;
  int checked = 0;
  for (const auto& point : r.path) {
    if (point.first < settle_ref) continue;
    ++checked;
    const double error = std::abs((point.second - point.first) - expected_offset_frames);
    if (error > worst) {
      worst = error;
      worst_ref = point.first;
    }
  }
  WARN("shift recovery: checked " << checked << " path points, worst error " << worst
                                  << " frames at reference frame " << worst_ref);
  // Non-vacuity: the steady-state region has to be most of the path, or the
  // band below would be asserted over a handful of points near the corner.
  REQUIRE(checked > r.reference_frames / 2);
  // Band: both path axes are whole frames while the delay is 10.77 of them, so
  // the nearest reachable offset is already 0.23 out; one chroma hop of
  // front-end slack on top gives 1.23, stated as 1.25 frames (29 ms).
  CHECK(worst <= 1.25);
}

TEST_CASE("chroma-DTW recovers a known time stretch within tolerance", "[.][slow][mir]") {
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
  // Path endpoints are whole frames, so the slope resolves to 1/dref = 0.023 at this size and
  // the observed 1.488 is the closest the grid comes to 1.5, half a frame out. The margin
  // admits two frames of endpoint quantization.
  REQUIRE(observed_slope == Catch::Approx(stretch).margin(0.05));
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

TEST_CASE("the banded chroma cost is unchanged by hoisting the column norms", "[mir]") {
  // The banded DP derives each column's norm once instead of once per cell, and
  // the two forms have to agree to the last bit. Both forms live here, so this
  // pins the algebra on its own; the case below pins the production reduction.
  // Exactness here is cheap insurance, not a guarded fragility: perturbing the
  // cost surface left the emitted path unchanged at one ulp and at 1e-4
  // relative, and first moved it at 1e-3 -- including on fixtures whose chroma
  // repeats with period 8, which makes exact ties structural rather than hoped
  // for.
  const int n_chroma = 12;
  const int ref_frames = 23;
  const int tgt_frames = 29;

  auto chroma_like = [](int rows, int frames, uint32_t seed) {
    std::vector<float> m(static_cast<size_t>(rows) * frames);
    uint32_t state = seed;
    for (float& v : m) {
      state = state * 1664525u + 1013904223u;
      v = static_cast<float>(state >> 8) / static_cast<float>(1u << 24);
    }
    return m;
  };
  std::vector<float> ref = chroma_like(n_chroma, ref_frames, 0x7f4a2c19u);
  std::vector<float> tgt = chroma_like(n_chroma, tgt_frames, 0x13c9a5e7u);
  // One silent frame per side, so the zero-denominator branch is reached.
  for (int c = 0; c < n_chroma; ++c) {
    ref[static_cast<size_t>(c) * ref_frames + 4] = 0.0f;
    tgt[static_cast<size_t>(c) * tgt_frames + 17] = 0.0f;
  }

  // The pre-hoist form: all three reductions in one pass over the chroma bins.
  auto fused = [&](int i, int j) -> float {
    double dot = 0.0, na = 0.0, nb = 0.0;
    for (int c = 0; c < n_chroma; ++c) {
      const double a = ref[static_cast<size_t>(c) * ref_frames + i];
      const double b = tgt[static_cast<size_t>(c) * tgt_frames + j];
      dot += a * b;
      na += a * a;
      nb += b * b;
    }
    const double denom = std::sqrt(na) * std::sqrt(nb);
    if (denom <= 0.0) return 1.0f;
    return static_cast<float>(1.0 - dot / denom);
  };

  auto column_norms = [&](const std::vector<float>& m, int frames) {
    std::vector<double> norms(static_cast<size_t>(frames));
    for (int i = 0; i < frames; ++i) {
      double n = 0.0;
      for (int c = 0; c < n_chroma; ++c) {
        const double v = m[static_cast<size_t>(c) * frames + i];
        n += v * v;
      }
      norms[static_cast<size_t>(i)] = std::sqrt(n);
    }
    return norms;
  };
  const std::vector<double> ref_norms = column_norms(ref, ref_frames);
  const std::vector<double> tgt_norms = column_norms(tgt, tgt_frames);

  auto hoisted = [&](int i, int j) -> float {
    const double denom = ref_norms[static_cast<size_t>(i)] * tgt_norms[static_cast<size_t>(j)];
    if (denom <= 0.0) return 1.0f;
    double dot = 0.0;
    for (int c = 0; c < n_chroma; ++c) {
      const double a = ref[static_cast<size_t>(c) * ref_frames + i];
      const double b = tgt[static_cast<size_t>(c) * tgt_frames + j];
      dot += a * b;
    }
    return static_cast<float>(1.0 - dot / denom);
  };

  for (int i = 0; i < ref_frames; ++i) {
    for (int j = 0; j < tgt_frames; ++j) {
      CAPTURE(i);
      CAPTURE(j);
      REQUIRE(hoisted(i, j) == fused(i, j));
    }
  }
}

TEST_CASE("the banded DTW path survives keeping only two accumulated-cost rows", "[mir]") {
  // The recurrence reads rows i-1 and i, so the accumulated cost is a two-row
  // window while the backpointers stay full. The window is re-based on each
  // row's own band, which only a band whose edges move can distinguish from a
  // constant-width one, so the band shapes below are the point of the case.
  const int n_chroma = 12;

  for (int trial = 0; trial < 60; ++trial) {
    BandRng rng{static_cast<uint32_t>(0x9e3779b9u + trial * 2654435761u)};
    const int ref_frames = rng.in(2, 24);
    const int tgt_frames = rng.in(2, 24);
    const int radius = rng.in(1, 6);
    std::vector<int> lo;
    std::vector<int> hi;
    switch (trial % 3) {
      case 0:
        make_band_widening(ref_frames, tgt_frames, radius, rng, lo, hi);
        break;
      case 1:
        make_band_narrowing(ref_frames, tgt_frames, radius, rng, lo, hi);
        break;
      default:
        make_band_disjoint(ref_frames, tgt_frames, radius, lo, hi);
        break;
    }

    std::vector<float> ref(static_cast<size_t>(n_chroma) * ref_frames);
    std::vector<float> tgt(static_cast<size_t>(n_chroma) * tgt_frames);
    for (float& v : ref) v = rng.unit();
    for (float& v : tgt) v = rng.unit();
    const CellCost cosine = [&](int i, int j) -> float {
      double dot = 0.0;
      double na = 0.0;
      double nb = 0.0;
      for (int c = 0; c < n_chroma; ++c) {
        const double a = ref[static_cast<size_t>(c) * ref_frames + i];
        const double b = tgt[static_cast<size_t>(c) * tgt_frames + j];
        dot += a * b;
        na += a * a;
        nb += b * b;
      }
      const double denom = std::sqrt(na) * std::sqrt(nb);
      if (denom <= 0.0) return 1.0f;
      return static_cast<float>(1.0 - dot / denom);
    };

    for (int regime = 0; regime < 2; ++regime) {
      const CellCost cost = regime == 0 ? CellCost(tie_rich_cost) : cosine;
      const BandedDtwOut oracle = banded_dtw_full_matrix(ref_frames, tgt_frames, lo, hi, cost);
      const BandedDtwOut windowed = banded_dtw_row_window(ref_frames, tgt_frames, lo, hi, cost);
      CAPTURE(trial);
      CAPTURE(regime);
      CAPTURE(ref_frames);
      CAPTURE(tgt_frames);
      REQUIRE(windowed.final_cost == oracle.final_cost);
      REQUIRE(windowed.path.size() == oracle.path.size());
      for (size_t k = 0; k < oracle.path.size(); ++k) {
        CAPTURE(k);
        REQUIRE(windowed.path[k].first == oracle.path[k].first);
        REQUIRE(windowed.path[k].second == oracle.path[k].second);
      }
    }
  }
}

TEST_CASE("the banded DTW band is bounded by its radius, not by the target length", "[mir]") {
  // The band's whole claim is that the DP allocates by band width rather than by
  // the full matrix. A band assembled from a running min over lo and a running
  // max over hi cannot hold it: the projection is non-decreasing, so that min
  // pins every row's floor to lo[0] and each row ends up spanning the target
  // from 0, which is the full matrix in all but name.
  const int ref_frames = 48;
  const int radius = 3;

  SECTION("each interior row keeps its radius and the interior is flat in tgt_frames") {
    long long interior_cells = -1;
    for (int tgt_frames : {60, 120, 240, 480, 960}) {
      std::vector<int> projected(ref_frames);
      for (int i = 0; i < ref_frames; ++i) projected[i] = std::min(tgt_frames - 1, i);
      std::vector<int> lo;
      std::vector<int> hi;
      make_band_projected(projected, tgt_frames, radius, lo, hi);
      CAPTURE(tgt_frames);
      // The two corner-anchored rows are the only ones allowed past the radius.
      for (int i = 1; i + 1 < ref_frames; ++i) {
        CAPTURE(i);
        REQUIRE(hi[i] - lo[i] + 1 <= 2 * radius + 1);
      }
      const long long cells = band_cells(lo, hi) - (hi[ref_frames - 1] - lo[ref_frames - 1] + 1);
      if (interior_cells < 0) interior_cells = cells;
      REQUIRE(cells == interior_cells);
    }
  }

  SECTION("a projection that jumps further than the band is wide stays bounded") {
    // What project_path hands the finer level: a coarse target index multiplied
    // by the pyramid's scale, so the centre jumps by a multiple of scale at each
    // coarse step and the repair has to run.
    const int tgt_frames = 160;
    const int scale = 4;
    std::vector<int> projected(ref_frames);
    for (int i = 0; i < ref_frames; ++i) {
      projected[i] = std::min(tgt_frames - 1, (i / scale) * 3 * scale);
    }
    std::vector<int> lo;
    std::vector<int> hi;
    make_band_projected(projected, tgt_frames, radius, lo, hi);
    // A row is either left at its radius or pulled back to exactly one column
    // past the row above: one less disconnects the band, one more pays for cells
    // no path can reach.
    for (int i = 1; i < ref_frames; ++i) {
      CAPTURE(i);
      const int unrepaired = std::max(0, projected[i] - radius);
      REQUIRE((lo[i] == unrepaired || lo[i] == hi[i - 1] + 1));
    }
    // A repaired row spans hi[i] - hi[i-1] and those telescope over a
    // non-decreasing hi, so the repair and the two anchors together cost at most
    // 2 * tgt_frames on top of the radius band.
    const long long bound =
        static_cast<long long>(ref_frames) * (2 * radius + 1) + 2LL * tgt_frames;
    REQUIRE(band_cells(lo, hi) <= bound);
    REQUIRE(band_cells(lo, hi) * 4 < static_cast<long long>(ref_frames) * tgt_frames);
  }
}

TEST_CASE("the banded DTW band overlaps row to row so a continuous path exists", "[mir]") {
  // Narrowing the band is what makes continuity a real question: a row whose
  // floor sits more than one column past the row above is unreachable, and the
  // traceback then walks out of the band instead of along a path. The condition
  // is asserted on the band and the consequence on the path.
  const int n_chroma = 12;

  for (int trial = 0; trial < 40; ++trial) {
    BandRng rng{static_cast<uint32_t>(0x2545f491u + trial * 2654435761u)};
    const int ref_frames = rng.in(3, 40);
    const int tgt_frames = rng.in(3, 60);
    const int radius = rng.in(1, 5);
    const int scale = rng.in(2, 4);

    std::vector<int> projected(ref_frames);
    switch (trial % 4) {
      case 0:
        for (int i = 0; i < ref_frames; ++i) projected[i] = std::min(tgt_frames - 1, i);
        break;
      case 1:
        for (int i = 0; i < ref_frames; ++i) {
          projected[i] = std::min(tgt_frames - 1, i * (tgt_frames - 1) / (ref_frames - 1));
        }
        break;
      case 2: {
        // The coarse-quantised staircase project_path produces.
        int coarse = 0;
        for (int i = 0; i < ref_frames; ++i) {
          if (i > 0 && i % scale == 0) coarse += rng.in(0, 3);
          projected[i] = std::clamp(coarse * scale, 0, tgt_frames - 1);
        }
        break;
      }
      default:
        // Not a shape project_path can produce, but the band must not depend on
        // an unstated monotonicity precondition to stay connected.
        for (int i = 0; i < ref_frames; ++i) {
          projected[i] = rng.in(0, tgt_frames - 1);
        }
        break;
    }

    std::vector<int> lo;
    std::vector<int> hi;
    make_band_projected(projected, tgt_frames, radius, lo, hi);

    CAPTURE(trial);
    CAPTURE(ref_frames);
    CAPTURE(tgt_frames);
    CAPTURE(radius);

    // The overlap condition, row by row: a step lands at most one column past
    // where the row above sat, and the row above's first reachable column has to
    // still be in this row.
    REQUIRE(lo[0] == 0);
    int reach_lo = lo[0];
    for (int i = 1; i < ref_frames; ++i) {
      CAPTURE(i);
      REQUIRE(lo[i] <= hi[i]);
      REQUIRE(lo[i] <= hi[i - 1] + 1);
      REQUIRE(hi[i] >= reach_lo);
      reach_lo = std::max(reach_lo, lo[i]);
    }
    REQUIRE(hi[ref_frames - 1] == tgt_frames - 1);

    std::vector<float> ref(static_cast<size_t>(n_chroma) * ref_frames);
    std::vector<float> tgt(static_cast<size_t>(n_chroma) * tgt_frames);
    for (float& v : ref) v = rng.unit();
    for (float& v : tgt) v = rng.unit();
    const CellCost cosine = [&](int i, int j) -> float {
      double dot = 0.0;
      double na = 0.0;
      double nb = 0.0;
      for (int c = 0; c < n_chroma; ++c) {
        const double a = ref[static_cast<size_t>(c) * ref_frames + i];
        const double b = tgt[static_cast<size_t>(c) * tgt_frames + j];
        dot += a * b;
        na += a * a;
        nb += b * b;
      }
      const double denom = std::sqrt(na) * std::sqrt(nb);
      if (denom <= 0.0) return 1.0f;
      return static_cast<float>(1.0 - dot / denom);
    };

    for (int regime = 0; regime < 2; ++regime) {
      const CellCost cost = regime == 0 ? CellCost(tie_rich_cost) : cosine;
      const BandedDtwOut oracle = banded_dtw_full_matrix(ref_frames, tgt_frames, lo, hi, cost);
      const BandedDtwOut windowed = banded_dtw_row_window(ref_frames, tgt_frames, lo, hi, cost);
      CAPTURE(regime);
      // Corner to corner, stepping by at most one on each axis.
      REQUIRE(windowed.final_cost < kBandInf);
      REQUIRE(path_is_continuous(windowed.path));
      REQUIRE(windowed.path.front().first == 0);
      REQUIRE(windowed.path.front().second == 0);
      REQUIRE(windowed.path.back().first == ref_frames - 1);
      REQUIRE(windowed.path.back().second == tgt_frames - 1);
      // Every element inside the band it was allowed to use.
      for (const auto& cell : windowed.path) {
        CAPTURE(cell.first);
        CAPTURE(cell.second);
        REQUIRE(cell.second >= lo[cell.first]);
        REQUIRE(cell.second <= hi[cell.first]);
      }
      // The two accumulated-cost shapes still agree element for element.
      REQUIRE(windowed.final_cost == oracle.final_cost);
      REQUIRE(windowed.path.size() == oracle.path.size());
      for (size_t k = 0; k < oracle.path.size(); ++k) {
        CAPTURE(k);
        REQUIRE(windowed.path[k].first == oracle.path[k].first);
        REQUIRE(windowed.path[k].second == oracle.path[k].second);
      }
    }
  }
}

TEST_CASE("the banded DTW traceback clamps to the last row's band at the corner", "[mir]") {
  // The band the DP builds always holds a reachable corner: the end anchor puts
  // hi[last] at tgt_frames-1 and the overlap repair makes every row reachable.
  // So the clamp is a guard, and the only shapes that reach it are bands that
  // break one of those two -- which is what these two are.
  const int ref_frames = 4;
  const int tgt_frames = 8;
  const CellCost cost = CellCost(tie_rich_cost);

  SECTION("a last row whose band stops short of the corner") {
    // hi[last] = 4 against a target end of 7, so the clamp moves the traceback
    // start rather than returning it unchanged.
    const std::vector<int> lo = {0, 0, 1, 2};
    const std::vector<int> hi = {1, 2, 3, 4};
    const BandedDtwOut oracle = banded_dtw_full_matrix(ref_frames, tgt_frames, lo, hi, cost);
    const BandedDtwOut windowed = banded_dtw_row_window(ref_frames, tgt_frames, lo, hi, cost);

    const BandPath expected = {{0, 0}, {0, 1}, {1, 2}, {2, 2}, {3, 3}, {3, 4}};
    REQUIRE(windowed.path.size() == expected.size());
    REQUIRE(oracle.path.size() == expected.size());
    for (size_t k = 0; k < expected.size(); ++k) {
      CAPTURE(k);
      REQUIRE(windowed.path[k].first == expected[k].first);
      REQUIRE(windowed.path[k].second == expected[k].second);
      REQUIRE(oracle.path[k].first == expected[k].first);
      REQUIRE(oracle.path[k].second == expected[k].second);
    }
    // Clamped to the last row's own band, not the row above it, and the result
    // is still a continuous path from the start corner.
    REQUIRE(windowed.path.back().second == hi[ref_frames - 1]);
    REQUIRE(path_is_continuous(windowed.path));
    REQUIRE(windowed.final_cost < kBandInf);
    REQUIRE(windowed.final_cost == oracle.final_cost);
  }

  SECTION("a corner in band but with no step reaching it") {
    // The gap between rows 1 and 2 is two columns wide, so every row past it is
    // unreachable; the corner is in band and the clamp is the identity, which is
    // why breaking the clamp cannot be seen on a band shaped like this.
    const std::vector<int> lo = {0, 0, 4, 4};
    const std::vector<int> hi = {1, 1, 7, 7};
    const BandedDtwOut oracle = banded_dtw_full_matrix(ref_frames, tgt_frames, lo, hi, cost);
    const BandedDtwOut windowed = banded_dtw_row_window(ref_frames, tgt_frames, lo, hi, cost);

    REQUIRE(windowed.final_cost >= kBandInf);
    REQUIRE(windowed.path.size() == 1);
    REQUIRE(windowed.path[0].first == ref_frames - 1);
    REQUIRE(windowed.path[0].second == tgt_frames - 1);
    REQUIRE(oracle.path.size() == windowed.path.size());
    REQUIRE(oracle.path[0].first == windowed.path[0].first);
    REQUIRE(oracle.path[0].second == windowed.path[0].second);
  }
}

TEST_CASE("the banded DTW tie-break is what the path assertion pins", "[mir]") {
  // Integer cell costs make the three-way min tie in a large fraction of the
  // band. Loosening the min to `<=` leaves every accumulated cost bit-identical
  // and hands those ties to a different predecessor: the cost stays equal and
  // the path moves, which is why the case above asserts the path element for
  // element instead of the cost alone.
  const int ref_frames = 19;
  const int tgt_frames = 23;
  BandRng rng{0x51ed2701u};
  std::vector<int> lo;
  std::vector<int> hi;
  make_band_narrowing(ref_frames, tgt_frames, 5, rng, lo, hi);

  const CellCost cost = CellCost(tie_rich_cost);
  REQUIRE(count_tied_cells(ref_frames, lo, hi, cost) > 20);

  const BandedDtwOut oracle = banded_dtw_full_matrix(ref_frames, tgt_frames, lo, hi, cost);
  const BandedDtwOut loose =
      banded_dtw_row_window(ref_frames, tgt_frames, lo, hi, cost, /*loose_tie=*/true);
  REQUIRE(loose.final_cost == oracle.final_cost);
  REQUIRE(loose.path != oracle.path);
}

namespace {

/// @brief Chroma-shaped matrix [n_chroma x frames], row-major, with a couple of
///        dominant pitch classes over a noise floor.
std::vector<float> detail_chroma(int n_chroma, int frames, uint64_t seed) {
  std::vector<float> m(static_cast<size_t>(n_chroma) * frames);
  uint64_t state = seed;
  for (int c = 0; c < n_chroma; ++c) {
    for (int t = 0; t < frames; ++t) {
      state = state * 6364136223846793005ULL + 1442695040888963407ULL;
      const float unit = static_cast<float>((state >> 40) & 0xFFFFFF) / 16777216.0f;
      const float tonic = ((c + t / 7) % 12 < 3) ? 0.9f : 0.05f;
      m[static_cast<size_t>(c) * frames + t] = tonic * (0.3f + 0.7f * unit) + 0.01f * unit;
    }
  }
  return m;
}

/// @brief A non-decreasing projection spanning the full target axis.
std::vector<int> detail_projection(int ref_frames, int tgt_frames) {
  std::vector<int> projected(static_cast<size_t>(ref_frames));
  for (int i = 0; i < ref_frames; ++i) {
    const double f =
        static_cast<double>(i) / static_cast<double>(ref_frames > 1 ? ref_frames - 1 : 1);
    projected[static_cast<size_t>(i)] = static_cast<int>(f * (tgt_frames - 1));
  }
  return projected;
}

}  // namespace

TEST_CASE("the production column-norm reduction sums bins in ascending order", "[mir]") {
  // Against the production function, not a local copy of it. The reduction is a
  // free function across a translation-unit boundary, so what runs here is the
  // out-of-line codegen rather than the copy the banded DP inlines. That costs
  // nothing for this claim -- both operands are floats widened to double, so
  // every product is exact in a double and contraction has no rounding to skip
  // -- but it does mean this case guards the summation ORDER and is not a
  // statement about generated code.
  const int n_chroma = 12;
  const int frames = 200;
  const std::vector<float> m = detail_chroma(n_chroma, frames, 0xABCDEF01u);

  const std::vector<double> produced =
      sonare::mir::detail::chroma_column_norms(m, n_chroma, frames);
  REQUIRE(produced.size() == static_cast<size_t>(frames));

  // The pre-hoist shape: the same terms in the same order, reached by a
  // three-accumulator pass. Exact agreement is the contract.
  for (int i = 0; i < frames; ++i) {
    CAPTURE(i);
    double na = 0.0;
    for (int c = 0; c < n_chroma; ++c) {
      const double v = m[static_cast<size_t>(c) * frames + i];
      na += v * v;
    }
    REQUIRE(produced[static_cast<size_t>(i)] == std::sqrt(na));
  }

  // Non-vacuity, at the assertion: summing the same terms in the opposite order
  // must disagree somewhere, or the exact comparison above proves nothing about
  // order. Reversal moved roughly half the columns by one to three ulp when this
  // was measured, so requiring a single disagreement is a wide margin.
  int reversed_disagreements = 0;
  for (int i = 0; i < frames; ++i) {
    double na = 0.0;
    for (int c = n_chroma - 1; c >= 0; --c) {
      const double v = m[static_cast<size_t>(c) * frames + i];
      na += v * v;
    }
    if (produced[static_cast<size_t>(i)] != std::sqrt(na)) ++reversed_disagreements;
  }
  REQUIRE(reversed_disagreements > 0);
}

TEST_CASE("the banded DTW path spans both corners in unit steps", "[mir]") {
  // The production banded DP, in the default run. The two cases that reach it
  // through a full alignment are tagged out of the default suite and compare
  // with a tolerance, so without this the DP's emitted path is unasserted here.
  const int n_chroma = 12;
  const int ref_frames = 120;
  const int tgt_frames = 150;
  const std::vector<float> ref = detail_chroma(n_chroma, ref_frames, 0xABCDEF01u);
  const std::vector<float> tgt = detail_chroma(n_chroma, tgt_frames, 0x1234ABCDu);

  const std::vector<std::pair<int, int>> path = sonare::mir::detail::banded_dtw_path(
      ref, n_chroma, ref_frames, tgt, tgt_frames, detail_projection(ref_frames, tgt_frames),
      /*band_radius=*/20);

  REQUIRE_FALSE(path.empty());
  REQUIRE(path.front() == std::pair<int, int>{0, 0});
  REQUIRE(path.back() == std::pair<int, int>{ref_frames - 1, tgt_frames - 1});
  for (size_t k = 1; k < path.size(); ++k) {
    CAPTURE(k);
    const int di = path[k].first - path[k - 1].first;
    const int dj = path[k].second - path[k - 1].second;
    // The symmetric P0 step set: diagonal, ref advance, or target advance.
    REQUIRE(di >= 0);
    REQUIRE(dj >= 0);
    REQUIRE(di + dj >= 1);
    REQUIRE(di <= 1);
    REQUIRE(dj <= 1);
  }

  // Deterministic: the same input must give the same discrete path.
  const std::vector<std::pair<int, int>> again = sonare::mir::detail::banded_dtw_path(
      ref, n_chroma, ref_frames, tgt, tgt_frames, detail_projection(ref_frames, tgt_frames),
      /*band_radius=*/20);
  REQUIRE(again == path);
}
