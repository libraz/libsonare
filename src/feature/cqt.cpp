#include "feature/cqt.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

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
#include "util/parallel_for.h"
#include "util/parameter_checks.h"

namespace sonare {

using sonare::constants::kEpsilon;

namespace {

/// @brief Cache key for CQT kernel
struct CqtKernelCacheKey {
  int sample_rate;
  int hop_length;
  float fmin;
  int n_bins;
  int bins_per_octave;
  float filter_scale;
  WindowType window;

  // Exact equality keeps the cache key identical to the configuration used to
  // build the kernel. Near-equal configurations intentionally remain distinct:
  // the transform must never return a kernel built from a different cold value.
  bool operator==(const CqtKernelCacheKey& other) const {
    return sample_rate == other.sample_rate && hop_length == other.hop_length &&
           fmin == other.fmin && n_bins == other.n_bins &&
           bins_per_octave == other.bins_per_octave && filter_scale == other.filter_scale &&
           window == other.window;
  }
};

CqtKernelCacheKey make_key(int sr, const CqtConfig& config) {
  return CqtKernelCacheKey{sr,
                           config.hop_length,
                           config.fmin,
                           config.n_bins,
                           config.bins_per_octave,
                           config.filter_scale,
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

/// @brief Maximum number of cached CQT kernels.
/// @details A chord analysis holds three kernels (full range, bass blend, bass cue) per sample
/// rate and tuning, about 1 MB a set; twelve slots keep four sets.
constexpr size_t kMaxCqtCacheSize = 12;
// The public NNLS-chroma default uses 36 bins/octave over seven octaves. At
// 44.1 kHz its longest kernel requires 252 * 131072 elements, so the resource
// ceiling must admit that shipped configuration while still rejecting hostile
// low-fmin / oversized requests.
constexpr size_t kMaxCqtKernelElements = 32 * 1024 * 1024;
static_assert(kMaxCqtHopLength == kMaxCqtFftLength / 2, "a hop spans at most half the FFT");

/// @brief Scratch the builders of one kernel may hold between them.
constexpr uint64_t kMaxCqtBuildScratchBytes = 64u * 1024u * 1024u;
// The 252-bin, 36 bins/octave kernel builds in about 170 ms on eight builders at 22.05 kHz and
// 85 ms on the sixteen the scratch budget would admit; at 44.1 kHz the budget admits eight.
constexpr int kMaxCqtBuilders = 8;

/// @brief Builders for @p rows kernel rows of @p n_fft columns on a host offering
///        @p host_concurrency workers, held within @ref kMaxCqtBuildScratchBytes.
int kernel_builder_count(int rows, int n_fft, int host_concurrency) {
  if (rows <= 1 || host_concurrency <= 1) return 1;
  const int offered = std::min({rows, host_concurrency, kMaxCqtBuilders});
  // Per builder: the padded row, its transform and the FFT's three buffers, then the
  // sparsifier's magnitude, order and kept-column arrays.
  const uint64_t bytes_per_column =
      5 * sizeof(std::complex<float>) + sizeof(float) + sizeof(std::pair<float, int>) + sizeof(int);
  const uint64_t per_builder_bytes = bytes_per_column * static_cast<uint64_t>(std::max(n_fft, 1));
  const uint64_t affordable = kMaxCqtBuildScratchBytes / per_builder_bytes;
  if (affordable >= static_cast<uint64_t>(offered)) return offered;
  return affordable == 0 ? 1 : static_cast<int>(affordable);
}

/// @brief Kernels a later build may copy rows from; weak, so the cache alone decides lifetime.
struct RowDonors {
  std::mutex mutex;
  std::vector<std::pair<CqtKernelCacheKey, std::weak_ptr<const CqtKernel>>> kernels;
};

RowDonors& row_donors() {
  static RowDonors donors;
  return donors;
}

/// @brief Live kernel on the same grid as @p key sharing the most rows with it, or null.
/// @details Bins of one (fmin, bins_per_octave, filter_scale) grid agree row for row except
///          at a kernel's last bin, whose length has no upper neighbour to be set by.
std::shared_ptr<const CqtKernel> find_row_donor(const CqtKernelCacheKey& key) {
  RowDonors& donors = row_donors();
  std::lock_guard<std::mutex> lock(donors.mutex);
  std::shared_ptr<const CqtKernel> best;
  int best_rows = 0;
  for (const auto& [other, weak] : donors.kernels) {
    if (other.sample_rate != key.sample_rate || other.fmin != key.fmin ||
        other.bins_per_octave != key.bins_per_octave || other.filter_scale != key.filter_scale ||
        other.window != key.window) {
      continue;
    }
    const int rows = std::min(other.n_bins, key.n_bins);
    if (rows <= best_rows) continue;
    if (std::shared_ptr<const CqtKernel> kernel = weak.lock()) {
      best = std::move(kernel);
      best_rows = rows;
    }
  }
  return best;
}

void remember_row_donor(const CqtKernelCacheKey& key, const std::shared_ptr<CqtKernel>& kernel) {
  RowDonors& donors = row_donors();
  std::lock_guard<std::mutex> lock(donors.mutex);
  auto& kernels = donors.kernels;
  kernels.erase(std::remove_if(kernels.begin(), kernels.end(),
                               [](const auto& entry) { return entry.second.expired(); }),
                kernels.end());
  kernels.emplace_back(key, kernel);
}

/// @brief Get or create a cached sparse CQT kernel.
CachedCqtKernel get_cached_kernel(int sr, const CqtConfig& config) {
  CqtKernelCacheKey key = make_key(sr, config);

  // Returned by value so the shared-pointer snapshot survives eviction. The
  // expensive wavelet and per-bin FFT build runs outside the cache lock.
  static LruCache<CqtKernelCacheKey, CachedCqtKernel, CqtKernelCacheKeyHash> cache(
      kMaxCqtCacheSize);
  return cache.get_or_build(key, [&]() -> CachedCqtKernel {
    const std::shared_ptr<const CqtKernel> donor = find_row_donor(key);
    std::shared_ptr<CqtKernel> kernel = CqtKernel::create(sr, config, donor.get());
    remember_row_donor(key, kernel);

    return CachedCqtKernel{std::move(kernel)};
  });
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
  return static_cast<float>(static_cast<double>(n_frames_) * hop_length_) / sample_rate_;
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

namespace {

// A bandwidth is measured against a neighbour, so a lone bin takes the Q its grid spacing implies;
// 0 lets wavelet_lengths measure every bin against its neighbours.
float lone_bin_q(const CqtConfig& config) {
  if (config.n_bins != 1) return 0.0f;
  return config.filter_scale / detail::geometric_relative_bandwidth(config.bins_per_octave);
}

// The lowest bin carries the longest filter. Refused before any length is derived, so a length
// past the float range meets the same refusal as one past the FFT cap.
void validate_longest_filter(const CqtConfig& config, int sr) {
  const double alpha = detail::geometric_relative_bandwidth(config.bins_per_octave);
  detail::validate_cqt_filter_length(
      std::ceil(static_cast<double>(config.filter_scale) * sr / (alpha * config.fmin)),
      kMaxCqtFftLength, {"fmin", "bins_per_octave", "filter_scale"});
}

}  // namespace

void validate_cqt_fmin(float fmin) { util::check::greater_than(fmin, 0.0, "fmin"); }

void validate_cqt_config(const CqtConfig& config) {
  util::check::integer_within<int>(config.hop_length, kCqtHopLengthDomain, "hop_length");
  validate_cqt_fmin(config.fmin);
  util::check::integer_within<int>(config.n_bins, kCqtCountDomain, "n_bins");
  util::check::integer_within<int>(config.bins_per_octave, kCqtCountDomain, "bins_per_octave");
  util::check::greater_than(config.filter_scale, 0.0, "filter_scale");
}

namespace detail {

void validate_cqt_top_bin(const std::vector<float>& frequencies, int sr) {
  if (frequencies.empty() || !(frequencies.back() < static_cast<float>(sr) * 0.5f)) {
    util::check::refuse_relation({"fmin", "n_bins", "bins_per_octave", "sr"},
                                 "{0} * 2^(({1} - 1) / {2}) must be less than {3} / 2");
  }
}

void validate_cqt_filter_length(double longest, int max_length, std::vector<std::string> subjects) {
  if (std::isfinite(longest) && longest <= static_cast<double>(max_length)) return;
  refuse_cqt_filter_length(max_length, std::move(subjects), "sr");
}

void refuse_cqt_filter_length(int max_length, std::vector<std::string> subjects, std::string rate) {
  std::string text;
  for (size_t i = 0; i < subjects.size(); ++i) {
    if (i > 0) text += i + 1 == subjects.size() ? " and " : ", ";
    text += "{" + std::to_string(i) + "}";
  }
  text += " must keep every filter at most " + std::to_string(max_length) + " samples long at {" +
          std::to_string(subjects.size()) + "}";
  subjects.push_back(std::move(rate));
  util::check::refuse_relation(std::move(subjects), text);
}

void validate_cqt_kernel_bins(int n_bins, int fft_length, std::size_t max_elements) {
  const auto most = static_cast<long long>(max_elements / static_cast<std::size_t>(fft_length));
  util::check::integer_in_range(n_bins, 1, most, "n_bins");
}

}  // namespace detail

// CqtKernel implementation
std::unique_ptr<CqtKernel> CqtKernel::create(int sr, const CqtConfig& config) {
  return create(sr, config, nullptr);
}

std::unique_ptr<CqtKernel> CqtKernel::create(int sr, const CqtConfig& config,
                                             const CqtKernel* donor) {
#ifndef __EMSCRIPTEN__
  return create(sr, config, donor, available_workers());
#else
  return create(sr, config, donor, 1);
#endif
}

std::unique_ptr<CqtKernel> CqtKernel::create(int sr, const CqtConfig& config,
                                             const CqtKernel* donor, int host_concurrency) {
  validate_cqt_config(config);
  SONARE_CHECK(sr > 0, ErrorCode::InvalidParameter);
  auto kernel = std::unique_ptr<CqtKernel>(new CqtKernel());

  // Center frequencies for all bins.
  kernel->frequencies_ = cqt_frequencies(config.fmin, config.n_bins, config.bins_per_octave);
  detail::validate_cqt_top_bin(kernel->frequencies_, sr);
  validate_longest_filter(config, sr);
  kernel->n_bins_ = config.n_bins;

  // Raw fractional lengths from librosa's wavelet_lengths (bpo-based).
  std::vector<float> raw_lengths =
      wavelet_lengths(kernel->frequencies_, sr, config.filter_scale, lone_bin_q(config));
  const float longest =
      raw_lengths.empty() ? 0.0f : *std::max_element(raw_lengths.begin(), raw_lengths.end());
  detail::validate_cqt_filter_length(std::ceil(static_cast<double>(longest)), kMaxCqtFftLength,
                                     {"fmin", "bins_per_octave", "filter_scale"});
  const long double longest_ceil = std::ceil(static_cast<long double>(longest));
  SONARE_CHECK(longest_ceil >= 1.0L, ErrorCode::InvalidParameter);
  const int predicted_max_length = std::max(2, static_cast<int>(longest_ceil));
  int predicted_n_fft = next_power_of_2(predicted_max_length);
  if (config.hop_length > 0) {
    predicted_n_fft = std::max(predicted_n_fft, 2 * next_power_of_2(config.hop_length));
  }
  SONARE_CHECK(predicted_n_fft <= kMaxCqtFftLength, ErrorCode::InvalidState);
  detail::validate_cqt_kernel_bins(config.n_bins, predicted_n_fft, kMaxCqtKernelElements);

  // Effective integer lengths, librosa's index range n in [floor(-ilen/2), floor(ilen/2)).
  const auto effective_length = [](float ilen) {
    const int s = static_cast<int>(std::floor(-ilen * 0.5f));
    const int e = static_cast<int>(std::floor(ilen * 0.5f));
    return e - s;
  };
  kernel->lengths_.resize(config.n_bins);
  for (int k = 0; k < config.n_bins; ++k) {
    kernel->lengths_[k] = effective_length(raw_lengths[k]);
  }

  // librosa pads every wavelet to next_pow2(ceil(longest)), then widens n_fft when it
  // is shorter than 2 * next_pow2(hop_length), centring each padded row again.
  const int n_fft_basis = next_power_of_2(std::max(1, static_cast<int>(longest_ceil)));
  int n_fft = n_fft_basis;
  if (config.hop_length > 0) n_fft = std::max(n_fft, 2 * next_power_of_2(config.hop_length));
  const int widen_pad = (n_fft - n_fft_basis) / 2;
  kernel->fft_length_ = n_fft;
  kernel->fft_basis_length_ = n_fft_basis;
  kernel->sample_rate_ = sr;
  kernel->window_ = config.window;

  // Every row costs a transform and a ranking of n_fft columns however short its wavelet is,
  // so a row the donor holds is copied.
  const std::vector<float>& freqs = kernel->frequencies_;
  const bool donor_fits = donor != nullptr && donor->sample_rate_ == sr &&
                          donor->window_ == config.window &&
                          donor->fft_basis_length_ == n_fft_basis && donor->fft_length_ == n_fft;
  const auto reusable = [&](int k) {
    return donor_fits && k < donor->n_bins_ &&
           donor->frequencies_[static_cast<size_t>(k)] == freqs[static_cast<size_t>(k)] &&
           donor->raw_lengths_[static_cast<size_t>(k)] == raw_lengths[static_cast<size_t>(k)];
  };

  // Builds rows [begin, end) into @p out exactly as one pass over the whole grid would: a row
  // reads nothing another row wrote, so any split of the grid yields the same rows.
  const auto build_rows = [&](int begin, int end, SparseComplexKernel& out) {
    out.row_offsets.reserve(static_cast<size_t>(end - begin) + 1);
    out.row_offsets.push_back(0);
    // An FFT instance and its buffers are per builder; neither is built for copied rows.
    std::unique_ptr<FFT> fft;
    std::vector<std::complex<float>> row;
    std::vector<std::complex<float>> freq_kernel;
    // Time-domain wavelets (selected periodic window * complex sinusoid, L1-normalised) of
    // the current run of built rows, packed back to back. Each is centred in its own n_fft
    // row only while that row is transformed: a dense n_bins x n_fft basis is hundreds of
    // MB at 44.1 kHz.
    std::vector<std::complex<float>> packed;
    size_t cursor = 0;
    int run_end = begin;
    int written_begin = 0;
    int written_end = 0;
    for (int k = begin; k < end; ++k) {
      if (reusable(k)) {
        const SparseComplexKernel& from = donor->kernel_;
        const int row_first = from.row_offsets[static_cast<size_t>(k)];
        const int row_last = from.row_offsets[static_cast<size_t>(k) + 1];
        out.column_indices.insert(out.column_indices.end(), from.column_indices.begin() + row_first,
                                  from.column_indices.begin() + row_last);
        out.values.insert(out.values.end(), from.values.begin() + row_first,
                          from.values.begin() + row_last);
        out.row_offsets.push_back(static_cast<int>(out.values.size()));
        continue;
      }
      if (k >= run_end) {
        run_end = k + 1;
        while (run_end < end && !reusable(run_end)) ++run_end;
        // A length is set by a bin's neighbours, so the run takes one bin on each side that
        // has one and leaves the grid's own edge bins as the edges of the sublist.
        const int first = k > 0 ? k - 1 : 0;
        const int last = run_end < config.n_bins ? run_end : run_end - 1;
        const std::vector<float> run_freqs(freqs.begin() + first, freqs.begin() + last + 1);
        const std::vector<float> run_lengths =
            wavelet_lengths(run_freqs, sr, config.filter_scale, lone_bin_q(config));
        packed = detail::wavelet_with_window(run_freqs, sr, config.filter_scale, /*is_cqt=*/true,
                                             /*pad_fft=*/false, lone_bin_q(config), config.window);
        size_t packed_total = 0;
        for (int g = first; g <= last; ++g) {
          const float length = run_lengths[static_cast<size_t>(g - first)];
          SONARE_CHECK(g < k || g >= run_end || length == raw_lengths[static_cast<size_t>(g)],
                       ErrorCode::InvalidState);
          packed_total += static_cast<size_t>(effective_length(length));
        }
        SONARE_CHECK(packed_total == packed.size(), ErrorCode::InvalidState);
        cursor = first < k ? static_cast<size_t>(effective_length(run_lengths.front())) : 0;
        if (!fft) {
          fft = std::make_unique<FFT>(n_fft);
          row.resize(static_cast<size_t>(n_fft));
          freq_kernel.resize(static_cast<size_t>(n_fft));
        }
      }
      const int length = kernel->lengths_[k];
      const int offset = (n_fft_basis - length) / 2 + widen_pad;
      // Scale each row by raw_length / n_fft (librosa.__vqt_filter_fft step), then FFT it.
      const float scale = raw_lengths[k] / static_cast<float>(n_fft);
      // Only the span the previous row wrote is non-zero.
      std::fill(row.begin() + written_begin, row.begin() + written_end,
                std::complex<float>(0.0f, 0.0f));
      for (int i = 0; i < length; ++i) {
        row[static_cast<size_t>(offset + i)] = packed[cursor + static_cast<size_t>(i)] * scale;
      }
      written_begin = offset;
      written_end = offset + length;
      cursor += static_cast<size_t>(length);
      fft->forward_complex(row.data(), freq_kernel.data());
      const size_t row_begin = out.values.size();
      detail::append_sparsified_kernel_row(out, freq_kernel.data(), n_fft);
      // We use the conjugate so that `result = stored_basis * FFT(signal)` evaluates
      // to a true cross-correlation at zero lag (i.e., the standard CQT inner
      // product `<kernel, signal>`). librosa's pipeline uses the convention
      // `fft_basis.dot(D)`, which is equivalent only because the wavelet is
      // single-sided in frequency (energy concentrated at +ω). Storing the
      // conjugate keeps our existing matmul path unchanged. A column's rank reads
      // its magnitude, which the conjugate shares, so only the kept ones take it.
      for (size_t i = row_begin; i < out.values.size(); ++i) {
        out.values[i] = std::conj(out.values[i]);
      }
    }
  };

  // Every row costs a transform and a ranking of n_fft columns, so the grid is split across
  // builders; their parts are joined in row order.
  int built_rows = 0;
  for (int k = 0; k < config.n_bins; ++k) built_rows += reusable(k) ? 0 : 1;
  const int builders = kernel_builder_count(built_rows, n_fft, host_concurrency);
  const int rows_per_part = (config.n_bins + builders - 1) / builders;
  std::vector<SparseComplexKernel> parts(static_cast<size_t>(builders));
  const auto build_parts = [&](int part_begin, int part_end) {
    for (int part = part_begin; part < part_end; ++part) {
      const int begin = std::min(part * rows_per_part, config.n_bins);
      const int end = std::min(begin + rows_per_part, config.n_bins);
      build_rows(begin, end, parts[static_cast<size_t>(part)]);
    }
  };
#ifndef __EMSCRIPTEN__
  parallel_for(builders, builders, build_parts);
#else
  build_parts(0, builders);
#endif

  SparseComplexKernel& joined = kernel->kernel_;
  joined.rows = config.n_bins;
  joined.cols = n_fft;
  joined.row_offsets.reserve(static_cast<size_t>(config.n_bins) + 1);
  joined.row_offsets.push_back(0);
  for (const SparseComplexKernel& part : parts) {
    const int base = static_cast<int>(joined.values.size());
    joined.column_indices.insert(joined.column_indices.end(), part.column_indices.begin(),
                                 part.column_indices.end());
    joined.values.insert(joined.values.end(), part.values.begin(), part.values.end());
    for (size_t r = 1; r < part.row_offsets.size(); ++r) {
      joined.row_offsets.push_back(base + part.row_offsets[r]);
    }
  }
  kernel->raw_lengths_ = std::move(raw_lengths);

  return kernel;
}

std::vector<float> cqt_frequencies(float fmin, int n_bins, int bins_per_octave) {
  std::vector<float> freqs(n_bins);
  for (int k = 0; k < n_bins; ++k) {
    freqs[k] = fmin * std::pow(2.0f, static_cast<float>(k) / bins_per_octave);
  }
  return freqs;
}

namespace detail {

std::vector<std::complex<float>> apply_sparse_kernel_frames(
    const Audio& audio, const SparseComplexKernel& kernel_matrix, int fft_length, int n_bins,
    const std::vector<float>& raw_lengths, int hop_length,
    const std::function<void(float)>& progress_callback, int& n_frames) {
  const int n_samples = static_cast<int>(audio.size());

  // Center padding: pad signal by fft_length/2 on each side.
  // This ensures the first frame is centered at t=0 and produces the expected number of
  // frames: 1 + n_samples / hop_length (approximately).
  int pad_length = fft_length / 2;
  int padded_length = n_samples + 2 * pad_length;
  std::vector<float> padded_signal(padded_length, 0.0f);
  std::copy(audio.data(), audio.data() + n_samples, padded_signal.begin() + pad_length);

  // Calculate number of frames from padded signal
  n_frames = 1 + (padded_length - fft_length) / hop_length;
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

  // Pre-allocate temporary buffers
  std::vector<float> frame(fft_length, 0.0f);
  std::vector<std::complex<float>> frame_fft(static_cast<size_t>(fft_length / 2 + 1));

  const float* data = padded_signal.data();

  // Per-bin /sqrt(L) factor for `scale=True` mode (the librosa default).
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
    int start = t * hop_length;

    // Extract frame with zero-padding if needed at boundaries
    std::fill(frame.begin(), frame.end(), 0.0f);
    int copy_length = std::min(fft_length, padded_length - start);
    if (copy_length > 0) {
      std::copy(data + start, data + start + copy_length, frame.begin());
    }

    // Real FFT: the half spectrum carries every bin the kernel reads.
    fft.forward(frame.data(), frame_fft.data());

    // Apply per-bin /sqrt(length) (librosa.vqt with scale=True).
    for (int k = 0; k < n_bins; ++k) {
      output[k * n_frames + t] =
          detail::sparse_kernel_row_dot(kernel_matrix, k, frame_fft.data(), fft_length) *
          inv_sqrt_len[k];
    }

    // Report progress
    if (progress_callback && (t % progress_interval == 0 || t == n_frames - 1)) {
      progress_callback(static_cast<float>(t + 1) / n_frames);
    }
  }

  return output;
}

}  // namespace detail

CqtResult cqt(const Audio& audio, const CqtConfig& config, CqtProgressCallback progress_callback) {
  validate_cqt_config(config);
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);

  int sr = audio.sample_rate();

  // Get cached row-compressed CQT kernel.
  auto cached = get_cached_kernel(sr, config);
  auto& kernel = cached.kernel;

  int fft_length = kernel->fft_length();
  int n_bins = kernel->n_bins();
  const auto& kernel_matrix = kernel->kernel();

  int n_frames = 0;
  std::vector<std::complex<float>> output = detail::apply_sparse_kernel_frames(
      audio, kernel_matrix, fft_length, n_bins, kernel->raw_lengths(), config.hop_length,
      progress_callback, n_frames);

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
  validate_cqt_config(config);
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);

  const int sr = audio.sample_rate();
  // librosa.pseudo_cqt: |basis| . |Hann STFT| at the kernel's own FFT size,
  // divided by sqrt(n_fft) (scale=True).
  auto cached = get_cached_kernel(sr, config);
  const CqtKernel& kernel = *cached.kernel;
  const int n_fft = kernel.fft_length();
  StftConfig stft_cfg;
  stft_cfg.n_fft = n_fft;
  stft_cfg.hop_length = config.hop_length;
  stft_cfg.window = WindowType::Hann;
  stft_cfg.center = true;
  Spectrogram spec = Spectrogram::compute(audio, stft_cfg);
  const std::vector<float>& mag = spec.magnitude();
  const int n_freq = spec.n_bins();
  const int n_frames = spec.n_frames();
  const SparseComplexKernel& basis = kernel.kernel();
  const float scale = 1.0f / std::sqrt(static_cast<float>(n_fft));

  // Phase is not estimated: the magnitude is stored in the real part so
  // magnitude() returns it.
  std::vector<std::complex<float>> data(static_cast<size_t>(config.n_bins) * n_frames,
                                        std::complex<float>(0.0f, 0.0f));
  // Sparse columns are ascending; each selected magnitude row is contiguous.
  std::vector<float> acc(static_cast<size_t>(n_frames), 0.0f);
  for (int k = 0; k < config.n_bins; ++k) {
    std::fill(acc.begin(), acc.end(), 0.0f);
    const int begin = basis.row_offsets[static_cast<size_t>(k)];
    const int end = basis.row_offsets[static_cast<size_t>(k + 1)];
    for (int index = begin; index < end; ++index) {
      const int b = basis.column_indices[static_cast<size_t>(index)];
      if (b >= n_freq) continue;
      const float p = std::abs(basis.values[static_cast<size_t>(index)]);
      if (p == 0.0f) continue;
      const float* mrow = mag.data() + static_cast<size_t>(b) * n_frames;
      for (int t = 0; t < n_frames; ++t) {
        acc[t] += p * mrow[t];
      }
    }
    for (int t = 0; t < n_frames; ++t) {
      data[k * n_frames + t] = std::complex<float>(acc[t] * scale, 0.0f);
    }
  }
  return CqtResult(std::move(data), config.n_bins, n_frames, kernel.frequencies(),
                   config.hop_length, sr);
}

CqtResult hybrid_cqt(const Audio& audio, const CqtConfig& config) {
  validate_cqt_config(config);
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);

  const int sr = audio.sample_rate();
  std::vector<float> freqs = cqt_frequencies(config.fmin, config.n_bins, config.bins_per_octave);
  detail::validate_cqt_top_bin(freqs, sr);
  validate_longest_filter(config, sr);

  // librosa.hybrid_cqt's split: a bin whose filter, rounded up to a power of two,
  // is shorter than two hops takes the pseudo CQT; longer ones the full CQT.
  const std::vector<float> lengths =
      wavelet_lengths(freqs, sr, config.filter_scale, lone_bin_q(config));
  int n_split = config.n_bins;
  for (int k = 0; k < config.n_bins; ++k) {
    if (std::exp2(std::ceil(std::log2(lengths[static_cast<size_t>(k)]))) <
        2.0f * static_cast<float>(config.hop_length)) {
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
  // The projection smears one non-finite bin across the whole STFT magnitude
  // grid, so a single NaN reaches every output sample. Rejected here rather than
  // at each binding so the C ABI, the WASM bindings (which call this directly)
  // and the in-process callers all inherit one rule.
  if (!numeric::all_finite(magnitude, n_bins, n_frames)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "griffinlim_cqt: magnitude contains a non-finite value");
  }

  // Seed Griffin-Lim with the CQT magnitude laid back onto the STFT grid of the
  // filter bank that produced it.
  // A bandwidth needs a neighbour to be measured against, so a lone bin is voiced
  // as the first of two and only its row is used.
  CqtConfig grid = config;
  grid.n_bins = std::max(n_bins, 2);
  auto cached = get_cached_kernel(sr, grid);
  const CqtKernel& kernel = *cached.kernel;
  const int n_fft = kernel.fft_length();
  const std::vector<float> stft_mag = detail::project_cqt_magnitude_to_stft(
      detail::build_cqt_projection(kernel.kernel(), n_fft), kernel.raw_lengths(), n_fft, magnitude,
      n_bins, n_frames);

  GriffinLimConfig gcfg;
  gcfg.n_iter = n_iter;
  return griffin_lim(stft_mag.data(), n_fft / 2 + 1, n_frames, n_fft, config.hop_length, sr, gcfg);
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
