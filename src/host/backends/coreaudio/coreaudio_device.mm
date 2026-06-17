/// @file coreaudio_device.mm
/// @brief AUHAL implementation of host::AudioDevice. See coreaudio_device.h.

#include "host/backends/coreaudio/coreaudio_device.h"

#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudio.h>

#include <atomic>
#include <vector>

namespace sonare::host::backends {
namespace {

/// Round a CoreAudio OSStatus check: returns true on noErr.
bool ok(OSStatus status) noexcept { return status == noErr; }

/// Read a UInt32 device property (latency / safety offset) on the given scope,
/// returning 0 when the property is absent.
UInt32 read_device_uint32(AudioObjectID device, AudioObjectPropertySelector selector,
                          AudioObjectPropertyScope scope) noexcept {
  if (device == kAudioObjectUnknown) return 0;
  AudioObjectPropertyAddress address{selector, scope, kAudioObjectPropertyElementMain};
  UInt32 value = 0;
  UInt32 size = sizeof(value);
  if (!ok(AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &value))) return 0;
  return value;
}

}  // namespace

struct CoreAudioDevice::Impl {
  AudioUnit unit = nullptr;
  AudioObjectID device_id = kAudioObjectUnknown;
  AudioDeviceCallback* callback = nullptr;
  AudioStreamConfig config{};

  // Pre-sized planar scratch the render callback deinterleaves CoreAudio's
  // buffer list into, so render() never allocates. Pointers handed to the
  // engine point into this scratch.
  std::vector<float> output_scratch;     // num_output_channels * max_block_size
  std::vector<float*> output_ptrs;       // num_output_channels
  std::vector<const float*> input_ptrs;  // num_input_channels (unused for output-only)

  std::atomic<bool> running{false};
  std::atomic<uint32_t> xruns{0};
  int reported_output_latency = 0;
  int reported_input_latency = 0;
  int64_t frame_counter = 0;

  static OSStatus render_trampoline(void* ref, AudioUnitRenderActionFlags* flags,
                                    const AudioTimeStamp* ts, UInt32 bus, UInt32 frames,
                                    AudioBufferList* data) noexcept {
    return static_cast<Impl*>(ref)->render(flags, ts, bus, frames, data);
  }

  OSStatus render(AudioUnitRenderActionFlags* /*flags*/, const AudioTimeStamp* ts, UInt32 /*bus*/,
                  UInt32 frames, AudioBufferList* data) noexcept {
    const int num_out = config.num_output_channels;
    if (callback == nullptr || data == nullptr || frames == 0) {
      return noErr;
    }
    const int n = static_cast<int>(frames);
    // Guard against a device handing us a larger block than negotiated; never
    // overrun the pre-sized scratch.
    const int clamped = n > config.max_block_size ? config.max_block_size : n;

    AudioBufferView view;
    view.outputs = output_ptrs.data();
    view.num_output_channels = num_out;
    view.inputs = nullptr;
    view.num_input_channels = 0;
    view.num_frames = clamped;
    view.time.sample_time = frame_counter;
    if (ts != nullptr && (ts->mFlags & kAudioTimeStampSampleTimeValid)) {
      view.time.sample_time = static_cast<int64_t>(ts->mSampleTime);
    }
    view.time.stream_time_seconds =
        config.sample_rate > 0.0 ? static_cast<double>(view.time.sample_time) / config.sample_rate
                                 : 0.0;
    view.time.input_xruns = 0;

    callback->render(view);

    // Interleave the planar engine output back into CoreAudio's buffers.
    // CoreAudio HAL output is canonical interleaved float32 by default.
    if (data->mNumberBuffers == 1) {
      auto* dst = static_cast<float*>(data->mBuffers[0].mData);
      const int dst_channels = static_cast<int>(data->mBuffers[0].mNumberChannels);
      for (int f = 0; f < clamped; ++f) {
        for (int c = 0; c < dst_channels; ++c) {
          const float sample = c < num_out ? output_ptrs[c][f] : 0.0f;
          dst[f * dst_channels + c] = sample;
        }
      }
    } else {
      // Non-interleaved device layout: one buffer per channel.
      const int buffers = static_cast<int>(data->mNumberBuffers);
      for (int c = 0; c < buffers; ++c) {
        auto* dst = static_cast<float*>(data->mBuffers[c].mData);
        const float* src = c < num_out ? output_ptrs[c] : nullptr;
        for (int f = 0; f < clamped; ++f) dst[f] = src != nullptr ? src[f] : 0.0f;
      }
    }
    frame_counter += clamped;
    return noErr;
  }
};

CoreAudioDevice::CoreAudioDevice() : impl_(std::make_unique<Impl>()) {}

CoreAudioDevice::~CoreAudioDevice() { close(); }

bool CoreAudioDevice::open(const AudioStreamConfig& config, AudioDeviceCallback* callback) {
  if (callback == nullptr || impl_->unit != nullptr) return false;
  impl_->config = config;
  impl_->callback = callback;

  // Instantiate the default-output AUHAL unit.
  AudioComponentDescription desc{};
  desc.componentType = kAudioUnitType_Output;
  desc.componentSubType = kAudioUnitSubType_HALOutput;
  desc.componentManufacturer = kAudioUnitManufacturer_Apple;
  AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
  if (comp == nullptr) return false;
  if (!ok(AudioComponentInstanceNew(comp, &impl_->unit)) || impl_->unit == nullptr) return false;

  // Bind the default output device and record its id for latency queries.
  AudioObjectPropertyAddress default_out{kAudioHardwarePropertyDefaultOutputDevice,
                                         kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain};
  UInt32 device_size = sizeof(impl_->device_id);
  AudioObjectGetPropertyData(kAudioObjectSystemObject, &default_out, 0, nullptr, &device_size,
                             &impl_->device_id);
  if (impl_->device_id != kAudioObjectUnknown) {
    AudioUnitSetProperty(impl_->unit, kAudioOutputUnitProperty_CurrentDevice,
                         kAudioUnitScope_Global, 0, &impl_->device_id, sizeof(impl_->device_id));
  }

  // Negotiate the canonical interleaved float32 stream format at the requested
  // sample rate / channel count.
  AudioStreamBasicDescription fmt{};
  fmt.mSampleRate = config.sample_rate;
  fmt.mFormatID = kAudioFormatLinearPCM;
  fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
  fmt.mChannelsPerFrame = static_cast<UInt32>(config.num_output_channels);
  fmt.mBitsPerChannel = 32;
  fmt.mFramesPerPacket = 1;
  fmt.mBytesPerFrame = fmt.mChannelsPerFrame * sizeof(float);
  fmt.mBytesPerPacket = fmt.mBytesPerFrame;
  if (!ok(AudioUnitSetProperty(impl_->unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input,
                               0, &fmt, sizeof(fmt)))) {
    close();
    return false;
  }

  // Request the negotiated maximum block size.
  auto max_frames = static_cast<UInt32>(config.max_block_size);
  AudioUnitSetProperty(impl_->unit, kAudioUnitProperty_MaximumFramesPerSlice,
                       kAudioUnitScope_Global, 0, &max_frames, sizeof(max_frames));

  // Wire the render callback.
  AURenderCallbackStruct cb{};
  cb.inputProc = &Impl::render_trampoline;
  cb.inputProcRefCon = impl_.get();
  if (!ok(AudioUnitSetProperty(impl_->unit, kAudioUnitProperty_SetRenderCallback,
                               kAudioUnitScope_Input, 0, &cb, sizeof(cb)))) {
    close();
    return false;
  }

  // Size the planar scratch the render callback writes into.
  const size_t out_ch = static_cast<size_t>(config.num_output_channels);
  const size_t block = static_cast<size_t>(config.max_block_size);
  impl_->output_scratch.assign(out_ch * block, 0.0f);
  impl_->output_ptrs.resize(out_ch);
  for (size_t c = 0; c < out_ch; ++c) {
    impl_->output_ptrs[c] = impl_->output_scratch.data() + c * block;
  }

  if (!ok(AudioUnitInitialize(impl_->unit))) {
    close();
    return false;
  }

  // Query the driver's actual latency now that the unit is initialized. Total
  // output latency = device latency + safety offset + the unit's own latency.
  const UInt32 device_latency = read_device_uint32(impl_->device_id, kAudioDevicePropertyLatency,
                                                   kAudioDevicePropertyScopeOutput);
  const UInt32 safety_offset = read_device_uint32(
      impl_->device_id, kAudioDevicePropertySafetyOffset, kAudioDevicePropertyScopeOutput);
  const UInt32 buffer_frames = read_device_uint32(
      impl_->device_id, kAudioDevicePropertyBufferFrameSize, kAudioDevicePropertyScopeOutput);
  impl_->reported_output_latency = static_cast<int>(device_latency + safety_offset + buffer_frames);
  impl_->reported_input_latency = 0;

  // Bridge the negotiated format back to the callback's open().
  if (!callback->open(impl_->config)) {
    close();
    return false;
  }
  return true;
}

bool CoreAudioDevice::start() {
  if (impl_->unit == nullptr || impl_->running.load()) return false;
  impl_->frame_counter = 0;
  if (!ok(AudioOutputUnitStart(impl_->unit))) return false;
  impl_->running.store(true);
  return true;
}

void CoreAudioDevice::stop() noexcept {
  if (impl_->unit == nullptr || !impl_->running.load()) return;
  AudioOutputUnitStop(impl_->unit);
  impl_->running.store(false);
}

void CoreAudioDevice::close() noexcept {
  if (impl_->unit == nullptr) return;
  stop();
  if (impl_->callback != nullptr) impl_->callback->close();
  AudioUnitUninitialize(impl_->unit);
  AudioComponentInstanceDispose(impl_->unit);
  impl_->unit = nullptr;
  impl_->callback = nullptr;
}

bool CoreAudioDevice::is_running() const noexcept { return impl_->running.load(); }

int CoreAudioDevice::input_latency_samples() const noexcept {
  return impl_->reported_input_latency;
}

int CoreAudioDevice::output_latency_samples() const noexcept {
  return impl_->reported_output_latency;
}

uint32_t CoreAudioDevice::xrun_count() const noexcept { return impl_->xruns.load(); }

}  // namespace sonare::host::backends
