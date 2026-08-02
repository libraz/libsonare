#include "feature/vqt.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <memory>

#include "core/fft.h"
#include "core/window.h"
#include "feature/sparse_kernel.h"
#include "util/exception.h"
#include "util/lru_cache.h"
#include "util/math_utils.h"
#include "util/numeric_validation.h"

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

struct CachedVqtKernel {
  std::shared_ptr<VqtKernel> kernel;
};

/// @brief Maximum number of cached VQT kernels
constexpr size_t kMaxVqtCacheSize = 2;
constexpr size_t kMaxVqtKernelElements = 32 * 1024 * 1024;
constexpr int kMaxVqtFftLength = 131072;

/// @brief Get or create a cached sparse VQT kernel.
CachedVqtKernel get_cached_vqt_kernel(int sr, const VqtConfig& config) {
  VqtKernelCacheKey key = make_key(sr, config);

  // Returned by value so the shared-pointer snapshot survives eviction; the
  // build runs under the lock (see LruCache::get_or_build_value).
  static LruCache<VqtKernelCacheKey, CachedVqtKernel, VqtKernelCacheKeyHash> cache(
      kMaxVqtCacheSize);
  return cache.get_or_build_value(key, [&]() -> CachedVqtKernel {
    auto kernel = VqtKernel::create(sr, config);

    return CachedVqtKernel{std::shared_ptr<VqtKernel>(std::move(kernel))};
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
  SONARE_CHECK(bins_per_octave > 0 && numeric::finite_non_negative(gamma),
               ErrorCode::InvalidParameter);
  const size_t n = frequencies.size();
  std::vector<float> bandwidths(n);

  // Relative bandwidth per bin, matching librosa.filters._relative_bandwidth and
  // the CQT filter geometry in filters/wavelet.cpp (wavelet_lengths with Q=0).
  // The local bins-per-octave is derived from the actual frequency spacing
  // rather than the nominal bins_per_octave argument, so that the gamma=0 VQT
  // bandwidth reproduces the CQT bandwidth exactly and the filter lengths
  // (filter_scale * sr / bandwidth) stay continuous with the CQT path as
  // gamma -> 0. librosa's VQT filter length is
  //   Q = filter_scale / alpha;  length = Q * sr / (freq + gamma / alpha)
  //                                     = filter_scale * sr / (alpha * freq + gamma),
  // so the bandwidth returned here is the denominator `alpha * freq + gamma`.
  // The previously-used simplified closed form `2^(1/bpo) - 1` disagrees with
  // this relative-bandwidth alpha by ~3% at bins_per_octave=12 (0.05946 vs the
  // correct 0.05770) and is discontinuous with this library's own CQT.
  //
  // The small `1e-9`/`1e-6` guards mirror filters/wavelet.cpp exactly; matching
  // them bit-for-bit is what keeps the gamma->0 limit continuous with CQT, so
  // they are intentionally the same raw literals used there rather than a
  // general-purpose epsilon constant.
  std::vector<float> alpha(n, 1.0f);
  if (n >= 2) {
    std::vector<float> logf(n);
    for (size_t i = 0; i < n; ++i) {
      logf[i] = std::log2(std::max(frequencies[i], 1e-9f));
    }
    std::vector<float> bpo(n);
    bpo[0] = 1.0f / std::max(logf[1] - logf[0], 1e-9f);
    bpo[n - 1] = 1.0f / std::max(logf[n - 1] - logf[n - 2], 1e-9f);
    for (size_t k = 1; k + 1 < n; ++k) {
      bpo[k] = 2.0f / std::max(logf[k + 1] - logf[k - 1], 1e-9f);
    }
    for (size_t k = 0; k < n; ++k) {
      const float t = std::pow(2.0f, 2.0f / bpo[k]);
      alpha[k] = (t - 1.0f) / (t + 1.0f);
    }
  }

  for (size_t k = 0; k < n; ++k) {
    // VQT bandwidth: alpha * f_k + gamma (with wavelet.cpp's alpha floor).
    const float a = std::max(alpha[k], 1e-6f);
    bandwidths[k] = a * frequencies[k] + gamma;
  }

  return bandwidths;
}

std::unique_ptr<VqtKernel> VqtKernel::create(int sr, const VqtConfig& config) {
  SONARE_CHECK(sr > 0 && config.n_bins > 0 && config.bins_per_octave > 0 &&
                   numeric::finite_positive(config.fmin) &&
                   numeric::finite_non_negative(config.gamma) &&
                   numeric::finite_positive(config.filter_scale),
               ErrorCode::InvalidParameter);
  auto kernel = std::unique_ptr<VqtKernel>(new VqtKernel());

  // Compute center frequencies
  kernel->frequencies_ = vqt_frequencies(config.fmin, config.n_bins, config.bins_per_octave);
  SONARE_CHECK(
      !kernel->frequencies_.empty() && kernel->frequencies_.back() < static_cast<float>(sr) * 0.5f,
      ErrorCode::InvalidParameter);
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
    const float raw_length = config.filter_scale * static_cast<float>(sr) / bandwidth;
    SONARE_CHECK(numeric::finite_positive(bandwidth) && numeric::finite_positive(raw_length) &&
                     raw_length <= static_cast<float>(INT_MAX),
                 ErrorCode::InvalidParameter);
    int length = static_cast<int>(std::ceil(raw_length));
    kernel->raw_lengths_[k] = raw_length;
    kernel->lengths_[k] = length;
    max_length = std::max(max_length, length);
  }

  // FFT length is next power of 2 of max filter length
  SONARE_CHECK(max_length <= kMaxVqtFftLength, ErrorCode::InvalidParameter);
  kernel->fft_length_ = next_power_of_2(max_length);
  size_t kernel_elements = 0;
  SONARE_CHECK(kernel->fft_length_ <= kMaxVqtFftLength &&
                   numeric::checked_size_product(static_cast<size_t>(config.n_bins),
                                                 static_cast<size_t>(kernel->fft_length_),
                                                 kMaxVqtKernelElements, &kernel_elements),
               ErrorCode::InvalidParameter);

  // Create FFT processor
  FFT fft(kernel->fft_length_);

  // Generate kernels in frequency domain
  kernel->kernel_.rows = config.n_bins;
  kernel->kernel_.cols = kernel->fft_length_;
  kernel->kernel_.row_offsets.reserve(static_cast<size_t>(config.n_bins) + 1);
  kernel->kernel_.row_offsets.push_back(0);

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

    // Store a sparsified conjugate row (correlation instead of convolution).
    for (auto& value : complex_freq_kernel) value = std::conj(value);
    detail::append_sparsified_kernel_row(kernel->kernel_, complex_freq_kernel.data(),
                                         kernel->fft_length_);
  }

  return kernel;
}

VqtResult vqt(const Audio& audio, const VqtConfig& config, VqtProgressCallback progress_callback) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(config.hop_length > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(config.n_bins > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(config.bins_per_octave > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(numeric::finite_positive(config.fmin), ErrorCode::InvalidParameter);
  SONARE_CHECK(numeric::finite_positive(config.filter_scale), ErrorCode::InvalidParameter);

  VqtConfig resolved = config;
  if (config.gamma < 0.0f || std::isnan(config.gamma)) {
    const float step = std::pow(2.0f, 2.0f / static_cast<float>(config.bins_per_octave));
    const float alpha = (step - 1.0f) / (step + 1.0f);
    resolved.gamma = 24.7f * alpha / 0.108f;
  }
  SONARE_CHECK(numeric::finite_non_negative(resolved.gamma), ErrorCode::InvalidParameter);

  // If gamma is 0, use CQT directly
  if (resolved.gamma == 0.0f) {
    return cqt(audio, resolved.to_cqt_config(), progress_callback);
  }

  int sr = audio.sample_rate();
  int n_samples = static_cast<int>(audio.size());

  // Get cached row-compressed VQT kernel.
  auto cached = get_cached_vqt_kernel(sr, resolved);
  auto& kernel = cached.kernel;
  const auto& kernel_matrix = kernel->kernel();

  int fft_length = kernel->fft_length();
  int n_bins = kernel->n_bins();

  // Center padding aligned with the CQT path.
  int pad_length = fft_length / 2;
  int padded_length = n_samples + 2 * pad_length;
  std::vector<float> padded_signal(padded_length, 0.0f);
  std::copy(audio.data(), audio.data() + n_samples, padded_signal.begin() + pad_length);

  // Calculate number of frames from padded signal
  int n_frames = 1 + (padded_length - fft_length) / resolved.hop_length;
  if (n_frames <= 0) {
    n_frames = 1;
  }

  // Allocate output (promote to size_t before multiplying to avoid int overflow)
  std::vector<std::complex<float>> output(static_cast<size_t>(n_bins) * n_frames);

  // Create FFT processor
  FFT fft(fft_length);

  // Temporary buffers (complex FFT for full spectrum)
  std::vector<float> frame(fft_length, 0.0f);
  std::vector<std::complex<float>> complex_frame(fft_length, {0.0f, 0.0f});
  std::vector<std::complex<float>> frame_fft(fft_length);

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

  // Process each frame against the sparse kernel.
  for (int t = 0; t < n_frames; ++t) {
    int start = t * resolved.hop_length;

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

    // Copy to output (apply librosa-compatible /sqrt(length) scaling)
    for (int k = 0; k < n_bins; ++k) {
      output[k * n_frames + t] =
          detail::sparse_kernel_row_dot(kernel_matrix, k, frame_fft.data()) * inv_sqrt_lengths[k];
    }

    // Report progress
    if (progress_callback && (t % progress_interval == 0 || t == n_frames - 1)) {
      progress_callback(static_cast<float>(t + 1) / n_frames);
    }
  }

  return CqtResult(std::move(output), n_bins, n_frames, kernel->frequencies(), resolved.hop_length,
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
