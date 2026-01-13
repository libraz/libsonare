#include "feature/vqt.h"

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

}  // namespace

CqtConfig VqtConfig::to_cqt_config() const {
  CqtConfig cqt_config;
  cqt_config.hop_length = hop_length;
  cqt_config.fmin = fmin;
  cqt_config.n_bins = n_bins;
  cqt_config.bins_per_octave = bins_per_octave;
  cqt_config.filter_scale = filter_scale;
  cqt_config.window = window;
  return cqt_config;
}

std::vector<float> vqt_frequencies(float fmin, int n_bins, int bins_per_octave) {
  return cqt_frequencies(fmin, n_bins, bins_per_octave);
}

std::vector<float> vqt_bandwidths(const std::vector<float>& frequencies, int bins_per_octave,
                                  float gamma) {
  std::vector<float> bandwidths(frequencies.size());

  // alpha = 2^(1/bins_per_octave) - 1
  float alpha = std::pow(2.0f, 1.0f / bins_per_octave) - 1.0f;

  for (size_t k = 0; k < frequencies.size(); ++k) {
    // VQT bandwidth: alpha * f_k + gamma
    bandwidths[k] = alpha * frequencies[k] + gamma;
  }

  return bandwidths;
}

std::unique_ptr<VqtKernel> VqtKernel::create(int sr, const VqtConfig& config) {
  auto kernel = std::unique_ptr<VqtKernel>(new VqtKernel());

  // Compute center frequencies
  kernel->frequencies_ = vqt_frequencies(config.fmin, config.n_bins, config.bins_per_octave);
  kernel->n_bins_ = config.n_bins;

  // Compute bandwidths
  kernel->bandwidths_ = vqt_bandwidths(kernel->frequencies_, config.bins_per_octave, config.gamma);

  // Compute filter lengths for each bin
  kernel->lengths_.resize(config.n_bins);
  int max_length = 0;

  for (int k = 0; k < config.n_bins; ++k) {
    // Filter length based on bandwidth: length = sr / bandwidth * filter_scale
    float bandwidth = kernel->bandwidths_[k];
    int length = static_cast<int>(std::ceil(config.filter_scale * sr / bandwidth));
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
      time_kernel[n] = window[n] * norm * std::cos(phase);
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

VqtResult vqt(const Audio& audio, const VqtConfig& config, VqtProgressCallback progress_callback) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(config.hop_length > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(config.n_bins > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(config.bins_per_octave > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(config.fmin > 0.0f, ErrorCode::InvalidParameter);

  // If gamma is 0, use CQT directly
  if (config.gamma == 0.0f) {
    return cqt(audio, config.to_cqt_config(), progress_callback);
  }

  int sr = audio.sample_rate();
  int n_samples = static_cast<int>(audio.size());

  // Create VQT kernel
  auto kernel = VqtKernel::create(sr, config);
  int fft_length = kernel->fft_length();
  int n_bins = kernel->n_bins();

  // Calculate number of frames
  int n_frames = 1 + (n_samples - fft_length) / config.hop_length;
  if (n_frames <= 0) {
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

      for (int i = 0; i < fft_length / 2 + 1; ++i) {
        sum += frame_fft[i] * freq_kernels[k * fft_length + i];
      }

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

Audio ivqt(const VqtResult& vqt_result, int length) {
  // VQT inverse is similar to CQT inverse
  return icqt(vqt_result, length);
}

}  // namespace sonare
