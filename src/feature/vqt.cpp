#include "feature/vqt.h"

#include <Eigen/Dense>
#include <algorithm>
#include <cmath>
#include <memory>

#include "core/fft.h"
#include "core/window.h"
#include "util/exception.h"
#include "util/lru_cache.h"
#include "util/math_utils.h"

namespace sonare {

using sonare::constants::kTwoPi;

namespace {

/// @brief Snaps a float key field to a fixed grid so bitwise-different but
/// logically-equal UI inputs (e.g. 32.70 vs 32.7000007) collapse to the same
/// value before the key is hashed/compared. Quantizing at key construction is
/// what lets us keep strict `==` in the key (so the equal/hash contract holds)
/// while still getting cache hits for near-equal inputs.
float quantize(float value, float grid) { return std::round(value / grid) * grid; }

/// @brief Cache key for VQT kernel
struct VqtKernelCacheKey {
  int sample_rate;
  int hop_length;
  float fmin;
  int n_bins;
  int bins_per_octave;
  float gamma;
  WindowType window;

  // Exact equality so the equal/hash contract holds: the hash mixes the raw
  // float bits of fmin/gamma, so a fuzzy operator== would let two
  // logically-"equal" keys hash to different buckets and silently miss the
  // cache — triggering a redundant (expensive) wavelet + per-bin FFT rebuild —
  // and, on a bucket collision, could even return the wrong cached kernel. The
  // float fields are quantized at construction (see make_key), so exact
  // comparison still produces cache hits for near-equal inputs.
  bool operator==(const VqtKernelCacheKey& other) const {
    return sample_rate == other.sample_rate && hop_length == other.hop_length &&
           fmin == other.fmin && n_bins == other.n_bins &&
           bins_per_octave == other.bins_per_octave && gamma == other.gamma &&
           window == other.window;
  }
};

/// @brief Builds a VQT cache key with float fields snapped to fixed grids.
/// @details fmin is in Hz; gamma is in Hz (the VQT bandwidth offset). Both use
/// a 0.001 grid: fine enough to resolve real config changes, coarse enough to
/// absorb UI float noise.
VqtKernelCacheKey make_key(int sr, const VqtConfig& config) {
  constexpr float kFminGrid = 0.001f;   // Hz
  constexpr float kGammaGrid = 0.001f;  // Hz
  return VqtKernelCacheKey{sr,
                           config.hop_length,
                           quantize(config.fmin, kFminGrid),
                           config.n_bins,
                           config.bins_per_octave,
                           quantize(config.gamma, kGammaGrid),
                           config.window};
}

struct VqtKernelCacheKeyHash {
  size_t operator()(const VqtKernelCacheKey& k) const {
    return std::hash<int>()(k.sample_rate) ^ (std::hash<int>()(k.hop_length) << 1) ^
           (std::hash<float>()(k.fmin) << 2) ^ (std::hash<int>()(k.n_bins) << 3) ^
           (std::hash<int>()(k.bins_per_octave) << 4) ^ (std::hash<float>()(k.gamma) << 5) ^
           (std::hash<int>()(static_cast<int>(k.window)) << 6);
  }
};

/// @brief Eigen matrix type for VQT kernel
using EigenKernelMatrix =
    Eigen::Matrix<std::complex<float>, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>;

/// @brief Cached kernel with its pre-computed Eigen matrix.
/// @details Both members are shared pointers, so callers take ownership of a
/// snapshot that survives concurrent eviction of the cache entry.
struct CachedVqtKernel {
  std::shared_ptr<VqtKernel> kernel;
  std::shared_ptr<EigenKernelMatrix> eigen_matrix;
};

/// @brief Maximum number of cached VQT kernels
constexpr size_t kMaxVqtCacheSize = 8;

/// @brief Get or create cached VQT kernel with Eigen matrix
CachedVqtKernel get_cached_vqt_kernel(int sr, const VqtConfig& config) {
  VqtKernelCacheKey key = make_key(sr, config);

  // Returned by value so the shared-pointer snapshot survives eviction; the
  // build runs under the lock (see LruCache::get_or_build_value).
  static LruCache<VqtKernelCacheKey, CachedVqtKernel, VqtKernelCacheKeyHash> cache(
      kMaxVqtCacheSize);
  return cache.get_or_build_value(key, [&]() -> CachedVqtKernel {
    auto kernel = VqtKernel::create(sr, config);

    // Pre-compute Eigen matrix (full complex FFT: all fft_length bins)
    int fft_length = kernel->fft_length();
    int n_bins = kernel->n_bins();
    const auto& freq_kernels = kernel->kernel();

    auto eigen_matrix = std::make_shared<EigenKernelMatrix>(n_bins, fft_length);
    for (int k = 0; k < n_bins; ++k) {
      for (int i = 0; i < fft_length; ++i) {
        (*eigen_matrix)(k, i) = freq_kernels[k * fft_length + i];
      }
    }
    return CachedVqtKernel{std::shared_ptr<VqtKernel>(std::move(kernel)), std::move(eigen_matrix)};
  });
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
  kernel->raw_lengths_.resize(config.n_bins);
  int max_length = 0;

  for (int k = 0; k < config.n_bins; ++k) {
    // Filter length based on bandwidth: length = sr / bandwidth * filter_scale.
    // Keep the fractional length for normalization (matches the CQT path, which
    // uses the fractional `wavelet_lengths`); the integer length is only the
    // sample count for windowing / FFT sizing.
    float bandwidth = kernel->bandwidths_[k];
    float raw_length = config.filter_scale * sr / bandwidth;
    int length = static_cast<int>(std::ceil(raw_length));
    kernel->raw_lengths_[k] = raw_length;
    kernel->lengths_[k] = length;
    max_length = std::max(max_length, length);
  }

  // FFT length is next power of 2 of max filter length
  kernel->fft_length_ = next_power_of_2(max_length);

  // Create FFT processor
  FFT fft(kernel->fft_length_);

  // Generate kernels in frequency domain
  kernel->kernel_.resize(config.n_bins * kernel->fft_length_);

  std::vector<std::complex<float>> complex_time_kernel(kernel->fft_length_, {0.0f, 0.0f});
  std::vector<std::complex<float>> complex_freq_kernel(kernel->fft_length_);

  const float inv_n_fft = 1.0f / static_cast<float>(kernel->fft_length_);

  for (int k = 0; k < config.n_bins; ++k) {
    float freq = kernel->frequencies_[k];
    int length = kernel->lengths_[k];
    float raw_length = kernel->raw_lengths_[k];

    // Create window
    std::vector<float> window = create_window(config.window, length);

    // Compute L1 normalization (equivalent to librosa's util.normalize(norm=1)
    // for a Hann-windowed complex sinusoid: |window[n] * exp(j*phase)| = window[n],
    // so sum(|kernel|) = sum(window)).
    float win_sum = 0.0f;
    for (int i = 0; i < length; ++i) {
      win_sum += window[i];
    }
    // Bake librosa's `lengths/n_fft` basis scaling into the kernel so that
    // (a) the per-frame inner product can drop the explicit `1/n_fft` factor and
    // (b) the final per-bin `1/sqrt(length)` scaling matches the CQT path's
    //     amplitude convention exactly (see cqt.cpp lines 226-231, 345-350).
    // This makes the gamma=0 (CQT delegation) and gamma>0 paths produce
    // continuous output magnitudes for the same input. The basis scaling uses
    // the FRACTIONAL raw length (matching cqt.cpp's `raw_lengths[k] / n_fft`),
    // not the truncated integer length.
    float norm = (win_sum > 0.0f) ? (raw_length * inv_n_fft) / win_sum : 0.0f;

    // Generate time-domain kernel: windowed complex sinusoid exp(+j*2*pi*f*idx/sr).
    //
    // The kernel must be *centered* inside the fft_length window, exactly like
    // the CQT path (filters::wavelet pad-centers each kernel and references the
    // sinusoid phase to the window center via `idx = floor(-length/2) + n`).
    // The analysis frames are center-padded, so their meaningful signal energy
    // sits in the middle of the fft_length window. A one-sided kernel placed at
    // samples [0, length) barely overlaps that energy, which silently dropped
    // the matched-bin magnitude by a large, frequency-dependent factor
    // (~3.46x for the 440 Hz bin). Centering restores the correct correlation so
    // gamma->0 VQT tracks CQT.
    std::fill(complex_time_kernel.begin(), complex_time_kernel.end(),
              std::complex<float>(0.0f, 0.0f));

    const int slot_offset = (kernel->fft_length_ - length) / 2;
    const int phase_start = -(length / 2);
    for (int n = 0; n < length; ++n) {
      float phase = kTwoPi * freq * (phase_start + n) / sr;
      float scaled_win = window[n] * norm;
      // Use exp(+j*phase) (cos, +sin) to match the CQT kernel convention in
      // filters/wavelet.cpp and librosa. Both paths store conj(FFT(kernel)), so
      // a sign flip here would conjugate the complex VQT response relative to
      // librosa (magnitude/chroma are unaffected, but phase consumers invert).
      complex_time_kernel[slot_offset + n] =
          std::complex<float>(scaled_win * std::cos(phase), scaled_win * std::sin(phase));
    }

    // Complex FFT of kernel
    fft.forward_complex(complex_time_kernel.data(), complex_freq_kernel.data());

    // Store conjugate over all fft_length bins (for correlation instead of convolution)
    for (int i = 0; i < kernel->fft_length_; ++i) {
      kernel->kernel_[k * kernel->fft_length_ + i] = std::conj(complex_freq_kernel[i]);
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

  // Get cached VQT kernel with pre-computed Eigen matrix
  auto cached = get_cached_vqt_kernel(sr, config);
  auto& kernel = cached.kernel;
  auto& kernel_matrix = *cached.eigen_matrix;

  int fft_length = kernel->fft_length();
  int n_bins = kernel->n_bins();

  // Center padding aligned with the CQT path.
  int pad_length = fft_length / 2;
  int padded_length = n_samples + 2 * pad_length;
  std::vector<float> padded_signal(padded_length, 0.0f);
  std::copy(audio.data(), audio.data() + n_samples, padded_signal.begin() + pad_length);

  // Calculate number of frames from padded signal
  int n_frames = 1 + (padded_length - fft_length) / config.hop_length;
  if (n_frames <= 0) {
    n_frames = 1;
  }

  using VectorXcf = Eigen::Matrix<std::complex<float>, Eigen::Dynamic, 1>;

  // Allocate output (promote to size_t before multiplying to avoid int overflow)
  std::vector<std::complex<float>> output(static_cast<size_t>(n_bins) * n_frames);

  // Create FFT processor
  FFT fft(fft_length);

  // Temporary buffers (complex FFT for full spectrum)
  std::vector<float> frame(fft_length, 0.0f);
  std::vector<std::complex<float>> complex_frame(fft_length, {0.0f, 0.0f});
  std::vector<std::complex<float>> frame_fft(fft_length);
  VectorXcf result(n_bins);

  const float* data = padded_signal.data();
  // Per-bin 1/sqrt(L) factor for librosa's `scale=True` mode (the default).
  // The kernel already absorbs the `lengths/n_fft` basis scaling (see
  // VqtKernel::create), so no explicit `1/n_fft` is applied to the inner
  // product here. The matching code path lives in cqt.cpp (`inv_sqrt_len`).
  // Use the FRACTIONAL raw length so the normalization is identical to the CQT
  // path (cqt.cpp uses `1 / sqrt(raw_lengths[k])`); the integer length would
  // introduce a frequency-dependent gain error, worst in the low bands.
  std::vector<float> inv_sqrt_lengths(n_bins, 1.0f);
  const auto& raw_lengths = kernel->raw_lengths();
  for (int k = 0; k < n_bins; ++k) {
    if (raw_lengths[k] > 0.0f) {
      inv_sqrt_lengths[k] = 1.0f / std::sqrt(raw_lengths[k]);
    }
  }

  // Progress reporting interval
  int progress_interval = std::max(1, n_frames / 20);

  // Process each frame with Eigen SIMD optimization
  for (int t = 0; t < n_frames; ++t) {
    int start = t * config.hop_length;

    // Extract frame with zero-padding if needed at boundaries
    std::fill(frame.begin(), frame.end(), 0.0f);
    int copy_length = std::min(fft_length, padded_length - start);
    if (copy_length > 0) {
      std::copy(data + start, data + start + copy_length, frame.begin());
    }

    // Copy real frame into complex buffer
    for (int n = 0; n < fft_length; ++n) {
      complex_frame[n] = {frame[n], 0.0f};
    }

    // Complex FFT of frame (full spectrum)
    fft.forward_complex(complex_frame.data(), frame_fft.data());

    // Compute all correlations at once using cached Eigen matrix.
    // The basis already absorbs the `lengths/n_fft` factor (see
    // VqtKernel::create), so no extra `1/n_fft` normalisation is needed here —
    // mirroring the CQT path.
    Eigen::Map<const VectorXcf> frame_vec(frame_fft.data(), fft_length);
    result.noalias() = kernel_matrix * frame_vec;

    // Copy to output (apply librosa-compatible /sqrt(length) scaling)
    for (int k = 0; k < n_bins; ++k) {
      output[k * n_frames + t] = result(k) * inv_sqrt_lengths[k];
    }

    // Report progress
    if (progress_callback && (t % progress_interval == 0 || t == n_frames - 1)) {
      progress_callback(static_cast<float>(t + 1) / n_frames);
    }
  }

  return CqtResult(std::move(output), n_bins, n_frames, kernel->frequencies(), config.hop_length,
                   sr);
}

Audio griffinlim_vqt(const float* magnitude, int n_bins, int n_frames, const VqtConfig& config,
                     int sr, int n_iter) {
  if (magnitude == nullptr || n_bins <= 0 || n_frames <= 0) return Audio();
  // VQT shares the CQT geometric frequency grid (vqt_frequencies == cqt_frequencies),
  // so the CQT Griffin-Lim projection applies directly to VQT magnitudes.
  return griffinlim_cqt(magnitude, n_bins, n_frames, config.to_cqt_config(), sr, n_iter);
}

Audio griffinlim_vqt(const VqtResult& vqt_result, int sr, int n_iter) {
  if (vqt_result.empty()) return Audio();

  const int n_bins = vqt_result.n_bins();
  const int n_frames = vqt_result.n_frames();
  const std::vector<float>& freqs = vqt_result.frequencies();

  // Recover the VQT configuration from the stored result. The frequency grid is
  // geometric: f_k = fmin * 2^(k / bins_per_octave).
  VqtConfig config;
  config.hop_length = vqt_result.hop_length();
  config.n_bins = n_bins;
  if (!freqs.empty()) {
    config.fmin = freqs.front();
  }
  if (freqs.size() >= 2 && freqs[0] > 0.0f && freqs[1] > freqs[0]) {
    const float ratio = std::log2(freqs[1] / freqs[0]);
    if (ratio > 0.0f) {
      config.bins_per_octave = std::max(1, static_cast<int>(std::lround(1.0f / ratio)));
    }
  }

  return griffinlim_vqt(vqt_result.magnitude().data(), n_bins, n_frames, config, sr, n_iter);
}

Audio ivqt(const VqtResult& vqt_result, int length) {
  // High-quality reconstruction path: Griffin-Lim on the VQT magnitude. The
  // legacy icqt pseudo-inverse remains directly callable for callers that
  // depend on the previous (lower-quality) behavior.
  Audio reconstructed = griffinlim_vqt(vqt_result, vqt_result.sample_rate());
  if (length > 0 && reconstructed.size() != static_cast<size_t>(length)) {
    std::vector<float> resized(static_cast<size_t>(length), 0.0f);
    const size_t copy_count = std::min(resized.size(), reconstructed.size());
    std::copy_n(reconstructed.data(), copy_count, resized.data());
    reconstructed = Audio::from_vector(std::move(resized), reconstructed.sample_rate());
  }
  return reconstructed;
}

}  // namespace sonare
