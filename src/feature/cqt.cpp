#include "feature/cqt.h"

#include <algorithm>
#include <cmath>

#include "core/fft.h"
#include "core/window.h"
#include "util/exception.h"

namespace sonare {

namespace {

/// @brief Finds smallest power of 2 >= n.
int next_power_of_2(int n) {
  int power = 1;
  while (power < n) {
    power *= 2;
  }
  return power;
}

/// @brief Computes Q factor for CQT.
float compute_q(int bins_per_octave, float filter_scale) {
  return filter_scale / (std::pow(2.0f, 1.0f / bins_per_octave) - 1.0f);
}

}  // namespace

// CqtResult implementation
CqtResult::CqtResult() = default;

CqtResult::CqtResult(std::vector<std::complex<float>> data, int n_bins, int n_frames,
                     std::vector<float> frequencies, int hop_length, int sample_rate)
    : data_(std::move(data)),
      n_bins_(n_bins),
      n_frames_(n_frames),
      hop_length_(hop_length),
      sample_rate_(sample_rate),
      frequencies_(std::move(frequencies)) {}

float CqtResult::duration() const {
  if (sample_rate_ == 0) {
    return 0.0f;
  }
  return static_cast<float>(n_frames_ * hop_length_) / sample_rate_;
}

MatrixView<std::complex<float>> CqtResult::complex_view() const {
  return MatrixView<std::complex<float>>(data_.data(), n_bins_, n_frames_);
}

const std::vector<float>& CqtResult::magnitude() const {
  if (magnitude_cache_.empty() && !data_.empty()) {
    magnitude_cache_.resize(data_.size());
    for (size_t i = 0; i < data_.size(); ++i) {
      magnitude_cache_[i] = std::abs(data_[i]);
    }
  }
  return magnitude_cache_;
}

const std::vector<float>& CqtResult::power() const {
  if (power_cache_.empty() && !data_.empty()) {
    power_cache_.resize(data_.size());
    for (size_t i = 0; i < data_.size(); ++i) {
      float mag = std::abs(data_[i]);
      power_cache_[i] = mag * mag;
    }
  }
  return power_cache_;
}

std::vector<float> CqtResult::to_db(float ref, float amin) const {
  const std::vector<float>& pwr = power();
  std::vector<float> db(pwr.size());

  float ref_power = ref * ref;
  for (size_t i = 0; i < pwr.size(); ++i) {
    float val = std::max(pwr[i], amin * amin);
    db[i] = 10.0f * std::log10(val / ref_power);
  }
  return db;
}

const std::complex<float>& CqtResult::at(int bin, int frame) const {
  SONARE_CHECK(bin >= 0 && bin < n_bins_, ErrorCode::InvalidParameter);
  SONARE_CHECK(frame >= 0 && frame < n_frames_, ErrorCode::InvalidParameter);
  return data_[bin * n_frames_ + frame];
}

// CqtKernel implementation
std::unique_ptr<CqtKernel> CqtKernel::create(int sr, const CqtConfig& config) {
  auto kernel = std::unique_ptr<CqtKernel>(new CqtKernel());

  float Q = compute_q(config.bins_per_octave, config.filter_scale);

  // Compute center frequencies for all bins
  kernel->frequencies_ = cqt_frequencies(config.fmin, config.n_bins, config.bins_per_octave);
  kernel->n_bins_ = config.n_bins;

  // Compute filter lengths for each bin
  kernel->lengths_.resize(config.n_bins);
  int max_length = 0;

  for (int k = 0; k < config.n_bins; ++k) {
    float freq = kernel->frequencies_[k];
    int length = static_cast<int>(std::ceil(Q * sr / freq));
    kernel->lengths_[k] = length;
    max_length = std::max(max_length, length);
  }

  // FFT length is next power of 2 of max filter length
  kernel->fft_length_ = next_power_of_2(max_length);

  // Create FFT processor
  FFT fft(kernel->fft_length_);

  // Generate kernels in frequency domain
  kernel->kernel_.resize(config.n_bins * kernel->fft_length_);

  std::vector<float> time_kernel(kernel->fft_length_, 0.0f);
  std::vector<std::complex<float>> freq_kernel(kernel->fft_length_ / 2 + 1);

  for (int k = 0; k < config.n_bins; ++k) {
    float freq = kernel->frequencies_[k];
    int length = kernel->lengths_[k];

    // Create window
    std::vector<float> window = create_window(config.window, length);

    // Compute normalization
    float win_sum = 0.0f;
    for (int i = 0; i < length; ++i) {
      win_sum += window[i];
    }
    float norm = 1.0f / win_sum;

    // Generate time-domain kernel: windowed complex sinusoid
    std::fill(time_kernel.begin(), time_kernel.end(), 0.0f);

    for (int n = 0; n < length; ++n) {
      float phase = 2.0f * M_PI * freq * n / sr;
      // Center the window
      int idx = n;
      time_kernel[idx] = window[n] * norm * std::cos(phase);
    }

    // FFT of kernel
    fft.forward(time_kernel.data(), freq_kernel.data());

    // Store conjugate (for correlation instead of convolution)
    for (int i = 0; i < kernel->fft_length_ / 2 + 1; ++i) {
      kernel->kernel_[k * kernel->fft_length_ + i] = std::conj(freq_kernel[i]);
    }
  }

  return kernel;
}

std::vector<float> cqt_frequencies(float fmin, int n_bins, int bins_per_octave) {
  std::vector<float> freqs(n_bins);
  for (int k = 0; k < n_bins; ++k) {
    freqs[k] = fmin * std::pow(2.0f, static_cast<float>(k) / bins_per_octave);
  }
  return freqs;
}

CqtResult cqt(const Audio& audio, const CqtConfig& config, CqtProgressCallback progress_callback) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(config.hop_length > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(config.n_bins > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(config.bins_per_octave > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(config.fmin > 0.0f, ErrorCode::InvalidParameter);

  int sr = audio.sample_rate();
  int n_samples = static_cast<int>(audio.size());

  // Create CQT kernel
  auto kernel = CqtKernel::create(sr, config);
  int fft_length = kernel->fft_length();
  int n_bins = kernel->n_bins();

  // Calculate number of frames
  int n_frames = 1 + (n_samples - fft_length) / config.hop_length;
  if (n_frames <= 0) {
    // Audio too short, pad and compute single frame
    n_frames = 1;
  }

  // Allocate output
  std::vector<std::complex<float>> output(n_bins * n_frames);

  // Create FFT processor
  FFT fft(fft_length);

  // Temporary buffers
  std::vector<float> frame(fft_length, 0.0f);
  std::vector<std::complex<float>> frame_fft(fft_length / 2 + 1);

  const float* data = audio.data();
  const auto& freq_kernels = kernel->kernel();

  // Progress reporting interval
  int progress_interval = std::max(1, n_frames / 20);

  // Process each frame
  for (int t = 0; t < n_frames; ++t) {
    int start = t * config.hop_length;

    // Extract frame with zero-padding
    std::fill(frame.begin(), frame.end(), 0.0f);
    int copy_length = std::min(fft_length, n_samples - start);
    if (copy_length > 0) {
      std::copy(data + start, data + start + copy_length, frame.begin());
    }

    // FFT of frame
    fft.forward(frame.data(), frame_fft.data());

    // Multiply with each kernel and sum (correlation)
    for (int k = 0; k < n_bins; ++k) {
      std::complex<float> sum(0.0f, 0.0f);

      // Correlation in frequency domain
      for (int i = 0; i < fft_length / 2 + 1; ++i) {
        sum += frame_fft[i] * freq_kernels[k * fft_length + i];
      }

      // Normalize by FFT length
      output[k * n_frames + t] = sum / static_cast<float>(fft_length);
    }

    // Report progress
    if (progress_callback && (t % progress_interval == 0 || t == n_frames - 1)) {
      progress_callback(static_cast<float>(t + 1) / n_frames);
    }
  }

  return CqtResult(std::move(output), n_bins, n_frames, kernel->frequencies(), config.hop_length,
                   sr);
}

Audio icqt(const CqtResult& cqt_result, int length) {
  if (cqt_result.empty()) {
    return Audio();
  }

  int n_bins = cqt_result.n_bins();
  int n_frames = cqt_result.n_frames();
  int hop_length = cqt_result.hop_length();
  int sr = cqt_result.sample_rate();

  // Estimate output length
  int output_length = length > 0 ? length : (n_frames - 1) * hop_length + hop_length;

  std::vector<float> output(output_length, 0.0f);
  std::vector<float> weight(output_length, 0.0f);

  const auto& frequencies = cqt_result.frequencies();

  // Simple overlap-add reconstruction
  // This is a simplified pseudo-inverse, not exact
  for (int t = 0; t < n_frames; ++t) {
    int center = t * hop_length;

    for (int k = 0; k < n_bins; ++k) {
      std::complex<float> coef = cqt_result.at(k, t);
      float freq = frequencies[k];

      // Estimate filter length for this bin
      float Q = 1.0f / (std::pow(2.0f, 1.0f / 12.0f) - 1.0f);
      int filter_length = static_cast<int>(Q * sr / freq);
      filter_length = std::min(filter_length, output_length);

      // Create windowed sinusoid
      for (int n = 0; n < filter_length; ++n) {
        int idx = center + n - filter_length / 2;
        if (idx >= 0 && idx < output_length) {
          float phase = 2.0f * M_PI * freq * n / sr;
          float win = 0.5f * (1.0f - std::cos(2.0f * M_PI * n / filter_length));

          // Real part of complex multiplication
          float val = std::real(coef) * std::cos(phase) - std::imag(coef) * std::sin(phase);
          output[idx] += val * win;
          weight[idx] += win;
        }
      }
    }
  }

  // Normalize by weight
  for (int i = 0; i < output_length; ++i) {
    if (weight[i] > 1e-6f) {
      output[i] /= weight[i];
    }
  }

  return Audio::from_vector(std::move(output), sr);
}

std::vector<float> cqt_to_chroma(const CqtResult& cqt_result, int n_chroma) {
  if (cqt_result.empty()) {
    return {};
  }

  int n_bins = cqt_result.n_bins();
  int n_frames = cqt_result.n_frames();

  std::vector<float> chroma(n_chroma * n_frames, 0.0f);

  const auto& mag = cqt_result.magnitude();

  // Map CQT bins to chroma bins
  for (int t = 0; t < n_frames; ++t) {
    for (int k = 0; k < n_bins; ++k) {
      int chroma_bin = k % n_chroma;
      chroma[chroma_bin * n_frames + t] += mag[k * n_frames + t];
    }
  }

  // Normalize each frame
  for (int t = 0; t < n_frames; ++t) {
    float max_val = 0.0f;
    for (int c = 0; c < n_chroma; ++c) {
      max_val = std::max(max_val, chroma[c * n_frames + t]);
    }
    if (max_val > 1e-6f) {
      for (int c = 0; c < n_chroma; ++c) {
        chroma[c * n_frames + t] /= max_val;
      }
    }
  }

  return chroma;
}

}  // namespace sonare
