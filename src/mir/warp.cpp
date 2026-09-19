#include "mir/warp.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include "core/spectrum.h"
#include "effects/hpss.h"
#include "effects/phase_vocoder.h"
#include "feature/chroma.h"
#include "mir/warp_detail.h"
#include "util/exception.h"
#include "util/sequence.h"

namespace sonare::mir {
namespace detail {

std::vector<double> chroma_column_norms(const std::vector<float>& m, int n_chroma, int frames) {
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
}

// Banded DTW: only cells within +/- band_radius of the projected path's target
// index (per reference frame) are evaluated, widened only where consecutive
// rows would otherwise not overlap and at the two corners. For the
// non-decreasing projection project_path produces, that bounds the evaluated
// cells at O(ref_frames * band_width + tgt_frames): each widened row spans
// hi[i] - hi[i-1], and those telescope over a non-decreasing hi.
//
// We implement this as a self-contained deterministic DP over the band so we do
// not have to thread a band mask through util/sequence::dtw. The cost is the
// cosine distance between chroma columns; steps are the symmetric P0 pattern
// {(1,1),(1,0),(0,1)} matching util/sequence's default.
std::vector<std::pair<int, int>> banded_dtw_path(const std::vector<float>& ref, int n_chroma,
                                                 int ref_frames, const std::vector<float>& tgt,
                                                 int tgt_frames,
                                                 const std::vector<int>& projected_tgt,
                                                 int band_radius) {
  const float kInf = 1e30f;
  // Per reference frame i, the target band is [lo[i], hi[i]] (inclusive).
  std::vector<int> lo(ref_frames), hi(ref_frames);
  for (int i = 0; i < ref_frames; ++i) {
    const int center = projected_tgt[i];
    lo[i] = std::max(0, center - band_radius);
    hi[i] = std::min(tgt_frames - 1, center + band_radius);
    if (hi[i] < lo[i]) hi[i] = lo[i];
  }
  // Anchor the start corner before the repair below, so the repair sees it.
  lo[0] = 0;
  // A diagonal or up step lands at most one column past where row i-1 sat, so
  // row i is enterable iff lo[i] <= hi[i-1] + 1 and hi[i] >= reach_lo, the
  // first column of row i-1 that is itself reachable. Repair on the row that
  // fails the condition, widening by the deficit alone; a running min over lo
  // and max over hi cannot express it, because a non-decreasing projection pins
  // every floor to lo[0] and never fires the max.
  int reach_lo = lo[0];
  for (int i = 1; i < ref_frames; ++i) {
    lo[i] = std::min(lo[i], hi[i - 1] + 1);
    hi[i] = std::max(hi[i], reach_lo);
    reach_lo = std::max(reach_lo, lo[i]);
  }
  // Anchor the end corner. The last row now runs contiguously from lo[last],
  // reachable from the row above, up to tgt_frames-1 via successive left steps,
  // so (ref_frames-1, tgt_frames-1) is reachable without widening any interior
  // row.
  hi[ref_frames - 1] = std::max(hi[ref_frames - 1], tgt_frames - 1);

  // Each operand's norm depends on one index only, so deriving it inside the
  // (i, j) loop below redoes the same ref_frames + tgt_frames reductions once
  // per cell. Each norm is still summed over c in the same order, so the
  // denominator is the same double, bit for bit.
  const std::vector<double> ref_norms = chroma_column_norms(ref, n_chroma, ref_frames);
  const std::vector<double> tgt_norms = chroma_column_norms(tgt, n_chroma, tgt_frames);

  auto cos_dist = [&](int i, int j) -> float {
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

  // The recurrence reads rows i-1 and i only, so the accumulated cost is a
  // two-row window over each row's own band. `back` stays full: the traceback
  // below walks every row.
  std::vector<std::vector<int>> back(ref_frames);  // 0=diag,1=up(ref-1),2=left(tgt-1)
  for (int i = 0; i < ref_frames; ++i) {
    back[i].assign(hi[i] - lo[i] + 1, -1);
  }
  std::vector<float> prev_acc;
  std::vector<float> curr_acc;
  int prev_lo = 0;
  int prev_hi = -1;  // Row -1 is an empty band, so every read of it is inf.
  int curr_row = 0;
  // Both windows are indexed by their own row's lo, because the band edges move
  // per row and a fixed width would read the neighbouring row off by its shift.
  auto at_prev = [&](int j) -> float {
    if (j < prev_lo || j > prev_hi) return kInf;
    return prev_acc[j - prev_lo];
  };
  auto at_curr = [&](int j) -> float {
    if (j < lo[curr_row] || j > hi[curr_row]) return kInf;
    return curr_acc[j - lo[curr_row]];
  };

  for (int i = 0; i < ref_frames; ++i) {
    curr_row = i;
    // Refilled per row: a cell the recurrence leaves unreachable must read back
    // as inf, not as whatever the row two above left in that slot.
    curr_acc.assign(hi[i] - lo[i] + 1, kInf);
    for (int j = lo[i]; j <= hi[i]; ++j) {
      const float local = cos_dist(i, j);
      if (i == 0 && j == 0) {
        curr_acc[j - lo[i]] = local;
        back[i][j - lo[i]] = -1;
        continue;
      }
      const float d = at_prev(j - 1);                   // diagonal
      const float u = at_prev(j);                       // ref advance
      const float l = (j > 0) ? at_curr(j - 1) : kInf;  // tgt advance
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
      if (best >= kInf) {
        // Unreachable cell inside the band: leave as inf.
        continue;
      }
      curr_acc[j - lo[i]] = best + local;
      back[i][j - lo[i]] = bk;
    }
    prev_acc.swap(curr_acc);
    prev_lo = lo[i];
    prev_hi = hi[i];
  }

  // Backtrack from the (ref_frames-1, tgt_frames-1) corner.
  std::vector<std::pair<int, int>> path;
  int i = ref_frames - 1;
  int j = tgt_frames - 1;
  // The sweep left its last row in the window's previous slot.
  if (j < lo[i] || j > hi[i] || at_prev(j) >= kInf) {
    // A guard, not a live path: the end anchor puts hi[last] at tgt_frames-1
    // and the overlap repair makes every row reachable, so the band built above
    // always holds a reachable corner and this clamp is the identity for it.
    j = std::clamp(j, lo[i], hi[i]);
  }
  while (i >= 0 && j >= 0) {
    path.emplace_back(i, j);
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
  std::reverse(path.begin(), path.end());
  return path;
}

}  // namespace detail

namespace {

// Octave span of the chroma CQT grid, matching ChromaCqtConfig's default
// (252 bins at 36 bins/octave) and the binding-level chroma entry points.
constexpr int kChromaOctaves = 7;

// Strictly-monotonic de-duplication of anchors sorted by warp_sample. Drops any
// anchor that does not strictly advance BOTH axes relative to the kept anchor,
// so the map is invertible in both directions.
std::vector<WarpAnchor> sanitize_anchors(std::vector<WarpAnchor> anchors) {
  // Non-finite anchors go before the sort, not with the monotonicity pass
  // below: every comparison against a NaN coordinate is false, which is not the
  // strict weak ordering std::sort requires, so leaving one in the input is
  // undefined behaviour rather than a wrong result. Dropping them can leave
  // fewer than two anchors, which the callers already treat as an invalid map.
  anchors.erase(std::remove_if(anchors.begin(), anchors.end(),
                               [](const WarpAnchor& a) {
                                 return !std::isfinite(a.warp_sample) ||
                                        !std::isfinite(a.source_sample);
                               }),
                anchors.end());
  std::sort(anchors.begin(), anchors.end(), [](const WarpAnchor& a, const WarpAnchor& b) {
    if (a.warp_sample != b.warp_sample) return a.warp_sample < b.warp_sample;
    return a.source_sample < b.source_sample;
  });
  std::vector<WarpAnchor> out;
  for (const WarpAnchor& a : anchors) {
    if (out.empty()) {
      out.push_back(a);
      continue;
    }
    const WarpAnchor& last = out.back();
    if (a.warp_sample > last.warp_sample && a.source_sample > last.source_sample) {
      out.push_back(a);
    }
  }
  return out;
}

// Linear interpolation / clamped-slope extrapolation along one axis of the map.
// `from` selects the key axis, `to` the value axis.
double piecewise_map(const std::vector<WarpAnchor>& anchors, double x, double WarpAnchor::*from,
                     double WarpAnchor::*to) {
  const size_t n = anchors.size();
  // Below the first anchor: extrapolate using the first segment's slope.
  if (x <= anchors.front().*from) {
    const double x0 = anchors[0].*from;
    const double y0 = anchors[0].*to;
    const double x1 = anchors[1].*from;
    const double y1 = anchors[1].*to;
    const double slope = (y1 - y0) / (x1 - x0);
    return y0 + slope * (x - x0);
  }
  // Above the last anchor: extrapolate using the last segment's slope.
  if (x >= anchors.back().*from) {
    const double x0 = anchors[n - 2].*from;
    const double y0 = anchors[n - 2].*to;
    const double x1 = anchors[n - 1].*from;
    const double y1 = anchors[n - 1].*to;
    const double slope = (y1 - y0) / (x1 - x0);
    return y1 + slope * (x - x1);
  }
  // Binary search for the bracketing segment on the key axis.
  size_t lo = 0;
  size_t hi = n - 1;
  while (hi - lo > 1) {
    const size_t mid = (lo + hi) / 2;
    if (anchors[mid].*from <= x) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  const double x0 = anchors[lo].*from;
  const double y0 = anchors[lo].*to;
  const double x1 = anchors[hi].*from;
  const double y1 = anchors[hi].*to;
  const double t = (x - x0) / (x1 - x0);
  return y0 + t * (y1 - y0);
}

// Mean-pool a chroma matrix [n_chroma x n_frames] (row-major) by `factor` along
// time. Used by the multiscale DTW coarsening.
std::vector<float> downsample_time(const std::vector<float>& feat, int n_chroma, int n_frames,
                                   int factor, int* out_frames) {
  const int out_n = (n_frames + factor - 1) / factor;
  std::vector<float> out(static_cast<size_t>(n_chroma) * out_n, 0.0f);
  for (int c = 0; c < n_chroma; ++c) {
    for (int of = 0; of < out_n; ++of) {
      const int start = of * factor;
      const int end = std::min(start + factor, n_frames);
      float acc = 0.0f;
      for (int f = start; f < end; ++f) {
        acc += feat[static_cast<size_t>(c) * n_frames + f];
      }
      out[static_cast<size_t>(c) * out_n + of] = acc / static_cast<float>(end - start);
    }
  }
  *out_frames = out_n;
  return out;
}

// Full DTW over two chroma matrices, returns the warping path as (ref, tgt)
// pairs in increasing order. Uses the shared util/sequence dtw (cosine metric).
std::vector<std::pair<int, int>> full_dtw_path(const std::vector<float>& ref, int n_chroma,
                                               int ref_frames, const std::vector<float>& tgt,
                                               int tgt_frames) {
  // util/sequence dtw expects features [rows x cols] with rows = feature dim.
  // X is reference, Y is target -> path pairs are (X-index, Y-index) = (ref, tgt).
  DtwResult r = dtw(ref.data(), n_chroma, ref_frames, tgt.data(), n_chroma, tgt_frames, "cosine");
  // Moved out rather than copied: `r` also holds the [ref_frames x tgt_frames]
  // accumulated-cost matrix, which nothing on this route reads.
  return std::move(r.path);  // already (X, Y) ordered start->end.
}

// Project a path from a coarse level (factors of `scale`) up to a finer level
// by multiplying indices and clamping. Returns, per finer reference frame, the
// projected target frame index.
std::vector<int> project_path(const std::vector<std::pair<int, int>>& coarse_path, int scale,
                              int fine_ref_frames, int fine_tgt_frames) {
  std::vector<int> projected(fine_ref_frames, 0);
  if (coarse_path.empty()) return projected;
  // Build a coarse ref->tgt lookup, then expand by `scale`.
  std::vector<int> coarse_tgt;
  int max_ref = 0;
  for (const auto& p : coarse_path) max_ref = std::max(max_ref, p.first);
  coarse_tgt.assign(max_ref + 1, 0);
  for (const auto& p : coarse_path) coarse_tgt[p.first] = p.second;
  for (int i = 0; i < fine_ref_frames; ++i) {
    const int ci = std::min(i / scale, max_ref);
    const int proj = coarse_tgt[ci] * scale;
    projected[i] = std::clamp(proj, 0, fine_tgt_frames - 1);
  }
  return projected;
}

// Multiscale DTW path at the original chroma resolution. See @ref mir_warp_mrmsdtw.
std::vector<std::pair<int, int>> mrmsdtw_path(const std::vector<float>& ref, int n_chroma,
                                              int ref_frames, const std::vector<float>& tgt,
                                              int tgt_frames, const ChromaDtwConfig& cfg) {
  // Small enough -> single full DTW.
  if (ref_frames <= cfg.max_full_frames && tgt_frames <= cfg.max_full_frames) {
    return full_dtw_path(ref, n_chroma, ref_frames, tgt, tgt_frames);
  }
  const int scale = std::max(2, cfg.scale_factor);
  // Build the coarsening pyramid (level 0 = original).
  struct Level {
    std::vector<float> ref, tgt;
    int ref_frames, tgt_frames;
  };
  std::vector<Level> pyramid;
  pyramid.push_back({ref, tgt, ref_frames, tgt_frames});
  while (std::max(pyramid.back().ref_frames, pyramid.back().tgt_frames) > cfg.coarse_max_frames) {
    const Level& top = pyramid.back();
    int rf = 0, tf = 0;
    std::vector<float> dr = downsample_time(top.ref, n_chroma, top.ref_frames, scale, &rf);
    std::vector<float> dt = downsample_time(top.tgt, n_chroma, top.tgt_frames, scale, &tf);
    pyramid.push_back({std::move(dr), std::move(dt), rf, tf});
  }
  // Full DTW at the coarsest level.
  const Level& coarse = pyramid.back();
  std::vector<std::pair<int, int>> path =
      full_dtw_path(coarse.ref, n_chroma, coarse.ref_frames, coarse.tgt, coarse.tgt_frames);
  // Refine down through finer levels inside a band.
  for (int lvl = static_cast<int>(pyramid.size()) - 2; lvl >= 0; --lvl) {
    const Level& fine = pyramid[lvl];
    std::vector<int> projected = project_path(path, scale, fine.ref_frames, fine.tgt_frames);
    path = detail::banded_dtw_path(fine.ref, n_chroma, fine.ref_frames, fine.tgt, fine.tgt_frames,
                                   projected, cfg.band_radius);
  }
  return path;
}

std::vector<float> hann_window(int n) {
  std::vector<float> w(static_cast<size_t>(n), 1.0f);
  if (n <= 1) return w;
  for (int i = 0; i < n; ++i) {
    w[static_cast<size_t>(i)] =
        static_cast<float>(0.5 - 0.5 * std::cos(constants::kTwoPiD * i / (n - 1)));
  }
  return w;
}

float window_similarity(const Audio& input, int candidate, const std::vector<float>& rendered,
                        int out_pos, int overlap) {
  double dot = 0.0;
  double na = 0.0;
  double nb = 0.0;
  for (int i = 0; i < overlap; ++i) {
    const int in_index = candidate + i;
    const float a =
        (in_index >= 0 && static_cast<size_t>(in_index) < input.size()) ? input[in_index] : 0.0f;
    const float b = rendered[static_cast<size_t>(out_pos + i)];
    dot += static_cast<double>(a) * b;
    na += static_cast<double>(a) * a;
    nb += static_cast<double>(b) * b;
  }
  if (na <= 1e-20 || nb <= 1e-20) {
    // Silence in either side gives no meaningful correlation. Prefer the
    // nominal position by returning a neutral score.
    return 0.0f;
  }
  return static_cast<float>(dot / (std::sqrt(na) * std::sqrt(nb)));
}

// Waveform Similarity OLA for the percussive component. `rate` < 1 lengthens,
// > 1 shortens (matching phase_vocoder). The nominal input advance is
// synthesis_hop * rate; each frame searches a deterministic local window around
// that position and chooses the candidate whose leading overlap is most similar
// to the already-rendered output.
Audio percussive_wsola(const Audio& audio, float rate, size_t target_length,
                       const WarpTsmConfig& config) {
  const int win_length = std::max(16, config.n_fft);
  const int synthesis_hop = std::max(1, config.hop_length);
  const double analysis_hop = std::max(1.0, static_cast<double>(synthesis_hop) * rate);
  const int search_radius = std::max(1, synthesis_hop / 2);
  const std::vector<float> window = hann_window(win_length);

  std::vector<float> rendered(target_length + static_cast<size_t>(win_length), 0.0f);
  std::vector<float> norm(rendered.size(), 0.0f);
  const int max_start = std::max(0, static_cast<int>(audio.size()) - win_length);

  for (int frame = 0;; ++frame) {
    const int out_pos = frame * synthesis_hop;
    if (static_cast<size_t>(out_pos) >= target_length) break;

    int nominal = static_cast<int>(std::llround(frame * analysis_hop));
    nominal = std::clamp(nominal, 0, max_start);
    int best = nominal;
    if (frame > 0) {
      const int overlap =
          std::max(0, std::min(win_length, std::min(out_pos, win_length - synthesis_hop)));
      float best_score = -2.0f;
      const int lo = std::max(0, nominal - search_radius);
      const int hi = std::min(max_start, nominal + search_radius);
      for (int candidate = lo; candidate <= hi; ++candidate) {
        const float score = window_similarity(audio, candidate, rendered, out_pos, overlap);
        if (score > best_score) {
          best_score = score;
          best = candidate;
        }
      }
    }

    for (int i = 0; i < win_length; ++i) {
      const size_t out_index = static_cast<size_t>(out_pos + i);
      if (out_index >= rendered.size()) break;
      const int in_index = best + i;
      const float sample =
          (in_index >= 0 && static_cast<size_t>(in_index) < audio.size()) ? audio[in_index] : 0.0f;
      const float w = window[static_cast<size_t>(i)];
      rendered[out_index] += sample * w;
      norm[out_index] += w;
    }
  }

  std::vector<float> out(target_length, 0.0f);
  for (size_t i = 0; i < target_length; ++i) {
    out[i] = norm[i] > 1e-8f ? rendered[i] / norm[i] : 0.0f;
  }
  return Audio::from_vector(std::move(out), audio.sample_rate());
}

// Core TSM: HPSS-split, stretch each component by `rate`, sum to `target_length`.
Audio tsm_rate(const Audio& audio, float rate, size_t target_length, const WarpTsmConfig& config) {
  StftConfig stft;
  stft.n_fft = config.n_fft;
  stft.hop_length = config.hop_length;
  stft.window = WindowType::Hann;
  stft.center = true;

  HpssConfig hpss_cfg;
  hpss_cfg.kernel_size_harmonic = config.hpss_kernel_harmonic;
  hpss_cfg.kernel_size_percussive = config.hpss_kernel_percussive;

  // Separate in the spectral domain so we keep one shared analysis grid.
  Spectrogram spec = Spectrogram::compute(audio, stft);
  HpssSpectrogramResult hp = hpss(spec, hpss_cfg);

  PhaseVocoderConfig pv;
  pv.hop_length = config.hop_length;

  // Harmonic: phase-locked phase vocoder (tonal coherence).
  Spectrogram h_stretched = phase_vocoder_phaselocked(hp.harmonic, rate, pv);

  const int len = static_cast<int>(target_length);
  Audio h_audio = h_stretched.to_audio(len);
  Audio p_source = hp.percussive.to_audio(static_cast<int>(audio.size()));
  Audio p_audio = percussive_wsola(p_source, rate, target_length, config);

  // Sum the two components into a fresh buffer of exactly target_length.
  std::vector<float> out(target_length, 0.0f);
  const size_t hn = h_audio.size();
  const size_t pn = p_audio.size();
  for (size_t i = 0; i < target_length; ++i) {
    float v = 0.0f;
    if (i < hn) v += h_audio[i];
    if (i < pn) v += p_audio[i];
    out[i] = v;
  }
  return Audio::from_vector(std::move(out), audio.sample_rate());
}

}  // namespace

// ---------------------------------------------------------------------------
// WarpMap
// ---------------------------------------------------------------------------

WarpMap WarpMap::from_anchors(std::vector<WarpAnchor> anchors) {
  std::vector<WarpAnchor> clean = sanitize_anchors(std::move(anchors));
  SONARE_CHECK(clean.size() >= 2, ErrorCode::InvalidParameter);
  return WarpMap(std::move(clean));
}

WarpMap WarpMap::from_markers(const std::vector<double>& warp_samples,
                              const std::vector<double>& source_samples) {
  SONARE_CHECK(warp_samples.size() == source_samples.size(), ErrorCode::InvalidParameter);
  std::vector<WarpAnchor> anchors;
  anchors.reserve(warp_samples.size());
  for (size_t i = 0; i < warp_samples.size(); ++i) {
    anchors.push_back(WarpAnchor{warp_samples[i], source_samples[i]});
  }
  return from_anchors(std::move(anchors));
}

double WarpMap::warp_to_source(double warp_sample) const {
  SONARE_CHECK(valid(), ErrorCode::InvalidState);
  return piecewise_map(anchors_, warp_sample, &WarpAnchor::warp_sample, &WarpAnchor::source_sample);
}

double WarpMap::source_to_warp(double source_sample) const {
  SONARE_CHECK(valid(), ErrorCode::InvalidState);
  return piecewise_map(anchors_, source_sample, &WarpAnchor::source_sample,
                       &WarpAnchor::warp_sample);
}

// ---------------------------------------------------------------------------
// chroma_dtw_align
// ---------------------------------------------------------------------------

ChromaDtwResult chroma_dtw_align(const Audio& reference, const Audio& target,
                                 const ChromaDtwConfig& config) {
  SONARE_CHECK(!reference.empty() && !target.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(config.bins_per_octave > 0 &&
                   config.bins_per_octave <= std::numeric_limits<int>::max() / kChromaOctaves,
               ErrorCode::InvalidParameter);

  // n_bins is a bin count, not an octave span, so it has to move with
  // bins_per_octave. ChromaCqtConfig defaults to 7 octaves at 36 bins/octave;
  // assigning bins_per_octave alone would leave 252 bins and stretch the grid
  // to 252/bins_per_octave octaves, whose top bin runs past Nyquist and is
  // rejected by the CQT kernel. Deriving n_bins from the octave span is what
  // the binding-level chroma entry points already do, so the grid here is the
  // same one those produce for a given resolution. At the default twelve bins
  // per octave the top bin sits at 3951 Hz, so any sample rate from 8 kHz up
  // carries the whole grid; a finer resolution raises that bin and needs the
  // sample rate to follow. Either way the CQT kernel rejects a grid that runs
  // past Nyquist, rather than this silently analysing a narrower,
  // sample-rate-dependent range.
  ChromaCqtConfig ccfg;
  ccfg.cqt.hop_length = config.hop_length;
  ccfg.cqt.bins_per_octave = config.bins_per_octave;
  ccfg.cqt.n_bins = config.bins_per_octave * kChromaOctaves;

  Chroma ref_chroma = chroma_cqt(reference, ccfg);
  Chroma tgt_chroma = chroma_cqt(target, ccfg);

  const int n_chroma = ref_chroma.n_chroma();
  const int ref_frames = ref_chroma.n_frames();
  const int tgt_frames = tgt_chroma.n_frames();
  SONARE_CHECK(ref_frames >= 2 && tgt_frames >= 2 && tgt_chroma.n_chroma() == n_chroma,
               ErrorCode::InvalidParameter);

  std::vector<float> ref_feat(ref_chroma.data(), ref_chroma.data() + n_chroma * ref_frames);
  std::vector<float> tgt_feat(tgt_chroma.data(), tgt_chroma.data() + n_chroma * tgt_frames);

  ChromaDtwResult result;
  result.reference_frames = ref_frames;
  result.target_frames = tgt_frames;
  result.path = mrmsdtw_path(ref_feat, n_chroma, ref_frames, tgt_feat, tgt_frames, config);

  // Anchors: warp_sample = target position, source_sample = reference position,
  // both at audio-sample resolution (frame * hop_length).
  const double hop = static_cast<double>(config.hop_length);
  result.anchors.reserve(result.path.size());
  double residual_sum = 0.0;
  const double ref_span = std::max(1, ref_frames - 1);
  const double tgt_span = std::max(1, tgt_frames - 1);
  for (const auto& p : result.path) {
    WarpAnchor a;
    a.warp_sample = p.second * hop;   // target
    a.source_sample = p.first * hop;  // reference
    result.anchors.push_back(a);
    // Residual around the global diagonal (ref_frac vs tgt_frac).
    const double ref_frac = p.first / ref_span;
    const double tgt_frac = p.second / tgt_span;
    residual_sum += std::abs(ref_frac - tgt_frac) * tgt_span;
  }
  if (!result.path.empty()) {
    result.mean_residual_frames =
        static_cast<float>(residual_sum / static_cast<double>(result.path.size()));
  }
  return result;
}

// ---------------------------------------------------------------------------
// Time-scale modification
// ---------------------------------------------------------------------------

Audio warp_to_length(const Audio& audio, size_t target_length, const WarpTsmConfig& config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(target_length > 0, ErrorCode::InvalidParameter);
  // rate maps input length -> target length: out = in / rate => rate = in/target.
  const float rate = static_cast<float>(audio.size()) / static_cast<float>(target_length);
  return tsm_rate(audio, rate, target_length, config);
}

Audio warp_to_map(const Audio& audio, const WarpMap& map, const WarpTsmConfig& config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(map.valid(), ErrorCode::InvalidState);
  const auto& anchors = map.anchors();
  const WarpAnchor& first = anchors.front();
  const WarpAnchor& last = anchors.back();
  const double warp_span = last.warp_sample - first.warp_sample;
  const double source_span = last.source_sample - first.source_sample;
  SONARE_CHECK(warp_span > 0.0 && source_span > 0.0, ErrorCode::InvalidState);
  const size_t target_length = static_cast<size_t>(std::llround(warp_span));
  SONARE_CHECK(target_length > 0, ErrorCode::InvalidState);

  std::vector<float> out;
  out.reserve(target_length);
  for (size_t i = 1; i < anchors.size(); ++i) {
    const WarpAnchor& a = anchors[i - 1];
    const WarpAnchor& b = anchors[i];
    const double segment_warp_span = b.warp_sample - a.warp_sample;
    const double segment_source_span = b.source_sample - a.source_sample;
    SONARE_CHECK(segment_warp_span > 0.0 && segment_source_span > 0.0, ErrorCode::InvalidState);

    const size_t segment_target_length = static_cast<size_t>(std::llround(segment_warp_span));
    SONARE_CHECK(segment_target_length > 0, ErrorCode::InvalidState);
    int64_t source_start = static_cast<int64_t>(std::llround(a.source_sample));
    int64_t source_end = static_cast<int64_t>(std::llround(b.source_sample));
    source_start = std::clamp<int64_t>(source_start, 0, static_cast<int64_t>(audio.size()));
    source_end = std::clamp<int64_t>(source_end, 0, static_cast<int64_t>(audio.size()));
    SONARE_CHECK(source_end > source_start, ErrorCode::InvalidState);

    std::vector<float> segment(static_cast<size_t>(source_end - source_start));
    for (size_t n = 0; n < segment.size(); ++n) {
      segment[n] = audio[static_cast<size_t>(source_start) + n];
    }
    Audio segment_audio = Audio::from_vector(std::move(segment), audio.sample_rate());
    Audio warped =
        tsm_rate(segment_audio, static_cast<float>(segment_source_span / segment_warp_span),
                 segment_target_length, config);
    out.insert(out.end(), warped.begin(), warped.end());
  }

  if (out.size() > target_length) {
    out.resize(target_length);
  } else if (out.size() < target_length) {
    out.resize(target_length, 0.0f);
  }
  return Audio::from_vector(std::move(out), audio.sample_rate());
}

namespace {

/// One median-collapse pass: a run of anchors sharing @p key is represented by
/// its middle element. Takes the key as a member pointer so the same pass serves
/// both axes without a second copy of the walk.
std::vector<WarpAnchor> collapse_ties(const std::vector<WarpAnchor>& anchors,
                                      double WarpAnchor::*key) {
  std::vector<WarpAnchor> out;
  out.reserve(anchors.size());
  size_t run_start = 0;
  while (run_start < anchors.size()) {
    size_t run_end = run_start + 1;
    while (run_end < anchors.size() && anchors[run_end].*key == anchors[run_start].*key) ++run_end;
    // Lower median, so an even run resolves the same way every time.
    out.push_back(anchors[run_start + (run_end - run_start - 1) / 2]);
    run_start = run_end;
  }
  return out;
}

}  // namespace

std::vector<WarpAnchor> strictly_increasing_anchors(const std::vector<WarpAnchor>& anchors) {
  if (anchors.size() < 2) return anchors;
  // Source first, then warp: the second pass drops entries from a sequence the
  // first pass already made strict, and a subsequence of a strictly increasing
  // sequence stays strictly increasing, so both properties hold at the end. The
  // reverse order would be equally valid; fixing one keeps the result
  // reproducible.
  std::vector<WarpAnchor> reduced =
      collapse_ties(collapse_ties(anchors, &WarpAnchor::source_sample), &WarpAnchor::warp_sample);
  if (reduced.size() < 2) return anchors;
  return reduced;
}

}  // namespace sonare::mir
