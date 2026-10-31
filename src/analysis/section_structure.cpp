#include "analysis/section_structure.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <functional>
#include <limits>

#include "analysis/analysis_evidence.h"
#include "analysis/analysis_rate.h"
#include "analysis/beat_analyzer.h"
#include "core/spectrum.h"
#include "feature/mel_spectrogram.h"
#include "feature/spectral.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/insertion_sort.h"
#include "util/math_utils.h"

namespace sonare {
namespace section_structure {

using constants::kEpsilon;
using constants::kInvSqrt2;

namespace {

/// Fraction of the body reference (second-loudest section once there are enough) below which
/// an edge section is quiet.
constexpr float kLowEnergyThreshold = 0.35f;
/// Vocal likelihood below which a section counts as instrumental.
constexpr float kInstrumentalVocalThreshold = 0.40f;
/// Lower edge of the vocal energy band in Hz.
constexpr float kVocalBandLowHz = 300.0f;
/// Upper edge of the vocal energy band in Hz.
constexpr float kVocalBandHighHz = 3400.0f;
/// Scales vocal-band ratio x tonality onto [0, 1]: a fully tonal half-band section reads as 1.
constexpr float kVocalLikelihoodGain = 2.0f;
/// With at least this many sections the loudest is set aside from the quiet reference.
constexpr size_t kMinSectionsToSetAside = 4;

/// A join candidate's length must lie within these multiples of the two halves' combined length.
constexpr float kJoinLengthLow = 0.8f;
constexpr float kJoinLengthHigh = 1.25f;
/// Relative tolerance under which two compared quantities are tied.
constexpr double kTieTolerance = 1e-6;

/// Confidence of a section of a repeating group: base plus its mean in-group pair score scaled.
constexpr float kGroupConfidenceBase = 0.5f;
constexpr float kGroupConfidenceScale = 0.5f;
/// Edge (Intro/Outro) confidence: by length or instrumental reading; quiet adds up to the scale.
constexpr float kEdgeConfidence = 0.6f;
constexpr float kQuietConfidenceScale = 0.3f;
/// Instrumental-by-group confidence: base plus its best pair score with the edge section.
constexpr float kInstrumentalGroupBase = 0.5f;
constexpr float kInstrumentalGroupScale = 0.5f;
/// Vocal-evidence confidence (Instrumental from its absence, Bridge from its presence).
constexpr float kVocalConfidenceBase = 0.5f;
constexpr float kVocalConfidenceScale = 0.4f;

bool tied(double a, double b) {
  if (!std::isfinite(a) || !std::isfinite(b)) return a == b;
  return std::abs(a - b) <= kTieTolerance * std::max(std::abs(a), std::abs(b));
}

/// a > b by more than the tie tolerance.
bool greater(double a, double b) { return a > b && !tied(a, b); }

float median_of(std::vector<float> values) {
  if (values.empty()) return 0.0f;
  return median(values.data(), values.size());
}

/// Energy every section is judged quiet against: the loudest is set aside once there are enough
/// sections for the rest to stand as the body, so one dominant chorus cannot make an ordinary
/// edge section read as quiet.
float low_energy_reference(const std::vector<float>& energies) {
  std::vector<float> sorted = energies;
  insertion_sort(sorted.begin(), sorted.end(), std::greater<float>());
  if (sorted.empty()) return 0.0f;
  const bool set_aside = sorted.size() >= kMinSectionsToSetAside && has_evidence(sorted[1]);
  return set_aside ? sorted[1] : sorted[0];
}

/// Analysis frames covering [start, end) seconds, clamped to @p count frames.
std::pair<int, int> frame_range(float start, float end, float frame_sec, size_t count) {
  const int limit = static_cast<int>(count);
  const int first = std::clamp(static_cast<int>(start / frame_sec), 0, limit);
  const int last = std::clamp(static_cast<int>(end / frame_sec), first, limit);
  return {first, last};
}

/// Median frame RMS over [start, end); a sub-frame section reads its one frame.
float section_energy(const FrameFeatures& frames, float start, float end) {
  if (frames.rms.empty() || start >= end || frames.frame_sec <= 0.0f) return 0.0f;
  const auto [first, last] = frame_range(start, end, frames.frame_sec, frames.rms.size());
  if (first >= last) {
    return first < static_cast<int>(frames.rms.size()) ? frames.rms[static_cast<size_t>(first)]
                                                       : 0.0f;
  }
  return median(&frames.rms[static_cast<size_t>(first)], static_cast<size_t>(last - first));
}

/// Vocal-band energy share weighted by tonality, over [start, end).
float section_vocal_likelihood(const FrameFeatures& frames, float start, float end) {
  if (frames.frame_sec <= 0.0f) return 0.0f;
  const auto [first, last] = frame_range(start, end, frames.frame_sec, frames.total_energy.size());
  double band = 0.0;
  double total = 0.0;
  double tonality = 0.0;
  for (int f = first; f < last; ++f) {
    band += frames.vocal_band_energy[static_cast<size_t>(f)];
    total += frames.total_energy[static_cast<size_t>(f)];
    tonality += frames.tonality[static_cast<size_t>(f)];
  }
  const int count = last - first;
  const float band_ratio = total > kEpsilon ? static_cast<float>(band / total) : 0.0f;
  const float mean_tonality = count > 0 ? static_cast<float>(tonality / count) : 0.0f;
  return std::clamp(band_ratio * mean_tonality * kVocalLikelihoodGain, 0.0f, 1.0f);
}

float span_seconds(const Span& span, const ColumnGrid& grid) {
  return grid.time(span.end) - grid.time(span.begin);
}

void normalize(float* values, int n) {
  double norm = 0.0;
  for (int i = 0; i < n; ++i) norm += static_cast<double>(values[i]) * values[i];
  if (norm <= kEpsilon) {
    std::fill(values, values + n, 0.0f);
    return;
  }
  const float inv = static_cast<float>(1.0 / std::sqrt(norm));
  for (int i = 0; i < n; ++i) values[i] *= inv;
}

}  // namespace

float ColumnGrid::time(int column) const {
  if (column >= size()) return duration_sec;
  return times[static_cast<size_t>(std::max(column, 0))];
}

int ColumnGrid::to_columns(float seconds) const {
  if (column_sec <= 0.0f) return 0;
  const float columns = seconds / column_sec;
  if (!std::isfinite(columns)) return std::numeric_limits<int>::max() / 2;
  return static_cast<int>(std::lround(
      std::clamp(columns, 0.0f, static_cast<float>(std::numeric_limits<int>::max() / 2))));
}

ChromaConfig section_chroma_config(const SectionConfig& config, int sample_rate) {
  const RateWindow window = window_at_rate(config.n_fft, sample_rate);
  ChromaConfig chroma_config;
  chroma_config.n_fft = window.n_fft;
  chroma_config.win_length = window.win_length;
  chroma_config.hop_length = config.hop_length;
  chroma_config.tuning = config.tuning;
  return chroma_config;
}

SectionGrid make_section_grid(const BeatAnalyzer& beats) {
  SectionGrid grid;
  grid.beat_times = beats.beat_times();
  grid.downbeat_indices = beats.downbeat_indices();
  grid.downbeats_reliable = beats.time_signature().confidence >= kDownbeatConfidence;
  return grid;
}

FrameFeatures frame_features(const Audio& audio, const Spectrogram& spec,
                             const SectionConfig& config) {
  FrameFeatures out;
  const int sr = audio.sample_rate();
  const int n_frames = spec.n_frames();
  out.n_frames = n_frames;
  out.frame_sec = static_cast<float>(config.hop_length) / static_cast<float>(sr);
  out.duration_sec = audio.duration();
  out.frame_times.resize(static_cast<size_t>(n_frames));
  for (int f = 0; f < n_frames; ++f) {
    out.frame_times[static_cast<size_t>(f)] = static_cast<float>(f) * out.frame_sec;
  }

  const ChromaConfig chroma_config = section_chroma_config(config, sr);
  MelFilterConfig mel_filter;
  mel_filter.n_mels = constants::kDefaultNMels;
  const MelSpectrogram mel = MelSpectrogram::from_spectrogram(spec, sr, mel_filter);
  const std::vector<float> mfcc = mel.mfcc(kFrameMfcc + 1);
  const int mel_frames = mel.n_frames();
  out.mfcc.assign(static_cast<size_t>(kFrameMfcc) * n_frames, 0.0f);
  for (int c = 0; c < kFrameMfcc; ++c) {
    for (int f = 0; f < std::min(n_frames, mel_frames); ++f) {
      out.mfcc[static_cast<size_t>(c) * n_frames + f] =
          mfcc[static_cast<size_t>(c + 1) * mel_frames + f];
    }
  }

  const Chroma chroma = Chroma::from_spectrogram(spec, sr, chroma_config.to_chroma_filter_config());
  out.chroma.assign(static_cast<size_t>(kFrameChroma) * n_frames, 0.0f);
  for (int c = 0; c < std::min(kFrameChroma, chroma.n_chroma()); ++c) {
    for (int f = 0; f < std::min(n_frames, chroma.n_frames()); ++f) {
      out.chroma[static_cast<size_t>(c) * n_frames + f] = chroma.at(c, f);
    }
  }

  out.rms =
      rms_energy(audio, chroma_config.to_stft_config().actual_win_length(), config.hop_length);

  const int n_bins = spec.n_bins();
  std::vector<float> mag(static_cast<size_t>(n_bins) * static_cast<size_t>(n_frames));
  const std::complex<float>* spectrum = spec.complex_data();
  for (size_t i = 0; i < mag.size(); ++i) mag[i] = std::abs(spectrum[i]);
  const std::vector<float> flatness =
      n_frames > 0 ? spectral_flatness(mag.data(), n_bins, n_frames) : std::vector<float>{};
  const float bin_hz = static_cast<float>(sr) / static_cast<float>(spec.n_fft());
  const int low_bin =
      std::clamp(static_cast<int>(std::floor(kVocalBandLowHz / bin_hz)), 0, n_bins - 1);
  const int high_bin =
      std::clamp(static_cast<int>(std::ceil(kVocalBandHighHz / bin_hz)), 0, n_bins - 1);
  out.vocal_band_energy.assign(static_cast<size_t>(n_frames), 0.0f);
  out.total_energy.assign(static_cast<size_t>(n_frames), 0.0f);
  out.tonality.assign(static_cast<size_t>(n_frames), 0.0f);
  for (int f = 0; f < n_frames; ++f) {
    double band = 0.0;
    double total = 0.0;
    for (int k = 0; k < n_bins; ++k) {
      const double m = mag[static_cast<size_t>(k) * n_frames + f];
      total += m * m;
      if (k >= low_bin && k <= high_bin) band += m * m;
    }
    out.vocal_band_energy[static_cast<size_t>(f)] = static_cast<float>(band);
    out.total_energy[static_cast<size_t>(f)] = static_cast<float>(total);
    if (static_cast<size_t>(f) < flatness.size()) {
      out.tonality[static_cast<size_t>(f)] =
          1.0f - std::clamp(flatness[static_cast<size_t>(f)], 0.0f, 1.0f);
    }
  }
  return out;
}

ColumnGrid column_grid(const SectionGrid& grid, float duration_sec, float frame_sec) {
  ColumnGrid out;
  out.duration_sec = std::max(0.0f, duration_sec);
  out.downbeats_reliable = grid.downbeats_reliable;

  // Beats inside the audio, a beat closer than one frame to the previous absorbed into it.
  std::vector<float> beats;
  std::vector<bool> downbeat;
  size_t next_downbeat = 0;
  for (size_t i = 0; i < grid.beat_times.size(); ++i) {
    bool is_downbeat = false;
    while (next_downbeat < grid.downbeat_indices.size() &&
           grid.downbeat_indices[next_downbeat] <= static_cast<int>(i)) {
      is_downbeat = is_downbeat || grid.downbeat_indices[next_downbeat] == static_cast<int>(i);
      ++next_downbeat;
    }
    const float t = grid.beat_times[i];
    if (t >= out.duration_sec) break;
    if (!beats.empty() && t - beats.back() < frame_sec) {
      if (is_downbeat) downbeat.back() = true;
      continue;
    }
    beats.push_back(t);
    downbeat.push_back(is_downbeat);
  }

  if (static_cast<int>(beats.size()) < kMinGridBeats) {
    for (int k = 0; static_cast<float>(k) * kFallbackGridSec < out.duration_sec || k == 0; ++k) {
      out.times.push_back(static_cast<float>(k) * kFallbackGridSec);
    }
  } else {
    std::vector<float> intervals;
    for (size_t i = 1; i < beats.size(); ++i) intervals.push_back(beats[i] - beats[i - 1]);
    const float delta = median_of(intervals);
    const float half = 0.5f * delta;

    // Before the first beat: columns stepping back from it, the first starting at 0.
    std::vector<float> lead;
    for (int k = 1; beats.front() - static_cast<float>(k) * delta >= half; ++k) {
      lead.push_back(beats.front() - static_cast<float>(k) * delta);
    }
    out.times.push_back(0.0f);
    for (auto it = lead.rbegin(); it != lead.rend(); ++it) out.times.push_back(*it);
    for (size_t i = 0; i < beats.size(); ++i) {
      if (i == 0 && beats[0] < half) {
        // The first beat opens the first column.
        if (downbeat[0]) out.downbeats.push_back(0);
      } else {
        if (i > 0) {
          const float gap = beats[i] - beats[i - 1];
          if (gap > 2.0f * delta) {
            const int pieces = std::max(1, static_cast<int>(std::lround(gap / delta)));
            for (int k = 1; k < pieces; ++k) {
              out.times.push_back(beats[i - 1] + gap * static_cast<float>(k) / pieces);
            }
          }
        }
        if (downbeat[i]) out.downbeats.push_back(out.size());
        out.times.push_back(beats[i]);
      }
    }
    for (int k = 1; beats.back() + static_cast<float>(k) * delta <= out.duration_sec - half; ++k) {
      out.times.push_back(beats.back() + static_cast<float>(k) * delta);
    }
  }

  // Long inputs: merge adjacent column pairs until the count fits.
  while (out.size() > kMaxColumns) {
    std::vector<float> times;
    for (int c = 0; c < out.size(); c += 2) times.push_back(out.times[static_cast<size_t>(c)]);
    std::vector<int> downbeats;
    for (int c : out.downbeats) {
      if (c % 2 == 0) downbeats.push_back(c / 2);
    }
    out.times = std::move(times);
    out.downbeats = std::move(downbeats);
  }

  std::vector<float> spacing;
  for (int c = 1; c < out.size(); ++c) {
    spacing.push_back(out.times[static_cast<size_t>(c)] - out.times[static_cast<size_t>(c - 1)]);
  }
  out.column_sec =
      spacing.empty() ? std::max(out.duration_sec, kFallbackGridSec) : median_of(spacing);
  return out;
}

ColumnMeans column_means(const FrameFeatures& frames, const ColumnGrid& grid) {
  const int n = grid.size();
  const int t_frames = frames.n_frames;
  ColumnMeans out;
  out.n_columns = n;
  out.mfcc.assign(static_cast<size_t>(n) * kFrameMfcc, 0.0f);
  out.chroma.assign(static_cast<size_t>(n) * kFrameChroma, 0.0f);
  out.audible.assign(static_cast<size_t>(n), 0);

  const auto first_frame = [&](int column) {
    if (column >= n) return t_frames;
    const float position = grid.times[static_cast<size_t>(column)] / frames.frame_sec;
    return std::clamp(static_cast<int>(std::ceil(position - 1e-3f)), 0, t_frames);
  };
  for (int c = 0; c < n && t_frames > 0; ++c) {
    int begin = first_frame(c);
    int end = first_frame(c + 1);
    if (begin >= end) {
      begin = std::min(begin, t_frames - 1);
      end = begin + 1;
    }
    double level = 0.0;
    for (int f = begin; f < end; ++f) {
      if (static_cast<size_t>(f) < frames.rms.size()) level += frames.rms[static_cast<size_t>(f)];
    }
    const float count = static_cast<float>(end - begin);
    if (!has_evidence(level / count)) continue;
    out.audible[static_cast<size_t>(c)] = 1;
    float* m = &out.mfcc[static_cast<size_t>(c) * kFrameMfcc];
    float* h = &out.chroma[static_cast<size_t>(c) * kFrameChroma];
    for (int f = begin; f < end; ++f) {
      for (int d = 0; d < kFrameMfcc; ++d) {
        m[d] += frames.mfcc[static_cast<size_t>(d) * t_frames + f];
      }
      for (int d = 0; d < kFrameChroma; ++d) {
        h[d] += frames.chroma[static_cast<size_t>(d) * t_frames + f];
      }
    }
    for (int d = 0; d < kFrameMfcc; ++d) m[d] /= count;
    normalize(h, kFrameChroma);
  }
  return out;
}

ColumnFeatures column_features(const ColumnMeans& means, FeatureSet features) {
  const int n = means.n_columns;
  std::vector<float> mfcc = means.mfcc;
  // Centre MFCC on the audible columns' mean, so what remains is how each column differs.
  std::vector<double> centre(kFrameMfcc, 0.0);
  int n_audible = 0;
  for (int c = 0; c < n; ++c) {
    if (!means.audible[static_cast<size_t>(c)]) continue;
    ++n_audible;
    for (int d = 0; d < kFrameMfcc; ++d) centre[d] += mfcc[static_cast<size_t>(c) * kFrameMfcc + d];
  }
  for (int c = 0; c < n; ++c) {
    if (!means.audible[static_cast<size_t>(c)]) continue;
    float* m = &mfcc[static_cast<size_t>(c) * kFrameMfcc];
    for (int d = 0; d < kFrameMfcc; ++d) m[d] -= static_cast<float>(centre[d] / n_audible);
    normalize(m, kFrameMfcc);
  }

  ColumnFeatures out;
  out.n_columns = n;
  const bool timbre = features != FeatureSet::kHarmonyOnly;
  const bool harmony = features != FeatureSet::kTimbreOnly;
  const float weight = timbre && harmony ? kInvSqrt2 : 1.0f;
  out.n_dims = (timbre ? kFrameMfcc : 0) + (harmony ? kFrameChroma : 0);
  out.values.assign(static_cast<size_t>(n) * out.n_dims, 0.0f);
  for (int c = 0; c < n; ++c) {
    float* dst = &out.values[static_cast<size_t>(c) * out.n_dims];
    int d = 0;
    if (timbre) {
      for (int k = 0; k < kFrameMfcc; ++k) {
        dst[d++] = weight * mfcc[static_cast<size_t>(c) * kFrameMfcc + k];
      }
    }
    if (harmony) {
      for (int k = 0; k < kFrameChroma; ++k) {
        dst[d++] = weight * means.chroma[static_cast<size_t>(c) * kFrameChroma + k];
      }
    }
  }
  return out;
}

ColumnContrast column_contrast(const ColumnMeans& means, int kernel_size) {
  const int n = means.n_columns;
  const int full_half = kernel_size / 2;
  ColumnContrast out;
  out.timbre.assign(static_cast<size_t>(n), 0.0f);
  out.harmony.assign(static_cast<size_t>(n), 0.0f);
  // Prefix sums over audible columns: count, MFCC and chroma.
  const size_t width = 1 + kFrameMfcc + kFrameChroma;
  std::vector<double> prefix((static_cast<size_t>(n) + 1) * width, 0.0);
  for (int c = 0; c < n; ++c) {
    const double* prev = &prefix[static_cast<size_t>(c) * width];
    double* next = &prefix[static_cast<size_t>(c + 1) * width];
    const bool audible = means.audible[static_cast<size_t>(c)] != 0;
    next[0] = prev[0] + (audible ? 1.0 : 0.0);
    for (int d = 0; d < kFrameMfcc; ++d) {
      next[1 + d] =
          prev[1 + d] + (audible ? means.mfcc[static_cast<size_t>(c) * kFrameMfcc + d] : 0.0);
    }
    for (int d = 0; d < kFrameChroma; ++d) {
      next[1 + kFrameMfcc + d] =
          prev[1 + kFrameMfcc + d] +
          (audible ? means.chroma[static_cast<size_t>(c) * kFrameChroma + d] : 0.0);
    }
  }
  const auto window = [&](int begin, int end, std::vector<double>& sum) {
    const double* lo = &prefix[static_cast<size_t>(begin) * width];
    const double* hi = &prefix[static_cast<size_t>(end) * width];
    for (size_t k = 0; k < width; ++k) sum[k] = hi[k] - lo[k];
    return sum[0];
  };
  std::vector<double> left(width);
  std::vector<double> right(width);
  for (int c = 0; c < n; ++c) {
    // The kernel's truncated half-width, so contrast reads the windows novelty compared.
    const int half = std::min({full_half, c, n - c});
    if (half <= 0 || 2 * half < full_half) continue;
    const double n_left = window(c - half, c, left);
    const double n_right = window(c, c + half, right);
    if (n_left <= 0.0 && n_right <= 0.0) continue;
    if (n_left <= 0.0 || n_right <= 0.0) {
      // Silence on one side only: an entry or exit, a change by any measure.
      out.timbre[static_cast<size_t>(c)] = std::numeric_limits<float>::infinity();
      out.harmony[static_cast<size_t>(c)] = std::numeric_limits<float>::infinity();
      continue;
    }
    double distance = 0.0;
    for (int d = 0; d < kFrameMfcc; ++d) {
      const double diff = left[1 + d] / n_left - right[1 + d] / n_right;
      distance += diff * diff;
    }
    out.timbre[static_cast<size_t>(c)] = static_cast<float>(std::sqrt(distance));
    double dot = 0.0;
    double norm_left = 0.0;
    double norm_right = 0.0;
    for (int d = 0; d < kFrameChroma; ++d) {
      const double l = left[1 + kFrameMfcc + d];
      const double r = right[1 + kFrameMfcc + d];
      dot += l * r;
      norm_left += l * l;
      norm_right += r * r;
    }
    const double norms = std::sqrt(norm_left * norm_right);
    out.harmony[static_cast<size_t>(c)] =
        norms > kEpsilon ? static_cast<float>(1.0 - dot / norms) : 0.0f;
  }
  return out;
}

std::vector<float> column_ssm(const ColumnFeatures& features) {
  const int n = features.n_columns;
  const int dims = features.n_dims;
  std::vector<float> ssm(static_cast<size_t>(n) * n, 0.0f);
  for (int i = 0; i < n; ++i) {
    const float* a = &features.values[static_cast<size_t>(i) * dims];
    for (int j = i; j < n; ++j) {
      const float* b = &features.values[static_cast<size_t>(j) * dims];
      float dot = 0.0f;
      for (int d = 0; d < dims; ++d) dot += a[d] * b[d];
      ssm[static_cast<size_t>(i) * n + j] = dot;
      ssm[static_cast<size_t>(j) * n + i] = dot;
    }
  }
  return ssm;
}

std::vector<float> smooth_diagonal(const std::vector<float>& ssm, int n, int length) {
  const int l = std::max(1, length);
  std::vector<float> out(static_cast<size_t>(n) * n, 0.0f);
  std::vector<double> prefix;
  // Each diagonal j - i = d independently: a running mean over the next l entries along it.
  for (int d = -(n - 1); d <= n - 1; ++d) {
    const int i0 = d < 0 ? -d : 0;
    const int j0 = d < 0 ? 0 : d;
    const int len = n - std::max(i0, j0);
    prefix.assign(static_cast<size_t>(len) + 1, 0.0);
    for (int k = 0; k < len; ++k) {
      prefix[static_cast<size_t>(k) + 1] =
          prefix[static_cast<size_t>(k)] + ssm[static_cast<size_t>(i0 + k) * n + (j0 + k)];
    }
    for (int k = 0; k < len; ++k) {
      const int stop = std::min(len, k + l);
      out[static_cast<size_t>(i0 + k) * n + (j0 + k)] = static_cast<float>(
          (prefix[static_cast<size_t>(stop)] - prefix[static_cast<size_t>(k)]) / (stop - k));
    }
  }
  return out;
}

PairScores pair_scores(const std::vector<Span>& spans, const std::vector<float>& smoothed, int n,
                       int slack) {
  PairScores out;
  const int m_count = static_cast<int>(spans.size());
  out.count = m_count;
  out.scores.assign(static_cast<size_t>(m_count) * m_count, 0.0f);
  out.offsets.assign(static_cast<size_t>(m_count) * m_count, 0);
  for (int a = 0; a < m_count; ++a) {
    out.scores[static_cast<size_t>(a) * m_count + a] = 1.0f;
    for (int b = a + 1; b < m_count; ++b) {
      const bool a_short = spans[a].length() <= spans[b].length();
      const Span& shorter = a_short ? spans[a] : spans[b];
      const Span& longer = a_short ? spans[b] : spans[a];
      const int m = shorter.length();
      const int len = longer.length();
      if (m <= 0 || static_cast<float>(m) < kMinLengthRatio * static_cast<float>(len)) continue;
      double best = -std::numeric_limits<double>::infinity();
      int best_offset = 0;
      for (int o = -slack; o <= len - m + slack; ++o) {
        const int start = longer.begin + o;
        if (start < 0 || start + m > n) continue;
        double sum = 0.0;
        for (int k = 0; k < m; ++k) {
          sum += smoothed[static_cast<size_t>(shorter.begin + k) * n + (start + k)];
        }
        if (sum / m > best) {
          best = sum / m;
          best_offset = o;
        }
      }
      if (!std::isfinite(best)) continue;
      const float score = std::clamp(static_cast<float>(best), 0.0f, 1.0f);
      out.scores[static_cast<size_t>(a) * m_count + b] = score;
      out.scores[static_cast<size_t>(b) * m_count + a] = score;
      const int shorter_index = a_short ? a : b;
      const int longer_index = a_short ? b : a;
      out.offsets[static_cast<size_t>(shorter_index) * m_count + longer_index] = best_offset;
      out.offsets[static_cast<size_t>(longer_index) * m_count + shorter_index] = -best_offset;
    }
  }
  return out;
}

std::vector<Span> join_split_sections(std::vector<Span> spans, const std::vector<float>& smoothed,
                                      const ColumnGrid& grid, float tau) {
  const int n = grid.size();
  const int slack = grid.to_columns(kAlignSlackSec);
  PairScores scores = pair_scores(spans, smoothed, n, slack);
  size_t a = 0;
  while (a + 1 < spans.size()) {
    const size_t b = a + 1;
    const float combined = span_seconds(spans[a], grid) + span_seconds(spans[b], grid);
    bool join = false;
    for (size_t x = 0; x < spans.size() && !join; ++x) {
      if (x == a || x == b) continue;
      const float ratio = span_seconds(spans[x], grid) / combined;
      if (ratio < kJoinLengthLow || ratio > kJoinLengthHigh) continue;
      const int ia = static_cast<int>(a);
      const int ib = static_cast<int>(b);
      const int ix = static_cast<int>(x);
      if (scores.score(ia, ix) < tau || scores.score(ib, ix) < tau) continue;
      const int expected = scores.offset(ia, ix) + spans[a].length();
      join = std::abs(scores.offset(ib, ix) - expected) <= slack;
    }
    if (!join) {
      ++a;
      continue;
    }
    spans[a].end = spans[b].end;
    spans.erase(spans.begin() + static_cast<std::ptrdiff_t>(b));
    scores = pair_scores(spans, smoothed, n, slack);
  }
  return spans;
}

std::vector<int> group_sections(const PairScores& scores, float tau) {
  const int m = scores.count;
  // Clusters stay ordered by their smallest member, which is their first element.
  std::vector<std::vector<int>> clusters;
  for (int i = 0; i < m; ++i) clusters.push_back({i});
  const double limit = 1.0 - static_cast<double>(tau);
  while (clusters.size() > 1) {
    double best = std::numeric_limits<double>::infinity();
    size_t best_p = 0;
    size_t best_q = 0;
    for (size_t p = 0; p < clusters.size(); ++p) {
      for (size_t q = p + 1; q < clusters.size(); ++q) {
        double distance = 0.0;
        for (int i : clusters[p]) {
          for (int j : clusters[q]) {
            distance = std::max(distance, 1.0 - static_cast<double>(scores.score(i, j)));
          }
        }
        // Scanned in (p, q) order, so a tie keeps the lexicographically smaller pair.
        if (distance < best && !tied(distance, best)) {
          best = distance;
          best_p = p;
          best_q = q;
        }
      }
    }
    if (best > limit) break;
    std::vector<int>& into = clusters[best_p];
    into.insert(into.end(), clusters[best_q].begin(), clusters[best_q].end());
    std::sort(into.begin(), into.end());
    clusters.erase(clusters.begin() + static_cast<std::ptrdiff_t>(best_q));
  }
  std::vector<int> cluster_of(static_cast<size_t>(m), 0);
  for (size_t c = 0; c < clusters.size(); ++c) {
    for (int i : clusters[c]) cluster_of[static_cast<size_t>(i)] = static_cast<int>(c);
  }
  std::vector<int> number(clusters.size(), -1);
  std::vector<int> groups(static_cast<size_t>(m), 0);
  int next = 0;
  for (int i = 0; i < m; ++i) {
    int& g = number[static_cast<size_t>(cluster_of[static_cast<size_t>(i)])];
    if (g < 0) g = next++;
    groups[static_cast<size_t>(i)] = g;
  }
  return groups;
}

void merge_adjacent(std::vector<Span>& spans, std::vector<int>& groups, const ColumnGrid& grid) {
  size_t i = 0;
  while (i + 1 < spans.size()) {
    const int g = groups[i];
    if (groups[i + 1] != g) {
      ++i;
      continue;
    }
    float longest_other = -1.0f;
    for (size_t k = 0; k < spans.size(); ++k) {
      if (k != i && k != i + 1 && groups[k] == g) {
        longest_other = std::max(longest_other, span_seconds(spans[k], grid));
      }
    }
    const float merged = grid.time(spans[i + 1].end) - grid.time(spans[i].begin);
    const float allowed = kMergeLengthRatio * longest_other;
    if (longest_other < 0.0f || (merged > allowed && !tied(merged, allowed))) {
      ++i;
      continue;
    }
    spans[i].end = spans[i + 1].end;
    spans.erase(spans.begin() + static_cast<std::ptrdiff_t>(i) + 1);
    groups.erase(groups.begin() + static_cast<std::ptrdiff_t>(i) + 1);
  }
}

Repetition resolve_repetition(std::vector<Span> spans, const std::vector<float>& smoothed,
                              const ColumnGrid& grid, float tau) {
  const int n = grid.size();
  const int slack = grid.to_columns(kAlignSlackSec);
  Repetition out;
  out.spans = join_split_sections(std::move(spans), smoothed, grid, tau);
  out.groups = group_sections(pair_scores(out.spans, smoothed, n, slack), tau);
  merge_adjacent(out.spans, out.groups, grid);
  // Every section one material: nothing tells them apart, so the track is one section.
  if (out.spans.size() >= 2 &&
      std::all_of(out.groups.begin(), out.groups.end(), [](int g) { return g == 0; })) {
    out.spans = {Span{0, n}};
    out.groups = {0};
  }
  out.scores = pair_scores(out.spans, smoothed, n, slack);
  return out;
}

std::vector<Boundary> merge_short_sections(const std::vector<Boundary>& boundaries,
                                           float duration_sec, float min_section_sec,
                                           float column_sec) {
  std::vector<Boundary> sorted = boundaries;
  std::sort(sorted.begin(), sorted.end(),
            [](const Boundary& a, const Boundary& b) { return a.time < b.time; });
  const float minimum = std::max(0.0f, min_section_sec);
  const size_t n = sorted.size() + 1;  // sections
  if (minimum <= 0.0f || n < 2) return sorted;
  const float reach = minimum - 0.5f * std::max(0.0f, column_sec);

  // Keep the boundary subset with the largest total strength (then the most boundaries) whose
  // sections all reach the floor. Cut k (1..n-1) is boundary k-1; cut 0 and cut n are the ends.
  struct Score {
    double strength = 0.0;
    size_t count = 0;
    bool feasible = false;
    bool operator<(const Score& o) const {
      return strength < o.strength || (strength == o.strength && count < o.count);
    }
  };
  const auto cut_time = [&](size_t k) {
    if (k == 0) return 0.0f;
    return k < n ? sorted[k - 1].time : duration_sec;
  };
  std::vector<Score> best(n + 1);
  std::vector<size_t> previous(n + 1, 0);
  best[0].feasible = true;
  for (size_t j = 1; j <= n; ++j) {
    for (size_t i = 0; i < j; ++i) {
      if (!best[i].feasible || cut_time(j) - cut_time(i) < reach) continue;
      Score candidate = best[i];
      if (j < n) {
        candidate.strength += static_cast<double>(sorted[j - 1].strength);
        ++candidate.count;
      }
      if (!best[j].feasible || best[j] < candidate) {
        best[j] = candidate;
        best[j].feasible = true;
        previous[j] = i;
      }
    }
  }
  // Shorter than the floor as a whole: one whole-track section.
  std::vector<Boundary> kept;
  if (best[n].feasible) {
    for (size_t k = previous[n]; k > 0; k = previous[k]) kept.push_back(sorted[k - 1]);
  }
  std::reverse(kept.begin(), kept.end());
  return kept;
}

std::vector<Boundary> snap_to_downbeats(const std::vector<Boundary>& boundaries,
                                        const ColumnGrid& grid) {
  std::vector<Boundary> moved = boundaries;
  if (grid.downbeats_reliable && !grid.downbeats.empty()) {
    for (Boundary& b : moved) {
      int nearest = -1;
      for (int d : grid.downbeats) {
        const int distance = std::abs(d - b.frame);
        if (distance <= 1 && (nearest < 0 || distance < std::abs(nearest - b.frame))) nearest = d;
      }
      if (nearest >= 0) {
        b.frame = nearest;
        b.time = grid.time(nearest);
      }
    }
  }
  std::sort(moved.begin(), moved.end(),
            [](const Boundary& a, const Boundary& b) { return a.frame < b.frame; });
  std::vector<Boundary> out;
  for (const Boundary& b : moved) {
    if (!out.empty() && out.back().frame == b.frame) {
      out.back().strength = std::max(out.back().strength, b.strength);
    } else {
      out.push_back(b);
    }
  }
  return out;
}

std::vector<Label> label_sections(const std::vector<LabelInput>& sections,
                                  const PairScores& scores) {
  const int m = static_cast<int>(sections.size());
  std::vector<Label> labels(static_cast<size_t>(m));
  if (m == 0) return labels;

  int n_groups = 0;
  for (const LabelInput& s : sections) n_groups = std::max(n_groups, s.group + 1);
  std::vector<int> count(static_cast<size_t>(n_groups), 0);
  std::vector<int> first(static_cast<size_t>(n_groups), m);
  std::vector<double> energy(static_cast<size_t>(n_groups), 0.0);
  std::vector<double> coverage(static_cast<size_t>(n_groups), 0.0);
  std::vector<float> energies;
  std::vector<float> lengths;
  for (int i = 0; i < m; ++i) {
    const LabelInput& s = sections[static_cast<size_t>(i)];
    const size_t g = static_cast<size_t>(s.group);
    ++count[g];
    first[g] = std::min(first[g], i);
    energy[g] += s.energy;
    coverage[g] += s.length_sec;
    energies.push_back(s.energy);
    lengths.push_back(s.length_sec);
  }

  // Rule 1: the repeating group with the most occurrences x mean energy is the Chorus.
  int chorus = -1;
  double chorus_fitness = 0.0;
  for (int g = 0; g < n_groups; ++g) {
    if (count[static_cast<size_t>(g)] < 2) continue;
    const double fitness = energy[static_cast<size_t>(g)];  // count x mean energy
    bool take = chorus < 0 || greater(fitness, chorus_fitness);
    if (!take && tied(fitness, chorus_fitness)) {
      const double cov = coverage[static_cast<size_t>(g)];
      const double best_cov = coverage[static_cast<size_t>(chorus)];
      take = greater(cov, best_cov) ||
             (tied(cov, best_cov) &&
              first[static_cast<size_t>(g)] < first[static_cast<size_t>(chorus)]);
    }
    if (take) {
      chorus = g;
      chorus_fitness = fitness;
    }
  }
  // Rule 2: of the other repeating groups, the one covering most time is the Verse.
  int verse = -1;
  for (int g = 0; g < n_groups; ++g) {
    if (g == chorus || count[static_cast<size_t>(g)] < 2) continue;
    const double cov = coverage[static_cast<size_t>(g)];
    if (verse < 0 || greater(cov, coverage[static_cast<size_t>(verse)]) ||
        (tied(cov, coverage[static_cast<size_t>(verse)]) &&
         first[static_cast<size_t>(g)] < first[static_cast<size_t>(verse)])) {
      verse = g;
    }
  }
  std::vector<SectionType> role(static_cast<size_t>(n_groups), SectionType::Unknown);
  if (chorus >= 0) role[static_cast<size_t>(chorus)] = SectionType::Chorus;
  if (verse >= 0) role[static_cast<size_t>(verse)] = SectionType::Verse;
  // Rule 3: every other repeating group that always leads into the Chorus and is shorter.
  const double chorus_length =
      chorus >= 0 ? coverage[static_cast<size_t>(chorus)] / count[static_cast<size_t>(chorus)]
                  : 0.0;
  for (int g = 0; g < n_groups && chorus >= 0; ++g) {
    if (g == chorus || g == verse || count[static_cast<size_t>(g)] < 2) continue;
    bool leads = true;
    for (int i = 0; i < m && leads; ++i) {
      if (sections[static_cast<size_t>(i)].group != g) continue;
      leads = i + 1 < m && sections[static_cast<size_t>(i) + 1].group == chorus;
    }
    const double length = coverage[static_cast<size_t>(g)] / count[static_cast<size_t>(g)];
    if (leads && length < chorus_length && !tied(length, chorus_length)) {
      role[static_cast<size_t>(g)] = SectionType::PreChorus;
    }
  }
  const auto in_role = [&](int i) {
    return role[static_cast<size_t>(sections[static_cast<size_t>(i)].group)] !=
           SectionType::Unknown;
  };

  std::vector<float> role_confidence(static_cast<size_t>(n_groups), 0.0f);
  for (int g = 0; g < n_groups; ++g) {
    double sum = 0.0;
    int pairs = 0;
    for (int i = 0; i < m; ++i) {
      for (int j = i + 1; j < m; ++j) {
        if (sections[static_cast<size_t>(i)].group == g &&
            sections[static_cast<size_t>(j)].group == g) {
          sum += scores.score(i, j);
          ++pairs;
        }
      }
    }
    const float mean = pairs > 0 ? static_cast<float>(sum / pairs) : 0.0f;
    role_confidence[static_cast<size_t>(g)] =
        std::clamp(kGroupConfidenceBase + kGroupConfidenceScale * mean, 0.0f, 1.0f);
  }
  std::vector<bool> labelled(static_cast<size_t>(m), false);
  for (int i = 0; i < m; ++i) {
    if (!in_role(i)) continue;
    const int g = sections[static_cast<size_t>(i)].group;
    labels[static_cast<size_t>(i)] = {role[static_cast<size_t>(g)],
                                      role_confidence[static_cast<size_t>(g)]};
    labelled[static_cast<size_t>(i)] = true;
  }

  // Rules 4 and 5: the first and last sections.
  const float reference = low_energy_reference(energies);
  const float edge_length =
      kEdgeLengthRatio * (chorus >= 0 ? static_cast<float>(chorus_length) : median_of(lengths));
  std::vector<int> edge_sections;  // Intro/Outro taken through the edge condition
  const auto edge = [&](int i, SectionType type) {
    const LabelInput& s = sections[static_cast<size_t>(i)];
    const float quiet_level = has_evidence(reference) ? s.energy / reference : 1.0f;
    const bool quiet = quiet_level < kLowEnergyThreshold;
    const bool short_edge = s.length_sec <= edge_length || tied(s.length_sec, edge_length);
    if (!in_role(i) && (quiet || short_edge)) {
      const float confidence =
          quiet ? kEdgeConfidence + kQuietConfidenceScale * (1.0f - quiet_level) : kEdgeConfidence;
      labels[static_cast<size_t>(i)] = {type, confidence};
      labelled[static_cast<size_t>(i)] = true;
      edge_sections.push_back(i);
    } else if (in_role(i) && s.vocal_likelihood < kInstrumentalVocalThreshold) {
      labels[static_cast<size_t>(i)] = {type, kEdgeConfidence};
      labelled[static_cast<size_t>(i)] = true;
    }
  };
  edge(0, SectionType::Intro);
  if (m > 1) edge(m - 1, SectionType::Outro);

  // Rule 6: Instrumental, by sharing the edge material or by lacking vocals beside a repeat.
  for (int i = 0; i < m; ++i) {
    if (labelled[static_cast<size_t>(i)]) continue;
    const LabelInput& s = sections[static_cast<size_t>(i)];
    float best = -1.0f;
    for (int e : edge_sections) {
      if (sections[static_cast<size_t>(e)].group == s.group)
        best = std::max(best, scores.score(i, e));
    }
    if (best >= 0.0f) {
      labels[static_cast<size_t>(i)] = {
          SectionType::Instrumental,
          std::clamp(kInstrumentalGroupBase + kInstrumentalGroupScale * best, 0.0f, 1.0f)};
      labelled[static_cast<size_t>(i)] = true;
      continue;
    }
    const bool beside_repeat = (i > 0 && in_role(i - 1)) || (i + 1 < m && in_role(i + 1));
    if (s.vocal_likelihood < kInstrumentalVocalThreshold && beside_repeat) {
      labels[static_cast<size_t>(i)] = {
          SectionType::Instrumental,
          kVocalConfidenceBase + kVocalConfidenceScale * (1.0f - s.vocal_likelihood)};
      labelled[static_cast<size_t>(i)] = true;
    }
  }

  // Rule 7: Bridge, between two Chorus occurrences.
  for (int i = 0; i < m && chorus >= 0; ++i) {
    if (labelled[static_cast<size_t>(i)]) continue;
    bool before = false;
    bool after = false;
    for (int k = 0; k < m; ++k) {
      if (sections[static_cast<size_t>(k)].group != chorus) continue;
      before = before || k < i;
      after = after || k > i;
    }
    if (before && after) {
      labels[static_cast<size_t>(i)] = {
          SectionType::Bridge,
          kVocalConfidenceBase +
              kVocalConfidenceScale * sections[static_cast<size_t>(i)].vocal_likelihood};
      labelled[static_cast<size_t>(i)] = true;
    }
  }
  // Rule 8: anything left stays Unknown with confidence 0.
  return labels;
}

StructureResult analyze_structure(const FrameFeatures& frames, const SectionGrid& grid,
                                  const SectionConfig& config, FeatureSet features) {
  StructureResult result;
  const ColumnGrid columns = column_grid(grid, frames.duration_sec, frames.frame_sec);
  const ColumnMeans means = column_means(frames, columns);
  const ColumnFeatures matrix = column_features(means, features);
  const int n = columns.size();
  const float min_section = std::max(0.0f, config.min_section_sec);

  BoundaryConfig boundary_config;
  boundary_config.threshold = config.boundary_threshold;
  boundary_config.kernel_size =
      2 * std::max(2, columns.to_columns(kKernelHalfPerSection * min_section));
  // Peak spacing only thins candidates; the section floor is enforced by the length merge.
  boundary_config.peak_distance = std::min(min_section, BoundaryConfig{}.peak_distance);
  const ColumnContrast contrast = column_contrast(means, boundary_config.kernel_size);
  std::vector<uint8_t> eligible(static_cast<size_t>(n), 0);
  for (int c = 0; c < n; ++c) {
    eligible[static_cast<size_t>(c)] =
        contrast.timbre[static_cast<size_t>(c)] >= kMinTimbreContrast ||
        contrast.harmony[static_cast<size_t>(c)] >= kMinHarmonyContrast;
  }
  const BoundaryDetector detector(matrix.values, matrix.n_dims, columns.times, std::move(eligible),
                                  boundary_config);
  const std::vector<Boundary> snapped = snap_to_downbeats(detector.boundaries(), columns);
  for (const Boundary& b : snapped) result.boundary_times.push_back(b.time);

  std::vector<Span> spans;
  int begin = 0;
  for (const Boundary& b :
       merge_short_sections(snapped, frames.duration_sec, min_section, columns.column_sec)) {
    spans.push_back({begin, b.frame});
    begin = b.frame;
  }
  spans.push_back({begin, n});

  const std::vector<float> smoothed =
      smooth_diagonal(column_ssm(matrix), n, std::max(1, columns.to_columns(min_section)));
  const Repetition repetition = resolve_repetition(std::move(spans), smoothed, columns);

  std::vector<LabelInput> inputs;
  float loudest = 0.0f;
  for (size_t i = 0; i < repetition.spans.size(); ++i) {
    const Span& span = repetition.spans[i];
    Section section;
    section.start = columns.time(span.begin);
    section.end = columns.time(span.end);
    section.group = repetition.groups[i];
    LabelInput input;
    input.group = section.group;
    input.length_sec = section.end - section.start;
    input.energy = section_energy(frames, section.start, section.end);
    input.vocal_likelihood = section_vocal_likelihood(frames, section.start, section.end);
    section.energy_level = input.energy;
    loudest = std::max(loudest, input.energy);
    inputs.push_back(input);
    result.sections.push_back(section);
  }

  // A track with no audible section carries nothing to classify.
  const bool audible = has_evidence(loudest);
  const std::vector<Label> labels =
      audible ? label_sections(inputs, repetition.scores) : std::vector<Label>(inputs.size());
  for (size_t i = 0; i < result.sections.size(); ++i) {
    Section& section = result.sections[i];
    section.energy_level = audible ? section.energy_level / loudest : 0.0f;
    section.type = labels[i].type;
    section.confidence = labels[i].confidence;
  }
  result.pair_scores = repetition.scores.scores;
  return result;
}

}  // namespace section_structure
}  // namespace sonare
