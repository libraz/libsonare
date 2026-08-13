#include "feature/chroma.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "core/convert.h"
#include "core/window.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/vector_normalize.h"

namespace sonare {

namespace {

std::vector<float> normalize_chroma_columns(std::vector<float> features, int n_chroma, int n_frames,
                                            int norm) {
  NormType norm_type = NormType::L2;
  if (norm == 0) {
    norm_type = NormType::Inf;
  } else if (norm == 1) {
    norm_type = NormType::L1;
  }
  return normalize_matrix(features.data(), n_chroma, n_frames, /*axis=*/0, norm_type);
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

  // Create chroma filterbank (cached — repeated calls reuse the same matrix).
  // The shared handle keeps the filterbank alive across the apply() call even if
  // another thread evicts the cache entry concurrently.
  std::shared_ptr<const std::vector<float>> filterbank =
      get_chroma_filterbank_cached(sr, spec.n_fft(), chroma_config);

  // Apply filterbank to power spectrum
  const std::vector<float>& power = spec.power();

  std::vector<float> chroma_features =
      apply_chroma_filterbank(power.data(), n_bins, n_frames, filterbank->data(), n_chroma);
  // librosa.feature.chroma_stft normalizes each frame with norm=np.inf (L-inf /
  // max norm) by default; match it so analyze()/chroma_stft agree with librosa
  // (and with the L-inf chroma_cqt path) per-frame, not just in argmax.
  chroma_features = normalize_chroma_columns(std::move(chroma_features), n_chroma, n_frames, 0);

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
  return normalize_chroma_columns(features_, n_chroma_, n_frames_, norm);
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

/// @brief Wraps CQT magnitude bins onto @p n_chroma pitch classes, mean-aggregated.
/// @details librosa.feature.chroma_cqt expects bins_per_octave to be a multiple of
///          n_chroma and gives each chroma bin the mean of the CQT bins falling onto
///          that pitch class. The bin -> class mapping itself is
///          fold_cqt_bins_to_chroma's; this wrapper only supplies the grid, which
///          arrives here as an explicit (bins_per_octave, fmin) pair rather than as
///          a frequency axis. @p fmin is expected to already include the tuning
///          shift (see chroma_cqt / bass_chroma); tuning is not re-applied here.
std::vector<float> wrap_cqt_to_chroma(const float* mag, int n_bins, int n_frames,
                                      int bins_per_octave, int n_chroma, float fmin) {
  return fold_cqt_bins_to_chroma(mag, n_bins, n_frames, bins_per_octave, n_chroma,
                                 chroma_class_of_frequency(fmin, n_chroma), ChromaFold::kMean);
}

/// @brief Shifts @p fmin by the requested tuning deviation.
/// @details Mirrors librosa: tuning is expressed in fractions of a CQT bin, so
///          the lowest analysis frequency moves to
///          `fmin * 2^(tuning / bins_per_octave)`. This is what makes sub-semitone
///          tuning actually affect the CQT grid (and therefore the chroma fold),
///          rather than only nudging the integer pitch-class offset.
float apply_tuning_to_fmin(float fmin, float tuning, int bins_per_octave) {
  if (fmin <= 0.0f || tuning == 0.0f || bins_per_octave <= 0) {
    return fmin;
  }
  return fmin * std::pow(2.0f, tuning / static_cast<float>(bins_per_octave));
}

/// @brief Applies a centered Hann smoothing kernel of length @p win along axis=time.
///
/// Matches librosa.feature.chroma_cens's smoothing: a SYMMETRIC Hann window
/// (get_window(..., fftbins=False)) normalized to unit sum, convolved with the
/// signal at mode='same' — i.e. zero-padded at the boundaries (out-of-range
/// samples contribute nothing), not reflected. Callers pass win_len_smooth + 2
/// (the symmetric Hann's endpoints are zero, so this yields win_len_smooth
/// effective taps, as librosa intends).
std::vector<float> smooth_rows_hann(const std::vector<float>& m, int rows, int cols, int win) {
  if (win <= 1 || cols == 0) return m;
  std::vector<float> kernel = create_window(WindowType::Hann, win, /*periodic=*/false);
  double sum = 0.0;
  for (int i = 0; i < win; ++i) sum += kernel[i];
  if (sum > 0.0) {
    for (int i = 0; i < win; ++i) kernel[i] /= static_cast<float>(sum);
  }
  std::vector<float> out(static_cast<size_t>(rows) * cols, 0.0f);
  // scipy.signal.convolve(..., mode='same') aligns the kernel so its (win-1)/2
  // offset lands on the current sample; out-of-range taps are treated as zero.
  const int half = (win - 1) / 2;
  for (int r = 0; r < rows; ++r) {
    for (int t = 0; t < cols; ++t) {
      float acc = 0.0f;
      for (int k = 0; k < win; ++k) {
        const int src = t + k - half;
        if (src < 0 || src >= cols) continue;  // zero-pad, not reflect
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

  // Apply tuning to the CQT grid itself (not just the chroma fold) so that
  // sub-semitone tuning actually shifts the analysis frequencies, matching
  // librosa.feature.chroma_cqt(tuning=...).
  CqtConfig cqt_config = config.cqt;
  cqt_config.fmin =
      apply_tuning_to_fmin(config.cqt.fmin, config.tuning, config.cqt.bins_per_octave);
  CqtResult result = cqt(audio, cqt_config);
  std::vector<float> mag = result.magnitude();
  int n_bins = result.n_bins();
  int n_frames = result.n_frames();

  if (config.threshold > 0.0f) {
    for (float& value : mag) {
      if (value < config.threshold) value = 0.0f;
    }
  }

  std::vector<float> chroma = wrap_cqt_to_chroma(
      mag.data(), n_bins, n_frames, config.cqt.bins_per_octave, config.n_chroma, cqt_config.fmin);

  if (config.normalize_frames && n_frames > 0) {
    // L-inf per-frame normalization matches librosa.feature.chroma_cqt's
    // default norm=np.inf; librosa parity is the source of truth for defaults.
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
  CqtConfig cqt_config = config.cqt;
  cqt_config.fmin =
      apply_tuning_to_fmin(config.cqt.fmin, config.tuning, config.cqt.bins_per_octave);
  CqtResult result = cqt(audio, cqt_config);
  if (result.empty()) {
    return Chroma();
  }

  const std::vector<float>& mag = result.magnitude();
  int n_bins = result.n_bins();
  int n_frames = result.n_frames();

  std::vector<float> chroma = wrap_cqt_to_chroma(
      mag.data(), n_bins, n_frames, config.cqt.bins_per_octave, config.n_chroma, cqt_config.fmin);

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

  // Step 3: quantize using librosa's "log-like" amplitude thresholds. librosa
  // (chroma_cens) adds 0.25 for EACH of the four thresholds {0.4, 0.2, 0.1,
  // 0.05} that the value strictly exceeds, so the quantized value lands in
  // {0, 0.25, 0.5, 0.75, 1.0}. (The previous table inserted a spurious 0.0
  // threshold and dropped the 0.4 threshold, over-counting small magnitudes and
  // under-counting magnitudes in [0.2, 0.4].)
  const float kThresholds[4] = {0.4f, 0.2f, 0.1f, 0.05f};
  const float kWeight = 0.25f;
  for (size_t i = 0; i < data.size(); ++i) {
    const float v = data[i];
    float q = 0.0f;
    for (int s = 0; s < 4; ++s) {
      if (v > kThresholds[s]) q += kWeight;
    }
    data[i] = q;
  }

  // Step 4: Hann smoothing across time, then final L2 normalize. librosa builds
  // the smoothing window as get_window('hann', win_len_smooth + 2, fftbins=False)
  // — the symmetric Hann's zero endpoints leave win_len_smooth effective taps.
  if (config.win_len_smooth > 1) {
    data = smooth_rows_hann(data, n_chroma, n_frames, config.win_len_smooth + 2);
  }
  if (n_frames > 0) {
    data = normalize_matrix(data.data(), n_chroma, n_frames, /*axis=*/0, NormType::L2);
  }

  return Chroma(std::move(data), n_chroma, n_frames, audio.sample_rate(),
                config.base.cqt.hop_length);
}

}  // namespace sonare
