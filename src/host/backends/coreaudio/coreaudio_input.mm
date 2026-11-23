/// @file coreaudio_input.mm
/// @brief Preallocated AUHAL input rendering buffers.

#include "host/backends/coreaudio/coreaudio_input.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace sonare::host::backends::detail {
namespace {

constexpr size_t kPlaneStrideAlignmentFloats = 4;
constexpr size_t kPlaneAlignmentBytes = kPlaneStrideAlignmentFloats * sizeof(float);

}  // namespace

bool CoreAudioInputBuffers::allocate(UInt32 channels, UInt32 max_frames) {
  if (channels == 0 || max_frames == 0 ||
      max_frames > std::numeric_limits<UInt32>::max() / sizeof(float)) {
    return false;
  }
  const size_t frames = static_cast<size_t>(max_frames);
  const size_t channel_count = static_cast<size_t>(channels);
  if (frames > std::numeric_limits<size_t>::max() - (kPlaneStrideAlignmentFloats - 1)) {
    return false;
  }
  const size_t plane_stride =
      (frames + kPlaneStrideAlignmentFloats - 1) & ~(kPlaneStrideAlignmentFloats - 1);
  if (plane_stride > std::numeric_limits<size_t>::max() / channel_count) return false;
  const size_t list_size =
      offsetof(AudioBufferList, mBuffers) + sizeof(AudioBuffer) * channel_count;
  samples_.assign(plane_stride * channel_count, 0.0f);
  if (reinterpret_cast<uintptr_t>(samples_.data()) % kPlaneAlignmentBytes != 0) {
    samples_.clear();
    return false;
  }
  list_storage_.assign(list_size, 0);
  channels_ = channels;
  max_frames_ = max_frames;
  plane_stride_ = plane_stride;

  auto* list = reinterpret_cast<AudioBufferList*>(list_storage_.data());
  list->mNumberBuffers = channels_;
  for (UInt32 channel_index = 0; channel_index < channels_; ++channel_index) {
    AudioBuffer& buffer = list->mBuffers[channel_index];
    buffer.mNumberChannels = 1;
    buffer.mDataByteSize = max_frames_ * sizeof(float);
    buffer.mData = samples_.data() + static_cast<size_t>(channel_index) * plane_stride_;
  }
  return true;
}

OSStatus CoreAudioInputBuffers::render(AudioUnit unit, AudioUnitRenderActionFlags* flags,
                                       const AudioTimeStamp* time, UInt32 frames) noexcept {
  if (unit == nullptr || frames > max_frames_) return kAudioUnitErr_InvalidPropertyValue;
  auto* list = reinterpret_cast<AudioBufferList*>(list_storage_.data());
  const UInt32 byte_size = frames * sizeof(float);
  for (UInt32 channel_index = 0; channel_index < channels_; ++channel_index) {
    list->mBuffers[channel_index].mDataByteSize = byte_size;
  }
  const OSStatus status = AudioUnitRender(unit, flags, time, 1, frames, list);
  if (status != noErr ||
      (flags != nullptr && (*flags & kAudioUnitRenderAction_OutputIsSilence) != 0)) {
    for (UInt32 channel_index = 0; channel_index < channels_; ++channel_index) {
      std::memset(samples_.data() + static_cast<size_t>(channel_index) * plane_stride_, 0,
                  static_cast<size_t>(frames) * sizeof(float));
    }
  }
  return status;
}

const float* CoreAudioInputBuffers::channel(UInt32 index) const noexcept {
  if (index >= channels_) return nullptr;
  return samples_.data() + static_cast<size_t>(index) * plane_stride_;
}

}  // namespace sonare::host::backends::detail
