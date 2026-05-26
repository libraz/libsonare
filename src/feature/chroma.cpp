#include "feature/chroma.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/convert.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/vector_normalize.h"

namespace sonare {

namespace {

std::vector<float> normalize_columns(std::vector<float> features, int n_chroma, int n_frames,
                                     int norm) {
  for (int t = 0; t < n_frames; ++t) {
    float norm_val = 0.0f;

    if (norm == 0) {
      for (int c = 0; c < n_chroma; ++c) {
        norm_val = std::max(norm_val, std::abs(features[c * n_frames + t]));
      }
    } else if (norm == 1) {
      for (int c = 0; c < n_chroma; ++c) {
        norm_val += std::abs(features[c * n_frames + t]);
      }
    } else {
      for (int c = 0; c < n_chroma; ++c) {
        float val = features[c * n_frames + t];
        norm_val += val * val;
      }
      norm_val = std::sqrt(norm_val);
    }

    if (norm_val > constants::kEpsilon) {
      for (int c = 0; c < n_chroma; ++c) {
        features[c * n_frames + t] /= norm_val;
      }
    } else {
      for (int c = 0; c < n_chroma; ++c) {
        features[c * n_frames + t] = 0.0f;
      }
    }
  }

  return features;
}

}  // namespace

Chroma::Chroma() : n_chroma_(0), n_frames_(0), sample_rate_(0), hop_length_(0) {}

Chroma::Chroma(std::vector<float> features, int n_chroma, int n_frames, int sample_rate,
               int hop_length)
    : features_(std::move(features)),
      n_chroma_(n_chroma),
      n_frames_(n_frames),
      sample_rate_(sample_rate),
      hop_length_(hop_length) {}

Chroma Chroma::compute(const Audio& audio, const ChromaConfig& config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);

  // Compute STFT
  Spectrogram spec = Spectrogram::compute(audio, config.to_stft_config());

  return from_spectrogram(spec, audio.sample_rate(), config.to_chroma_filter_config());
}

Chroma Chroma::from_spectrogram(const Spectrogram& spec, int sr,
                                const ChromaFilterConfig& chroma_config) {
  SONARE_CHECK(!spec.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(sr > 0, ErrorCode::InvalidParameter);

  int n_bins = spec.n_bins();
  int n_frames = spec.n_frames();
  int n_chroma = chroma_config.n_chroma;

  // Create chroma filterbank
  std::vector<float> filterbank = create_chroma_filterbank(sr, spec.n_fft(), chroma_config);

  // Apply filterbank to power spectrum
  const std::vector<float>& power = spec.power();

  std::vector<float> chroma_features =
      apply_chroma_filterbank(power.data(), n_bins, n_frames, filterbank.data(), n_chroma);
  chroma_features = normalize_columns(std::move(chroma_features), n_chroma, n_frames, 2);

  return Chroma(std::move(chroma_features), n_chroma, n_frames, sr, spec.hop_length());
}

float Chroma::duration() const {
  if (sample_rate_ <= 0 || hop_length_ <= 0) {
    return 0.0f;
  }
  return static_cast<float>(n_frames_ * hop_length_) / static_cast<float>(sample_rate_);
}

MatrixView<float> Chroma::features() const {
  return MatrixView<float>(features_.data(), n_chroma_, n_frames_);
}

std::array<float, 12> Chroma::mean_energy() const {
  std::array<float, 12> result = {};

  if (n_frames_ == 0 || n_chroma_ != 12) {
    return result;
  }

  for (int c = 0; c < 12; ++c) {
    float sum = 0.0f;
    for (int t = 0; t < n_frames_; ++t) {
      sum += features_[c * n_frames_ + t];
    }
    result[c] = sum / static_cast<float>(n_frames_);
  }

  return result;
}

std::array<float, 12> Chroma::weighted_mean_energy(const std::vector<float>& frame_weights) const {
  std::array<float, 12> result = {};

  if (n_frames_ == 0 || n_chroma_ != 12 || frame_weights.empty()) {
    return mean_energy();
  }

  const int n = std::min(n_frames_, static_cast<int>(frame_weights.size()));
  float total_weight = 0.0f;
  for (int t = 0; t < n; ++t) {
    const float weight = std::max(0.0f, frame_weights[t]);
    total_weight += weight;
    for (int c = 0; c < 12; ++c) {
      result[c] += features_[c * n_frames_ + t] * weight;
    }
  }

  if (total_weight <= constants::kEpsilon) {
    return mean_energy();
  }

  for (float& value : result) {
    value /= total_weight;
  }

  return result;
}

std::vector<float> Chroma::normalize(int norm) const {
  std::vector<float> result(features_.size());

  for (int t = 0; t < n_frames_; ++t) {
    float norm_val = 0.0f;

    // Compute norm
    if (norm == 0) {
      // Max norm (infinity norm)
      for (int c = 0; c < n_chroma_; ++c) {
        float val = std::abs(features_[c * n_frames_ + t]);
        if (val > norm_val) norm_val = val;
      }
    } else if (norm == 1) {
      // L1 norm
      for (int c = 0; c < n_chroma_; ++c) {
        norm_val += std::abs(features_[c * n_frames_ + t]);
      }
    } else {
      // L2 norm (default)
      for (int c = 0; c < n_chroma_; ++c) {
        float val = features_[c * n_frames_ + t];
        norm_val += val * val;
      }
      norm_val = std::sqrt(norm_val);
    }

    // Normalize
    if (norm_val > constants::kEpsilon) {
      for (int c = 0; c < n_chroma_; ++c) {
        result[c * n_frames_ + t] = features_[c * n_frames_ + t] / norm_val;
      }
    } else {
      // Zero out if norm is too small
      for (int c = 0; c < n_chroma_; ++c) {
        result[c * n_frames_ + t] = 0.0f;
      }
    }
  }

  return result;
}

std::vector<int> Chroma::dominant_pitch_class() const {
  std::vector<int> result(n_frames_);

  for (int t = 0; t < n_frames_; ++t) {
    int max_idx = 0;
    float max_val = features_[t];

    for (int c = 1; c < n_chroma_; ++c) {
      float val = features_[c * n_frames_ + t];
      if (val > max_val) {
        max_val = val;
        max_idx = c;
      }
    }

    result[t] = max_idx;
  }

  return result;
}

float Chroma::at(int chroma, int frame) const {
  SONARE_CHECK(chroma >= 0 && chroma < n_chroma_, ErrorCode::InvalidParameter);
  SONARE_CHECK(frame >= 0 && frame < n_frames_, ErrorCode::InvalidParameter);
  return features_[chroma * n_frames_ + frame];
}

namespace {

/// @brief Wraps CQT magnitude bins onto @p n_chroma pitch classes.
/// @details CQT bin 0 corresponds to @p fmin, whose pitch class is generally not C.
///          The fold `(b % bins_per_octave) * n_chroma / bins_per_octave` assigns the
///          lowest bin of each octave to class 0, so we add the pitch-class offset of
///          @p fmin (with @p tuning, in fractional semitones) and take a positive modulo
///          to align chroma class 0 with C, matching librosa.feature.chroma_cqt.
std::vector<float> wrap_cqt_to_chroma(const float* mag, int n_bins, int n_frames,
                                      int bins_per_octave, int n_chroma, float fmin, float tuning) {
  std::vector<float> chroma(static_cast<size_t>(n_chroma) * n_frames, 0.0f);
  // Pitch class of fmin relative to C. hz_to_midi yields MIDI numbers where C is a
  // multiple of 12 (pitch class 0), so (round(midi) mod 12) is the C-relative class.
  // Account for tuning (fractional semitones) before rounding.
  int pitch_class_offset = 0;
  if (fmin > 0.0f && n_chroma > 0) {
    const float midi = hz_to_midi(fmin) + tuning;
    int pc = static_cast<int>(std::lround(midi)) % n_chroma;
    if (pc < 0) pc += n_chroma;
    pitch_class_offset = pc;
  }
  // librosa.feature.chroma_cqt expects bins_per_octave to be a multiple of n_chroma.
  // Each chroma bin gets the mean of all CQT bins falling onto that pitch class.
  std::vector<int> counts(n_chroma, 0);
  for (int b = 0; b < n_bins; ++b) {
    int idx = ((b % bins_per_octave) * n_chroma) / bins_per_octave;
    idx = (idx + pitch_class_offset) % n_chroma;
    if (idx < 0) idx += n_chroma;
    counts[idx] += 1;
    for (int t = 0; t < n_frames; ++t) {
      chroma[idx * n_frames + t] += mag[b * n_frames + t];
    }
  }
  for (int c = 0; c < n_chroma; ++c) {
    if (counts[c] > 0) {
      float denom = static_cast<float>(counts[c]);
      for (int t = 0; t < n_frames; ++t) {
        chroma[c * n_frames + t] /= denom;
      }
    }
  }
  return chroma;
}

/// @brief Applies a centered Hann smoothing kernel of length @p win along axis=time.
std::vector<float> smooth_rows_hann(const std::vector<float>& m, int rows, int cols, int win) {
  if (win <= 1 || cols == 0) return m;
  std::vector<float> kernel(win);
  double sum = 0.0;
  for (int i = 0; i < win; ++i) {
    // periodic Hann (matches scipy/librosa default sym=False inside windows.get_window).
    kernel[i] =
        0.5f - 0.5f * std::cos(constants::kTwoPi * static_cast<float>(i) / static_cast<float>(win));
    sum += kernel[i];
  }
  if (sum > 0.0) {
    for (int i = 0; i < win; ++i) kernel[i] /= static_cast<float>(sum);
  }
  std::vector<float> out(static_cast<size_t>(rows) * cols, 0.0f);
  int half = win / 2;
  for (int r = 0; r < rows; ++r) {
    for (int t = 0; t < cols; ++t) {
      float acc = 0.0f;
      for (int k = 0; k < win; ++k) {
        int src = t + k - half;
        if (src < 0)
          src = -src;
        else if (src >= cols)
          src = 2 * cols - src - 2;
        if (src < 0) src = 0;
        if (src >= cols) src = cols - 1;
        acc += kernel[k] * m[r * cols + src];
      }
      out[r * cols + t] = acc;
    }
  }
  return out;
}

}  // namespace

Chroma chroma_cqt(const Audio& audio, const ChromaCqtConfig& config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(config.n_chroma > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(config.cqt.bins_per_octave % config.n_chroma == 0, ErrorCode::InvalidParameter);

  CqtResult result = cqt(audio, config.cqt);
  const std::vector<float>& mag = result.magnitude();
  int n_bins = result.n_bins();
  int n_frames = result.n_frames();

  std::vector<float> chroma =
      wrap_cqt_to_chroma(mag.data(), n_bins, n_frames, config.cqt.bins_per_octave, config.n_chroma,
                         config.cqt.fmin, config.tuning);

  if (config.threshold > 0.0f) {
    for (int t = 0; t < n_frames; ++t) {
      float maxv = 0.0f;
      for (int c = 0; c < config.n_chroma; ++c) {
        maxv = std::max(maxv, chroma[c * n_frames + t]);
      }
      float gate = config.threshold * maxv;
      for (int c = 0; c < config.n_chroma; ++c) {
        if (chroma[c * n_frames + t] < gate) chroma[c * n_frames + t] = 0.0f;
      }
    }
  }

  if (config.normalize_frames && n_frames > 0) {
    chroma = normalize_matrix(chroma.data(), config.n_chroma, n_frames, /*axis=*/0, NormType::Inf);
  }

  return Chroma(std::move(chroma), config.n_chroma, n_frames, audio.sample_rate(),
                config.cqt.hop_length);
}

Chroma bass_chroma(const Audio& audio, const BassChromaConfig& config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(config.n_chroma > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(config.cqt.bins_per_octave % config.n_chroma == 0, ErrorCode::InvalidParameter);

  // Run a CQT over the bass frequency range (fmin / bins_per_octave / n_bins all
  // come from config.cqt, so the number of octaves covered is honored by n_bins),
  // then fold the magnitude bins onto chroma classes.
  CqtResult result = cqt(audio, config.cqt);
  if (result.empty()) {
    return Chroma();
  }

  const std::vector<float>& mag = result.magnitude();
  int n_bins = result.n_bins();
  int n_frames = result.n_frames();

  std::vector<float> chroma =
      wrap_cqt_to_chroma(mag.data(), n_bins, n_frames, config.cqt.bins_per_octave, config.n_chroma,
                         config.cqt.fmin, config.tuning);

  if (config.normalize_frames && n_frames > 0) {
    chroma = normalize_matrix(chroma.data(), config.n_chroma, n_frames, /*axis=*/0, NormType::Inf);
  }
  return Chroma(std::move(chroma), config.n_chroma, n_frames, audio.sample_rate(),
                config.cqt.hop_length);
}

Chroma chroma_cens(const Audio& audio, const ChromaCensConfig& config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);

  // Step 1: chroma_cqt without per-frame normalization (CENS uses L1 then quantization).
  ChromaCqtConfig base = config.base;
  base.normalize_frames = false;
  Chroma cq = chroma_cqt(audio, base);
  int n_chroma = cq.n_chroma();
  int n_frames = cq.n_frames();

  // Step 2: L1 normalize each frame (librosa.feature.chroma_cens default norm=2 + L1 before).
  std::vector<float> data(cq.data(), cq.data() + static_cast<size_t>(n_chroma) * n_frames);
  data = normalize_matrix(data.data(), n_chroma, n_frames, /*axis=*/0, NormType::L1);

  // Step 3: quantize using librosa thresholds and weights.
  const float kThresholds[5] = {0.0f, 0.05f, 0.1f, 0.2f, 0.4f};
  const float kWeights[5] = {0.25f, 0.25f, 0.25f, 0.25f, 0.0f};
  for (size_t i = 0; i < data.size(); ++i) {
    float v = data[i];
    float q = 0.0f;
    for (int s = 0; s < 5; ++s) {
      if (v > kThresholds[s]) q += kWeights[s];
    }
    data[i] = q;
  }

  // Step 4: Hann smoothing across time, then final L2 normalize.
  if (config.win_len_smooth > 1) {
    data = smooth_rows_hann(data, n_chroma, n_frames, config.win_len_smooth);
  }
  if (n_frames > 0) {
    data = normalize_matrix(data.data(), n_chroma, n_frames, /*axis=*/0, NormType::L2);
  }

  return Chroma(std::move(data), n_chroma, n_frames, audio.sample_rate(),
                config.base.cqt.hop_length);
}

}  // namespace sonare
