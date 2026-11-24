#include "filters/mel.h"

#include <Eigen/Core>
#include <memory>

#include "core/convert.h"
#include "util/exception.h"
#include "util/lru_cache.h"
#include "util/parameter_checks.h"
#include "util/zero_is_default.h"

namespace sonare {

namespace {

constexpr util::check::NumericDomain kMelSampleRateDomain = util::check::integer_domain_of<int>(1);
constexpr util::check::NumericDomain kMelFftSizeDomain = util::check::integer_domain_of<int>(1);
constexpr util::check::NumericDomain kMelBinCountDomain = util::check::integer_domain_of<int>(1);
constexpr util::check::NumericDomain kMelFrameCountDomain = util::check::integer_domain_of<int>(1);
constexpr util::check::NumericDomain kMelApplyBandCountDomain =
    util::check::integer_domain_of<int>(1);

/// @brief Cache key for Mel filterbank.
struct MelFilterbankCacheKey {
  int sample_rate;
  int n_fft;
  int n_mels;
  float fmin;
  float fmax;
  bool htk;
  MelNorm norm;

  // Exact equality so the equal/hash contract holds: the hash mixes the raw float
  // bits of fmin/fmax, so a fuzzy operator== would let two logically-"equal" keys
  // hash to different buckets and silently miss the cache. Any fmax==0 -> sr/2
  // normalization is already applied before the key is constructed, so exact
  // comparison is correct here.
  bool operator==(const MelFilterbankCacheKey& other) const {
    return sample_rate == other.sample_rate && n_fft == other.n_fft && n_mels == other.n_mels &&
           htk == other.htk && norm == other.norm && fmin == other.fmin && fmax == other.fmax;
  }
};

struct MelFilterbankCacheKeyHash {
  size_t operator()(const MelFilterbankCacheKey& k) const {
    return std::hash<int>()(k.sample_rate) ^ (std::hash<int>()(k.n_fft) << 1) ^
           (std::hash<int>()(k.n_mels) << 2) ^ (std::hash<int>()(static_cast<int>(k.htk)) << 3) ^
           (std::hash<int>()(static_cast<int>(k.norm)) << 4) ^ (std::hash<float>()(k.fmin) << 5) ^
           (std::hash<float>()(k.fmax) << 6);
  }
};

/// @brief Maximum number of cached Mel filterbanks.
constexpr size_t kMaxMelCacheSize = 8;

float checked_mel_filterbank_fmax(int sr, int n_fft, const MelFilterConfig& config) {
  util::check::integer_within<int>(sr, kMelSampleRateDomain, "sample_rate");
  util::check::integer_within<int>(n_fft, kMelFftSizeDomain, "n_fft");
  util::check::integer_within<int>(config.n_mels, kMelBandsDomain, "n_mels");
  return validate_mel_bounds(config.fmin, config.fmax, sr);
}

}  // namespace

float validate_mel_bounds(float fmin, float fmax, int sample_rate) {
  validate_mel_bounds(fmin, fmax);
  const double nyquist = static_cast<double>(sample_rate) / 2.0;
  util::check::at_most(fmax, nyquist, "fmax");
  const float resolved_fmax = ZeroIsDefault(fmax).or_default(static_cast<float>(nyquist));
  if (!(resolved_fmax > fmin)) {
    util::check::refuse_relation({"fmax", "fmin"}, "{0} must be greater than {1}");
  }
  return resolved_fmax;
}

std::vector<float> mel_frequency_points(int n_points, float fmin, float fmax, bool htk) {
  util::check::integer_within<int>(n_points, kMelFrequencyCountDomain, "n_points");
  if (!(fmax > fmin))
    util::check::refuse_relation({"fmax", "fmin"}, "{0} must be greater than {1}");

  /// Convert frequency range to Mel scale
  float mel_min = htk ? hz_to_mel_htk(fmin) : hz_to_mel(fmin);
  float mel_max = htk ? hz_to_mel_htk(fmax) : hz_to_mel(fmax);

  /// Create n_points points equally spaced in Mel scale; a single point is the start.
  std::vector<float> freqs(n_points);

  for (int i = 0; i < n_points; ++i) {
    float mel = n_points == 1 ? mel_min : mel_min + (mel_max - mel_min) * i / (n_points - 1);
    freqs[i] = htk ? mel_to_hz_htk(mel) : mel_to_hz(mel);
  }

  return freqs;
}

std::vector<float> create_mel_filterbank(int sr, int n_fft, const MelFilterConfig& config) {
  const float fmax = checked_mel_filterbank_fmax(sr, n_fft, config);
  const float fmin = config.fmin;
  const int n_bins = n_fft / 2 + 1;

  /// Get the band edges: n_mels centres plus the two outer edges
  std::vector<float> mel_freqs = mel_frequency_points(config.n_mels + 2, fmin, fmax, config.htk);

  /// Convert frequencies to FFT bin indices (can be fractional)
  std::vector<float> bin_freqs(mel_freqs.size());
  float bin_width = static_cast<float>(sr) / n_fft;
  for (size_t i = 0; i < mel_freqs.size(); ++i) {
    bin_freqs[i] = mel_freqs[i] / bin_width;
  }

  /// Create filterbank matrix [n_mels x n_bins]
  // size_t operands: the int product wraps for a large band count, and a wrapped
  // extent sizes the buffer while the loop below still writes n_mels * n_bins.
  std::vector<float> filterbank(static_cast<size_t>(config.n_mels) * static_cast<size_t>(n_bins),
                                0.0f);

  for (int m = 0; m < config.n_mels; ++m) {
    float* row = filterbank.data() + static_cast<size_t>(m) * static_cast<size_t>(n_bins);
    float left = bin_freqs[m];
    float center = bin_freqs[m + 1];
    float right = bin_freqs[m + 2];

    /// Create triangular filter
    for (int k = 0; k < n_bins; ++k) {
      float bin = static_cast<float>(k);

      if (bin >= left && bin <= center) {
        /// Rising edge
        if (center > left) {
          row[k] = (bin - left) / (center - left);
        }
      } else if (bin > center && bin <= right) {
        /// Falling edge
        if (right > center) {
          row[k] = (right - bin) / (right - center);
        }
      }
    }

    /// Apply Slaney normalization (area normalization)
    if (config.norm == MelNorm::Slaney) {
      float enorm = 2.0f / (mel_freqs[m + 2] - mel_freqs[m]);
      for (int k = 0; k < n_bins; ++k) {
        row[k] *= enorm;
      }
    }
  }

  return filterbank;
}

std::shared_ptr<const std::vector<float>> get_mel_filterbank_cached(int sr, int n_fft,
                                                                    const MelFilterConfig& config) {
  const float fmax = checked_mel_filterbank_fmax(sr, n_fft, config);
  MelFilterbankCacheKey key{sr, n_fft, config.n_mels, config.fmin, fmax, config.htk, config.norm};

  // Cache shared_ptrs to immutable filterbanks: create_mel_filterbank is the
  // expensive part (built outside the lock), and the returned handle keeps the
  // vector alive even if another thread evicts the cache entry concurrently.
  static LruCache<MelFilterbankCacheKey, std::shared_ptr<const std::vector<float>>,
                  MelFilterbankCacheKeyHash>
      cache(kMaxMelCacheSize);
  return cache.get_or_build(key, [&] {
    return std::make_shared<const std::vector<float>>(create_mel_filterbank(sr, n_fft, config));
  });
}

std::vector<float> apply_mel_filterbank(const float* power, int n_bins, int n_frames,
                                        const float* filterbank, int n_mels) {
  util::check::non_null(power, "power");
  util::check::non_null(filterbank, "filterbank");
  util::check::integer_within<int>(n_bins, kMelBinCountDomain, "n_bins");
  util::check::integer_within<int>(n_frames, kMelFrameCountDomain, "n_frames");
  util::check::integer_within<int>(n_mels, kMelApplyBandCountDomain, "n_mels");

  // Output: [n_mels x n_frames], extent in size_t so a long signal cannot wrap it.
  std::vector<float> mel_spec(static_cast<size_t>(n_mels) * static_cast<size_t>(n_frames));

  // Use Eigen for optimized matrix multiplication
  // filterbank: [n_mels x n_bins] (row-major)
  // power: [n_bins x n_frames] (row-major)
  // result: [n_mels x n_frames] (row-major)
  Eigen::Map<const Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>> fb_map(
      filterbank, n_mels, n_bins);
  Eigen::Map<const Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>> power_map(
      power, n_bins, n_frames);
  Eigen::Map<Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>> result_map(
      mel_spec.data(), n_mels, n_frames);

  // BLAS-optimized matrix multiplication
  result_map.noalias() = fb_map * power_map;

  return mel_spec;
}

}  // namespace sonare
