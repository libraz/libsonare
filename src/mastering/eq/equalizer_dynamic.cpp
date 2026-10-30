#include <algorithm>
#include <cmath>

#include "mastering/eq/dynamic_detector.h"
#include "mastering/eq/equalizer.h"
#include "rt/biquad_design.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/dsp_primitives.h"
#include "util/exception.h"
#include "util/non_finite_state.h"

namespace sonare::mastering::eq {

using sonare::discard_if_non_finite;
using sonare::constants::kFloorDb;

namespace {

float level_db(double sum, double count) {
  return linear_to_db(static_cast<float>(std::sqrt(sum / std::max(count, 1.0))));
}

}  // namespace

void EqualizerProcessor::accumulate_band_detector(size_t band_index, const float* const* channels,
                                                  int num_channels, int count, int excluded_channel,
                                                  const EqBand& band, int readable_samples) {
  if (count <= 0 || num_channels <= 0) return;

  const BandDetectorDesign design =
      design_band_detector(sample_rate_, band.frequency_hz, band.dyn.sidechain_freq_hz,
                           band.dyn.sidechain_q, band.dyn.attack_ms, band.dyn.release_ms);
  const size_t look = static_cast<size_t>(band_detector_delay_samples(
      sample_rate_, band.dyn.detector_delay_ms, max_detector_delay_samples_));

  auto& states = detector_states_[band_index];
  // States are preallocated in prepare(); only grow if a wider channel count is
  // seen (e.g. an unprepared offline call). Never shrink so state persists.
  if (states.size() < static_cast<size_t>(num_channels)) {
    states.resize(static_cast<size_t>(num_channels));
  }

  double sum = 0.0;
  int counted = 0;
  for (int ch = 0; ch < num_channels; ++ch) {
    if (ch == excluded_channel) continue;
    ++counted;
    BandDetectorChannel& state = states[static_cast<size_t>(ch)];
    state.set_delay(look);
    sum += state.accumulate(design, channels[ch], count, readable_samples);
  }
  band_detector_sum_[band_index] += sum;
  band_detector_count_[band_index] += static_cast<double>(counted) * count;
}

void EqualizerProcessor::clear_control_accumulators() noexcept {
  detector_sum_ = 0.0;
  detector_count_ = 0.0;
  band_detector_sum_.fill(0.0);
  band_detector_count_.fill(0.0);
  auto_gain_input_sum_ = 0.0;
  auto_gain_output_sum_ = 0.0;
  auto_gain_count_ = 0.0;
}

float EqualizerProcessor::dynamic_gain_delta(const EqBand& band, float detector_db,
                                             float threshold_db) {
  if (!band.enabled || !band.dyn.enabled || detector_db <= threshold_db ||
      band.dyn.range_db == 0.0f) {
    return 0.0f;
  }
  return dynamic_compression_delta(detector_db, threshold_db, band.dyn.ratio, band.dyn.range_db);
}

void EqualizerProcessor::accumulate_control_input(const float* const* channels, int num_channels,
                                                  int offset, int count) {
  if (has_dynamic_bands_) {
    // The host's excluded plane (an LFE) is left out of the main-input detector;
    // an external key is the caller's own choice of channels and keeps them all.
    const int excluded = detector_excluded_channel(num_channels);
    double sum = 0.0;
    int counted = 0;
    for (int ch = 0; ch < num_channels; ++ch) {
      if (ch == excluded) continue;
      ++counted;
      for (int i = 0; i < count; ++i) {
        const double sample = channels[ch][i];
        sum += sample * sample;
      }
    }
    detector_sum_ += sum;
    detector_count_ += static_cast<double>(counted) * count;

    for (size_t i = 0; i < kMaxBands; ++i) {
      const auto& band = bands_[i];
      if (!band.enabled || !band.dyn.enabled) continue;
      const bool use_external = band.dyn.external_sidechain && sidechain_channels_ != nullptr;
      if (!use_external) {
        accumulate_band_detector(i, channels, num_channels, count, excluded, band, count);
        continue;
      }
      // A key shorter than the block reads as silence past its end.
      const int readable = std::clamp(sidechain_num_samples_ - offset, 0, count);
      for (int ch = 0; ch < sidechain_num_channels_; ++ch) {
        chunk_sidechain_[static_cast<size_t>(ch)] =
            sidechain_channels_[ch] + (readable > 0 ? offset : 0);
      }
      accumulate_band_detector(i, chunk_sidechain_.data(), sidechain_num_channels_, count, -1, band,
                               readable);
    }
  }
  if (auto_gain_enabled_) {
    double sum = 0.0;
    for (int ch = 0; ch < num_channels; ++ch) {
      for (int i = 0; i < count; ++i) {
        const double sample = channels[ch][i];
        sum += sample * sample;
      }
    }
    auto_gain_input_sum_ += sum;
    auto_gain_count_ += static_cast<double>(num_channels) * count;
  }
}

bool EqualizerProcessor::settle_control() {
  bool discarded = false;
  if (has_dynamic_bands_) {
    last_detector_db_ = level_db(detector_sum_, detector_count_);
    for (size_t i = 0; i < kMaxBands; ++i) {
      const auto& band = bands_[i];
      if (band.enabled && band.dyn.enabled) {
        if (!dynamic_reading_seen_[i]) {
          dynamic_gain_live_[i] = false;
          dynamic_reading_seen_[i] = true;
        }
        last_band_detector_db_[i] = level_db(band_detector_sum_[i], band_detector_count_[i]);
        if (band.dyn.auto_threshold) {
          const float target = last_band_detector_db_[i] - 6.0f;
          if (auto_threshold_db_[i] <= kFloorDb + 1.0f) {
            auto_threshold_db_[i] = target;
          } else {
            const double smoothing_samples = std::max(sample_rate_ * 0.250, 1.0);
            const float coeff = static_cast<float>(
                1.0 - std::exp(-static_cast<double>(kControlIntervalSamples) / smoothing_samples));
            auto_threshold_db_[i] += coeff * (target - auto_threshold_db_[i]);
          }
        }
      } else {
        dynamic_reading_seen_[i] = false;
        last_band_detector_db_[i] = kFloorDb;
        last_applied_gain_db_[i] = 0.0f;
      }
    }
    // Before the detector's reading reaches coefficient design, so a poisoned
    // detector cannot hand a non-finite gain to a band.
    discarded |= discard_non_finite_dynamic_state();
    update_iir_bands_preserving_state(kControlIntervalSamples);
  }
  if (auto_gain_enabled_) {
    const float input_db = level_db(auto_gain_input_sum_, auto_gain_count_);
    const float output_db = level_db(auto_gain_output_sum_, auto_gain_count_);
    if (input_db <= kFloorDb + 1.0f || output_db <= kFloorDb + 1.0f) {
      last_auto_gain_db_ = 0.0f;
    } else {
      const float target_db = std::clamp(input_db - output_db, -24.0f, 24.0f);
      const double smoothing_samples = std::max(sample_rate_ * 0.050, 1.0);
      const float coeff = static_cast<float>(
          1.0 - std::exp(-static_cast<double>(kControlIntervalSamples) / smoothing_samples));
      smoothed_auto_gain_db_ += coeff * (target_db - smoothed_auto_gain_db_);
      last_auto_gain_db_ = smoothed_auto_gain_db_;
    }
    // The smoother is driven by the interval RMS of both the input and the output, so a
    // non-finite sample reaches it whether or not it survived the filters.
    const bool smoothed_discarded = discard_if_non_finite(smoothed_auto_gain_db_, 0.0f);
    if (smoothed_discarded) last_auto_gain_db_ = 0.0f;
    discarded |= smoothed_discarded;
    discarded |= discard_if_non_finite(last_auto_gain_db_, 0.0f);
    auto_gain_linear_ = db_to_linear(last_auto_gain_db_);
  }
  clear_control_accumulators();
  return discarded;
}

void EqualizerProcessor::validate_sidechain(int expected_samples) const {
  if (sidechain_channels_ == nullptr) {
    return;
  }
  if (sidechain_num_samples_ > expected_samples) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "sidechain length must not exceed process block length");
  }
}

void EqualizerProcessor::apply_auto_gain(float* const* channels, int num_channels,
                                         int num_samples) noexcept {
  if (!auto_gain_enabled_ || channels == nullptr || num_channels <= 0 || num_samples <= 0) {
    last_auto_gain_db_ = 0.0f;
    auto_gain_linear_ = 1.0f;
    return;
  }
  double sum = 0.0;
  for (int ch = 0; ch < num_channels; ++ch) {
    for (int i = 0; i < num_samples; ++i) {
      const double sample = channels[ch][i];
      sum += sample * sample;
    }
  }
  auto_gain_output_sum_ += sum;
  if (auto_gain_linear_ == 1.0f) return;
  for (int ch = 0; ch < num_channels; ++ch) {
    for (int i = 0; i < num_samples; ++i) {
      channels[ch][i] *= auto_gain_linear_;
    }
  }
}

void EqualizerProcessor::apply_output_gain_and_pan(float* const* channels, int num_channels,
                                                   int num_samples) noexcept {
  if (channels == nullptr || num_channels <= 0 || num_samples <= 0) {
    return;
  }
  const float gain = db_to_linear(output_gain_db_);
  float left_gain = gain;
  float right_gain = gain;
  if (num_channels >= 2) {
    if (output_pan_ < 0.0f) {
      right_gain *= 1.0f + output_pan_;
    } else if (output_pan_ > 0.0f) {
      left_gain *= 1.0f - output_pan_;
    }
  }
  if (num_channels == 1) {
    for (int i = 0; i < num_samples; ++i) {
      channels[0][i] *= gain;
    }
    return;
  }
  for (int i = 0; i < num_samples; ++i) {
    channels[0][i] *= left_gain;
    channels[1][i] *= right_gain;
  }
  for (int ch = 2; ch < num_channels; ++ch) {
    for (int i = 0; i < num_samples; ++i) {
      channels[ch][i] *= gain;
    }
  }
}
}  // namespace sonare::mastering::eq
