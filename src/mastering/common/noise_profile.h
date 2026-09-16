#pragma once

/// @file noise_profile.h
/// @brief Noise-floor reporting and channel-linked STFT masking, shared by the
///        classical denoiser and the classical dereverberator.
///
/// Two things live here because both modules need both and neither owns them.
///
/// The band grid is the 32-band geometric grid from 20 Hz to Nyquist the
/// mastering report's per-band energy delta uses, so a repair noise floor and a
/// tonal-balance change can be read on one axis.
///
/// @ref LinkedSpectra is the channel-linked contract itself: one analysis grid
/// for every channel, one channel-summed power spectrum to build a gain mask
/// from, and one mask applied to every channel. A mask that is identical across
/// channels is a real scalar per (bin, frame), so it cannot move an interchannel
/// level or phase difference -- which a per-channel mask does, and which a
/// time-domain transfer ratio only approximates because one ratio per sample
/// scales every band alike.

#include <complex>
#include <cstddef>
#include <vector>

#include "core/audio.h"
#include "core/spectrum.h"
#include "util/constants.h"

namespace sonare::mastering::common {

/// @brief Bands a repair noise floor's shape is reported in.
inline constexpr std::size_t kRepairNoiseBandCount = 32;

/// @brief Lowest band edge in Hz; the bands run geometrically from here to Nyquist.
inline constexpr float kRepairNoiseBandLowHz = 20.0f;

/// @brief First STFT bin of every band, plus the one-past-the-end bin of the last.
/// @details Non-decreasing, clamped to the spectrum. A band narrower than the bin
///          spacing comes out empty rather than borrowing its neighbour's bin.
/// @param n_fft FFT size the bins belong to.
/// @param sample_rate Sample rate the bins belong to.
/// @param out Receives @ref kRepairNoiseBandCount + 1 bin indices.
void repair_noise_band_bins(int n_fft, int sample_rate, int* out);

/// @brief A noise floor in dBFS, broadband and band by band.
struct NoiseFloorDbfs {
  float broadband = sonare::constants::kFloorDb;
  float bands[kRepairNoiseBandCount] = {};
};

/// @brief Expresses a per-bin noise PSD as a level in dBFS.
/// @details The STFT carries an unknown constant (window power, centring, the
///   transform's own normalization), so nothing here assumes one: the noise PSD
///   and the observed power go through the same one-sided sum and the ratio is
///   applied to the signal's measured mean square. The constant cancels and the
///   result is the noise's share of a level that was measured in the time
///   domain.
///
///   The per-bin PSD is smoothed across bins before it is summed. An estimator
///   that reads a bin's noise from the quietest frames reports a steady tone as
///   noise, because the tone is in the quiet frames too; a noise floor is smooth
///   in frequency and a tone is a few bins wide, so a median over a bin
///   neighbourhood keeps the first and drops the second.
/// @param noise_psd Per-bin, per-frame noise PSD, [bins x frames] bin-major.
/// @param power Observed power over the same grid, [bins x frames] bin-major.
/// @param bins Number of one-sided bins.
/// @param frames Number of frames.
/// @param signal_mean_square Mean square of the time-domain signal the STFT came from.
/// @param sample_rate Sample rate, for the band edges.
/// @return Levels in dBFS; a band holding no bin reports @ref sonare::constants::kFloorDb.
NoiseFloorDbfs noise_floor_dbfs(const double* noise_psd, const float* power, int bins, int frames,
                                double signal_mean_square, int sample_rate);

/// @brief @ref noise_floor_dbfs from per-bin sums instead of the two planes.
/// @details The plane entry folds both inputs to one sum per bin before it does
///   anything else, so a caller walking the STFT frame by frame accumulates the
///   same two arrays and never holds a [bins x frames] plane. Each bin sums its
///   frames in ascending order either way, which is what makes the two entries
///   agree bit for bit rather than merely closely.
/// @param noise_psd_sum Sum over frames of the noise PSD, one per bin.
/// @param power_sum Sum over frames of the observed power, one per bin.
/// @param frames Number of frames the sums span.
NoiseFloorDbfs noise_floor_dbfs_from_sums(const double* noise_psd_sum, const double* power_sum,
                                          int bins, int frames, double signal_mean_square,
                                          int sample_rate);

/// @brief STFTs of a channel set on one analysis grid.
/// @details Holds each channel's spectrum plus the channel-summed power a linked
///   gain mask is built from. Single-channel use is not a special case: the sum
///   of one channel is that channel, so a linked repair fed one channel produces
///   the mono result bit for bit.
class LinkedSpectra {
 public:
  /// @brief Analyzes every channel with @p config.
  /// @param channels Channel buffers; every one the same length and sample rate.
  /// @param channel_count Number of entries in @p channels; must be at least one.
  /// @param config Shared analysis geometry.
  /// @throws SonareException(InvalidParameter) for no channels, a null channel,
  ///         or a channel whose length or sample rate differs from the first.
  static LinkedSpectra compute(const Audio* const* channels, std::size_t channel_count,
                               const StftConfig& config);

  bool empty() const;
  std::size_t channel_count() const noexcept { return spectra_.size(); }
  int n_bins() const;
  int n_frames() const;
  const Spectrogram& channel(std::size_t index) const { return spectra_[index]; }

  /// @brief Sum over channels of each channel's cached re^2 + im^2 power.
  const std::vector<float>& summed_power() const noexcept { return summed_power_; }

  /// @brief Sum over channels of |z|^2, squared from the magnitude.
  /// @details Not derivable from @ref summed_power: `abs(z)^2` and `re^2 + im^2`
  ///   part company in the last bits, and the denoiser's gain recursion is
  ///   defined by the first while its noise estimator reads the second.
  std::vector<double> magnitude_power_sum() const;

  /// @brief Applies one real gain per (bin, frame) to every channel's spectrum.
  /// @param gains [bins x frames] bin-major, same layout as @ref summed_power.
  /// @return One masked complex spectrum per channel.
  std::vector<std::vector<std::complex<float>>> masked(const double* gains) const;

  /// @brief Resynthesizes per-channel spectra back to @p out_length samples.
  std::vector<Audio> resynthesize(const std::vector<std::vector<std::complex<float>>>& spectra,
                                  int out_length) const;

 private:
  std::vector<Spectrogram> spectra_;
  std::vector<float> summed_power_;
};

}  // namespace sonare::mastering::common
