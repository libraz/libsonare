#include "streaming/stream_analyzer_utils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

#include "analysis/chord_templates.h"
#include "core/fft.h"
#include "filters/mel.h"
#include "util/constants.h"
#include "util/dsp_primitives.h"
#include "util/frequency_bins.h"
#include "util/math_utils.h"
#include "util/non_finite_sample.h"

namespace sonare::streaming_detail {

using sonare::constants::kEpsilon;

ChromaFilterConfig make_chroma_config(float tuning_ref_hz) {
  ChromaFilterConfig config;
  config.n_chroma = 12;
  config.tuning = constants::kSemitonesPerOctave * std::log2(tuning_ref_hz / constants::kA4Hz);
  return config;
}

std::vector<float> compute_bin_frequencies(int n_bins, int sr, int n_fft) {
  return util::bin_frequencies(n_bins, sr, n_fft);
}

float compute_centroid_frame(const float* magnitude, int n_bins, const float* frequencies) {
  float sum_weighted = 0.0f;
  float sum_mag = 0.0f;
  for (int k = 0; k < n_bins; ++k) {
    sum_weighted += frequencies[k] * magnitude[k];
    sum_mag += magnitude[k];
  }
  return sum_mag > kEpsilon ? sum_weighted / sum_mag : 0.0f;
}

float compute_flatness_frame(const float* magnitude, int n_bins) {
  /// Match the batch feature::spectral_flatness definition exactly so the
  /// streaming value does not drift from the offline one:
  ///   * operate on POWER (magnitude^2), not magnitude,
  ///   * floor power at amin (= kEpsilon) over EVERY bin (not just nonzero ones),
  ///   * normalize the geometric/arithmetic means by the full n_bins,
  ///   * return 0 for silent frames (max raw power <= amin), like librosa,
  ///     instead of the maximally-flat 1.0 the floored ratio would yield.
  if (n_bins <= 0) {
    return 0.0f;
  }

  constexpr float kAmin = kEpsilon;

  float raw_max_power = 0.0f;
  float sum_linear = 0.0f;
  float sum_log = 0.0f;
  for (int k = 0; k < n_bins; ++k) {
    const float raw_power = magnitude[k] * magnitude[k];
    raw_max_power = std::max(raw_max_power, raw_power);
    const float power = std::max(raw_power, kAmin);
    sum_linear += power;
    sum_log += std::log(power);
  }

  /// Silent frame: entire spectrum at/below the floor.
  if (raw_max_power <= kAmin) {
    return 0.0f;
  }

  const float n = static_cast<float>(n_bins);
  const float geometric_mean = std::exp(sum_log / n);
  const float arithmetic_mean = sum_linear / n;
  return geometric_mean / arithmetic_mean;
}

float compute_rms_frame(const float* samples, int n_fft) {
  return rms(samples, static_cast<size_t>(std::max(n_fft, 0)));
}

void compute_autocorrelation_streaming(const float* signal, int signal_size, int max_lag,
                                       std::vector<float>& autocorr) {
  autocorr.resize(static_cast<size_t>(std::max(max_lag, 0)));
  std::fill(autocorr.begin(), autocorr.end(), 0.0f);
  if (signal == nullptr || signal_size <= 0 || max_lag <= 0) {
    return;
  }

  double mean_value = 0.0;
  for (int i = 0; i < signal_size; ++i) {
    mean_value += signal[i];
  }
  mean_value /= static_cast<double>(signal_size);

  double variance = 0.0;
  for (int i = 0; i < signal_size; ++i) {
    const double centered = static_cast<double>(signal[i]) - mean_value;
    variance += centered * centered;
  }
  if (variance < constants::kEpsilon) {
    return;
  }

  // max_lag is small for the supported BPM range (~86 at 44.1 kHz/512), so a
  // direct bounded correlation is cheaper than constructing an FFT and, most
  // importantly for the callback, uses only the prepared output vector.
  for (int lag = 0; lag < max_lag; ++lag) {
    double sum = 0.0;
    for (int i = 0; i + lag < signal_size; ++i) {
      sum += (static_cast<double>(signal[i]) - mean_value) *
             (static_cast<double>(signal[i + lag]) - mean_value);
    }
    autocorr[static_cast<size_t>(lag)] = static_cast<float>(sum / variance);
  }
}

TempoEstimate find_best_tempo(const std::vector<float>& autocorr, int sr, int hop_length,
                              float bpm_min, float bpm_max) {
  // The batch estimator's selection on fixed storage: the strongest in-range peaks are the
  // candidates, as its top histogram bins are, and every other peak is evidence.
  constexpr size_t kMaxEvidence = 512;
  std::array<BpmHistogramBin, bpm_constants::kTopBins> candidates{};
  std::array<BpmHistogramBin, kMaxEvidence> evidence{};
  size_t n_candidates = 0;
  size_t n_evidence = 0;
  int candidate_count = 0;
  int max_votes = 0;
  // A full store replaces its weakest entry, so what is kept never depends on lag order.
  const auto keep = [](auto& store, size_t& n, const BpmHistogramBin& bin) {
    if (n < store.size()) {
      store[n++] = bin;
      return;
    }
    auto weakest = std::min_element(store.begin(), store.end(),
                                    [](const auto& a, const auto& b) { return a.votes < b.votes; });
    if (bin.votes > weakest->votes) *weakest = bin;
  };
  for_each_tempo_peak(autocorr.data(), static_cast<int>(autocorr.size()), sr, hop_length, bpm_min,
                      bpm_max, [&](const TempoPeak& peak) {
                        const BpmHistogramBin bin{peak.bpm, peak.votes};
                        if (peak.in_range) {
                          ++candidate_count;
                          max_votes = std::max(max_votes, peak.votes);
                          keep(candidates, n_candidates, bin);
                        } else {
                          keep(evidence, n_evidence, bin);
                        }
                      });

  /// No in-range peak: report "unknown" via BPM 0 with confidence 0 so the
  /// caller can detect the absence of an estimate rather than a plausible 120.
  if (max_votes <= 0) {
    return {0.0f, 0.0f, candidate_count};
  }
  std::sort(candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(n_candidates),
            [](const BpmHistogramBin& a, const BpmHistogramBin& b) { return a.votes > b.votes; });
  const BeatTempoChoice choice =
      choose_beat_tempo(candidates.data(), n_candidates, evidence.data(), n_evidence);
  if (choice.bpm <= 0.0f) {
    return {0.0f, 0.0f, candidate_count};
  }
  const float confidence = static_cast<float>(choice.votes) / static_cast<float>(max_votes);
  return {choice.bpm, confidence, candidate_count};
}

namespace {

/// Maps @p value onto [0, 1] for a quantizer. A degenerate range and a
/// non-finite value both land on the floor: the bounds are caller-supplied and
/// unvalidated, so an equal pair divides by zero, and a min/max pair yields its
/// constant argument against a NaN -- which would encode the maximum code for a
/// value the analyzer never computed.
float normalize_for_quantizer(float value, float min_val, float max_val) {
  if (!(max_val > min_val)) return 0.0f;
  float normalized = (value - min_val) / (max_val - min_val);
  (void)resolve_non_finite(SampleDestination::kIrreversibleOutput, normalized);
  return std::clamp(normalized, 0.0f, 1.0f);
}

}  // namespace

uint8_t quantize_to_u8(float value, float min_val, float max_val) {
  const float normalized = normalize_for_quantizer(value, min_val, max_val);
  return static_cast<uint8_t>(normalized * 255.0f + 0.5f);
}

int16_t quantize_to_i16(float value, float min_val, float max_val) {
  const float normalized = normalize_for_quantizer(value, min_val, max_val);
  // Round-to-nearest and clamp so the endpoints map symmetrically:
  // normalized 0 -> -32768, normalized 1 -> 32767. The previous
  // truncating cast turned -32767.5 into -32767, so 0 never reached -32768.
  long q = std::lround(normalized * 65535.0f - 32768.0f);
  q = std::max<long>(-32768, std::min<long>(32767, q));
  return static_cast<int16_t>(q);
}

float dequantize_from_u8(uint8_t quantized, float min_val, float max_val) {
  return min_val + (static_cast<float>(quantized) / 255.0f) * (max_val - min_val);
}

float dequantize_from_i16(int16_t quantized, float min_val, float max_val) {
  return min_val + ((static_cast<float>(quantized) + 32768.0f) / 65535.0f) * (max_val - min_val);
}

float single_power_to_db(float power_val, float ref, float amin) {
  float result;
  power_to_db(&power_val, 1, ref, amin, -1.0f, &result);
  return result;
}

int count_shared_notes(int root1, int quality1, int root2, int quality2) {
  /// Both note sets come from the same interval table the chord templates are
  /// generated from, so every quality the detector can report is spelled the way
  /// it was matched. Deriving a triad here instead (minor third for quality 1, a
  /// major third and a perfect fifth for everything else) made a diminished
  /// chord read as its parallel major and share all three notes with it.
  const uint16_t mask1 = chord_pitch_class_mask(root1, quality1);
  const uint16_t mask2 = chord_pitch_class_mask(root2, quality2);

  uint16_t shared_mask = static_cast<uint16_t>(mask1 & mask2);
  int shared = 0;
  while (shared_mask != 0) {
    shared_mask = static_cast<uint16_t>(shared_mask & (shared_mask - 1));
    ++shared;
  }
  return shared;
}

bool are_chords_confusable(int root1, int quality1, int root2, int quality2) {
  return count_shared_notes(root1, quality1, root2, quality2) >= 2;
}

std::array<float, 12> compute_median_chroma(const std::vector<std::array<float, 12>>& history,
                                            size_t start, size_t count,
                                            std::array<float, 12>& scratch) {
  std::array<float, 12> result = {};
  if (history.empty() || count == 0) {
    return result;
  }

  const size_t n_frames = std::min(count, scratch.size());

  for (int c = 0; c < 12; ++c) {
    for (size_t f = 0; f < n_frames; ++f) {
      scratch[f] = history[(start + f) % history.size()][c];
    }

    std::sort(scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t>(n_frames));

    if (n_frames % 2 == 0) {
      result[c] = (scratch[n_frames / 2 - 1] + scratch[n_frames / 2]) * 0.5f;
    } else {
      result[c] = scratch[n_frames / 2];
    }
  }

  return result;
}

}  // namespace sonare::streaming_detail
