#include <algorithm>
#include <cmath>

#include "filters/chroma.h"
#include "streaming/stream_analyzer.h"
#include "streaming/stream_analyzer_utils.h"

namespace sonare {

using namespace streaming_detail;

size_t StreamAnalyzer::available_frames() const { return output_buffer_.size(); }

std::vector<StreamFrame> StreamAnalyzer::read_frames(size_t max_frames) {
  size_t count = std::min(max_frames, output_buffer_.size());
  std::vector<StreamFrame> result;
  result.reserve(count);

  for (size_t i = 0; i < count; ++i) {
    result.push_back(std::move(output_buffer_.front()));
    output_buffer_.pop_front();
  }

  return result;
}

void StreamAnalyzer::read_frames_soa(size_t max_frames, FrameBuffer& buffer) {
  buffer.clear();

  size_t count = std::min(max_frames, output_buffer_.size());
  buffer.n_frames = count;

  if (count == 0) {
    return;
  }

  buffer.reserve(count, config_.n_mels);

  for (size_t i = 0; i < count; ++i) {
    StreamFrame& frame = output_buffer_.front();

    buffer.timestamps.push_back(frame.timestamp);
    buffer.onset_strength.push_back(frame.onset_strength);
    buffer.rms_energy.push_back(frame.rms_energy);
    buffer.spectral_centroid.push_back(frame.spectral_centroid);
    buffer.spectral_flatness.push_back(frame.spectral_flatness);
    buffer.chord_root.push_back(frame.chord_root);
    buffer.chord_quality.push_back(frame.chord_quality);
    buffer.chord_confidence.push_back(frame.chord_confidence);

    buffer.mel.insert(buffer.mel.end(), frame.mel.begin(), frame.mel.end());
    buffer.chroma.insert(buffer.chroma.end(), frame.chroma.begin(), frame.chroma.end());

    output_buffer_.pop_front();
  }
}

void StreamAnalyzer::read_frames_quantized_u8(size_t max_frames, QuantizedFrameBufferU8& buffer,
                                              const QuantizeConfig& qconfig) {
  buffer.clear();

  size_t count = std::min(max_frames, output_buffer_.size());
  buffer.n_frames = count;

  if (count == 0) {
    return;
  }

  buffer.reserve(count, config_.n_mels);

  for (size_t i = 0; i < count; ++i) {
    StreamFrame& frame = output_buffer_.front();

    buffer.timestamps.push_back(frame.timestamp);

    for (float mel_power : frame.mel) {
      float db = single_power_to_db(mel_power);
      buffer.mel.push_back(quantize_to_u8(db, qconfig.mel_db_min, qconfig.mel_db_max));
    }

    for (float c : frame.chroma) {
      buffer.chroma.push_back(quantize_to_u8(c, 0.0f, 1.0f));
    }

    buffer.onset_strength.push_back(quantize_to_u8(frame.onset_strength, 0.0f, qconfig.onset_max));
    buffer.rms_energy.push_back(quantize_to_u8(frame.rms_energy, 0.0f, qconfig.rms_max));
    buffer.spectral_centroid.push_back(
        quantize_to_u8(frame.spectral_centroid, 0.0f, qconfig.centroid_max));
    buffer.spectral_flatness.push_back(quantize_to_u8(frame.spectral_flatness, 0.0f, 1.0f));

    output_buffer_.pop_front();
  }
}

void StreamAnalyzer::read_frames_quantized_i16(size_t max_frames, QuantizedFrameBufferI16& buffer,
                                               const QuantizeConfig& qconfig) {
  buffer.clear();

  size_t count = std::min(max_frames, output_buffer_.size());
  buffer.n_frames = count;

  if (count == 0) {
    return;
  }

  buffer.reserve(count, config_.n_mels);

  for (size_t i = 0; i < count; ++i) {
    StreamFrame& frame = output_buffer_.front();

    buffer.timestamps.push_back(frame.timestamp);

    for (float mel_power : frame.mel) {
      float db = single_power_to_db(mel_power);
      buffer.mel.push_back(quantize_to_i16(db, qconfig.mel_db_min, qconfig.mel_db_max));
    }

    for (float c : frame.chroma) {
      buffer.chroma.push_back(quantize_to_i16(c, 0.0f, 1.0f));
    }

    buffer.onset_strength.push_back(quantize_to_i16(frame.onset_strength, 0.0f, qconfig.onset_max));
    buffer.rms_energy.push_back(quantize_to_i16(frame.rms_energy, 0.0f, qconfig.rms_max));
    buffer.spectral_centroid.push_back(
        quantize_to_i16(frame.spectral_centroid, 0.0f, qconfig.centroid_max));
    buffer.spectral_flatness.push_back(quantize_to_i16(frame.spectral_flatness, 0.0f, 1.0f));

    output_buffer_.pop_front();
  }
}

void StreamAnalyzer::reset(size_t base_sample_offset) {
  cumulative_samples_ = base_sample_offset;
  cumulative_samples_exact_ = static_cast<double>(base_sample_offset);
  frame_count_ = 0;
  emitted_frame_count_ = 0;

  overlap_buffer_.clear();
  overlap_read_pos_ = 0;
  output_buffer_.clear();

  if (config_.compute_mel) {
    std::fill(prev_mel_log_.begin(), prev_mel_log_.end(), 0.0f);
  }
  has_prev_frame_ = false;

  onset_accumulator_.clear();
  chroma_sum_.fill(0.0f);
  chroma_frame_count_ = 0;
  last_key_update_time_ = 0.0f;
  last_bpm_update_time_ = 0.0f;
  current_estimate_ = ProgressiveEstimate();

  prev_chord_root_ = -1;
  prev_chord_quality_ = -1;
  chord_stable_time_ = 0.0f;
  chroma_history_.clear();
  full_chroma_history_.clear();

  bar_tracking_active_ = false;
  bar_duration_ = 0.0f;
  current_bar_index_ = -1;
  bar_start_time_ = 0.0f;
  bar_chord_votes_.fill(0);
  bar_vote_count_ = 0;

  pattern_locked_ = false;
}

void StreamAnalyzer::set_expected_duration(float duration_seconds) {
  expected_duration_ = duration_seconds;
}

void StreamAnalyzer::set_normalization_gain(float gain) {
  normalization_gain_ = std::clamp(gain, 0.01f, 100.0f);
}

void StreamAnalyzer::set_tuning_ref_hz(float ref_hz) {
  ref_hz = std::clamp(ref_hz, 220.0f, 880.0f);
  config_.tuning_ref_hz = ref_hz;

  if (config_.compute_chroma) {
    ChromaFilterConfig chroma_config;
    chroma_config.n_chroma = 12;
    chroma_config.tuning = constants::kSemitonesPerOctave * std::log2(ref_hz / constants::kA4Hz);
    chroma_config.fmin = 65.0f;
    chroma_filterbank_ =
        create_chroma_filterbank(internal_sample_rate_, config_.n_fft, chroma_config);
  }
}

AnalyzerStats StreamAnalyzer::stats() const {
  AnalyzerStats stats;
  stats.total_frames = frame_count_;
  stats.total_samples = cumulative_samples_;
  stats.duration_seconds = static_cast<float>(cumulative_samples_) / config_.sample_rate;
  stats.estimate = current_estimate_;

  return stats;
}

float StreamAnalyzer::current_time() const {
  return static_cast<float>(cumulative_samples_) / static_cast<float>(config_.sample_rate);
}

}  // namespace sonare
