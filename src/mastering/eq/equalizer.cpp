#include "mastering/eq/equalizer.h"

#include <algorithm>
#include <cmath>

#include "mastering/eq/spectrum_registry.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/exception.h"

namespace sonare::mastering::eq {

using sonare::constants::kFloorDb;

EqualizerProcessor::EqualizerProcessor(EqualizerProcessorConfig config)
    : config_(config),
      left_channel_fir_(config_.linear_phase_config),
      right_channel_fir_(config_.linear_phase_config),
      mid_fir_(config_.linear_phase_config),
      side_fir_(config_.linear_phase_config) {
  if (config_.max_channels < 0) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "EqualizerProcessor max_channels must be non-negative");
  }
}

EqualizerProcessor::~EqualizerProcessor() {
  if (config_.spectrum_instance_id != 0) {
    SpectrumRegistry::instance().remove(config_.spectrum_instance_id);
  }
}

void EqualizerProcessor::prepare(double sample_rate, int max_block_size) {
  if (max_block_size < 0) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "EqualizerProcessor max_block_size must be non-negative");
  }
  if (!(sample_rate > 0.0)) {
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  }
  sample_rate_ = sample_rate;
  max_block_size_ = max_block_size;
  stereo_iir_.prepare(sample_rate, max_block_size);
  left_iir_.prepare(sample_rate, max_block_size);
  right_iir_.prepare(sample_rate, max_block_size);
  mid_iir_.prepare(sample_rate, max_block_size);
  side_iir_.prepare(sample_rate, max_block_size);
  left_channel_fir_.prepare(sample_rate, max_block_size);
  right_channel_fir_.prepare(sample_rate, max_block_size);
  mid_fir_.prepare(sample_rate, max_block_size);
  side_fir_.prepare(sample_rate, max_block_size);
  stereo_iir_.prepare_channels(config_.max_channels);
  left_iir_.prepare_channels(1);
  right_iir_.prepare_channels(1);
  mid_iir_.prepare_channels(1);
  side_iir_.prepare_channels(1);
  left_channel_fir_.prepare_channels(1);
  right_channel_fir_.prepare_channels(1);
  mid_fir_.prepare_channels(1);
  side_fir_.prepare_channels(1);
  mid_buffer_.assign(static_cast<size_t>(std::max(max_block_size, 0)), 0.0f);
  side_buffer_.assign(static_cast<size_t>(std::max(max_block_size, 0)), 0.0f);
  prepared_ = true;
  last_band_detector_db_.fill(kFloorDb);
  last_applied_gain_db_.fill(0.0f);
  smoothed_gain_db_.fill(0.0f);
  auto_threshold_db_.fill(kFloorDb);
  // Preallocate persistent detector state per band so the audio-thread detector
  // never resizes. Sized to the IIR backend's prepared channel capacity, with
  // each channel's lookahead ring preallocated to the maximum supported lookahead.
  max_detector_lookahead_samples_ =
      static_cast<int>(std::round(sample_rate_ * kMaxDetectorLookaheadMs * 0.001));
  for (auto& states : detector_states_) {
    states.assign(static_cast<size_t>(std::max(config_.max_channels, 0)), DetectorState{});
    for (auto& state : states) {
      state.look_ring.assign(static_cast<size_t>(std::max(max_detector_lookahead_samples_, 0)),
                             0.0f);
    }
  }
  validate_backend_capacity(bands_, phase_mode_);
  rebuild_iir();
}

void EqualizerProcessor::process(float* const* channels, int num_channels, int num_samples) {
  ensure_prepared(prepared_, "EqualizerProcessor");
  validate_process_args(channels, num_channels, num_samples);
  if (num_channels > config_.max_channels) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "EqualizerProcessor num_channels exceeds prepared max_channels");
  }
  if (num_samples > max_block_size_) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "EqualizerProcessor num_samples exceeds prepared max_block_size");
  }
  validate_sidechain(num_samples);
  EqualizerSpectrumSnapshot pre_snapshot;
  capture_stream(const_cast<const float* const*>(channels), num_channels, num_samples,
                 pre_snapshot.pre, pre_snapshot.pre_count);
  const float input_db =
      rms_db(const_cast<const float* const*>(channels), num_channels, num_samples);
  if (has_dynamic_bands_) {
    update_dynamic_state(const_cast<const float* const*>(channels), num_channels, num_samples);
    update_iir_bands_preserving_state(num_samples);
  }
  if (has_lr_linear_bands_) {
    if (num_channels >= 1) {
      process_mono_fir(left_channel_fir_, channels[0], num_samples);
    }
    if (num_channels >= 2) {
      process_mono_fir(right_channel_fir_, channels[1], num_samples);
    }
  }
  if (has_mid_side_linear_bands_) {
    if (num_channels != 2) {
      throw SonareException(ErrorCode::InvalidParameter,
                            "EqualizerProcessor Mid/Side placement requires stereo input");
    }
    for (int i = 0; i < num_samples; ++i) {
      const float left = channels[0][i];
      const float right = channels[1][i];
      mid_buffer_[static_cast<size_t>(i)] = (left + right) * 0.5f;
      side_buffer_[static_cast<size_t>(i)] = (left - right) * 0.5f;
    }
    process_mono_fir(mid_fir_, mid_buffer_.data(), num_samples);
    process_mono_fir(side_fir_, side_buffer_.data(), num_samples);
    for (int i = 0; i < num_samples; ++i) {
      const float mid = mid_buffer_[static_cast<size_t>(i)];
      const float side = side_buffer_[static_cast<size_t>(i)];
      channels[0][i] = mid + side;
      channels[1][i] = mid - side;
    }
  }
  stereo_iir_.process(channels, num_channels, num_samples);

  if (num_channels >= 1) {
    process_mono_backend(left_iir_, channels[0], num_samples);
  }
  if (num_channels >= 2) {
    process_mono_backend(right_iir_, channels[1], num_samples);
  }

  if (has_mid_side_bands_) {
    if (num_channels != 2) {
      throw SonareException(ErrorCode::InvalidParameter,
                            "EqualizerProcessor Mid/Side placement requires stereo input");
    }
    for (int i = 0; i < num_samples; ++i) {
      const float left = channels[0][i];
      const float right = channels[1][i];
      mid_buffer_[static_cast<size_t>(i)] = (left + right) * 0.5f;
      side_buffer_[static_cast<size_t>(i)] = (left - right) * 0.5f;
    }
    process_mono_backend(mid_iir_, mid_buffer_.data(), num_samples);
    process_mono_backend(side_iir_, side_buffer_.data(), num_samples);
    for (int i = 0; i < num_samples; ++i) {
      const float mid = mid_buffer_[static_cast<size_t>(i)];
      const float side = side_buffer_[static_cast<size_t>(i)];
      channels[0][i] = mid + side;
      channels[1][i] = mid - side;
    }
  }
  apply_auto_gain(channels, num_channels, num_samples, input_db);
  apply_output_gain_and_pan(channels, num_channels, num_samples);
  publish_spectrum_snapshot(pre_snapshot, const_cast<const float* const*>(channels), num_channels,
                            num_samples);
  clear_sidechain();
}

void EqualizerProcessor::reset() {
  stereo_iir_.reset();
  left_iir_.reset();
  right_iir_.reset();
  mid_iir_.reset();
  side_iir_.reset();
  left_channel_fir_.reset();
  right_channel_fir_.reset();
  mid_fir_.reset();
  side_fir_.reset();
  last_detector_db_ = kFloorDb;
  last_band_detector_db_.fill(kFloorDb);
  for (auto& states : detector_states_) {
    for (auto& state : states) {
      // Zero the filter/envelope state and the live lookahead window without
      // releasing the preallocated ring (a default-assign would free it).
      state.filter_a_z1 = state.filter_a_z2 = 0.0;
      state.filter_b_z1 = state.filter_b_z2 = 0.0;
      state.envelope = 0.0;
      std::fill(state.look_ring.begin(), state.look_ring.end(), 0.0f);
      state.look_pos = 0;
    }
  }
  last_applied_gain_db_.fill(0.0f);
  smoothed_gain_db_.fill(0.0f);
  auto_threshold_db_.fill(kFloorDb);
  last_auto_gain_db_ = 0.0f;
  smoothed_auto_gain_db_ = 0.0f;
  clear_sidechain();
}

bool EqualizerProcessor::set_parameter(unsigned int param_id, float value) {
  const size_t band_index = param_id / 3u;
  if (band_index >= kMaxBands) {
    return false;
  }
  EqBand band = bands_[band_index];
  switch (param_id % 3u) {
    case 0:
      band.frequency_hz =
          std::clamp(value, 1.0e-3f, static_cast<float>(sample_rate_ * 0.5) - 1.0e-3f);
      break;
    case 1:
      band.gain_db = value;
      break;
    case 2:
      band.q = std::max(value, 1.0e-6f);
      break;
    default:
      return false;
  }
  if (!prepared_) {
    const EqBand old_band = bands_[band_index];
    bands_[band_index] = band;
    try {
      validate_supported_band(band, phase_mode_);
      validate_backend_capacity(bands_, phase_mode_);
    } catch (...) {
      bands_[band_index] = old_band;
      throw;
    }
    return true;
  }
  const PhaseMode resolved_phase = band.phase == PhaseMode::Inherit ? phase_mode_ : band.phase;
  if (resolved_phase == PhaseMode::LinearPhase) {
    set_band(band_index, band);
    return true;
  }
  const EqBand old_band = bands_[band_index];
  bands_[band_index] = band;
  try {
    validate_supported_band(band, phase_mode_);
    validate_backend_capacity(bands_, phase_mode_);
    update_iir_bands_preserving_state();
  } catch (...) {
    bands_[band_index] = old_band;
    update_iir_bands_preserving_state();
    throw;
  }
  return true;
}

bool EqualizerProcessor::parameter_is_realtime_safe(unsigned int param_id) const noexcept {
  const size_t band_index = param_id / 3u;
  if (band_index >= kMaxBands) {
    return false;
  }
  const EqBand& band = bands_[band_index];
  const PhaseMode resolved_phase = band.phase == PhaseMode::Inherit ? phase_mode_ : band.phase;
  return resolved_phase != PhaseMode::LinearPhase;
}

int EqualizerProcessor::latency_samples() const noexcept {
  if (!has_linear_bands_) {
    return 0;
  }
  int latency = 0;
  if (has_lr_linear_bands_) {
    latency += left_channel_fir_.latency_samples();
  }
  if (has_mid_side_linear_bands_) {
    latency += mid_fir_.latency_samples();
  }
  return latency;
}

void EqualizerProcessor::set_phase_mode(PhaseMode mode) {
  if (mode == PhaseMode::Inherit) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "EqualizerProcessor global phase mode cannot be Inherit");
  }
  for (const auto& band : bands_) {
    validate_supported_band(band, mode);
  }
  validate_backend_capacity(bands_, mode);
  const PhaseMode old_mode = phase_mode_;
  phase_mode_ = mode;
  if (prepared_) {
    try {
      rebuild_iir();
    } catch (...) {
      phase_mode_ = old_mode;
      rebuild_iir();
      throw;
    }
  }
}

void EqualizerProcessor::set_band(size_t index, const EqBand& band) {
  validate_band_index(index);
  validate_supported_band(band, phase_mode_);
  const EqBand old_band = bands_[index];
  bands_[index] = band;
  try {
    validate_backend_capacity(bands_, phase_mode_);
  } catch (...) {
    bands_[index] = old_band;
    throw;
  }
  if (prepared_) {
    try {
      rebuild_iir();
    } catch (...) {
      bands_[index] = old_band;
      rebuild_iir();
      throw;
    }
  }
}

void EqualizerProcessor::clear_band(size_t index) {
  validate_band_index(index);
  const EqBand old_band = bands_[index];
  bands_[index] = {};
  if (prepared_) {
    try {
      rebuild_iir();
    } catch (...) {
      bands_[index] = old_band;
      rebuild_iir();
      throw;
    }
  }
}

void EqualizerProcessor::clear() {
  const auto old_bands = bands_;
  for (auto& band : bands_) {
    band = {};
  }
  if (prepared_) {
    try {
      rebuild_iir();
    } catch (...) {
      bands_ = old_bands;
      rebuild_iir();
      throw;
    }
  }
}

void EqualizerProcessor::set_gain_scale(float scale) {
  if (!std::isfinite(scale) || scale < 0.0f || scale > 2.0f) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "EqualizerProcessor gain scale must be in 0..2");
  }
  gain_scale_ = scale;
  if (prepared_) {
    rebuild_iir();
  }
}

void EqualizerProcessor::set_output_gain_db(float gain_db) {
  if (!std::isfinite(gain_db)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "EqualizerProcessor output gain must be finite");
  }
  output_gain_db_ = gain_db;
}

void EqualizerProcessor::set_output_pan(float pan) {
  if (!std::isfinite(pan) || pan < -1.0f || pan > 1.0f) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "EqualizerProcessor output pan must be in -1..1");
  }
  output_pan_ = pan;
}

const EqBand& EqualizerProcessor::band(size_t index) const {
  validate_band_index(index);
  return bands_[index];
}

void EqualizerProcessor::set_sidechain(const float* const* channels, int num_channels,
                                       int num_samples) {
  if (num_channels < 0 || num_samples < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "sidechain dimensions must be non-negative");
  }
  if (num_channels == 0 || num_samples == 0) {
    clear_sidechain();
    return;
  }
  if (channels == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter, "sidechain channels must not be null");
  }
  // Detector state is preallocated to config_.max_channels in prepare(); reject a
  // wider sidechain on the control thread so the audio-thread detector path can
  // never reallocate detector_states_.
  if (num_channels > config_.max_channels) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "sidechain channel count exceeds the prepared max_channels");
  }
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) {
      throw SonareException(ErrorCode::InvalidParameter, "sidechain channel must not be null");
    }
  }
  sidechain_channels_ = channels;
  sidechain_num_channels_ = num_channels;
  sidechain_num_samples_ = num_samples;
}

void EqualizerProcessor::clear_sidechain() {
  sidechain_channels_ = nullptr;
  sidechain_num_channels_ = 0;
  sidechain_num_samples_ = 0;
}

float EqualizerProcessor::last_band_detector_db(size_t index) const {
  validate_band_index(index);
  return last_band_detector_db_[index];
}

float EqualizerProcessor::last_applied_gain_db(size_t index) const {
  validate_band_index(index);
  return last_applied_gain_db_[index];
}

}  // namespace sonare::mastering::eq
