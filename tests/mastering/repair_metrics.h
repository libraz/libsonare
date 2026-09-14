/// @file repair_metrics.h
/// @brief Restoration quality metrics: segmental SNR, log kurtosis ratio, log-spectral distance.
///
/// The C++ half of a pair. `tools/mastering-eval/metrics_repair.py` is the other half and carries
/// the definitions in full; `repair_metrics_test.cpp` pins the two to the same values on fixed
/// vectors. A change to one is a change to both, or the pin goes red.
///
/// Two contracts keep the languages on the same numbers. Samples arrive as float and every
/// accumulation runs in double, so both sides start from bit-identical input and neither
/// accumulates in float. The transform is a local double-precision radix-2 FFT rather than
/// sonare::FFT, whose result is float and depends on which backend the build configured; the
/// spectral metrics therefore require a power-of-two frame length.
///
/// These are measurement tools for tests and the evaluation harness, not shipped library code.

#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <vector>

#include "util/constants.h"

namespace sonare::test::repair_metrics {

/// Default frame length in samples. 1024 at 48 kHz is 21 ms, inside the 15-30 ms convention
/// segmental SNR is defined over, and a power of two for the spectral metrics.
inline constexpr int kFrameLength = 1024;

/// Default hop in samples: half of kFrameLength.
inline constexpr int kHopLength = 512;

/// Per-frame segmental SNR lower clip.
inline constexpr double kSegSnrFloorDb = -10.0;

/// Per-frame segmental SNR upper clip, and the value a bit-exact frame contributes.
inline constexpr double kSegSnrCeilingDb = 35.0;

/// A frame whose clean RMS is below this is excluded: its SNR is set by the noise floor of the
/// reference rather than by the processing.
inline constexpr double kSegSnrSilenceDbfs = -50.0;

/// Power-spectrum floor, mirroring the Python half bit for bit.
inline constexpr double kSpectrumEpsilon = static_cast<double>(constants::kSpectrumEpsilon);

namespace detail {

inline double quiet_nan() { return std::numeric_limits<double>::quiet_NaN(); }

inline void check_pair(const std::vector<float>& a, const std::vector<float>& b, int frame_length,
                       int hop_length) {
  if (a.size() != b.size()) {
    throw std::invalid_argument("repair_metrics: signals must be the same length");
  }
  if (frame_length <= 0 || hop_length <= 0) {
    throw std::invalid_argument("repair_metrics: frame_length and hop_length must be positive");
  }
}

inline void check_power_of_two(int frame_length) {
  if ((frame_length & (frame_length - 1)) != 0) {
    throw std::invalid_argument("repair_metrics: frame_length must be a power of two");
  }
}

inline std::size_t frame_count(std::size_t n, int frame_length, int hop_length) {
  const auto length = static_cast<std::size_t>(frame_length);
  if (n < length) return 0;
  return 1 + (n - length) / static_cast<std::size_t>(hop_length);
}

inline std::vector<double> to_double(const std::vector<float>& x) {
  return std::vector<double>(x.begin(), x.end());
}

/// In-place iterative radix-2 Cooley-Tukey FFT; the twiddle is evaluated per butterfly rather than
/// carried by recurrence, which keeps the result within a few ulp of the Python half's transform.
inline void fft(std::vector<std::complex<double>>& a) {
  const std::size_t n = a.size();
  for (std::size_t i = 1, j = 0; i < n; ++i) {
    std::size_t bit = n >> 1;
    for (; (j & bit) != 0; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (std::size_t len = 2; len <= n; len <<= 1) {
    const double angle = -constants::kTwoPiD / static_cast<double>(len);
    const std::size_t half = len / 2;
    for (std::size_t base = 0; base < n; base += len) {
      for (std::size_t k = 0; k < half; ++k) {
        const double theta = angle * static_cast<double>(k);
        const std::complex<double> w(std::cos(theta), std::sin(theta));
        const std::complex<double> u = a[base + k];
        const std::complex<double> v = a[base + k + half] * w;
        a[base + k] = u + v;
        a[base + k + half] = u - v;
      }
    }
  }
}

inline std::vector<double> hann_periodic(int length) {
  std::vector<double> w(static_cast<std::size_t>(length));
  for (std::size_t n = 0; n < w.size(); ++n) {
    w[n] = 0.5 - 0.5 * std::cos(constants::kTwoPiD * static_cast<double>(n) /
                                static_cast<double>(length));
  }
  return w;
}

/// 10*log10 of the windowed power spectrum, bins 1..N/2 inclusive, one row per frame. DC is
/// excluded: it carries the frame's offset, not its spectrum.
inline std::vector<std::vector<double>> log_power_spectra(const std::vector<double>& x,
                                                          int frame_length, int hop_length) {
  check_power_of_two(frame_length);
  const std::size_t count = frame_count(x.size(), frame_length, hop_length);
  const auto length = static_cast<std::size_t>(frame_length);
  const std::vector<double> window = hann_periodic(frame_length);
  std::vector<std::vector<double>> rows;
  rows.reserve(count);
  for (std::size_t f = 0; f < count; ++f) {
    const std::size_t offset = f * static_cast<std::size_t>(hop_length);
    std::vector<std::complex<double>> buffer(length);
    for (std::size_t i = 0; i < length; ++i) {
      buffer[i] = std::complex<double>(window[i] * x[offset + i], 0.0);
    }
    fft(buffer);
    std::vector<double> row(length / 2);
    for (std::size_t k = 1; k <= length / 2; ++k) {
      const double power =
          buffer[k].real() * buffer[k].real() + buffer[k].imag() * buffer[k].imag();
      row[k - 1] = 10.0 * std::log10(power + kSpectrumEpsilon);
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

inline double root_mean_square(const std::vector<double>& x) {
  double sum = 0.0;
  for (const double v : x) sum += v * v;
  return std::sqrt(sum / static_cast<double>(x.size()));
}

/// Scale `other` by one global factor so its RMS equals the reference's.
inline std::vector<double> rms_matched(const std::vector<double>& reference,
                                       const std::vector<double>& other) {
  const double rms = root_mean_square(other);
  if (rms <= 0.0) return other;
  const double scale = root_mean_square(reference) / rms;
  std::vector<double> scaled(other.size());
  for (std::size_t i = 0; i < other.size(); ++i) scaled[i] = other[i] * scale;
  return scaled;
}

/// Raw (non-excess) kurtosis of every cell pooled in row-major order; NaN without spread.
inline double pooled_kurtosis(const std::vector<std::vector<double>>& rows) {
  std::size_t count = 0;
  double sum = 0.0;
  for (const auto& row : rows) {
    for (const double v : row) {
      sum += v;
      ++count;
    }
  }
  if (count == 0) return quiet_nan();
  const double mean = sum / static_cast<double>(count);
  double m2 = 0.0;
  double m4 = 0.0;
  for (const auto& row : rows) {
    for (const double v : row) {
      const double d = v - mean;
      const double squared = d * d;
      m2 += squared;
      m4 += squared * squared;
    }
  }
  m2 /= static_cast<double>(count);
  m4 /= static_cast<double>(count);
  if (m2 <= 0.0) return quiet_nan();
  return m4 / (m2 * m2);
}

}  // namespace detail

/// @brief A segmental SNR together with how much of it came off the clip.
///
/// A row whose frames all sat on the ceiling reports kSegSnrCeilingDb however the processing
/// changed, so it reads as "not worse" against any baseline and lets every regression through. The
/// clip stays where the convention puts it; this is what lets a caller see it carrying the value.
/// Mirrors metrics_repair.SegmentalSnrReport field for field.
struct SegmentalSnrReport {
  double value = 0.0;              ///< Exactly what segmental_snr() returns.
  std::size_t active_frames = 0;   ///< Frames above the silence threshold, the averaged set.
  std::size_t ceiling_frames = 0;  ///< Active frames taken at kSegSnrCeilingDb.
  std::size_t floor_frames = 0;    ///< Active frames taken at kSegSnrFloorDb.

  /// Share of the averaged frames that hit the ceiling; 0.0 when no frame was active.
  double ceiling_fraction() const {
    if (active_frames == 0) return 0.0;
    return static_cast<double>(ceiling_frames) / static_cast<double>(active_frames);
  }

  /// Share of the averaged frames that hit the floor; 0.0 when no frame was active.
  double floor_fraction() const {
    if (active_frames == 0) return 0.0;
    return static_cast<double>(floor_frames) / static_cast<double>(active_frames);
  }

  /// Whether the value is itself a clip bound, which happens only when every averaged frame hit
  /// that bound. Such a row cannot report a change in either direction.
  bool saturated() const { return value == kSegSnrCeilingDb || value == kSegSnrFloorDb; }
};

/// @brief Segmental SNR with the clip counts the value alone cannot show.
/// @param clean Clean reference signal, one channel.
/// @param processed Processed signal of the same length.
/// @param sample_rate Rate the signals were measured at. The framing is in samples and the
///        arithmetic does not read this; it is in the signature so that every metric is called the
///        same way and each recorded row carries the rate its frames were taken at.
/// @param frame_length Frame length in samples.
/// @param hop_length Hop in samples.
/// @return The value, the averaged population and the two clip counts. With no active frame the
///         value is NaN, the counts are 0 and saturated() is false.
///
/// Frames below kSegSnrSilenceDbfs are excluded and each frame is clipped to
/// [kSegSnrFloorDb, kSegSnrCeilingDb]; without both, one near-silent or bit-exact frame decides the
/// file's average. Deliberately gain-sensitive: the reference is the clean signal at its own level.
inline SegmentalSnrReport segmental_snr_report(const std::vector<float>& clean,
                                               const std::vector<float>& processed,
                                               double sample_rate, int frame_length = kFrameLength,
                                               int hop_length = kHopLength) {
  (void)sample_rate;
  detail::check_pair(clean, processed, frame_length, hop_length);
  SegmentalSnrReport report;
  report.value = detail::quiet_nan();
  const std::size_t count = detail::frame_count(clean.size(), frame_length, hop_length);
  if (count == 0) return report;

  const double silence_mean_square = std::pow(10.0, kSegSnrSilenceDbfs / 10.0);
  const auto length = static_cast<std::size_t>(frame_length);
  double total = 0.0;
  for (std::size_t f = 0; f < count; ++f) {
    const std::size_t offset = f * static_cast<std::size_t>(hop_length);
    double signal = 0.0;
    double error = 0.0;
    for (std::size_t i = 0; i < length; ++i) {
      const double reference = static_cast<double>(clean[offset + i]);
      const double difference = reference - static_cast<double>(processed[offset + i]);
      signal += reference * reference;
      error += difference * difference;
    }
    if (signal / static_cast<double>(frame_length) < silence_mean_square) continue;
    const double snr = error > 0.0 ? 10.0 * std::log10(signal / error) : kSegSnrCeilingDb;
    const double clipped = std::clamp(snr, kSegSnrFloorDb, kSegSnrCeilingDb);
    total += clipped;
    ++report.active_frames;
    if (clipped == kSegSnrCeilingDb) ++report.ceiling_frames;
    if (clipped == kSegSnrFloorDb) ++report.floor_frames;
  }
  if (report.active_frames == 0) return SegmentalSnrReport{detail::quiet_nan(), 0, 0, 0};
  report.value = total / static_cast<double>(report.active_frames);
  return report;
}

/// @brief Frame-averaged segmental SNR in dB between a clean reference and a processed signal.
/// @param clean Clean reference signal, one channel.
/// @param processed Processed signal of the same length.
/// @param sample_rate See segmental_snr_report().
/// @param frame_length Frame length in samples.
/// @param hop_length Hop in samples.
/// @return Mean per-frame SNR in dB over the active frames, NaN when no frame is active.
///
/// Thin wrapper over segmental_snr_report(), which carries the arithmetic and the saturation
/// counts; the two cannot disagree about the value.
inline double segmental_snr(const std::vector<float>& clean, const std::vector<float>& processed,
                            double sample_rate, int frame_length = kFrameLength,
                            int hop_length = kHopLength) {
  return segmental_snr_report(clean, processed, sample_rate, frame_length, hop_length).value;
}

/// @brief Ratio of the processed log-spectral kurtosis to the unprocessed input's.
/// @param unprocessed Signal as it entered the processing, one channel.
/// @param processed Signal as it left, same length.
/// @param sample_rate See segmental_snr().
/// @param frame_length Frame length in samples, a power of two.
/// @param hop_length Hop in samples.
/// @return 1.0 when the processing created no new isolated spectral peaks, above 1.0 when it did,
///         NaN when either log spectrum has zero spread.
///
/// One scalar over every (frame, bin) cell pooled, after Uemura and Saruwatari. The processed
/// signal is RMS-matched to the input first: a kurtosis is already blind to the constant log offset
/// a gain produces, but the epsilon floor is not.
inline double log_kurtosis_ratio(const std::vector<float>& unprocessed,
                                 const std::vector<float>& processed, double sample_rate,
                                 int frame_length = kFrameLength, int hop_length = kHopLength) {
  (void)sample_rate;
  detail::check_pair(unprocessed, processed, frame_length, hop_length);
  const std::vector<double> source = detail::to_double(unprocessed);
  const std::vector<double> output = detail::rms_matched(source, detail::to_double(processed));

  const auto source_rows = detail::log_power_spectra(source, frame_length, hop_length);
  const auto output_rows = detail::log_power_spectra(output, frame_length, hop_length);
  if (source_rows.empty()) return detail::quiet_nan();

  const double source_kurtosis = detail::pooled_kurtosis(source_rows);
  if (!std::isfinite(source_kurtosis) || source_kurtosis == 0.0) return detail::quiet_nan();
  return detail::pooled_kurtosis(output_rows) / source_kurtosis;
}

/// @brief Frame-averaged log-spectral distance in dB, after matching the two signals' RMS.
/// @param reference Reference signal, one channel.
/// @param processed Processed signal of the same length.
/// @param sample_rate See segmental_snr().
/// @param frame_length Frame length in samples, a power of two.
/// @param hop_length Hop in samples.
/// @return Mean per-frame RMS log-spectral difference in dB, NaN when there is no frame.
///
/// The processed signal is scaled by one global factor before the comparison, so a pure gain reads
/// 0 rather than |20*log10(g)|; a metric that moves under a gain change is reading loudness as
/// quality. Only the flat gain is removed - a tilt still registers.
inline double log_spectral_distance(const std::vector<float>& reference,
                                    const std::vector<float>& processed, double sample_rate,
                                    int frame_length = kFrameLength, int hop_length = kHopLength) {
  (void)sample_rate;
  detail::check_pair(reference, processed, frame_length, hop_length);
  const std::vector<double> source = detail::to_double(reference);
  const std::vector<double> output = detail::rms_matched(source, detail::to_double(processed));

  const auto source_rows = detail::log_power_spectra(source, frame_length, hop_length);
  const auto output_rows = detail::log_power_spectra(output, frame_length, hop_length);
  if (source_rows.empty()) return detail::quiet_nan();

  double total = 0.0;
  for (std::size_t f = 0; f < source_rows.size(); ++f) {
    double sum = 0.0;
    for (std::size_t k = 0; k < source_rows[f].size(); ++k) {
      const double d = source_rows[f][k] - output_rows[f][k];
      sum += d * d;
    }
    total += std::sqrt(sum / static_cast<double>(source_rows[f].size()));
  }
  return total / static_cast<double>(source_rows.size());
}

}  // namespace sonare::test::repair_metrics
