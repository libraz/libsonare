#include "analysis/melody_analyzer.h"

#include <algorithm>
#include <cmath>

#include "feature/pitch.h"
#include "util/exception.h"

namespace sonare {

MelodyAnalyzer::MelodyAnalyzer(const Audio& audio, const MelodyConfig& config)
    : config_(config), sr_(audio.sample_rate()) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);

  if (config.use_pyin) {
    // pYIN path: Viterbi-smoothed contour with (optional) frame centering, so
    // the contour and timestamps match librosa.pyin(center=...). Frame i is
    // centered at i*hop_length when center=true.
    PitchConfig pc;
    pc.frame_length = config.frame_length;
    pc.hop_length = config.hop_length;
    pc.fmin = config.fmin;
    pc.fmax = config.fmax;
    pc.threshold = config.threshold;
    pc.fill_na = true;  // emit 0 (unvoiced) instead of NaN so downstream stays finite
    pc.center = config.center;
    PitchResult res = pyin(audio, pc);

    const float inv_sr = 1.0f / sr_;
    for (int i = 0; i < res.n_frames(); ++i) {
      float frequency = res.f0[i];
      if (!std::isfinite(frequency) || frequency <= 0.0f) frequency = 0.0f;

      PitchPoint point;
      // librosa.pyin(center=True) timestamps frame i at i*hop_length (the pad
      // shifts each window so its center lands there). The left-aligned plain
      // YIN path below instead uses start/sr.
      point.time = static_cast<float>(i) * config.hop_length * inv_sr;
      point.frequency = frequency;
      point.confidence = (frequency > 0.0f) ? res.voiced_prob[i] : 0.0f;
      contour_.pitches.push_back(point);
    }

    compute_contour_features();
    return;
  }

  // Plain-YIN path (default). Detect pitch frame by frame using the FFT-based
  // YIN in feature/pitch. Confidence is derived from the CMNDF minimum
  // (1 - d'(tau*)) inside yin_with_confidence. NOTE: no Viterbi smoothing and
  // no frame centering — frame i is LEFT-aligned (time = start/sr), diverging
  // from librosa.pyin(center=True). See MelodyConfig::use_pyin.
  const float* samples = audio.data();
  size_t n_samples = audio.size();

  int frame_size = config.frame_length;
  int hop = config.hop_length;

  for (size_t start = 0; start + frame_size <= n_samples; start += hop) {
    float time = static_cast<float>(start) / sr_;

    float confidence = 0.0f;
    float frequency = yin_with_confidence(samples + start, frame_size, sr_, config.fmin,
                                          config.fmax, config.threshold, &confidence);

    PitchPoint point;
    point.time = time;
    point.frequency = frequency;
    point.confidence = (frequency > 0.0f) ? confidence : 0.0f;

    contour_.pitches.push_back(point);
  }

  compute_contour_features();
}

void MelodyAnalyzer::compute_contour_features() {
  if (contour_.pitches.empty()) {
    return;
  }

  // Filter out unvoiced frames
  std::vector<float> voiced_frequencies;
  std::vector<float> voiced_times;
  for (const auto& p : contour_.pitches) {
    if (p.frequency > 0.0f) {
      voiced_frequencies.push_back(p.frequency);
      voiced_times.push_back(p.time);
    }
  }

  if (voiced_frequencies.empty()) {
    return;
  }

  // Mean pitch is perceptual/logarithmic, so average in log-frequency space
  // and convert back to Hz.
  float sum_log = 0.0f;
  for (float f : voiced_frequencies) {
    sum_log += std::log(f);
  }
  contour_.mean_frequency = std::exp(sum_log / voiced_frequencies.size());

  // Pitch range (in octaves)
  float min_freq = *std::min_element(voiced_frequencies.begin(), voiced_frequencies.end());
  float max_freq = *std::max_element(voiced_frequencies.begin(), voiced_frequencies.end());

  if (min_freq > 0.0f) {
    contour_.pitch_range_octaves = std::log2(max_freq / min_freq);
  }

  // Pitch stability (based on pitch variance)
  float mean_log = 0.0f;
  for (float f : voiced_frequencies) {
    mean_log += std::log2(f);
  }
  mean_log /= voiced_frequencies.size();

  float var_log = 0.0f;
  for (float f : voiced_frequencies) {
    float diff = std::log2(f) - mean_log;
    var_log += diff * diff;
  }
  var_log /= voiced_frequencies.size();

  // Convert variance to stability (lower variance = higher stability)
  // Typical variance range is 0 to 1 octave^2
  contour_.pitch_stability = std::max(0.0f, 1.0f - std::sqrt(var_log));

  // Estimate vibrato rate (simplified)
  // Look for periodic variations in pitch
  if (voiced_frequencies.size() >= 8) {
    // Compute pitch differences
    std::vector<float> pitch_diff;
    for (size_t i = 1; i < voiced_frequencies.size(); ++i) {
      pitch_diff.push_back(voiced_frequencies[i] - voiced_frequencies[i - 1]);
    }

    // Count zero crossings in pitch difference (rough vibrato estimate)
    int zero_crossings = 0;
    for (size_t i = 1; i < pitch_diff.size(); ++i) {
      if ((pitch_diff[i] >= 0.0f && pitch_diff[i - 1] < 0.0f) ||
          (pitch_diff[i] < 0.0f && pitch_diff[i - 1] >= 0.0f)) {
        zero_crossings++;
      }
    }

    // Convert to vibrato rate (Hz). The numerator counts zero crossings over the
    // voiced subset, so the denominator must span the same voiced frames (not the
    // full timeline including unvoiced gaps) to keep the ratio consistent.
    float duration = voiced_times.back() - voiced_times.front();
    if (duration > 0.0f) {
      contour_.vibrato_rate = static_cast<float>(zero_crossings) / (2.0f * duration);
    }
  }
}

std::vector<float> MelodyAnalyzer::pitch_times() const {
  std::vector<float> times;
  times.reserve(contour_.pitches.size());
  for (const auto& p : contour_.pitches) {
    times.push_back(p.time);
  }
  return times;
}

std::vector<float> MelodyAnalyzer::pitch_frequencies() const {
  std::vector<float> frequencies;
  frequencies.reserve(contour_.pitches.size());
  for (const auto& p : contour_.pitches) {
    frequencies.push_back(p.frequency);
  }
  return frequencies;
}

std::vector<float> MelodyAnalyzer::pitch_confidences() const {
  std::vector<float> confidences;
  confidences.reserve(contour_.pitches.size());
  for (const auto& p : contour_.pitches) {
    confidences.push_back(p.confidence);
  }
  return confidences;
}

}  // namespace sonare
