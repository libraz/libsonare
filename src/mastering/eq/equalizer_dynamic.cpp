#include <algorithm>
#include <cmath>

#include "mastering/eq/equalizer.h"
#include "rt/biquad_design.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/dsp_primitives.h"
#include "util/exception.h"

namespace sonare::mastering::eq {

using sonare::constants::kFloorDb;

float EqualizerProcessor::detector_db(const float* const* channels, int num_channels,
                                      int num_samples) {
  double sum = 0.0;
  for (int ch = 0; ch < num_channels; ++ch) {
    for (int i = 0; i < num_samples; ++i) {
      const double sample = channels[ch][i];
      sum += sample * sample;
    }
  }
  const double count = static_cast<double>(num_channels) * static_cast<double>(num_samples);
  return linear_to_db(static_cast<float>(std::sqrt(sum / std::max(count, 1.0))));
}

float EqualizerProcessor::band_detector_db(size_t band_index, const float* const* channels,
                                           int num_channels, int num_samples, double sample_rate,
                                           const EqBand& band) {
  if (num_samples <= 0 || num_channels <= 0) return kFloorDb;

  // Shared bandpass coefficients for this band's detector. Each channel keeps its
  // own persistent z-state in detector_states_ so the filter and envelope carry
  // over between blocks (see DetectorState comment in equalizer.h).
  const double detector_frequency =
      band.dyn.sidechain_freq_hz > 0.0f ? band.dyn.sidechain_freq_hz : band.frequency_hz;
  const double frequency =
      std::clamp(static_cast<double>(detector_frequency), 1.0, sample_rate * 0.5 - 1.0);
  const auto coeffs = rt::rbj_bandpass_d(
      frequency, sample_rate, std::max(static_cast<double>(band.dyn.sidechain_q), 1.0e-6));
  const double b0 = coeffs.b0;
  const double b1 = coeffs.b1;
  const double b2 = coeffs.b2;
  const double a1 = coeffs.a1;
  const double a2 = coeffs.a2;

  const double attack = sonare::time_to_attack_release_rate(sample_rate, band.dyn.attack_ms);
  const double release = sonare::time_to_attack_release_rate(sample_rate, band.dyn.release_ms);
  // Clamp the lookahead to the ring capacity preallocated in prepare() so the
  // audio-thread path never reallocates (automation past the bound saturates).
  const int lookahead_samples =
      std::clamp(static_cast<int>(std::round(sample_rate * band.dyn.lookahead_ms * 0.001)), 0,
                 max_detector_lookahead_samples_);

  auto& states = detector_states_[band_index];
  // States are preallocated in prepare(); only grow if a wider channel count is
  // seen (e.g. an unprepared offline call). Never shrink so state persists.
  if (states.size() < static_cast<size_t>(num_channels)) {
    states.resize(static_cast<size_t>(num_channels));
  }

  const auto biquad = [&](double x, double& z1, double& z2) {
    const double y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  };

  double sum = 0.0;
  for (int ch = 0; ch < num_channels; ++ch) {
    DetectorState& s = states[static_cast<size_t>(ch)];
    // Re-window the persistent lookahead ring when the lookahead changes. The
    // ring is preallocated; offline/unprepared paths that grew states above may
    // have an unsized ring, so grow it here (only off the audio thread).
    const size_t look = static_cast<size_t>(std::max(lookahead_samples, 0));
    if (s.look_size != look) {
      if (s.look_ring.size() < look) {
        s.look_ring.assign(look, 0.0f);
      } else {
        std::fill(s.look_ring.begin(), s.look_ring.begin() + look, 0.0f);
      }
      s.look_size = look;
      s.look_pos = 0;
    }
    double envelope = s.envelope;
    // The detector consumes a continuous stream: a per-channel FIFO carries the
    // last `look` input samples from the previous block into this one, so a
    // transient near a block boundary is still detected (within ~look samples)
    // instead of the final sample being repeated.
    for (int i = 0; i < num_samples; ++i) {
      float input;
      if (look > 0) {
        input = s.look_ring[s.look_pos];
        s.look_ring[s.look_pos] = channels[ch][i];
        s.look_pos = (s.look_pos + 1) % look;
      } else {
        input = channels[ch][i];
      }
      const double stage_a = biquad(input, s.filter_a_z1, s.filter_a_z2);
      const double rectified = std::abs(biquad(stage_a, s.filter_b_z1, s.filter_b_z2));
      const double coeff = rectified > envelope ? attack : release;
      envelope += coeff * (rectified - envelope);
      sum += envelope * envelope;
    }
    s.envelope = envelope;
  }
  const double count = static_cast<double>(num_channels) * static_cast<double>(num_samples);
  return linear_to_db(static_cast<float>(std::sqrt(sum / std::max(count, 1.0))));
}

float EqualizerProcessor::rms_db(const float* const* channels, int num_channels,
                                 int num_samples) noexcept {
  if (channels == nullptr || num_channels <= 0 || num_samples <= 0) {
    return kFloorDb;
  }
  double sum = 0.0;
  size_t count = 0;
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) {
      continue;
    }
    for (int i = 0; i < num_samples; ++i) {
      const double sample = channels[ch][i];
      sum += sample * sample;
      ++count;
    }
  }
  return count == 0 ? kFloorDb
                    : linear_to_db(static_cast<float>(std::sqrt(sum / static_cast<double>(count))));
}

float EqualizerProcessor::dynamic_gain_delta(const EqBand& band, float detector_db,
                                             float threshold_db) {
  if (!band.enabled || !band.dyn.enabled || detector_db <= threshold_db ||
      band.dyn.range_db == 0.0f) {
    return 0.0f;
  }

  const float over_db = detector_db - threshold_db;
  const float compressed_db = over_db * (1.0f - 1.0f / band.dyn.ratio);
  const float range = std::abs(band.dyn.range_db);
  const float amount = std::min(range, compressed_db);
  return band.dyn.range_db < 0.0f ? -amount : amount;
}

void EqualizerProcessor::update_dynamic_state(const float* const* channels, int num_channels,
                                              int num_samples) {
  last_detector_db_ = detector_db(channels, num_channels, num_samples);
  for (size_t i = 0; i < kMaxBands; ++i) {
    const auto& band = bands_[i];
    if (band.enabled && band.dyn.enabled) {
      const bool use_external = band.dyn.external_sidechain && sidechain_channels_ != nullptr;
      const float* const* detector_channels = use_external ? sidechain_channels_ : channels;
      const int detector_num_channels = use_external ? sidechain_num_channels_ : num_channels;
      last_band_detector_db_[i] = band_detector_db(i, detector_channels, detector_num_channels,
                                                   num_samples, sample_rate_, band);
      if (band.dyn.auto_threshold) {
        const float target = last_band_detector_db_[i] - 6.0f;
        if (auto_threshold_db_[i] <= kFloorDb + 1.0f) {
          auto_threshold_db_[i] = target;
        } else {
          const double smoothing_samples = std::max(sample_rate_ * 0.250, 1.0);
          const float coeff = static_cast<float>(
              1.0 - std::exp(-static_cast<double>(num_samples) / smoothing_samples));
          auto_threshold_db_[i] += coeff * (target - auto_threshold_db_[i]);
        }
      }
    } else {
      last_band_detector_db_[i] = kFloorDb;
      last_applied_gain_db_[i] = 0.0f;
    }
  }
}

void EqualizerProcessor::validate_sidechain(int expected_samples) const {
  if (sidechain_channels_ == nullptr) {
    return;
  }
  if (sidechain_num_samples_ != expected_samples) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "sidechain length must match process block length");
  }
}

void EqualizerProcessor::apply_auto_gain(float* const* channels, int num_channels, int num_samples,
                                         float input_db) noexcept {
  if (!auto_gain_enabled_ || channels == nullptr || num_channels <= 0 || num_samples <= 0) {
    last_auto_gain_db_ = 0.0f;
    return;
  }
  const float output_db =
      rms_db(const_cast<const float* const*>(channels), num_channels, num_samples);
  if (input_db <= kFloorDb + 1.0f || output_db <= kFloorDb + 1.0f) {
    last_auto_gain_db_ = 0.0f;
    return;
  }
  const float target_db = std::clamp(input_db - output_db, -24.0f, 24.0f);
  const double smoothing_samples = std::max(sample_rate_ * 0.050, 1.0);
  const float coeff =
      static_cast<float>(1.0 - std::exp(-static_cast<double>(num_samples) / smoothing_samples));
  smoothed_auto_gain_db_ += coeff * (target_db - smoothed_auto_gain_db_);
  last_auto_gain_db_ = smoothed_auto_gain_db_;
  const float gain = db_to_linear(last_auto_gain_db_);
  for (int ch = 0; ch < num_channels; ++ch) {
    for (int i = 0; i < num_samples; ++i) {
      channels[ch][i] *= gain;
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
