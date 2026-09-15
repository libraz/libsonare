#pragma once

/// @file hpss.h
/// @brief Harmonic-Percussive Source Separation (HPSS).

#include <cstddef>
#include <vector>

#include "core/audio.h"
#include "core/spectrum.h"

namespace sonare {

/// @brief Largest median-filter kernel either HPSS direction accepts, counted in frames.
/// @details Set past the edge of meaningful input rather than to a memory
///          budget: a kernel is a median width in STFT frames or bins, and an
///          hour at hop 512 / 48 kHz is about 337k frames, so a caller at this
///          bound has stopped choosing a resolution. It bounds one factor of the
///          scratch residency and cannot bound the product; the worker count is
///          what holds that, through @ref median_filter_worker_count.
///          Shares a value with @ref kMaxStftNFft by coincidence: that one counts
///          samples and is pinned by an assertion, this one may move freely.
inline constexpr int kMaxHpssKernelSize = 1 << 19;

/// @brief Scratch the median-filter workers may hold at once, summed over all of them.
/// @details A budget, not a measurement. The per-worker arrays are scratch rather
///          than data, so they are sized to stay small beside the spectrogram
///          they filter; this leaves the largest legal kernel a double-digit
///          worker count and a 262145-bin staging column a worker per 2 MiB.
inline constexpr std::size_t kMaxHpssScratchBytes = 64u * 1024u * 1024u;

/// @brief Workers a median filter divides its work across, for one shape.
/// @param total Rows for the horizontal filter, columns for the vertical one.
/// @param kernel_size Median kernel; every worker holds two arrays this wide.
/// @param staged_column_length @c n_bins for the vertical filter, which stages a
///        column pair per worker as well; 0 for the horizontal filter, which does not.
/// @param host_concurrency Workers the host offers.
/// @return At least 1, and never above @p total or @p host_concurrency.
/// @details Scratch residency is a PRODUCT of per-worker bytes and worker count,
///          so a ceiling on either factor alone leaves the other free to scale it.
///          The worker count is the factor that gives way, keeping the total
///          within @ref kMaxHpssScratchBytes on a host of any size.
int median_filter_worker_count(int total, int kernel_size, int staged_column_length,
                               int host_concurrency);

/// @brief Configuration for HPSS algorithm.
/// @details HPSS separates audio into harmonic (tonal) and percussive (transient)
///          components using median filtering. Horizontal filtering enhances
///          harmonics, vertical filtering enhances percussives.
struct HpssConfig {
  /// Horizontal median filter size: odd, positive, at most @ref kMaxHpssKernelSize.
  int kernel_size_harmonic = 31;
  /// Vertical median filter size: odd, positive, at most @ref kMaxHpssKernelSize.
  int kernel_size_percussive = 31;
  float power = 2.0f;              ///< Exponent for mask computation (typically 1.0-2.0)
  float margin_harmonic = 1.0f;    ///< Weight for harmonic mask (> 1.0 favors harmonic)
  float margin_percussive = 1.0f;  ///< Weight for percussive mask (> 1.0 favors percussive)
                                   ///< Soft mask (librosa parity, margin applied before the
                                   ///< power): mask_harm = H^p / (H^p + (margin_h * P)^p),
                                   ///< mask_perc = P^p / (P^p + (margin_p * H)^p)
  bool use_soft_mask = true;       ///< true = soft masks (smooth blend),
                                   ///< false = hard masks (binary assignment)
};

/// @brief Result of HPSS on spectrogram.
struct HpssSpectrogramResult {
  Spectrogram harmonic;    ///< Harmonic component spectrogram
  Spectrogram percussive;  ///< Percussive component spectrogram
};

/// @brief Result of HPSS on audio.
struct HpssAudioResult {
  Audio harmonic;    ///< Harmonic component audio
  Audio percussive;  ///< Percussive component audio
};

/// @brief Result of HPSS with residual on spectrogram.
struct HpssSpectrogramResultWithResidual {
  Spectrogram harmonic;    ///< Harmonic component spectrogram
  Spectrogram percussive;  ///< Percussive component spectrogram
  Spectrogram residual;    ///< Residual component spectrogram
};

/// @brief Result of HPSS with residual on audio.
struct HpssAudioResultWithResidual {
  Audio harmonic;    ///< Harmonic component audio
  Audio percussive;  ///< Percussive component audio
  Audio residual;    ///< Residual component audio
};

/// @brief Applies horizontal median filter to magnitude spectrogram.
/// @param magnitude Magnitude spectrogram [n_bins x n_frames]
/// @param n_bins Number of frequency bins
/// @param n_frames Number of time frames
/// @param kernel_size Filter kernel size (must be odd)
/// @return Filtered magnitude [n_bins x n_frames]
std::vector<float> median_filter_horizontal(const float* magnitude, int n_bins, int n_frames,
                                            int kernel_size);

/// @brief Applies vertical median filter to magnitude spectrogram.
/// @param magnitude Magnitude spectrogram [n_bins x n_frames]
/// @param n_bins Number of frequency bins
/// @param n_frames Number of time frames
/// @param kernel_size Filter kernel size (must be odd)
/// @return Filtered magnitude [n_bins x n_frames]
std::vector<float> median_filter_vertical(const float* magnitude, int n_bins, int n_frames,
                                          int kernel_size);

/// @brief Performs HPSS on a spectrogram.
/// @param spec Input spectrogram
/// @param config HPSS configuration
/// @return Harmonic and percussive spectrograms
HpssSpectrogramResult hpss(const Spectrogram& spec, const HpssConfig& config = HpssConfig());

/// @brief Performs HPSS on audio and returns separated audio signals.
/// @param audio Input audio
/// @param config HPSS configuration
/// @param stft_config STFT configuration for analysis/synthesis
/// @return Harmonic and percussive audio signals
HpssAudioResult hpss(const Audio& audio, const HpssConfig& config = HpssConfig(),
                     const StftConfig& stft_config = StftConfig());

/// @brief Extracts only harmonic component from audio.
/// @param audio Input audio
/// @param config HPSS configuration
/// @param stft_config STFT configuration
/// @return Harmonic audio
Audio harmonic(const Audio& audio, const HpssConfig& config = HpssConfig(),
               const StftConfig& stft_config = StftConfig());

/// @brief Extracts only percussive component from audio.
/// @param audio Input audio
/// @param config HPSS configuration
/// @param stft_config STFT configuration
/// @return Percussive audio
Audio percussive(const Audio& audio, const HpssConfig& config = HpssConfig(),
                 const StftConfig& stft_config = StftConfig());

/// @brief Performs HPSS with residual component on spectrogram.
/// @param spec Input spectrogram
/// @param config HPSS configuration
/// @return Harmonic, percussive, and residual spectrograms
/// @details Residual = Original - Harmonic - Percussive
HpssSpectrogramResultWithResidual hpss_with_residual(const Spectrogram& spec,
                                                     const HpssConfig& config = HpssConfig());

/// @brief Performs HPSS with residual component on audio.
/// @param audio Input audio
/// @param config HPSS configuration
/// @param stft_config STFT configuration
/// @return Harmonic, percussive, and residual audio signals
HpssAudioResultWithResidual hpss_with_residual(const Audio& audio,
                                               const HpssConfig& config = HpssConfig(),
                                               const StftConfig& stft_config = StftConfig());

/// @brief Extracts residual component from audio (what remains after H+P removal).
/// @param audio Input audio
/// @param config HPSS configuration
/// @param stft_config STFT configuration
/// @return Residual audio
Audio residual(const Audio& audio, const HpssConfig& config = HpssConfig(),
               const StftConfig& stft_config = StftConfig());

}  // namespace sonare
