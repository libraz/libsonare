#include "feature/vqt.h"

#include <algorithm>
#include <cmath>
#include <memory>

#include "core/fft.h"
#include "core/spectrum.h"
#include "core/window.h"
#include "feature/sparse_kernel.h"
#include "feature/spectral_projection.h"
#include "filters/wavelet.h"
#include "util/exception.h"
#include "util/lru_cache.h"
#include "util/math_utils.h"
#include "util/numeric_validation.h"

namespace sonare {

using sonare::constants::kTwoPi;

namespace {

/// @brief Cache key for VQT kernel
struct VqtKernelCacheKey {
  int sample_rate;
  int hop_length;
  float fmin;
  int n_bins;
  int bins_per_octave;
  float gamma;
  float filter_scale;
  WindowType window;

  // Exact equality: a near-equal configuration must never reuse another kernel.
  bool operator==(const VqtKernelCacheKey& other) const {
    return sample_rate == other.sample_rate && hop_length == other.hop_length &&
           fmin == other.fmin && n_bins == other.n_bins &&
           bins_per_octave == other.bins_per_octave && gamma == other.gamma &&
           filter_scale == other.filter_scale && window == other.window;
  }
};

VqtKernelCacheKey make_key(int sr, const VqtConfig& config) {
  return VqtKernelCacheKey{
      sr,           config.hop_length,   config.fmin,  config.n_bins, config.bins_per_octave,
      config.gamma, config.filter_scale, config.window};
}

struct VqtKernelCacheKeyHash {
  size_t operator()(const VqtKernelCacheKey& k) const {
    return std::hash<int>()(k.sample_rate) ^ (std::hash<int>()(k.hop_length) << 1) ^
           (std::hash<float>()(k.fmin) << 2) ^ (std::hash<int>()(k.n_bins) << 3) ^
           (std::hash<int>()(k.bins_per_octave) << 4) ^ (std::hash<float>()(k.gamma) << 5) ^
           (std::hash<float>()(k.filter_scale) << 6) ^
           (std::hash<int>()(static_cast<int>(k.window)) << 7);
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

  // Returned by value so the shared-pointer snapshot survives eviction. The
  // expensive wavelet and per-bin FFT build runs outside the cache lock.
  static LruCache<VqtKernelCacheKey, CachedVqtKernel, VqtKernelCacheKeyHash> cache(
      kMaxVqtCacheSize);
  return cache.get_or_build(key, [&]() -> CachedVqtKernel {
    auto kernel = VqtKernel::create(sr, config);

    return CachedVqtKernel{std::shared_ptr<VqtKernel>(std::move(kernel))};
  });
}

}  // namespace

float resolve_vqt_gamma(const VqtConfig& config) {
  if (config.gamma < 0.0f || std::isnan(config.gamma)) {
    const float step = std::pow(2.0f, 2.0f / static_cast<float>(config.bins_per_octave));
    const float alpha = (step - 1.0f) / (step + 1.0f);
    return 24.7f * alpha / 0.108f;
  }
  return config.gamma;
}

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

  // Relative bandwidth is shared with wavelet_lengths so gamma=0 produces the
  // same geometry as CQT for irregular frequency grids and at both endpoints.
  const std::vector<float> alpha = detail::relative_bandwidths(frequencies);

  for (size_t k = 0; k < n; ++k) {
    // VQT bandwidth: alpha * f_k + gamma.
    bandwidths[k] = alpha[k] * frequencies[k] + gamma;
    SONARE_CHECK(numeric::finite_positive(bandwidths[k]), ErrorCode::InvalidParameter);
  }

  return bandwidths;
}

std::unique_ptr<VqtKernel> VqtKernel::create(int sr, const VqtConfig& config) {
  validate_cqt_fmin(config.fmin);
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
    const long double rounded_length = std::ceil(static_cast<long double>(raw_length));
    SONARE_CHECK(numeric::finite_positive(bandwidth) && numeric::finite_positive(raw_length) &&
                     std::isfinite(rounded_length) && rounded_length >= 1.0L &&
                     rounded_length <= static_cast<long double>(kMaxVqtFftLength),
                 ErrorCode::InvalidParameter);
    int length = static_cast<int>(rounded_length);
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
  validate_cqt_fmin(config.fmin);
  SONARE_CHECK(numeric::finite_positive(config.filter_scale), ErrorCode::InvalidParameter);

  VqtConfig resolved = config;
  resolved.gamma = resolve_vqt_gamma(config);
  SONARE_CHECK(numeric::finite_non_negative(resolved.gamma), ErrorCode::InvalidParameter);

  // If gamma is 0, use CQT directly
  if (resolved.gamma == 0.0f) {
    return cqt(audio, resolved.to_cqt_config(), progress_callback);
  }

  int sr = audio.sample_rate();

  // Get cached row-compressed VQT kernel.
  auto cached = get_cached_vqt_kernel(sr, resolved);
  auto& kernel = cached.kernel;
  const auto& kernel_matrix = kernel->kernel();

  int fft_length = kernel->fft_length();
  int n_bins = kernel->n_bins();

  int n_frames = 0;
  std::vector<std::complex<float>> output = detail::apply_sparse_kernel_frames(
      audio, kernel_matrix, fft_length, n_bins, kernel->raw_lengths(), resolved.hop_length,
      progress_callback, n_frames);

  return CqtResult(std::move(output), n_bins, n_frames, kernel->frequencies(), resolved.hop_length,
                   sr);
}

Audio griffinlim_vqt(const float* magnitude, int n_bins, int n_frames, const VqtConfig& config,
                     int sr, int n_iter) {
  if (magnitude == nullptr || n_bins <= 0 || n_frames <= 0) return Audio();
  // The projection smears one non-finite bin across the whole STFT magnitude
  // grid, so a single NaN reaches every output sample. Checked before the
  // gamma=0 delegation so the message names the entry point the caller used.
  // Rejected here rather than at each binding so the C ABI, the WASM bindings
  // (which call this directly) and the in-process callers all inherit one rule.
  if (!numeric::all_finite(magnitude, n_bins, n_frames)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "griffinlim_vqt: magnitude contains a non-finite value");
  }

  VqtConfig resolved = config;
  resolved.gamma = resolve_vqt_gamma(config);
  // Keep the established CQT path bit-compatible when gamma resolves to zero.
  if (resolved.gamma == 0.0f) {
    return griffinlim_cqt(magnitude, n_bins, n_frames, resolved.to_cqt_config(), sr, n_iter);
  }

  // Seed Griffin-Lim with the VQT magnitude laid back onto the STFT grid of the
  // filter bank that produced it, gamma-widened bandwidths included.
  // A bandwidth needs a neighbour to be measured against, so a lone bin is voiced
  // as the first of two and only its row is used.
  resolved.n_bins = std::max(n_bins, 2);
  auto cached = get_cached_vqt_kernel(sr, resolved);
  const VqtKernel& kernel = *cached.kernel;
  const int n_fft = kernel.fft_length();
  const int n_freq = n_fft / 2 + 1;
  const std::vector<float> stft_mag = detail::project_cqt_magnitude_to_stft(
      detail::build_cqt_projection(kernel.kernel(), n_fft), kernel.raw_lengths(), n_fft, magnitude,
      n_bins, n_frames);

  GriffinLimConfig gcfg;
  gcfg.n_iter = n_iter;
  return griffin_lim(stft_mag.data(), n_freq, n_frames, n_fft, resolved.hop_length, sr, gcfg);
}

Audio griffinlim_vqt(const VqtResult& vqt_result, int sr, int n_iter) {
  if (vqt_result.empty()) return Audio();

  const int n_bins = vqt_result.n_bins();
  const int n_frames = vqt_result.n_frames();
  const std::vector<float>& freqs = vqt_result.frequencies();

  // Recover the frequency-grid configuration from the stored result. CqtResult
  // intentionally carries no VQT-only metadata, so this legacy overload uses
  // gamma=0 and preserves its historical CQT-equivalent reconstruction.
  VqtConfig config;
  config.hop_length = vqt_result.hop_length();
  config.n_bins = n_bins;
  config.gamma = 0.0f;
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

Audio griffinlim_vqt(const VqtResult& vqt_result, const VqtConfig& config, int sr, int n_iter) {
  if (vqt_result.empty()) return Audio();
  return griffinlim_vqt(vqt_result.magnitude().data(), vqt_result.n_bins(), vqt_result.n_frames(),
                        config, sr, n_iter);
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
