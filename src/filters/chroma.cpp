#include "filters/chroma.h"

#include <Eigen/Core>
#include <cmath>
#include <memory>

#include "core/convert.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/lru_cache.h"

namespace sonare {

namespace {

/// @brief Snaps a float key field to a fixed grid so bitwise-different but
/// logically-equal UI inputs (e.g. 32.70 vs 32.7000007) collapse to the same
/// value before the key is hashed/compared. Quantizing at key construction is
/// what lets us keep strict `==` in the key (so the equal/hash contract holds)
/// while still getting cache hits for near-equal inputs.
float quantize(float value, float grid) { return std::round(value / grid) * grid; }

/// @brief Cache key for Chroma filterbank.
struct ChromaFilterbankCacheKey {
  int sample_rate;
  int n_fft;
  int n_chroma;
  float tuning;
  float ctroct;
  float octwidth;
  bool base_c;
  ChromaFilterNorm norm;

  // Exact equality so the equal/hash contract holds: the hash mixes the raw
  // float bits, so a fuzzy operator== would let two logically-"equal" keys hash
  // to different buckets and silently miss the cache (and, on a bucket
  // collision, could even return the wrong entry). The float fields are
  // quantized at construction (see make_key), so exact comparison still produces
  // cache hits for near-equal inputs.
  bool operator==(const ChromaFilterbankCacheKey& other) const {
    return sample_rate == other.sample_rate && n_fft == other.n_fft && n_chroma == other.n_chroma &&
           norm == other.norm && tuning == other.tuning && ctroct == other.ctroct &&
           octwidth == other.octwidth && base_c == other.base_c;
  }
};

/// @brief Builds a Chroma cache key with float fields snapped to fixed grids.
/// @details tuning is in fractions of a chroma bin; fmin is in Hz. Grids match
/// the historical fuzzy tolerances (1e-4) so previously-distinct keys stay
/// distinct while float noise collapses.
ChromaFilterbankCacheKey make_key(int sr, int n_fft, const ChromaFilterConfig& config) {
  constexpr float kGrid = 1e-4f;
  return ChromaFilterbankCacheKey{sr,
                                  n_fft,
                                  config.n_chroma,
                                  quantize(config.tuning, kGrid),
                                  quantize(config.ctroct, kGrid),
                                  quantize(config.octwidth, kGrid),
                                  config.base_c,
                                  config.norm};
}

struct ChromaFilterbankCacheKeyHash {
  size_t operator()(const ChromaFilterbankCacheKey& k) const {
    return std::hash<int>()(k.sample_rate) ^ (std::hash<int>()(k.n_fft) << 1) ^
           (std::hash<int>()(k.n_chroma) << 2) ^ (std::hash<bool>()(k.base_c) << 3) ^
           (std::hash<int>()(static_cast<int>(k.norm)) << 4) ^ (std::hash<float>()(k.tuning) << 5) ^
           (std::hash<float>()(k.ctroct) << 6) ^ (std::hash<float>()(k.octwidth) << 7);
  }
};

/// @brief Maximum number of cached Chroma filterbanks.
constexpr size_t kMaxChromaCacheSize = 8;

}  // namespace

int hz_to_pitch_class(float hz, float tuning) {
  if (hz <= 0.0f) {
    return -1;
  }
  float chroma = hz_to_chroma(hz, tuning);
  return static_cast<int>(chroma) % 12;
}

float hz_to_chroma(float hz, float tuning) {
  if (hz <= 0.0f) {
    return -1.0f;
  }

  // Convert Hz to MIDI note number (with tuning adjustment)
  float midi = hz_to_midi(hz) - tuning;

  // Extract fractional pitch class [0, 12)
  float chroma = std::fmod(midi, constants::kSemitonesPerOctave);
  if (chroma < 0.0f) {
    chroma += constants::kSemitonesPerOctave;
  }
  return chroma;
}

std::vector<float> create_chroma_filterbank(int sr, int n_fft, const ChromaFilterConfig& config) {
  SONARE_CHECK(sr > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(n_fft > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(config.n_chroma > 0, ErrorCode::InvalidParameter);

  // Direct port of librosa.filters.chroma: Gaussian bumps per FFT bin, an
  // optional per-octave Gaussian envelope (ctroct/octwidth), column (axis=0)
  // normalization, and a base-C rotation. See tests/librosa/reference/chroma.json.
  const int n_bins = n_fft / 2 + 1;
  const int n_chroma = config.n_chroma;
  const double a440 =
      constants::kA4Hz * std::pow(2.0, static_cast<double>(config.tuning) / n_chroma);

  // frqbins[k] = n_chroma * log2(freq_k * 16 / A440); bin 0 is a synthetic value
  // 1.5 octaves below bin 1 (librosa's broad 0 Hz handling). We need one extra
  // bin (n_bins) to derive the bin-width difference of the last kept column.
  auto frqbin = [&](int k) -> double {
    const double freq = static_cast<double>(k) * sr / n_fft;
    return n_chroma * std::log2(freq * 16.0 / a440);
  };
  std::vector<double> frqbins(n_bins + 1);
  frqbins[1] = frqbin(1);
  frqbins[0] = frqbins[1] - 1.5 * n_chroma;
  for (int k = 2; k <= n_bins; ++k) frqbins[k] = frqbin(k);

  std::vector<float> filterbank(static_cast<size_t>(n_chroma) * n_bins, 0.0f);
  const double n_chroma2 = std::round(0.5 * n_chroma);
  const int roll = 3 * (n_chroma / 12);  // base_c rotation (rows shift up by 3 per 12)

  for (int k = 0; k < n_bins; ++k) {
    const double fb = frqbins[k];
    const double binwidth = std::max(frqbins[k + 1] - frqbins[k], 1.0);

    // Gaussian bumps over the chroma circle (2*D narrows them like librosa).
    std::vector<double> col(n_chroma);
    for (int c = 0; c < n_chroma; ++c) {
      double d = fb - c;
      d = std::fmod(d + n_chroma2 + 10.0 * n_chroma, static_cast<double>(n_chroma)) - n_chroma2;
      const double z = 2.0 * d / binwidth;
      col[c] = std::exp(-0.5 * z * z);
    }

    // Column normalization (axis=0) — librosa default L2.
    if (config.norm != ChromaFilterNorm::None) {
      double scale = 0.0;
      if (config.norm == ChromaFilterNorm::L1) {
        for (double v : col) scale += v;
      } else {
        for (double v : col) scale += v * v;
        scale = std::sqrt(scale);
      }
      if (scale > 0.0) {
        for (double& v : col) v /= scale;
      }
    }

    // Per-octave Gaussian envelope on the FFT bin (octwidth <= 0 disables it).
    if (config.octwidth > 0.0f) {
      const double oct = fb / n_chroma - config.ctroct;
      const double env = std::exp(-0.5 * (oct / config.octwidth) * (oct / config.octwidth));
      for (double& v : col) v *= env;
    }

    // base_c: write the rolled row so pitch class 0 aligns with C.
    for (int c = 0; c < n_chroma; ++c) {
      const int dst = roll == 0 ? c : ((c - roll) % n_chroma + n_chroma) % n_chroma;
      filterbank[static_cast<size_t>(dst) * n_bins + k] = static_cast<float>(col[c]);
    }
  }

  return filterbank;
}

std::shared_ptr<const std::vector<float>> get_chroma_filterbank_cached(
    int sr, int n_fft, const ChromaFilterConfig& config) {
  ChromaFilterbankCacheKey key = make_key(sr, n_fft, config);

  // Cache shared_ptrs to immutable filterbanks — see get_mel_filterbank_cached
  // for rationale (build outside the lock; handle survives concurrent eviction).
  static LruCache<ChromaFilterbankCacheKey, std::shared_ptr<const std::vector<float>>,
                  ChromaFilterbankCacheKeyHash>
      cache(kMaxChromaCacheSize);
  return cache.get_or_build(key, [&] {
    return std::make_shared<const std::vector<float>>(create_chroma_filterbank(sr, n_fft, config));
  });
}

std::vector<float> apply_chroma_filterbank(const float* power, int n_bins, int n_frames,
                                           const float* filterbank, int n_chroma) {
  SONARE_CHECK(power != nullptr, ErrorCode::InvalidParameter);
  SONARE_CHECK(filterbank != nullptr, ErrorCode::InvalidParameter);

  // Output: [n_chroma x n_frames]
  std::vector<float> chromagram(n_chroma * n_frames);

  // Use Eigen for optimized matrix multiplication
  // filterbank: [n_chroma x n_bins] (row-major)
  // power: [n_bins x n_frames] (row-major)
  // result: [n_chroma x n_frames] (row-major)
  Eigen::Map<const Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>> fb_map(
      filterbank, n_chroma, n_bins);
  Eigen::Map<const Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>> power_map(
      power, n_bins, n_frames);
  Eigen::Map<Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>> result_map(
      chromagram.data(), n_chroma, n_frames);

  // BLAS-optimized matrix multiplication
  result_map.noalias() = fb_map * power_map;

  return chromagram;
}

}  // namespace sonare
