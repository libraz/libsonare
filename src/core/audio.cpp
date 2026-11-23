#include "core/audio.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

#include "core/audio_io.h"
#include "core/audio_io_detail.h"
#include "util/exception.h"
#include "util/parameter_checks.h"
#include "util/refusal_predicates.h"

namespace sonare {

Audio::Audio() : buffer_(nullptr), offset_(0), length_(0), sample_rate_(0) {}

Audio::Audio(std::shared_ptr<const std::vector<float>> buffer, size_t offset, size_t length,
             int sample_rate)
    : buffer_(std::move(buffer)), offset_(offset), length_(length), sample_rate_(sample_rate) {}

namespace {

using RefusalProblem = std::optional<SonareParameterRefusal>;

RefusalProblem buffer_refusal(const char* name, const std::string& predicate) {
  return SonareParameterRefusal::predicate(ErrorCode::InvalidParameter, name, predicate);
}

RefusalProblem extent_problem(const char* name, std::size_t length, int sample_rate) {
  if (length == 0) return buffer_refusal(name, "must be a non-empty buffer");
  if (sample_rate < kMinAudioSampleRate || sample_rate > kMaxAudioSampleRate) {
    const double lo = kMinAudioSampleRate;
    const double hi = kMaxAudioSampleRate;
    return SonareParameterRefusal::predicate(ErrorCode::InvalidParameter, "sample_rate",
                                             util::refusal::integer_range_predicate(lo, hi),
                                             RefusalDomain{lo, hi, true, true});
  }
  if (length > kMaxAudioBufferSize) {
    return buffer_refusal(name,
                          "must hold at most " + std::to_string(kMaxAudioBufferSize) + " samples");
  }
  return std::nullopt;
}

// first_index is the caller's index of samples[0], so a scanned copy reports the caller's index.
RefusalProblem finite_range_problem(const char* name, const float* samples, std::size_t begin,
                                    std::size_t end, std::size_t first_index) {
  for (std::size_t i = begin; i < end; ++i) {
    if (!std::isfinite(samples[i])) {
      return buffer_refusal(
          name, "contains a non-finite sample at index " + std::to_string(first_index + i));
    }
  }
  return std::nullopt;
}

void refuse_if(const RefusalProblem& problem) {
  if (problem) throw *problem;
}

// The O(1) preconditions, which describe the buffer as a whole and so are
// identical for a whole-buffer and a windowed validation.
void check_offline_audio_bounds(const char* name, const float* samples, std::size_t length,
                                int sample_rate) {
  if (samples == nullptr) refuse_if(buffer_refusal(name, "must be a non-empty buffer"));
  validate_offline_audio_extent(name, length, sample_rate);
}

}  // namespace

void validate_offline_audio_extent(const char* name, std::size_t length, int sample_rate) {
  refuse_if(extent_problem(name, length, sample_rate));
}

std::string offline_audio_input_problem(const char* name, const float* samples, std::size_t length,
                                        int sample_rate) {
  RefusalProblem problem = samples == nullptr ? buffer_refusal(name, "must be a non-empty buffer")
                                              : extent_problem(name, length, sample_rate);
  if (!problem) problem = finite_range_problem(name, samples, 0, length, 0);
  return problem ? std::string(problem->what()) : std::string();
}

void validate_offline_audio_input(const char* name, const float* samples, std::size_t length,
                                  int sample_rate) {
  check_offline_audio_bounds(name, samples, length, sample_rate);
  validate_offline_audio_samples(name, samples, length, 0);
}

void validate_offline_audio_window(const char* name, const float* samples, std::size_t length,
                                   int sample_rate, std::size_t scan_offset,
                                   std::size_t scan_count) {
  check_offline_audio_bounds(name, samples, length, sample_rate);
  // Clamp rather than add: scan_offset + scan_count overflows on a caller-supplied offset.
  const std::size_t begin = std::min(scan_offset, length);
  const std::size_t end = begin + std::min(scan_count, length - begin);
  refuse_if(finite_range_problem(name, samples, begin, end, 0));
}

void validate_offline_audio_samples(const char* name, const float* samples, std::size_t count,
                                    std::size_t first_index) {
  refuse_if(finite_range_problem(name, samples, 0, count, first_index));
}

Audio Audio::from_buffer(const float* samples, size_t size, int sample_rate) {
  util::check::integer_at_least(sample_rate, 1, "sample_rate");
  // Reject null + size>0 to avoid UB in the iterator-pair vector constructor.
  // A null pointer with size==0 is treated as an empty buffer (valid).
  SONARE_CHECK_MSG(samples != nullptr || size == 0, ErrorCode::InvalidParameter,
                   "samples is null but size > 0");
  if (size == 0) {
    auto buffer = std::make_shared<std::vector<float>>();
    return Audio(buffer, 0, 0, sample_rate);
  }
  auto buffer = std::make_shared<std::vector<float>>(samples, samples + size);
  return Audio(buffer, 0, size, sample_rate);
}

Audio Audio::from_vector(std::vector<float> samples, int sample_rate) {
  util::check::integer_at_least(sample_rate, 1, "sample_rate");
  size_t size = samples.size();
  auto buffer = std::make_shared<std::vector<float>>(std::move(samples));
  return Audio(buffer, 0, size, sample_rate);
}

namespace {

/// Applies the shared offline-analysis policy to a freshly decoded buffer.
///
/// A decoder hands back samples that never passed a public buffer entry point,
/// so without this the same sample data was accepted from a file and rejected
/// from a buffer: a float WAV carrying NaN, or one declaring a rate outside the
/// supported range, produced a usable handle whose analyses returned NaN. The
/// policy is `validate_offline_audio_input`'s, reported in decoder terms -- an
/// unusable declared rate is a malformed container, samples that are not finite
/// are a decode that produced garbage.
Audio validated_decoded_audio(std::vector<float> samples, int sample_rate) {
  SONARE_CHECK_MSG(!samples.empty(), ErrorCode::DecodeFailed, "decoded audio has no samples");
  SONARE_CHECK_MSG(sample_rate >= kMinAudioSampleRate && sample_rate <= kMaxAudioSampleRate,
                   ErrorCode::InvalidFormat, "decoded sample rate is outside the supported range");
  SONARE_CHECK_MSG(samples.size() <= kMaxAudioBufferSize, ErrorCode::DecodeFailed,
                   "decoded audio is too large");
  for (const float sample : samples) {
    SONARE_CHECK_MSG(std::isfinite(sample), ErrorCode::DecodeFailed,
                     "decoded audio contains a non-finite sample");
  }
  return Audio::from_vector(std::move(samples), sample_rate);
}

/// One channel of an interleaved buffer, @p frames long.
std::vector<float> deinterleave_channel(const std::vector<float>& interleaved, std::size_t stride,
                                        std::size_t channel, std::size_t frames) {
  std::vector<float> plane(frames);
  for (std::size_t frame = 0; frame < frames; ++frame) {
    plane[frame] = interleaved[frame * stride + channel];
  }
  return plane;
}

}  // namespace

Audio Audio::from_file(const std::string& path) {
  auto decoded = load_audio_input_once(path, RetainChannels::None);
  return audio_from_decoded_mono(std::move(decoded.mono), decoded.sample_rate);
}

Audio audio_from_decoded_mono(std::vector<float> samples, int sample_rate) {
  return validated_decoded_audio(std::move(samples), sample_rate);
}

Audio Audio::from_file_channel(const std::string& path, int channel_index) {
  auto [interleaved, sample_rate, channels] = load_audio_interleaved(path);
  SONARE_CHECK_MSG(channels > 0, ErrorCode::DecodeFailed, "decoded audio reports no channels");
  util::check::integer_in_range(channel_index, 0, channels - 1, "channel_index");
  const std::size_t frames = interleaved.size() / static_cast<std::size_t>(channels);
  std::vector<float> plane = deinterleave_channel(interleaved, static_cast<std::size_t>(channels),
                                                  static_cast<std::size_t>(channel_index), frames);
  // The same policy from_file applies: one plane of a stereo file is decoded
  // audio like any other, and accepting a NaN here because it arrived beside a
  // finite channel is the gap this whole entry point exists to close.
  return validated_decoded_audio(std::move(plane), sample_rate);
}

Audio Audio::from_memory(const uint8_t* data, size_t size) {
  // load_buffer / detect_format dereference `data` when size is large enough;
  // reject the obviously-invalid (null, size>0) and (anything, size==0) cases up front.
  SONARE_CHECK_MSG(data != nullptr, ErrorCode::InvalidParameter, "data is null");
  SONARE_CHECK_MSG(size > 0, ErrorCode::InvalidParameter, "size is zero");
  auto [samples, sample_rate] = load_buffer(data, size);
  return validated_decoded_audio(std::move(samples), sample_rate);
}

std::vector<Audio> Audio::from_memory_channels(const uint8_t* data, size_t size) {
  SONARE_CHECK_MSG(data != nullptr, ErrorCode::InvalidParameter, "data is null");
  SONARE_CHECK_MSG(size > 0, ErrorCode::InvalidParameter, "size is zero");
  auto [interleaved, sample_rate, channels] = load_buffer_interleaved(data, size);
  SONARE_CHECK_MSG(channels > 0, ErrorCode::DecodeFailed, "decoded audio reports no channels");
  const std::size_t stride = static_cast<std::size_t>(channels);
  const std::size_t frames = interleaved.size() / stride;
  std::vector<Audio> planes;
  planes.reserve(stride);
  for (std::size_t channel = 0; channel < stride; ++channel) {
    planes.push_back(validated_decoded_audio(
        deinterleave_channel(interleaved, stride, channel, frames), sample_rate));
  }
  return planes;
}

const float* Audio::data() const {
  if (!buffer_) {
    return nullptr;
  }
  // std::vector::data() may be null for an empty vector; pointer arithmetic on
  // that value is not defined even with a zero offset.
  if (buffer_->empty()) {
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

  // Convert a time in seconds to a sample index in double, reject non-finite,
  // and clamp to [0, length_] before casting to size_t: a non-finite or
  // out-of-range float would otherwise make the float->size_t cast undefined.
  // slice_samples re-clamps, but the cast itself must be well-defined. Mirrors
  // the double-promote-then-clamp guard used in convert.cpp.
  const auto time_to_sample = [this](float time) -> size_t {
    const double s = static_cast<double>(time) * static_cast<double>(sample_rate_);
    if (!std::isfinite(s) || s <= 0.0) {
      return 0;
    }
    return static_cast<size_t>(std::min(s, static_cast<double>(length_)));
  };

  size_t start_sample = time_to_sample(std::max(0.0f, start_time));
  size_t end_sample;
  if (end_time < 0.0f) {
    end_sample = length_;
  } else {
    end_sample = time_to_sample(end_time);
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
    return Audio::from_buffer(nullptr, 0, sample_rate_);
  }

  size_t new_offset = offset_ + start_sample;
  size_t new_length = end_sample - start_sample;

  return Audio(buffer_, new_offset, new_length, sample_rate_);
}

float Audio::operator[](size_t index) const {
  if (index >= length_) {
    util::check::refuse("index", "must be less than " + std::to_string(length_));
  }
  return (*buffer_)[offset_ + index];
}

}  // namespace sonare
