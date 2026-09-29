#include "analysis/melody_analyzer.h"

#include <algorithm>
#include <cmath>

#include "feature/pitch.h"
#include "util/constants.h"
#include "util/exception.h"

namespace sonare {

using sonare::constants::kCentsPerOctave;

namespace {

/// How far the pitch must travel back from its last extreme, in cents, before
/// the reversal counts as half a vibrato cycle. Well under the ~20 cents peak to
/// peak of a shallow vibrato and above the few cents a tracker jitters by.
constexpr float kVibratoReversalCents = 10.0f;

}  // namespace

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
  for (const auto& p : contour_.pitches) {
    if (p.frequency > 0.0f) {
      voiced_frequencies.push_back(p.frequency);
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

  // Estimate vibrato rate from direction reversals of the pitch contour, two per
  // cycle. A reversal counts only once the pitch has travelled
  // kVibratoReversalCents back from its last extreme, so tracker jitter on a held
  // note is not an oscillation. Each continuous voiced run is handled
  // independently so unvoiced gaps neither create a spurious cross-gap movement
  // nor dilute the denominator.
  if (voiced_frequencies.size() >= 8) {
    int zero_crossings = 0;
    float voiced_run_duration = 0.0f;
    bool in_run = false;
    float run_start_time = 0.0f;
    float last_voiced_time = 0.0f;
    float extreme_cents = 0.0f;
    float low_cents = 0.0f;
    float high_cents = 0.0f;
    int direction = 0;  // +1 rising, -1 falling, 0 not yet moved far enough
    size_t run_frames = 0;

    const auto finish_run = [&]() {
      if (in_run && run_frames >= 2) {
        voiced_run_duration += std::max(0.0f, last_voiced_time - run_start_time);
      }
      in_run = false;
      direction = 0;
      run_frames = 0;
    };

    for (const auto& point : contour_.pitches) {
      if (point.frequency <= 0.0f) {
        finish_run();
        continue;
      }

      const float cents = kCentsPerOctave * std::log2(point.frequency);
      if (!in_run) {
        in_run = true;
        run_start_time = point.time;
        last_voiced_time = point.time;
        low_cents = high_cents = cents;
        run_frames = 1;
        continue;
      }

      if (direction == 0) {
        low_cents = std::min(low_cents, cents);
        high_cents = std::max(high_cents, cents);
        if (cents - low_cents >= kVibratoReversalCents) {
          direction = 1;
          extreme_cents = cents;
        } else if (high_cents - cents >= kVibratoReversalCents) {
          direction = -1;
          extreme_cents = cents;
        }
      } else if (direction > 0) {
        if (cents > extreme_cents) {
          extreme_cents = cents;
        } else if (extreme_cents - cents >= kVibratoReversalCents) {
          ++zero_crossings;
          direction = -1;
          extreme_cents = cents;
        }
      } else if (cents < extreme_cents) {
        extreme_cents = cents;
      } else if (cents - extreme_cents >= kVibratoReversalCents) {
        ++zero_crossings;
        direction = 1;
        extreme_cents = cents;
      }
      last_voiced_time = point.time;
      ++run_frames;
    }
    finish_run();

    if (voiced_run_duration > 0.0f) {
      contour_.vibrato_rate = static_cast<float>(zero_crossings) / (2.0f * voiced_run_duration);
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
