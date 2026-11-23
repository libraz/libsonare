#pragma once

#include <AudioUnit/AudioUnit.h>

#include <cstdint>
#include <vector>

namespace sonare::host::backends::detail {

class CoreAudioInputBuffers final {
 public:
  bool allocate(UInt32 channels, UInt32 max_frames);
  OSStatus render(AudioUnit unit, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* time,
                  UInt32 frames) noexcept;
  const float* channel(UInt32 index) const noexcept;

 private:
  std::vector<float> samples_;
  std::vector<uint8_t> list_storage_;
  size_t plane_stride_ = 0;
  UInt32 channels_ = 0;
  UInt32 max_frames_ = 0;
};

}  // namespace sonare::host::backends::detail
