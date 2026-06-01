#include "filters/chroma.h"

#include <Eigen/Core>
#include <cmath>
#include <list>
#include <mutex>
#include <unordered_map>

#include "core/convert.h"
#include "util/constants.h"
#include "util/exception.h"

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
  float fmin;
  int n_octaves;
  ChromaFilterNorm norm;

  // Exact equality so the equal/hash contract holds: the hash mixes the raw
  // float bits of tuning/fmin, so a fuzzy operator== would let two
  // logically-"equal" keys hash to different buckets and silently miss the
  // cache (and, on a bucket collision, could even return the wrong entry). The
  // float fields are quantized at construction (see make_key), so exact
  // comparison still produces cache hits for near-equal inputs.
  bool operator==(const ChromaFilterbankCacheKey& other) const {
    return sample_rate == other.sample_rate && n_fft == other.n_fft && n_chroma == other.n_chroma &&
           n_octaves == other.n_octaves && norm == other.norm && tuning == other.tuning &&
           fmin == other.fmin;
  }
};

/// @brief Builds a Chroma cache key with float fields snapped to fixed grids.
/// @details tuning is in fractions of a chroma bin; fmin is in Hz. Grids match
/// the historical fuzzy tolerances (1e-4) so previously-distinct keys stay
/// distinct while float noise collapses.
ChromaFilterbankCacheKey make_key(int sr, int n_fft, const ChromaFilterConfig& config) {
  constexpr float kTuningGrid = 1e-4f;  // fractions of a chroma bin
  constexpr float kFminGrid = 1e-4f;    // Hz
  return ChromaFilterbankCacheKey{sr,
                                  n_fft,
                                  config.n_chroma,
                                  quantize(config.tuning, kTuningGrid),
                                  quantize(config.fmin, kFminGrid),
                                  config.n_octaves,
                                  config.norm};
}

struct ChromaFilterbankCacheKeyHash {
  size_t operator()(const ChromaFilterbankCacheKey& k) const {
    return std::hash<int>()(k.sample_rate) ^ (std::hash<int>()(k.n_fft) << 1) ^
           (std::hash<int>()(k.n_chroma) << 2) ^ (std::hash<int>()(k.n_octaves) << 3) ^
           (std::hash<int>()(static_cast<int>(k.norm)) << 4) ^ (std::hash<float>()(k.tuning) << 5) ^
           (std::hash<float>()(k.fmin) << 6);
  }
};

/// @brief Maximum number of cached Chroma filterbanks.
constexpr size_t kMaxChromaCacheSize = 8;

/// @brief Cached filterbank with a back-pointer into the LRU list.
/// @details The stored iterator lets cache hits and evictions touch the LRU
/// queue in O(1) (via `splice`/`erase`) instead of doing an O(n)
/// `lru.remove(key)` while holding the mutex.
struct CachedChromaFilterbank {
  std::vector<float> filterbank;
  std::list<ChromaFilterbankCacheKey>::iterator lru_it;
};

/// @brief Chroma filterbank cache state with LRU eviction.
struct ChromaFilterbankCache {
  std::mutex mutex;
  std::unordered_map<ChromaFilterbankCacheKey, CachedChromaFilterbank, ChromaFilterbankCacheKeyHash>
      map;
  std::list<ChromaFilterbankCacheKey> lru;
};

ChromaFilterbankCache& chroma_filterbank_cache() {
  static ChromaFilterbankCache cache;
  return cache;
}

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

  int n_bins = n_fft / 2 + 1;
  int n_chroma = config.n_chroma;

  // Minimum frequency (default to C1)
  float fmin = config.fmin > 0.0f ? config.fmin : constants::kC1Hz;

  // NOTE: librosa's STFT chroma maps EVERY FFT bin at/above fmin to a pitch
  // class (no upper octave bound) — ChromaFilterConfig::n_octaves does not apply
  // to this STFT path (it is a CQT-chroma concept). Bounding by fmin*2^n_octaves
  // here diverges from the librosa reference, so we intentionally do not cap.

  // Create filterbank [n_chroma x n_bins]
  std::vector<float> filterbank(n_chroma * n_bins, 0.0f);

  // For each FFT bin, compute contribution to each chroma bin
  float bin_width = static_cast<float>(sr) / n_fft;

  for (int k = 1; k < n_bins; ++k) {  // Skip DC bin
    float freq = k * bin_width;

    if (freq < fmin) {
      continue;
    }

    // Get fractional chroma for this frequency
    float chroma = hz_to_chroma(freq, config.tuning);
    if (chroma < 0.0f) {
      continue;
    }

    // Scale to n_chroma bins
    float scaled_chroma = chroma * n_chroma / constants::kSemitonesPerOctave;

    // Distribute energy to neighboring chroma bins (using triangular window)
    int chroma_low = static_cast<int>(std::floor(scaled_chroma)) % n_chroma;
    int chroma_high = (chroma_low + 1) % n_chroma;
    float frac = scaled_chroma - std::floor(scaled_chroma);

    // Weight by proximity
    filterbank[chroma_low * n_bins + k] += (1.0f - frac);
    filterbank[chroma_high * n_bins + k] += frac;
  }

  // Normalize each chroma row. librosa.filters.chroma defaults to L2 (norm=2);
  // we expose L1 / None for callers that need the historical behavior.
  if (config.norm != ChromaFilterNorm::None) {
    for (int c = 0; c < n_chroma; ++c) {
      float scale = 0.0f;
      if (config.norm == ChromaFilterNorm::L1) {
        for (int k = 0; k < n_bins; ++k) {
          scale += filterbank[c * n_bins + k];
        }
      } else {  // L2
        for (int k = 0; k < n_bins; ++k) {
          const float v = filterbank[c * n_bins + k];
          scale += v * v;
        }
        scale = std::sqrt(scale);
      }
      if (scale > 0.0f) {
        const float inv = 1.0f / scale;
        for (int k = 0; k < n_bins; ++k) {
          filterbank[c * n_bins + k] *= inv;
        }
      }
    }
  }

  return filterbank;
}

const std::vector<float>& get_chroma_filterbank_cached(int sr, int n_fft,
                                                       const ChromaFilterConfig& config) {
  ChromaFilterbankCacheKey key = make_key(sr, n_fft, config);

  ChromaFilterbankCache& cache = chroma_filterbank_cache();
  {
    std::lock_guard<std::mutex> lock(cache.mutex);
    auto it = cache.map.find(key);
    if (it != cache.map.end()) {
      // O(1) MRU update: splice the existing list node to the front instead of
      // searching with `lru.remove(key)` (was O(n) while holding the mutex).
      cache.lru.splice(cache.lru.begin(), cache.lru, it->second.lru_it);
      return it->second.filterbank;
    }
  }

  // Build outside the lock — see get_mel_filterbank_cached for rationale.
  std::vector<float> fb = create_chroma_filterbank(sr, n_fft, config);

  std::lock_guard<std::mutex> lock(cache.mutex);
  auto it = cache.map.find(key);
  if (it != cache.map.end()) {
    cache.lru.splice(cache.lru.begin(), cache.lru, it->second.lru_it);
    return it->second.filterbank;
  }
  while (cache.map.size() >= kMaxChromaCacheSize && !cache.lru.empty()) {
    auto oldest_it = std::prev(cache.lru.end());
    cache.map.erase(*oldest_it);
    cache.lru.erase(oldest_it);
  }
  cache.lru.push_front(key);
  auto [ins, _] = cache.map.emplace(key, CachedChromaFilterbank{std::move(fb), cache.lru.begin()});
  return ins->second.filterbank;
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
