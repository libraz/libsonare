#include "core/audio.h"

#include <algorithm>
#include <cmath>

#include "core/audio_io.h"
#include "util/exception.h"

namespace sonare {

Audio::Audio() : buffer_(nullptr), offset_(0), length_(0), sample_rate_(0) {}

Audio::Audio(std::shared_ptr<const std::vector<float>> buffer, size_t offset, size_t length,
             int sample_rate)
    : buffer_(std::move(buffer)), offset_(offset), length_(length), sample_rate_(sample_rate) {}

Audio Audio::from_buffer(const float* samples, size_t size, int sample_rate) {
  SONARE_CHECK(sample_rate > 0, ErrorCode::InvalidParameter);
  auto buffer = std::make_shared<std::vector<float>>(samples, samples + size);
  return Audio(buffer, 0, size, sample_rate);
}

Audio Audio::from_vector(std::vector<float> samples, int sample_rate) {
  SONARE_CHECK(sample_rate > 0, ErrorCode::InvalidParameter);
  size_t size = samples.size();
  auto buffer = std::make_shared<std::vector<float>>(std::move(samples));
  return Audio(buffer, 0, size, sample_rate);
}

Audio Audio::from_file(const std::string& path) {
  auto [samples, sample_rate] = load_audio(path);
  return from_vector(std::move(samples), sample_rate);
}

Audio Audio::from_memory(const uint8_t* data, size_t size) {
  auto [samples, sample_rate] = load_buffer(data, size);
  return from_vector(std::move(samples), sample_rate);
}

const float* Audio::data() const {
  if (!buffer_) {
    return nullptr;
  }
  return buffer_->data() + offset_;
}

size_t Audio::size() const { return length_; }

float Audio::duration() const {
  if (sample_rate_ == 0) {
    return 0.0f;
  }
  return static_cast<float>(length_) / static_cast<float>(sample_rate_);
}

Audio Audio::slice(float start_time, float end_time) const {
  if (!buffer_ || sample_rate_ == 0) {
    return Audio();
  }

  size_t start_sample = static_cast<size_t>(std::max(0.0f, start_time) * sample_rate_);
  size_t end_sample;
  if (end_time < 0.0f) {
    end_sample = length_;
  } else {
    end_sample = static_cast<size_t>(end_time * sample_rate_);
  }

  return slice_samples(start_sample, end_sample);
}

Audio Audio::slice_samples(size_t start_sample, size_t end_sample) const {
  if (!buffer_) {
    return Audio();
  }

  // Clamp to valid range
  start_sample = std::min(start_sample, length_);
  if (end_sample == static_cast<size_t>(-1) || end_sample > length_) {
    end_sample = length_;
  }
  if (start_sample >= end_sample) {
    return Audio();
  }

  size_t new_offset = offset_ + start_sample;
  size_t new_length = end_sample - start_sample;

  return Audio(buffer_, new_offset, new_length, sample_rate_);
}

Audio Audio::to_mono() const {
  // Already mono, just return a copy
  if (!buffer_) {
    return Audio();
  }

  // Create a copy with its own buffer
  std::vector<float> samples(data(), data() + size());
  return from_vector(std::move(samples), sample_rate_);
}

float Audio::operator[](size_t index) const {
  SONARE_CHECK(index < length_, ErrorCode::InvalidParameter);
  return (*buffer_)[offset_ + index];
}

}  // namespace sonare
