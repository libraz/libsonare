#include "mir/warp.h"

#include <algorithm>
#include <cmath>

#include "core/spectrum.h"
#include "effects/hpss.h"
#include "effects/phase_vocoder.h"
#include "feature/chroma.h"
#include "util/exception.h"
#include "util/sequence.h"

namespace sonare::mir {
namespace {

// Strictly-monotonic de-duplication of anchors sorted by warp_sample. Drops any
// anchor that does not strictly advance BOTH axes relative to the kept anchor,
// so the map is invertible in both directions.
std::vector<WarpAnchor> sanitize_anchors(std::vector<WarpAnchor> anchors) {
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
  return r.path;  // already (X, Y) ordered start->end.
}

// Banded DTW: only cells within +/- band_radius of the projected path's target
// index (per reference frame) are evaluated. O(ref_frames * band_width) memory.
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
  // Ensure monotonic, overlapping bands so a continuous path exists.
  for (int i = 1; i < ref_frames; ++i) {
    lo[i] = std::min(lo[i], lo[i - 1]);
    hi[i] = std::max(hi[i], hi[i - 1]);
  }
  lo[0] = 0;  // anchor the start corner.

  auto cos_dist = [&](int i, int j) -> float {
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

  // Accumulated cost stored densely per row over [lo[i], hi[i]].
  std::vector<std::vector<float>> acc(ref_frames);
  std::vector<std::vector<int>> back(ref_frames);  // 0=diag,1=up(ref-1),2=left(tgt-1)
  for (int i = 0; i < ref_frames; ++i) {
    const int width = hi[i] - lo[i] + 1;
    acc[i].assign(width, kInf);
    back[i].assign(width, -1);
  }
  auto at = [&](int i, int j) -> float {
    if (i < 0 || j < lo[i] || j > hi[i]) return kInf;
    return acc[i][j - lo[i]];
  };

  for (int i = 0; i < ref_frames; ++i) {
    for (int j = lo[i]; j <= hi[i]; ++j) {
      const float local = cos_dist(i, j);
      if (i == 0 && j == 0) {
        acc[i][j - lo[i]] = local;
        back[i][j - lo[i]] = -1;
        continue;
      }
      const float d = at(i - 1, j - 1);               // diagonal
      const float u = at(i - 1, j);                   // ref advance
      const float l = (j > 0) ? at(i, j - 1) : kInf;  // tgt advance
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
      acc[i][j - lo[i]] = best + local;
      back[i][j - lo[i]] = bk;
    }
  }

  // Backtrack from the (ref_frames-1, tgt_frames-1) corner.
  std::vector<std::pair<int, int>> path;
  int i = ref_frames - 1;
  int j = tgt_frames - 1;
  if (j < lo[i] || j > hi[i] || acc[i][j - lo[i]] >= kInf) {
    // Corner outside the band (degenerate); fall back to nearest in-band cell.
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
    path = banded_dtw_path(fine.ref, n_chroma, fine.ref_frames, fine.tgt, fine.tgt_frames,
                           projected, cfg.band_radius);
  }
  return path;
}

// Stretch a spectrogram by integer frame replication / decimation WITHOUT phase
// propagation: each output frame copies the nearest source frame verbatim. This
// is the percussive OLA path (transient-preserving) described in @ref
// mir_warp_tsm. `rate` < 1 lengthens, > 1 shortens (matching phase_vocoder).
Spectrogram percussive_frame_stretch(const Spectrogram& spec, float rate) {
  const int n_bins = spec.n_bins();
  const int in_frames = spec.n_frames();
  const int out_frames = std::max(1, static_cast<int>(std::round(in_frames / rate)));
  std::vector<std::complex<float>> out(static_cast<size_t>(n_bins) * out_frames);
  const std::complex<float>* in = spec.complex_data();
  for (int of = 0; of < out_frames; ++of) {
    // Nearest-source-frame mapping (deterministic, transient-preserving).
    int src = (out_frames > 1) ? static_cast<int>(std::round(static_cast<double>(of) *
                                                             (in_frames - 1) / (out_frames - 1)))
                               : 0;
    src = std::clamp(src, 0, in_frames - 1);
    for (int b = 0; b < n_bins; ++b) {
      out[static_cast<size_t>(b) * out_frames + of] = in[static_cast<size_t>(b) * in_frames + src];
    }
  }
  return Spectrogram::from_complex(out.data(), n_bins, out_frames, spec.n_fft(), spec.hop_length(),
                                   spec.sample_rate(), spec.center(), spec.win_length());
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
  // Percussive: frame-replication OLA (transient preservation), per @ref mir_warp_tsm.
  Spectrogram p_stretched = percussive_frame_stretch(hp.percussive, rate);

  const int len = static_cast<int>(target_length);
  Audio h_audio = h_stretched.to_audio(len);
  Audio p_audio = p_stretched.to_audio(len);

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

  ChromaCqtConfig ccfg;
  ccfg.cqt.hop_length = config.hop_length;
  ccfg.cqt.bins_per_octave = config.bins_per_octave;

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
  // Output spans the warp axis from the first to the last anchor; the realized
  // (current-scope) warp is a single global rate equal to the average
  // source/warp slope across the map. Variable-rate segment warping is a
  // documented future extension.
  const WarpAnchor& first = map.anchors().front();
  const WarpAnchor& last = map.anchors().back();
  const double warp_span = last.warp_sample - first.warp_sample;
  const double source_span = last.source_sample - first.source_sample;
  SONARE_CHECK(warp_span > 0.0 && source_span > 0.0, ErrorCode::InvalidState);
  const size_t target_length = static_cast<size_t>(std::llround(warp_span));
  SONARE_CHECK(target_length > 0, ErrorCode::InvalidState);
  const float rate = static_cast<float>(source_span / warp_span);
  return tsm_rate(audio, rate, target_length, config);
}

}  // namespace sonare::mir
