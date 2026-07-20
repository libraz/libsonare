/// @file coreaudio_device.mm
/// @brief AUHAL implementation of host::AudioDevice. See coreaudio_device.h.

#include "host/backends/coreaudio/coreaudio_device.h"
#include "host/backends/coreaudio/coreaudio_render_utils.h"

#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudio.h>
#include <mach/mach_time.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
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

bool host_ticks_to_ns(uint64_t ticks, const mach_timebase_info_data_t& timebase,
                      uint64_t* out) noexcept {
  if (out == nullptr || timebase.denom == 0) return false;
  const unsigned __int128 scaled = static_cast<unsigned __int128>(ticks) *
                                   static_cast<unsigned __int128>(timebase.numer) /
                                   static_cast<unsigned __int128>(timebase.denom);
  if (scaled > std::numeric_limits<uint64_t>::max()) return false;
  *out = static_cast<uint64_t>(scaled);
  return true;
}

}  // namespace

struct CoreAudioDevice::Impl {
  AudioUnit unit = nullptr;
  AudioObjectID device_id = kAudioObjectUnknown;
  AudioDeviceCallback* callback = nullptr;
  // True only after callback->open() has succeeded. close() runs on every
  // mid-open failure path (the AU instance already exists), so it must not call
  // callback->close() unless the paired open() actually ran.
  bool callback_opened = false;
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
  // HAL sample times need not start at zero. Latch the per-start origin so all
  // engine and MIDI-facing frame coordinates remain transport-relative.
  int64_t device_sample_origin = -1;
  mach_timebase_info_data_t timebase{};
  MidiHostTimeMapper midi_time_mapper;
  // Device sample time expected at the start of the next render callback (the
  // previous call's mSampleTime + its frame count). -1 until the first callback.
  // A callback whose mSampleTime jumps past this expected value means the HAL
  // skipped samples between cycles, i.e. an output overload / xrun.
  int64_t expected_next_sample_time = -1;

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
    // Guard against a device handing us a larger block than negotiated; never
    // overrun the pre-sized scratch. The unrendered device-buffer tail is
    // explicitly zeroed below, while clock accounting still advances by the
    // full hardware frame count.
    auto frame_plan = detail::plan_coreaudio_callback(frames, config.max_block_size, frame_counter);
    uint32_t callback_xruns = frame_plan.xrun_delta;
    if (frame_plan.xrun_delta != 0) {
      xruns.fetch_add(frame_plan.xrun_delta, std::memory_order_relaxed);
    }

    AudioBufferView view;
    view.outputs = output_ptrs.data();
    view.num_output_channels = num_out;
    view.inputs = nullptr;
    view.num_input_channels = 0;
    view.num_frames = frame_plan.render_frames;
    view.time.sample_time = frame_counter;
    if (ts != nullptr && (ts->mFlags & kAudioTimeStampSampleTimeValid)) {
      const int64_t current = static_cast<int64_t>(ts->mSampleTime);
      if (device_sample_origin < 0) device_sample_origin = current;
      const int64_t relative = std::max<int64_t>(0, current - device_sample_origin);
      view.time.sample_time = relative;
      // A forward discontinuity in the device sample clock between callbacks
      // means the HAL could not service the previous cycle in time and skipped
      // samples: count it as an xrun. The first callback (expected == -1) and an
      // exact/behind timestamp (steady state) never count.
      if (expected_next_sample_time >= 0 && current > expected_next_sample_time) {
        xruns.fetch_add(1, std::memory_order_relaxed);
        ++callback_xruns;
      }
      frame_plan = detail::plan_coreaudio_callback(frames, config.max_block_size, relative);
      expected_next_sample_time = current + static_cast<int64_t>(frames);
    }
    if (ts != nullptr && (ts->mFlags & kAudioTimeStampHostTimeValid)) {
      host_ticks_to_ns(ts->mHostTime, timebase, &view.time.host_time_ns);
    }
    if (view.time.host_time_ns != 0) {
      midi_time_mapper.publish_anchor(view.time.host_time_ns, view.time.sample_time,
                                      config.sample_rate);
    }
    view.time.stream_time_seconds =
        config.sample_rate > 0.0 ? static_cast<double>(view.time.sample_time) / config.sample_rate
                                 : 0.0;
    view.time.input_xruns = callback_xruns;

    callback->render(view);

    // Interleave the planar engine output back into CoreAudio's buffers.
    // CoreAudio HAL output is canonical interleaved float32 by default. The
    // SDK-free helpers zero the entire device buffer, including an oversize
    // callback tail that the bounded engine request did not render.
    if (data->mNumberBuffers == 1) {
      auto* dst = static_cast<float*>(data->mBuffers[0].mData);
      const int dst_channels = static_cast<int>(data->mBuffers[0].mNumberChannels);
      const size_t capacity = dst_channels > 0
                                  ? data->mBuffers[0].mDataByteSize /
                                        (sizeof(float) * static_cast<size_t>(dst_channels))
                                  : 0;
      detail::copy_coreaudio_interleaved(dst, capacity, dst_channels, output_ptrs.data(), num_out,
                                         frame_plan.render_frames);
    } else {
      // Non-interleaved device layout: one buffer per channel.
      const int buffers = static_cast<int>(data->mNumberBuffers);
      for (int c = 0; c < buffers; ++c) {
        auto* dst = static_cast<float*>(data->mBuffers[c].mData);
        const float* src = c < num_out ? output_ptrs[c] : nullptr;
        const size_t capacity = data->mBuffers[c].mDataByteSize / sizeof(float);
        detail::copy_coreaudio_planar_channel(dst, capacity, src, frame_plan.render_frames);
      }
    }
    frame_counter = frame_plan.next_sample_time;
    return noErr;
  }
};

CoreAudioDevice::CoreAudioDevice() : impl_(std::make_unique<Impl>()) {}

CoreAudioDevice::~CoreAudioDevice() { close(); }

bool CoreAudioDevice::open(const AudioStreamConfig& config, AudioDeviceCallback* callback) {
  if (callback == nullptr || impl_->unit != nullptr || !std::isfinite(config.sample_rate) ||
      config.sample_rate <= 0.0 || config.max_block_size <= 0 || config.num_output_channels <= 0 ||
      config.num_input_channels > 0) {
    return false;
  }
  impl_->config = config;
  impl_->callback = callback;
  mach_timebase_info(&impl_->timebase);

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
  impl_->device_id = kAudioObjectUnknown;
  if (!ok(AudioObjectGetPropertyData(kAudioObjectSystemObject, &default_out, 0, nullptr,
                                     &device_size, &impl_->device_id))) {
    impl_->device_id = kAudioObjectUnknown;
  }
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
  if (!ok(AudioUnitSetProperty(impl_->unit, kAudioUnitProperty_MaximumFramesPerSlice,
                               kAudioUnitScope_Global, 0, &max_frames, sizeof(max_frames)))) {
    close();
    return false;
  }

  // Wire the render callback.
  AURenderCallbackStruct cb{};
  cb.inputProc = &Impl::render_trampoline;
  cb.inputProcRefCon = impl_.get();
  if (!ok(AudioUnitSetProperty(impl_->unit, kAudioUnitProperty_SetRenderCallback,
                               kAudioUnitScope_Input, 0, &cb, sizeof(cb)))) {
    close();
    return false;
  }

  if (!ok(AudioUnitInitialize(impl_->unit))) {
    close();
    return false;
  }

  // The AU may round MaximumFramesPerSlice during initialization. Query the
  // actual value and make that negotiated maximum authoritative for both the
  // callback contract and the RT scratch allocation.
  UInt32 negotiated_frames = 0;
  UInt32 negotiated_size = sizeof(negotiated_frames);
  if (!ok(AudioUnitGetProperty(impl_->unit, kAudioUnitProperty_MaximumFramesPerSlice,
                               kAudioUnitScope_Global, 0, &negotiated_frames, &negotiated_size)) ||
      negotiated_frames == 0 ||
      negotiated_frames > static_cast<UInt32>(std::numeric_limits<int>::max())) {
    close();
    return false;
  }
  impl_->config.max_block_size = static_cast<int>(negotiated_frames);
  const size_t out_ch = static_cast<size_t>(config.num_output_channels);
  const size_t block = static_cast<size_t>(impl_->config.max_block_size);
  impl_->output_scratch.assign(out_ch * block, 0.0f);
  impl_->output_ptrs.resize(out_ch);
  for (size_t c = 0; c < out_ch; ++c) {
    impl_->output_ptrs[c] = impl_->output_scratch.data() + c * block;
  }

  // Query the driver's actual latency now that the unit is initialized. Total
  // output latency = device latency + safety offset + the buffer frame size.
  // (The output AudioUnit's own kAudioUnitProperty_Latency is not added here; on
  // the default HAL output unit it is typically negligible.)
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
  impl_->callback_opened = true;
  return true;
}

bool CoreAudioDevice::start() {
  if (impl_->unit == nullptr || impl_->running.load()) return false;
  impl_->frame_counter = 0;
  impl_->device_sample_origin = -1;
  impl_->expected_next_sample_time = -1;  // no baseline until the first callback
  impl_->midi_time_mapper.reset();
  impl_->xruns.store(0, std::memory_order_relaxed);
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
  if (impl_->callback != nullptr && impl_->callback_opened) impl_->callback->close();
  impl_->callback_opened = false;
  AudioUnitUninitialize(impl_->unit);
  AudioComponentInstanceDispose(impl_->unit);
  impl_->unit = nullptr;
  impl_->callback = nullptr;
  impl_->device_id = kAudioObjectUnknown;
}

bool CoreAudioDevice::is_running() const noexcept { return impl_->running.load(); }

int CoreAudioDevice::input_latency_samples() const noexcept {
  return impl_->reported_input_latency;
}

int CoreAudioDevice::output_latency_samples() const noexcept {
  return impl_->reported_output_latency;
}

uint32_t CoreAudioDevice::xrun_count() const noexcept { return impl_->xruns.load(); }

MidiHostTimeMapper& CoreAudioDevice::midi_time_mapper() noexcept { return impl_->midi_time_mapper; }

}  // namespace sonare::host::backends
