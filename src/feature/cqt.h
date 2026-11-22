#pragma once

/// @file cqt.h
/// @brief Constant-Q Transform (CQT) for music signal analysis.

#include <algorithm>
#include <complex>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/audio.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/parameter_checks.h"
#include "util/types.h"

namespace sonare {

/// Row-compressed complex matrix used by CQT/VQT frequency kernels.
struct SparseComplexKernel {
  int rows = 0;
  int cols = 0;
  std::vector<int> row_offsets;
  std::vector<int> column_indices;
  std::vector<std::complex<float>> values;

  size_t nonzeros() const noexcept { return values.size(); }
};

/// @brief Progress callback for CQT computation.
using CqtProgressCallback = std::function<void(float progress)>;

/// @brief CQT configuration.
struct CqtConfig {
  int hop_length = 512;                  ///< Hop length in samples
  float fmin = constants::kC1Hz;         ///< Minimum frequency in Hz (C1)
  int n_bins = 84;                       ///< Number of frequency bins (7 octaves * 12)
  int bins_per_octave = 12;              ///< Bins per octave (12 for semitones)
  float filter_scale = 1.0f;             ///< Filter length scale factor
  WindowType window = WindowType::Hann;  ///< Window function for filters
};

/// @brief CQT result container.
/// @note Thread safety: A single CqtResult instance is NOT thread-safe for
///       concurrent access to magnitude()/power() methods due to lazy caching.
///       Each thread should have its own CqtResult instance, or external
///       synchronization is required. The cqt() function itself is thread-safe.
class CqtResult {
 public:
  /// @brief Default constructor creates empty result.
  CqtResult();

  /// @brief Creates CqtResult from computed data.
  CqtResult(std::vector<std::complex<float>> data, int n_bins, int n_frames,
            std::vector<float> frequencies, int hop_length, int sample_rate);

  /// @brief Returns number of frequency bins.
  int n_bins() const { return n_bins_; }

  /// @brief Returns number of time frames.
  int n_frames() const { return n_frames_; }

  /// @brief Returns hop length used.
  int hop_length() const { return hop_length_; }

  /// @brief Returns sample rate.
  int sample_rate() const { return sample_rate_; }

  /// @brief Returns true if result is empty.
  bool empty() const { return n_frames_ == 0 || n_bins_ == 0; }

  /// @brief Returns duration in seconds.
  float duration() const;

  /// @brief Returns view of complex CQT coefficients [n_bins x n_frames].
  MatrixView<std::complex<float>> complex_view() const;

  /// @brief Returns pointer to complex data.
  const std::complex<float>* complex_data() const { return data_.data(); }

  /// @brief Returns magnitude [n_bins x n_frames].
  const std::vector<float>& magnitude() const;

  /// @brief Returns power spectrum [n_bins x n_frames].
  const std::vector<float>& power() const;

  /// @brief Returns magnitude in decibels.
  /// @param ref Reference value (default 1.0)
  /// @param amin Minimum amplitude to avoid log(0) (default constants::kEpsilon)
  /// @param top_db Threshold below max dB to clamp (default constants::kDefaultTopDb, +inf to
  /// disable; negative or NaN is refused)
  std::vector<float> to_db(float ref = 1.0f, float amin = constants::kEpsilon,
                           float top_db = constants::kDefaultTopDb) const;

  /// @brief Returns center frequencies for each bin.
  const std::vector<float>& frequencies() const { return frequencies_; }

  /// @brief Access complex value at (bin, frame).
  const std::complex<float>& at(int bin, int frame) const;

 private:
  std::vector<std::complex<float>> data_;  ///< Complex CQT [n_bins * n_frames]
  int n_bins_ = 0;
  int n_frames_ = 0;
  int hop_length_ = 0;
  int sample_rate_ = 0;
  std::vector<float> frequencies_;

  mutable std::vector<float> magnitude_cache_;
  mutable std::vector<float> power_cache_;
};

/// @brief CQT kernel for efficient computation.
class CqtKernel {
 public:
  /// @brief Creates CQT kernel for given configuration.
  /// @param sr Sample rate
  /// @param config CQT configuration
  static std::unique_ptr<CqtKernel> create(int sr, const CqtConfig& config);

  /// @brief Creates the same kernel as create(sr, config), copying the rows @p donor already holds.
  /// @details A row is a function of its centre frequency, its raw length, the sample rate, the
  ///          window and the FFT geometry; a donor row equal in all of them is copied rather than
  ///          rebuilt. Rows the donor cannot supply are built as usual, so any donor (or none)
  ///          yields an identical kernel.
  /// @param donor Kernel to take matching rows from, or nullptr
  static std::unique_ptr<CqtKernel> create(int sr, const CqtConfig& config, const CqtKernel* donor);

  /// @brief Creates the same kernel as create(sr, config, donor) on a host offering
  ///        @p host_concurrency workers.
  /// @details Rows are built by up to that many threads and joined in row order, so the kernel
  ///          does not depend on the count. The other overloads pass the host's own.
  static std::unique_ptr<CqtKernel> create(int sr, const CqtConfig& config, const CqtKernel* donor,
                                           int host_concurrency);

  /// @brief Returns FFT length used by kernel.
  int fft_length() const { return fft_length_; }

  /// @brief Returns number of frequency bins.
  int n_bins() const { return n_bins_; }

  /// @brief Returns center frequencies for each bin.
  const std::vector<float>& frequencies() const { return frequencies_; }

  /// @brief Returns the row-compressed frequency-domain kernel.
  const SparseComplexKernel& kernel() const { return kernel_; }

  /// @brief Returns filter lengths for each bin.
  const std::vector<int>& lengths() const { return lengths_; }

  /// @brief Returns raw fractional filter lengths from `wavelet_lengths`.
  const std::vector<float>& raw_lengths() const { return raw_lengths_; }

 private:
  CqtKernel() = default;

  int fft_length_ = 0;
  int fft_basis_length_ = 0;  ///< FFT length before widening for the hop
  int sample_rate_ = 0;
  WindowType window_ = WindowType::Hann;
  int n_bins_ = 0;
  std::vector<float> frequencies_;
  SparseComplexKernel kernel_;
  std::vector<int> lengths_;        ///< Effective integer length per bin
  std::vector<float> raw_lengths_;  ///< Raw fractional lengths (librosa)
};

/// @brief Refuses a CQT/VQT minimum frequency that is not finite and positive, naming fmin.
/// @throws SonareException(InvalidParameter).
void validate_cqt_fmin(float fmin);

/// @brief Refuses a CQT configuration member outside its domain, naming it.
/// @details Checks hop_length, fmin, n_bins, bins_per_octave and filter_scale in that order.
/// @throws SonareParameterRefusal(InvalidParameter).
void validate_cqt_config(const CqtConfig& config);

/// @brief The longest filter, and so the largest FFT, a CQT-family kernel builds.
inline constexpr int kMaxCqtFftLength = 131072;

/// @brief The largest hop a CQT-family transform takes, half its largest FFT.
inline constexpr int kMaxCqtHopLength = 65536;

/// @brief The hops a CQT-family transform takes: [1, @ref kMaxCqtHopLength].
inline constexpr util::check::NumericDomain kCqtHopLengthDomain =
    util::check::integer_domain(1, kMaxCqtHopLength);

/// @brief The positive counts of a CqtConfig (n_bins, bins_per_octave).
inline constexpr util::check::NumericDomain kCqtCountDomain =
    util::check::integer_domain_of<int>(1);

namespace detail {

/// @brief Refuses a grid whose top bin is not below Nyquist, naming fmin, n_bins,
///        bins_per_octave and sr.
void validate_cqt_top_bin(const std::vector<float>& frequencies, int sr);

/// @brief Refuses a filter longer than @p max_length samples at sr, naming @p subjects: the
///        members the filter length is a function of.
void validate_cqt_filter_length(double longest, int max_length, std::vector<std::string> subjects);

/// @brief Refuses a filter past @p max_length samples, naming @p subjects and then @p rate, the
///        sample rate the length is measured at.
[[noreturn]] void refuse_cqt_filter_length(int max_length, std::vector<std::string> subjects,
                                           std::string rate);

/// @brief Runs @p call for an entry whose filter_scale is fixed, restating a filter-length
///        refusal in @p subjects and @p rate, the members such a caller can move.
template <typename Call>
auto with_fixed_filter_scale(std::vector<std::string> subjects, std::string rate,
                             Call&& call) -> decltype(call()) {
  try {
    return call();
  } catch (const SonareParameterRefusal& e) {
    const std::vector<std::string>& named = e.subjects();
    if (std::find(named.begin(), named.end(), "filter_scale") == named.end()) throw;
    refuse_cqt_filter_length(kMaxCqtFftLength, std::move(subjects), std::move(rate));
  }
}

/// @brief Runs @p call for an entry whose fmin, n_bins and filter_scale are derived from its
///        bins per octave, restating a top-bin, filter-length or kernel-size refusal in
///        @p bins_per_octave and @p rate, the arguments such a caller can move.
template <typename Call>
auto with_derived_cqt_grid(std::string bins_per_octave, std::string rate,
                           Call&& call) -> decltype(call()) {
  try {
    return call();
  } catch (const SonareParameterRefusal& e) {
    const std::vector<std::string>& named = e.subjects();
    const auto names = [&](const char* subject) {
      return std::find(named.begin(), named.end(), subject) != named.end();
    };
    if (names("n_bins") && names("sr")) {
      util::check::refuse_relation({std::move(bins_per_octave), std::move(rate)},
                                   "{0} puts the top chroma bin at or above half of {1}");
    }
    if (names("filter_scale")) {
      refuse_cqt_filter_length(kMaxCqtFftLength, {std::move(bins_per_octave)}, std::move(rate));
    }
    if (named.size() == 1 && named.front() == "n_bins") {
      util::check::refuse_relation({std::move(bins_per_octave), std::move(rate)},
                                   "{0} puts more bins in the CQT kernel than fit at {1}");
    }
    throw;
  }
}

/// @brief Refuses an @p n_bins whose kernel at @p fft_length exceeds @p max_elements, stating
///        the largest n_bins that fits.
void validate_cqt_kernel_bins(int n_bins, int fft_length, std::size_t max_elements);

}  // namespace detail

/// @brief Computes Constant-Q Transform.
/// @param audio Input audio
/// @param config CQT configuration
/// @param progress_callback Optional progress callback
/// @return CQT result
/// @note Center padding is applied so each frame is aligned around its timestamp.
///       Input is padded with fft_length/2 zeros on each side.
/// @note Thread-safe. Uses mutex-protected kernel cache internally.
CqtResult cqt(const Audio& audio, const CqtConfig& config = CqtConfig(),
              CqtProgressCallback progress_callback = nullptr);

/// @brief Computes pseudo-inverse CQT (reconstruction).
/// @param cqt_result CQT coefficients
/// @param length Target output length in samples (0 = auto)
/// @return Reconstructed audio
/// @warning This is a simplified reconstruction and may not produce high-quality
///          results. Consider using Griffin-Lim with CQT magnitude for better quality.
/// @deprecated Prefer using phase vocoder or Griffin-Lim based methods for
///             high-quality audio reconstruction.
[[deprecated("Use Griffin-Lim or phase vocoder for better reconstruction quality")]]
Audio icqt(const CqtResult& cqt_result, int length = 0);

/// @brief Computes CQT frequencies for given configuration.
/// @param fmin Minimum frequency
/// @param n_bins Number of bins
/// @param bins_per_octave Bins per octave
/// @return Vector of center frequencies
std::vector<float> cqt_frequencies(float fmin, int n_bins, int bins_per_octave);

/// @brief Aggregation applied to the CQT bins folded onto one pitch class.
enum class ChromaFold {
  kSum,   ///< Sum the bins (librosa.filters.cq_to_chroma's unnormalized fold)
  kMean,  ///< Divide by the number of bins folded onto the class
};

/// @brief Folds CQT magnitude bins onto pitch classes.
/// @details The single definition of the CQT-bin to pitch-class mapping. Every
///          chroma extractor routes through it, so the bins-per-octave centering
///          shift and the fmin pitch-class rotation cannot drift between the
///          summed and the mean entry points.
///
///          Each pitch class merges `n_merge = bins_per_octave / n_chroma` CQT
///          bins. The merge window is centered on its target class (shift by
///          `n_merge / 2`), mirroring librosa.filters.cq_to_chroma's
///          `np.roll(-(n_merge // 2))`, so a bin sitting at the low edge of a
///          semitone group folds onto the correct class rather than the one
///          below it. With `bins_per_octave == n_chroma` the shift is zero, so
///          the common 12-bin path is unaffected. @p fmin_pitch_class then
///          rotates class 0 onto C; a plain `bin % n_chroma` is only correct for
///          the 12-bins-per-octave, C-aligned case.
/// @param magnitude CQT magnitude [n_bins x n_frames] row-major
/// @param n_bins Number of CQT bins
/// @param n_frames Number of time frames
/// @param bins_per_octave CQT bins per octave
/// @param n_chroma Number of pitch classes
/// @param fmin_pitch_class C-relative pitch class of CQT bin 0
/// @param aggregation Summed or mean aggregation
/// @param counts Optional out-param receiving the bin count per pitch class
/// @return Chroma matrix [n_chroma x n_frames] row-major (empty if the grid is
///         degenerate, i.e. n_chroma or bins_per_octave is not positive)
std::vector<float> fold_cqt_bins_to_chroma(const float* magnitude, int n_bins, int n_frames,
                                           int bins_per_octave, int n_chroma, int fmin_pitch_class,
                                           ChromaFold aggregation,
                                           std::vector<int>* counts = nullptr);

/// @brief C-relative pitch class of a frequency.
/// @details hz_to_midi yields MIDI numbers where C is a multiple of 12. The
///          MIDI pitch class modulo 12 is scaled and rounded to @p n_chroma, so
///          resolutions other than 12 subdivide the twelve pitch classes rather
///          than the absolute MIDI numbering.
/// @param hz Frequency in Hz (non-positive yields class 0)
/// @param n_chroma Number of pitch classes
/// @return Pitch class in [0, n_chroma)
int chroma_class_of_frequency(float hz, int n_chroma);

/// Internal shared CQT-bin to pitch-class fold used by chroma extractors.
/// Derives the bin grid from @p frequencies and sums; see fold_cqt_bins_to_chroma.
std::vector<float> accumulate_cqt_pitch_classes(const std::vector<float>& magnitude, int n_bins,
                                                int n_frames, int n_chroma,
                                                const std::vector<float>& frequencies,
                                                std::vector<int>* counts = nullptr);

/// @brief Hybrid CQT: uses CQT for high-Q bins and STFT for low-Q bins.
/// @details Mirrors librosa.hybrid_cqt. Bins whose filter length exceeds the
/// chosen FFT size fall back to the regular CQT, while the remaining bins are
/// computed via STFT and summed onto a single CQT-aligned bin grid.
/// @param audio Input audio.
/// @param config CQT configuration.
/// @return CqtResult with magnitudes only (phase fields are zero).
CqtResult hybrid_cqt(const Audio& audio, const CqtConfig& config = CqtConfig());

/// @brief Pseudo-CQT: a faster single-FFT approximation of the CQT magnitude.
/// @details Mirrors librosa.pseudo_cqt. Lower fidelity than @ref cqt at the
/// very low octaves, but ~10x faster.
CqtResult pseudo_cqt(const Audio& audio, const CqtConfig& config = CqtConfig());

/// @brief Reconstructs audio from a CQT magnitude via Griffin-Lim.
/// @param magnitude CQT magnitude [n_bins x n_frames] row-major.
/// @param n_bins Number of CQT bins.
/// @param n_frames Number of time frames.
/// @param config CQT configuration that produced the magnitude.
/// @param sr Sample rate of the original signal.
/// @param n_iter Number of Griffin-Lim iterations.
/// @return Reconstructed audio.
/// @throw sonare::SonareException (InvalidParameter) on any non-finite element
///        of @p magnitude. The finiteness precondition lives here so every
///        surface reports it identically.
Audio griffinlim_cqt(const float* magnitude, int n_bins, int n_frames, const CqtConfig& config,
                     int sr, int n_iter = constants::kDefaultGriffinLimIterations);

/// @brief Converts CQT to chroma features.
/// @param cqt_result CQT result
/// @param n_chroma Number of chroma bins (default 12)
/// @return Chroma features [n_chroma x n_frames]
std::vector<float> cqt_to_chroma(const CqtResult& cqt_result, int n_chroma = 12);

}  // namespace sonare
