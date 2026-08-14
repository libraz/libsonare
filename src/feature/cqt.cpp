#include "feature/cqt.h"

#include <algorithm>
#include <cmath>
#include <memory>

#include "core/convert.h"
#include "core/fft.h"
#include "core/spectrum.h"
#include "feature/sparse_kernel.h"
#include "feature/spectral_projection.h"
#include "filters/wavelet.h"
#include "util/exception.h"
#include "util/lru_cache.h"
#include "util/math_utils.h"
#include "util/numeric_validation.h"

namespace sonare {

using sonare::constants::kEpsilon;

namespace {

/// @brief Snaps a float key field to a fixed grid so bitwise-different but
/// logically-equal UI inputs (e.g. 32.70 vs 32.7000007) collapse to the same
/// value before the key is hashed/compared. Quantizing at key construction is
/// what lets us keep strict `==` in the key (so the equal/hash contract holds)
/// while still getting cache hits for near-equal inputs.
float quantize(float value, float grid) { return std::round(value / grid) * grid; }

/// @brief Cache key for CQT kernel
struct CqtKernelCacheKey {
  int sample_rate;
  int hop_length;
  float fmin;
  int n_bins;
  int bins_per_octave;
  float filter_scale;
  WindowType window;

  // Exact equality so the equal/hash contract holds: the hash mixes the raw
  // float bits of fmin/filter_scale, so a fuzzy operator== would let two
  // logically-"equal" keys hash to different buckets and silently miss the
  // cache — triggering a redundant (expensive) wavelet + per-bin FFT rebuild —
  // and, on a bucket collision, could even return the wrong cached kernel. The
  // float fields are quantized at construction (see make_key), so exact
  // comparison still produces cache hits for near-equal inputs.
  bool operator==(const CqtKernelCacheKey& other) const {
    return sample_rate == other.sample_rate && hop_length == other.hop_length &&
           fmin == other.fmin && n_bins == other.n_bins &&
           bins_per_octave == other.bins_per_octave && filter_scale == other.filter_scale &&
           window == other.window;
  }
};

/// @brief Builds a CQT cache key with float fields snapped to fixed grids.
/// @details fmin is in Hz (0.001 Hz grid: fine enough to resolve real config
/// changes, coarse enough to absorb UI float noise); filter_scale is
/// dimensionless (1e-4 grid, matching the historical fuzzy tolerance).
CqtKernelCacheKey make_key(int sr, const CqtConfig& config) {
  constexpr float kFminGrid = 0.001f;        // Hz
  constexpr float kFilterScaleGrid = 1e-4f;  // dimensionless
  return CqtKernelCacheKey{sr,
                           config.hop_length,
                           quantize(config.fmin, kFminGrid),
                           config.n_bins,
                           config.bins_per_octave,
                           quantize(config.filter_scale, kFilterScaleGrid),
                           config.window};
}

struct CqtKernelCacheKeyHash {
  size_t operator()(const CqtKernelCacheKey& k) const {
    return std::hash<int>()(k.sample_rate) ^ (std::hash<int>()(k.hop_length) << 1) ^
           (std::hash<float>()(k.fmin) << 2) ^ (std::hash<int>()(k.n_bins) << 3) ^
           (std::hash<int>()(k.bins_per_octave) << 4) ^ (std::hash<float>()(k.filter_scale) << 5) ^
           (std::hash<int>()(static_cast<int>(k.window)) << 6);
  }
};

struct CachedCqtKernel {
  std::shared_ptr<CqtKernel> kernel;
};

/// @brief Maximum number of cached CQT kernels
constexpr size_t kMaxCqtCacheSize = 2;
// The public NNLS-chroma default uses 36 bins/octave over seven octaves. At
// 44.1 kHz its longest kernel requires 252 * 131072 elements, so the resource
// ceiling must admit that shipped configuration while still rejecting hostile
// low-fmin / oversized requests.
constexpr size_t kMaxCqtKernelElements = 32 * 1024 * 1024;
constexpr int kMaxCqtFftLength = 131072;

/// @brief Get or create a cached sparse CQT kernel.
CachedCqtKernel get_cached_kernel(int sr, const CqtConfig& config) {
  CqtKernelCacheKey key = make_key(sr, config);

  // Returned by value so the shared-pointer snapshot survives eviction; the
  // build runs under the lock (see LruCache::get_or_build_value).
  static LruCache<CqtKernelCacheKey, CachedCqtKernel, CqtKernelCacheKeyHash> cache(
      kMaxCqtCacheSize);
  return cache.get_or_build_value(key, [&]() -> CachedCqtKernel {
    std::shared_ptr<CqtKernel> kernel = CqtKernel::create(sr, config);

    return CachedCqtKernel{std::move(kernel)};
  });
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
    detail::fill_magnitude_cache(data_, power_cache_, magnitude_cache_);
  }
  return magnitude_cache_;
}

const std::vector<float>& CqtResult::power() const {
  if (power_cache_.empty() && !data_.empty()) {
    detail::fill_power_cache(data_, magnitude_cache_, power_cache_);
  }
  return power_cache_;
}

std::vector<float> CqtResult::to_db(float ref, float amin, float top_db) const {
  const std::vector<float>& pwr = power();
  std::vector<float> db(pwr.size());
  power_to_db(pwr.data(), pwr.size(), ref, amin, top_db, db.data());
  return db;
}

const std::complex<float>& CqtResult::at(int bin, int frame) const {
  SONARE_CHECK(bin >= 0 && bin < n_bins_, ErrorCode::InvalidParameter);
  SONARE_CHECK(frame >= 0 && frame < n_frames_, ErrorCode::InvalidParameter);
  return data_[bin * n_frames_ + frame];
}

// CqtKernel implementation
std::unique_ptr<CqtKernel> CqtKernel::create(int sr, const CqtConfig& config) {
  SONARE_CHECK(sr > 0 && config.hop_length > 0 && config.hop_length <= kMaxCqtFftLength / 2 &&
                   config.n_bins > 0 && config.bins_per_octave > 0 && std::isfinite(config.fmin) &&
                   config.fmin > 0.0f && std::isfinite(config.filter_scale) &&
                   config.filter_scale > 0.0f,
               ErrorCode::InvalidParameter);
  auto kernel = std::unique_ptr<CqtKernel>(new CqtKernel());

  // Center frequencies for all bins.
  kernel->frequencies_ = cqt_frequencies(config.fmin, config.n_bins, config.bins_per_octave);
  SONARE_CHECK(
      !kernel->frequencies_.empty() && kernel->frequencies_.back() < static_cast<float>(sr) * 0.5f,
      ErrorCode::InvalidParameter);
  kernel->n_bins_ = config.n_bins;

  // Raw fractional lengths from librosa's wavelet_lengths (bpo-based).
  std::vector<float> raw_lengths = wavelet_lengths(kernel->frequencies_, sr, config.filter_scale);
  const float longest =
      raw_lengths.empty() ? 0.0f : *std::max_element(raw_lengths.begin(), raw_lengths.end());
  int predicted_n_fft = next_power_of_2(std::max(2, static_cast<int>(std::ceil(longest))));
  if (config.hop_length > 0) {
    predicted_n_fft = std::max(predicted_n_fft, 2 * next_power_of_2(config.hop_length));
  }
  size_t predicted_elements = 0;
  SONARE_CHECK(predicted_n_fft <= kMaxCqtFftLength &&
                   numeric::checked_size_product(static_cast<size_t>(config.n_bins),
                                                 static_cast<size_t>(predicted_n_fft),
                                                 kMaxCqtKernelElements, &predicted_elements),
               ErrorCode::InvalidParameter);

  // Build the time-domain wavelet basis (Hann window * complex sinusoid,
  // L1-normalised, pad-centred to a common length n_fft = next_pow2(ceil(max))).
  int n_fft_basis = 0;
  std::vector<std::complex<float>> basis =
      wavelet(kernel->frequencies_, sr, config.filter_scale, /*is_cqt=*/true,
              /*pad_fft=*/true, /*Q=*/0.0f, &n_fft_basis);
  int n_fft = n_fft_basis;

  // librosa widens n_fft when it is shorter than 2 * next_pow2(hop_length).
  if (config.hop_length > 0) {
    int min_n_fft = 2 * next_power_of_2(config.hop_length);
    if (n_fft < min_n_fft) {
      // Re-pad each kernel into a wider slot.
      const int new_nfft = min_n_fft;
      std::vector<std::complex<float>> wider(static_cast<size_t>(config.n_bins) * new_nfft,
                                             std::complex<float>(0.0f, 0.0f));
      const int pad = (new_nfft - n_fft) / 2;
      for (int k = 0; k < config.n_bins; ++k) {
        for (int i = 0; i < n_fft; ++i) {
          wider[k * new_nfft + (pad + i)] = basis[k * n_fft + i];
        }
      }
      basis.swap(wider);
      n_fft = new_nfft;
    }
  }

  // Effective integer lengths (for backward-compatible reporting).
  kernel->lengths_.resize(config.n_bins);
  for (int k = 0; k < config.n_bins; ++k) {
    const float ilen = raw_lengths[k];
    const int s = static_cast<int>(std::floor(-ilen * 0.5f));
    const int e = static_cast<int>(std::floor(ilen * 0.5f));
    kernel->lengths_[k] = e - s;
  }
  kernel->fft_length_ = n_fft;

  // Scale each row by raw_length / n_fft (librosa.__vqt_filter_fft step).
  for (int k = 0; k < config.n_bins; ++k) {
    const float scale = raw_lengths[k] / static_cast<float>(n_fft);
    auto* row = basis.data() + static_cast<size_t>(k) * n_fft;
    for (int i = 0; i < n_fft; ++i) row[i] *= scale;
  }
  kernel->raw_lengths_ = std::move(raw_lengths);

  // FFT each kernel into frequency domain.
  FFT fft(n_fft);
  kernel->kernel_.rows = config.n_bins;
  kernel->kernel_.cols = n_fft;
  kernel->kernel_.row_offsets.reserve(static_cast<size_t>(config.n_bins) + 1);
  kernel->kernel_.row_offsets.push_back(0);
  std::vector<std::complex<float>> freq_kernel(n_fft);
  for (int k = 0; k < config.n_bins; ++k) {
    fft.forward_complex(basis.data() + static_cast<size_t>(k) * n_fft, freq_kernel.data());
    // We use the conjugate so that `result = stored_basis * FFT(signal)` evaluates
    // to a true cross-correlation at zero lag (i.e., the standard CQT inner
    // product `<kernel, signal>`). librosa's pipeline uses the convention
    // `fft_basis.dot(D)`, which is equivalent only because the wavelet is
    // single-sided in frequency (energy concentrated at +ω). Storing the
    // conjugate keeps our existing matmul path unchanged.
    for (auto& value : freq_kernel) value = std::conj(value);
    detail::append_sparsified_kernel_row(kernel->kernel_, freq_kernel.data(), n_fft);
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
  SONARE_CHECK(std::isfinite(config.fmin) && config.fmin > 0.0f &&
                   std::isfinite(config.filter_scale) && config.filter_scale > 0.0f,
               ErrorCode::InvalidParameter);

  int sr = audio.sample_rate();
  int n_samples = static_cast<int>(audio.size());

  // Get cached row-compressed CQT kernel.
  auto cached = get_cached_kernel(sr, config);
  auto& kernel = cached.kernel;

  int fft_length = kernel->fft_length();
  int n_bins = kernel->n_bins();
  const auto& kernel_matrix = kernel->kernel();

  // Center padding: pad signal by fft_length/2 on each side.
  // This ensures the first frame is centered at t=0 and produces the expected number of
  // frames: 1 + n_samples / hop_length (approximately).
  int pad_length = fft_length / 2;
  int padded_length = n_samples + 2 * pad_length;
  std::vector<float> padded_signal(padded_length, 0.0f);
  std::copy(audio.data(), audio.data() + n_samples, padded_signal.begin() + pad_length);

  // Calculate number of frames from padded signal
  int n_frames = 1 + (padded_length - fft_length) / config.hop_length;
  if (n_frames <= 0) {
    n_frames = 1;
  }

  // Pre-allocate output (promote to size_t before multiplying to avoid int overflow)
  size_t output_elements = 0;
  SONARE_CHECK(
      numeric::checked_size_product(static_cast<size_t>(n_bins), static_cast<size_t>(n_frames),
                                    kMaxAudioBufferSize, &output_elements),
      ErrorCode::InvalidParameter);
  std::vector<std::complex<float>> output(output_elements);

  // Create FFT processor
  FFT fft(fft_length);

  // Pre-allocate temporary buffers (complex FFT for full spectrum)
  std::vector<float> frame(fft_length, 0.0f);
  std::vector<std::complex<float>> complex_frame(fft_length, {0.0f, 0.0f});
  std::vector<std::complex<float>> frame_fft(fft_length);

  const float* data = padded_signal.data();

  // Per-bin /sqrt(L) factor for `scale=True` mode (the librosa default).
  const auto& raw_lengths = kernel->raw_lengths();
  std::vector<float> inv_sqrt_len(n_bins, 1.0f);
  for (int k = 0; k < n_bins; ++k) {
    if (raw_lengths[k] > 0.0f) {
      inv_sqrt_len[k] = 1.0f / std::sqrt(raw_lengths[k]);
    }
  }

  // Progress reporting interval
  int progress_interval = std::max(1, n_frames / 20);

  // Process each frame against the sparse kernel.
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

    // Apply per-bin /sqrt(length) (librosa.vqt with scale=True).
    for (int k = 0; k < n_bins; ++k) {
      output[k * n_frames + t] =
          detail::sparse_kernel_row_dot(kernel_matrix, k, frame_fft.data()) * inv_sqrt_len[k];
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

  const int n_bins = cqt_result.n_bins();
  const int n_frames = cqt_result.n_frames();
  const int hop_length = cqt_result.hop_length();
  const int sr = cqt_result.sample_rate();
  const auto& frequencies = cqt_result.frequencies();
  if (frequencies.empty()) {
    return Audio();
  }

  int bins_per_octave = static_cast<int>(constants::kSemitonesPerOctave);
  if (frequencies.size() >= 2 && frequencies[0] > 0.0f && frequencies[1] > frequencies[0]) {
    const float ratio = frequencies[1] / frequencies[0];
    const float estimated_bpo = 1.0f / std::log2(ratio);
    bins_per_octave = std::max(1, static_cast<int>(std::lround(estimated_bpo)));
  }

  CqtConfig config;
  config.hop_length = hop_length;
  config.fmin = frequencies.front();
  config.n_bins = n_bins;
  config.bins_per_octave = bins_per_octave;

  auto cached = get_cached_kernel(sr, config);
  const auto& kernel = *cached.kernel;
  const auto& basis = kernel.kernel();
  const auto& lengths = kernel.raw_lengths();
  const int n_fft = kernel.fft_length();
  const int n_freq = n_fft / 2 + 1;

  // Per-bin reciprocal kernel power. The stored basis is `conj(FFT(wavelet))`
  // for an analytic-like wavelet, so its energy is concentrated on one half of
  // the spectrum (whichever half holds +ω after the conjugate). The previous
  // implementation summed only `[0, n_fft/2]`, missing most of that energy and
  // doubling the reconstructed amplitude. Match librosa's icqt: sum over all
  // `n_fft` bins of `|inv_basis|^2` so `freq_power = (n_fft / length) /
  // sum(|basis|^2)` is computed against the full kernel energy.
  std::vector<float> freq_power(n_bins, 0.0f);
  for (int k = 0; k < n_bins; ++k) {
    double power = 0.0;
    const int begin = basis.row_offsets[static_cast<size_t>(k)];
    const int end = basis.row_offsets[static_cast<size_t>(k + 1)];
    for (int index = begin; index < end; ++index) {
      power += std::norm(basis.values[static_cast<size_t>(index)]);
    }
    if (power > 0.0 && lengths[k] > 0.0f) {
      freq_power[k] = static_cast<float>((static_cast<double>(n_fft) / lengths[k]) / power);
    }
  }

  std::vector<float> output_padded(static_cast<size_t>(n_fft + (n_frames - 1) * hop_length), 0.0f);
  std::vector<float> weight(output_padded.size(), 0.0f);
  std::vector<std::complex<float>> spectrum(n_freq);
  std::vector<float> frame(n_fft, 0.0f);
  FFT fft(n_fft);

  for (int t = 0; t < n_frames; ++t) {
    std::fill(spectrum.begin(), spectrum.end(), std::complex<float>(0.0f, 0.0f));
    for (int k = 0; k < n_bins; ++k) {
      const float scale = std::sqrt(std::max(lengths[k], 0.0f)) * freq_power[k];
      const std::complex<float> coefficient = scale * cqt_result.at(k, t);
      const int begin = basis.row_offsets[static_cast<size_t>(k)];
      const int end = basis.row_offsets[static_cast<size_t>(k + 1)];
      for (int index = begin; index < end; ++index) {
        const int bin = basis.column_indices[static_cast<size_t>(index)];
        if (bin < n_freq) {
          spectrum[static_cast<size_t>(bin)] +=
              std::conj(basis.values[static_cast<size_t>(index)]) * coefficient;
        }
      }
    }
    fft.inverse(spectrum.data(), frame.data());

    // librosa's icqt delegates OLA to `istft(window="ones")`, which divides
    // the sum-of-frames by the per-sample sum of `window**2 == 1`, i.e. the
    // overlap count. We replicate that here with `weight[idx] += 1`.
    const int start = t * hop_length;
    for (int n = 0; n < n_fft; ++n) {
      const size_t idx = static_cast<size_t>(start + n);
      output_padded[idx] += frame[n];
      weight[idx] += 1.0f;
    }
  }

  // OLA coverage counter is integer-like; this floor only skips uncovered
  // padded tails and avoids division by exact zero.
  constexpr float kOverlapWeightFloor = 1.0e-6f;
  for (size_t i = 0; i < output_padded.size(); ++i) {
    if (weight[i] > kOverlapWeightFloor) output_padded[i] /= weight[i];
  }

  const int crop_start = n_fft / 2;
  int output_length =
      length > 0 ? length : std::max(0, static_cast<int>(output_padded.size()) - 2 * crop_start);
  std::vector<float> output(static_cast<size_t>(output_length), 0.0f);
  for (int i = 0; i < output_length; ++i) {
    const int src = crop_start + i;
    if (src >= 0 && src < static_cast<int>(output_padded.size())) output[i] = output_padded[src];
  }

  return Audio::from_vector(std::move(output), sr);
}

CqtResult pseudo_cqt(const Audio& audio, const CqtConfig& config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(config.hop_length > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(config.n_bins > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(config.bins_per_octave > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(config.fmin > 0.0f, ErrorCode::InvalidParameter);

  const int sr = audio.sample_rate();
  std::vector<float> freqs = cqt_frequencies(config.fmin, config.n_bins, config.bins_per_octave);

  // Single STFT magnitude at an n_fft chosen to capture the lowest bin's Q.
  const int n_fft = detail::choose_pseudo_cqt_nfft(config, sr);
  StftConfig stft_cfg;
  stft_cfg.n_fft = n_fft;
  stft_cfg.hop_length = config.hop_length;
  stft_cfg.window = config.window;
  stft_cfg.center = true;
  Spectrogram spec = Spectrogram::compute(audio, stft_cfg);
  const std::vector<float>& mag = spec.magnitude();
  const int n_freq = spec.n_bins();
  const int n_frames = spec.n_frames();

  const float bin_to_hz = static_cast<float>(sr) / static_cast<float>(n_fft);
  const float semitone_ratio =
      std::pow(2.0f, 1.0f / static_cast<float>(std::max(config.bins_per_octave, 1)));
  std::vector<float> bandwidths(freqs.size(), 0.0f);
  for (size_t k = 0; k < freqs.size(); ++k) {
    bandwidths[k] = freqs[k] * (semitone_ratio - 1.0f);
  }
  std::vector<float> P = detail::build_cqt_projection(freqs, bandwidths, n_freq, bin_to_hz);
  const std::vector<float> lengths = wavelet_lengths(freqs, sr, config.filter_scale);

  // C = P @ |STFT|. Phase is not estimated (pseudo CQT yields magnitudes only;
  // we store the result in the real part of the CqtResult so magnitude()
  // returns it).
  std::vector<std::complex<float>> data(static_cast<size_t>(config.n_bins) * n_frames,
                                        std::complex<float>(0.0f, 0.0f));
  for (int k = 0; k < config.n_bins; ++k) {
    const float scale = lengths[static_cast<size_t>(k)] > 0.0f
                            ? 1.0f / std::sqrt(lengths[static_cast<size_t>(k)])
                            : 1.0f;
    for (int t = 0; t < n_frames; ++t) {
      float acc = 0.0f;
      for (int b = 0; b < n_freq; ++b) {
        acc += P[k * n_freq + b] * mag[b * n_frames + t];
      }
      data[k * n_frames + t] = std::complex<float>(acc * scale, 0.0f);
    }
  }
  return CqtResult(std::move(data), config.n_bins, n_frames, std::move(freqs), config.hop_length,
                   sr);
}

CqtResult hybrid_cqt(const Audio& audio, const CqtConfig& config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(config.hop_length > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(config.n_bins > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(config.bins_per_octave > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(config.fmin > 0.0f, ErrorCode::InvalidParameter);

  const int sr = audio.sample_rate();
  const float Q = compute_q(config.bins_per_octave, config.filter_scale);
  std::vector<float> freqs = cqt_frequencies(config.fmin, config.n_bins, config.bins_per_octave);

  // Split point: bins whose CQT filter is shorter than `short_threshold` use
  // pseudo CQT (cheap STFT projection); longer-filter bins fall back to the
  // full CQT (slow but accurate). librosa's threshold is roughly 2 * hop;
  // 256 samples is a reasonable practical default.
  const int short_threshold = std::max(256, 2 * config.hop_length);
  int n_split = config.n_bins;
  for (int k = 0; k < config.n_bins; ++k) {
    const int len = static_cast<int>(std::ceil(Q * sr / std::max(freqs[k], 1.0f)));
    if (len <= short_threshold) {
      n_split = k;
      break;
    }
  }

  std::vector<std::complex<float>> data;
  int n_frames = 0;

  if (n_split > 0) {
    CqtConfig low = config;
    low.n_bins = n_split;
    CqtResult low_result = cqt(audio, low);
    n_frames = low_result.n_frames();
    data.assign(static_cast<size_t>(config.n_bins) * n_frames, std::complex<float>(0.0f, 0.0f));
    for (int k = 0; k < n_split; ++k) {
      for (int t = 0; t < n_frames; ++t) {
        data[k * n_frames + t] = low_result.at(k, t);
      }
    }
  }
  if (n_split < config.n_bins) {
    CqtConfig high = config;
    high.n_bins = config.n_bins - n_split;
    high.fmin = freqs[n_split];
    CqtResult high_result = pseudo_cqt(audio, high);

    if (n_frames == 0) {
      n_frames = high_result.n_frames();
      data.assign(static_cast<size_t>(config.n_bins) * n_frames, std::complex<float>(0.0f, 0.0f));
    }
    const int copy_n = std::min(n_frames, high_result.n_frames());
    for (int k = n_split; k < config.n_bins; ++k) {
      for (int t = 0; t < copy_n; ++t) {
        data[k * n_frames + t] = high_result.at(k - n_split, t);
      }
    }
  }
  if (n_frames == 0) return CqtResult();
  return CqtResult(std::move(data), config.n_bins, n_frames, std::move(freqs), config.hop_length,
                   sr);
}

Audio griffinlim_cqt(const float* magnitude, int n_bins, int n_frames, const CqtConfig& config,
                     int sr, int n_iter) {
  if (magnitude == nullptr || n_bins <= 0 || n_frames <= 0) return Audio();

  std::vector<float> freqs = cqt_frequencies(config.fmin, n_bins, config.bins_per_octave);
  const int n_fft = detail::choose_pseudo_cqt_nfft(config, sr);
  const int n_freq = n_fft / 2 + 1;
  const float bin_to_hz = static_cast<float>(sr) / static_cast<float>(n_fft);

  // Build the same Gaussian projection as pseudo_cqt and use its transpose to
  // smear CQT magnitudes back onto an STFT magnitude grid. This is a smoother
  // seed for Griffin-Lim than the naive nearest-bin projection.
  const float semitone_ratio =
      std::pow(2.0f, 1.0f / static_cast<float>(std::max(config.bins_per_octave, 1)));
  std::vector<float> bandwidths(freqs.size(), 0.0f);
  for (size_t k = 0; k < freqs.size(); ++k) {
    bandwidths[k] = freqs[k] * (semitone_ratio - 1.0f);
  }
  std::vector<float> P = detail::build_cqt_projection(freqs, bandwidths, n_freq, bin_to_hz);

  std::vector<float> stft_mag(static_cast<size_t>(n_freq) * n_frames, 0.0f);
  for (int b = 0; b < n_freq; ++b) {
    for (int t = 0; t < n_frames; ++t) {
      float acc = 0.0f;
      for (int k = 0; k < n_bins; ++k) {
        acc += P[k * n_freq + b] * magnitude[k * n_frames + t];
      }
      stft_mag[b * n_frames + t] = acc;
    }
  }

  GriffinLimConfig gcfg;
  gcfg.n_iter = n_iter;
  return griffin_lim(stft_mag.data(), n_freq, n_frames, n_fft, config.hop_length, sr, gcfg);
}

int chroma_class_of_frequency(float hz, int n_chroma) {
  if (hz <= 0.0f || n_chroma <= 0) return 0;

  // Chroma classes are subdivisions of the twelve pitch classes, not of the
  // absolute MIDI number. Reduce the MIDI pitch modulo one octave before
  // scaling and rounding it to the requested chroma resolution, matching
  // wavelet::cq_to_chroma/librosa.filters.cq_to_chroma at tuning=0.
  const double midi_pitch_class = std::fmod(static_cast<double>(hz_to_midi(hz)), 12.0);
  int chroma_class =
      static_cast<int>(std::lround(midi_pitch_class * (static_cast<double>(n_chroma) / 12.0)));
  chroma_class %= n_chroma;
  if (chroma_class < 0) chroma_class += n_chroma;
  return chroma_class;
}

std::vector<float> fold_cqt_bins_to_chroma(const float* mag, int n_bins, int n_frames,
                                           int bins_per_octave, int n_chroma, int fmin_pitch_class,
                                           ChromaFold aggregation, std::vector<int>* counts) {
  if (n_chroma <= 0 || bins_per_octave <= 0 || n_frames < 0 || n_bins < 0) {
    if (counts) counts->clear();
    return {};
  }
  std::vector<float> chroma(static_cast<size_t>(n_chroma) * n_frames, 0.0f);
  std::vector<int> local_counts(n_chroma, 0);
  if (counts) counts->assign(n_chroma, 0);

  const int n_merge = bins_per_octave / n_chroma;
  const int merge_half = n_merge / 2;

  for (int k = 0; k < n_bins; ++k) {
    const int shifted = ((k % bins_per_octave) + merge_half) % bins_per_octave;
    const int within_octave = shifted * n_chroma / bins_per_octave;
    const int chroma_bin = ((within_octave + fmin_pitch_class) % n_chroma + n_chroma) % n_chroma;
    local_counts[chroma_bin] += 1;
    if (counts) (*counts)[chroma_bin] += 1;
    for (int t = 0; t < n_frames; ++t) {
      chroma[chroma_bin * n_frames + t] += mag[k * n_frames + t];
    }
  }

  if (aggregation == ChromaFold::kMean) {
    for (int c = 0; c < n_chroma; ++c) {
      if (local_counts[c] > 0) {
        const float denom = static_cast<float>(local_counts[c]);
        for (int t = 0; t < n_frames; ++t) {
          chroma[c * n_frames + t] /= denom;
        }
      }
    }
  }
  return chroma;
}

std::vector<float> accumulate_cqt_pitch_classes(const std::vector<float>& mag, int n_bins,
                                                int n_frames, int n_chroma,
                                                const std::vector<float>& freqs,
                                                std::vector<int>* counts) {
  // The CQT result carries its grid only as a frequency axis, so recover the
  // bin spacing and the pitch class of bin 0 before handing them to the shared
  // fold.
  int bins_per_octave = static_cast<int>(constants::kSemitonesPerOctave);
  if (freqs.size() >= 2 && freqs[0] > 0.0f && freqs[1] > freqs[0]) {
    const float ratio = freqs[1] / freqs[0];
    bins_per_octave = std::max(1, static_cast<int>(std::lround(1.0f / std::log2(ratio))));
  }
  const int fmin_pitch_class = freqs.empty() ? 0 : chroma_class_of_frequency(freqs[0], n_chroma);

  return fold_cqt_bins_to_chroma(mag.data(), n_bins, n_frames, bins_per_octave, n_chroma,
                                 fmin_pitch_class, ChromaFold::kSum, counts);
}

std::vector<float> cqt_to_chroma(const CqtResult& cqt_result, int n_chroma) {
  if (cqt_result.empty()) {
    return {};
  }

  int n_bins = cqt_result.n_bins();
  int n_frames = cqt_result.n_frames();

  // Sum CQT magnitudes per pitch class. NOTE: this departs from librosa's
  // unnormalized cq_to_chroma fold by additionally L-inf normalizing each frame
  // below; the divergent normalization is intentional for this entry point and is
  // kept bit-for-bit. The chroma_cqt/bass_chroma path uses wrap_cqt_to_chroma
  // (mean aggregation + caller-controlled normalization) instead.
  std::vector<float> chroma = accumulate_cqt_pitch_classes(
      cqt_result.magnitude(), n_bins, n_frames, n_chroma, cqt_result.frequencies(), nullptr);

  // Normalize each frame (L-inf)
  for (int t = 0; t < n_frames; ++t) {
    float max_val = 0.0f;
    for (int c = 0; c < n_chroma; ++c) {
      max_val = std::max(max_val, chroma[c * n_frames + t]);
    }
    if (max_val > constants::kEpsilon) {
      for (int c = 0; c < n_chroma; ++c) {
        chroma[c * n_frames + t] /= max_val;
      }
    }
  }

  return chroma;
}

}  // namespace sonare
