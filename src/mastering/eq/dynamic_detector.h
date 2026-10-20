#pragma once

/// @file dynamic_detector.h
/// @brief Shared detector primitives for the dynamic-EQ processors.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include "rt/biquad_design.h"
#include "util/db.h"
#include "util/dsp_primitives.h"

namespace sonare::mastering::eq {

/// Maximum per-band detector delay the detector rings are preallocated for. A
/// delay past this bound saturates so the audio-thread path never resizes.
inline constexpr float kMaxDetectorDelayMs = 20.0f;

/// @brief Broadband RMS level of a block, in dB.
/// @details Root-mean-square over all channels and samples, converted to dB.
///          The accumulation is double-precision and the sample count is floored
///          at 1 to avoid a divide-by-zero on an empty block. Shared by the
///          static parametric EQ (dynamic bands) and the dedicated dynamic EQ so
///          their broadband detector reads identically. Only the first
///          @p readable_samples positions are read (negative = all); the rest of
///          the block counts as silence, so a short external key is zero-padded.
inline float broadband_detector_db(const float* const* channels, int num_channels, int num_samples,
                                   int excluded_channel, int readable_samples = -1) {
  const int readable =
      (readable_samples < 0 || readable_samples > num_samples) ? num_samples : readable_samples;
  double sum = 0.0;
  int counted = 0;
  for (int ch = 0; ch < num_channels; ++ch) {
    if (ch == excluded_channel) continue;
    ++counted;
    for (int i = 0; i < readable; ++i) {
      const double sample = channels[ch][i];
      sum += sample * sample;
    }
  }
  const double count = static_cast<double>(counted) * static_cast<double>(num_samples);
  return linear_to_db(static_cast<float>(std::sqrt(sum / std::max(count, 1.0))));
}

/// @brief Downward-compression gain delta (dB) for a band above its threshold.
/// @details Applies the compression curve to the detector level once the caller
///          has confirmed the band is active and @p detector_db exceeds
///          @p threshold_db: the over-threshold excess is scaled by
///          @c (1 - 1/ratio), clamped to @c |range_db|, and signed by
///          @p range_db (a negative range cuts, a positive range boosts). The
///          caller supplies the band-specific enable/threshold guard.
inline float dynamic_compression_delta(float detector_db, float threshold_db, float ratio,
                                       float range_db) {
  const float over_db = detector_db - threshold_db;
  const float compressed_db = over_db * (1.0f - 1.0f / ratio);
  const float range = std::abs(range_db);
  const float amount = std::min(range, compressed_db);
  return range_db < 0.0f ? -amount : amount;
}

/// @brief Coefficients and timing of one dynamic band's side-chain detector.
struct BandDetectorDesign {
  rt::BiquadCoeffsD coeffs;  ///< Band-pass applied twice (4th order).
  double attack = 1.0;       ///< Envelope follower weight while the level rises.
  double release = 1.0;      ///< Envelope follower weight while the level falls.
};

/// @brief Designs a band detector in double precision.
/// @details The band-pass centres on @p sidechain_freq_hz when positive, otherwise on the band's
///          own @p frequency_hz, clamped inside the audible range at @p sample_rate.
inline BandDetectorDesign design_band_detector(double sample_rate, float frequency_hz,
                                               float sidechain_freq_hz, float sidechain_q,
                                               float attack_ms, float release_ms) {
  const double detector_frequency = sidechain_freq_hz > 0.0f ? sidechain_freq_hz : frequency_hz;
  const double frequency = std::clamp(detector_frequency, 1.0, sample_rate * 0.5 - 1.0);
  BandDetectorDesign design;
  design.coeffs = rt::rbj_bandpass_d(frequency, sample_rate,
                                     std::max(static_cast<double>(sidechain_q), 1.0e-6));
  design.attack = time_to_attack_release_rate(sample_rate, attack_ms);
  design.release = time_to_attack_release_rate(sample_rate, release_ms);
  return design;
}

/// @brief Detector delay in samples, saturated at the preallocated ring capacity.
inline int band_detector_delay_samples(double sample_rate, float delay_ms, int max_samples) {
  return static_cast<int>(std::clamp(std::round(sample_rate * delay_ms * 0.001), 0.0,
                                     static_cast<double>(std::max(max_samples, 0))));
}

/// @brief Persistent detector state of one band on one channel.
/// @details The filters, envelope and delay ring carry over between blocks, so the detector reads
///          a continuous stream. The delay is a FIFO on the detector's own input: a larger delay
///          makes the band react later and adds nothing to the audio path.
struct BandDetectorChannel {
  rt::BiquadStateD filter_a;
  rt::BiquadStateD filter_b;
  double envelope = 0.0;
  std::vector<float>
      look_ring;  ///< Sized to the maximum delay; the first look_size entries are live.
  size_t look_size = 0;
  size_t look_pos = 0;

  /// Sizes the ring for @p max_delay_samples and returns the state to rest.
  void prepare(int max_delay_samples) {
    look_ring.assign(static_cast<size_t>(std::max(max_delay_samples, 0)), 0.0f);
    look_size = 0;
    look_pos = 0;
    filter_a.reset();
    filter_b.reset();
    envelope = 0.0;
  }

  /// Zeroes the filters, envelope and live delay window without releasing the ring.
  void reset() {
    filter_a.reset();
    filter_b.reset();
    envelope = 0.0;
    std::fill(look_ring.begin(), look_ring.begin() + static_cast<std::ptrdiff_t>(look_size), 0.0f);
    look_pos = 0;
  }

  /// Re-windows the ring to @p delay_samples, zeroing it, when the delay changed.
  void set_delay(size_t delay_samples) {
    if (look_size == delay_samples) return;
    if (look_ring.size() < delay_samples) {
      look_ring.assign(delay_samples, 0.0f);
    } else {
      std::fill(look_ring.begin(), look_ring.begin() + static_cast<std::ptrdiff_t>(delay_samples),
                0.0f);
    }
    look_size = delay_samples;
    look_pos = 0;
  }

  /// @brief Runs the detector over one block and returns the sum of the squared envelope.
  /// @details Only the first @p readable_samples inputs are read (negative = all); the rest of the
  ///          block counts as silence so a short external key is zero-padded.
  double accumulate(const BandDetectorDesign& design, const float* input, int num_samples,
                    int readable_samples) {
    const int readable =
        (readable_samples < 0 || readable_samples > num_samples) ? num_samples : readable_samples;
    filter_a.set(design.coeffs);
    filter_b.set(design.coeffs);
    double sum = 0.0;
    for (int i = 0; i < num_samples; ++i) {
      const float fresh = i < readable ? input[i] : 0.0f;
      float sample = fresh;
      if (look_size > 0) {
        sample = look_ring[look_pos];
        look_ring[look_pos] = fresh;
        look_pos = (look_pos + 1) % look_size;
      }
      const double rectified = std::abs(filter_b.process(filter_a.process(sample)));
      const double coeff = rectified > envelope ? design.attack : design.release;
      envelope += coeff * (rectified - envelope);
      sum += envelope * envelope;
    }
    return sum;
  }
};

/// @brief Detector level in dB from a summed squared envelope over @p counted channels.
inline float band_detector_level_db(double sum, int counted, int num_samples) {
  const double count = static_cast<double>(counted) * static_cast<double>(num_samples);
  return linear_to_db(static_cast<float>(std::sqrt(sum / std::max(count, 1.0))));
}

}  // namespace sonare::mastering::eq
