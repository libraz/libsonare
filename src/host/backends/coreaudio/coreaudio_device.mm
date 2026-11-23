/// @file coreaudio_device.mm
/// @brief AUHAL implementation of host::AudioDevice. See coreaudio_device.h.

#include "host/backends/coreaudio/coreaudio_device.h"
#include "host/backends/apple_time.h"
#include "host/backends/coreaudio/coreaudio_input.h"
#include "host/backends/coreaudio/coreaudio_render_utils.h"

#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>
#include <Block.h>
#include <CoreAudio/CoreAudio.h>
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace sonare::host::backends {

using detail::host_ticks_to_ns;
namespace {

bool ok(OSStatus status) noexcept { return status == noErr; }

UInt32 read_device_uint32(AudioObjectID device, AudioObjectPropertySelector selector,
                          AudioObjectPropertyScope scope) noexcept {
  if (device == kAudioObjectUnknown) return 0;
  AudioObjectPropertyAddress address{selector, scope, kAudioObjectPropertyElementMain};
  UInt32 value = 0;
  UInt32 size = sizeof(value);
  if (!ok(AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &value))) return 0;
  return value;
}

bool read_device_bool(AudioObjectID device, AudioObjectPropertySelector selector,
                      bool* out_value) noexcept {
  if (device == kAudioObjectUnknown || out_value == nullptr) return false;
  AudioObjectPropertyAddress address{selector, kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain};
  UInt32 value = 0;
  UInt32 size = sizeof(value);
  if (!ok(AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &value))) return false;
  *out_value = value != 0;
  return true;
}

double read_device_nominal_sample_rate(AudioObjectID device) noexcept {
  if (device == kAudioObjectUnknown) return 0.0;
  AudioObjectPropertyAddress address{kAudioDevicePropertyNominalSampleRate,
                                     kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain};
  Float64 value = 0.0;
  UInt32 size = sizeof(value);
  if (!ok(AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &value)) ||
      !std::isfinite(value) || value <= 0.0) {
    return 0.0;
  }
  return static_cast<double>(value);
}

int device_channel_count(AudioObjectID device, AudioObjectPropertyScope scope) {
  if (device == kAudioObjectUnknown) return 0;
  AudioObjectPropertyAddress address{kAudioDevicePropertyStreamConfiguration, scope,
                                     kAudioObjectPropertyElementMain};
  UInt32 size = 0;
  if (!ok(AudioObjectGetPropertyDataSize(device, &address, 0, nullptr, &size)) || size == 0) {
    return 0;
  }
  std::vector<uint8_t> storage(size, 0);
  auto* list = reinterpret_cast<AudioBufferList*>(storage.data());
  if (!ok(AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, list))) return 0;
  UInt32 channels = 0;
  for (UInt32 index = 0; index < list->mNumberBuffers; ++index) {
    channels += list->mBuffers[index].mNumberChannels;
  }
  return channels > static_cast<UInt32>(std::numeric_limits<int>::max())
             ? std::numeric_limits<int>::max()
             : static_cast<int>(channels);
}

std::vector<AudioObjectID> enumerate_device_ids() {
  AudioObjectPropertyAddress address{kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain};
  UInt32 size = 0;
  if (!ok(AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, nullptr, &size)) ||
      size == 0) {
    return {};
  }
  std::vector<AudioObjectID> devices(size / sizeof(AudioObjectID), kAudioObjectUnknown);
  if (!ok(AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size,
                                     devices.data()))) {
    return {};
  }
  return devices;
}

size_t utf8_prefix_size(const std::string& value, size_t capacity) noexcept {
  if (capacity == 0) return 0;
  size_t copied = std::min(value.size(), capacity - 1);
  if (copied < value.size()) {
    while (copied > 0 && (static_cast<unsigned char>(value[copied]) & 0xC0u) == 0x80u) {
      --copied;
    }
  }
  return copied;
}

bool copy_device_string(AudioObjectID device, AudioObjectPropertySelector selector,
                        std::string* out) {
  if (device == kAudioObjectUnknown || out == nullptr) return false;
  AudioObjectPropertyAddress address{selector, kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain};
  CFStringRef value = nullptr;
  UInt32 size = sizeof(value);
  if (!ok(AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &value)) ||
      value == nullptr) {
    return false;
  }
  const CFIndex capacity =
      CFStringGetMaximumSizeForEncoding(CFStringGetLength(value), kCFStringEncodingUTF8) + 1;
  std::vector<char> storage(static_cast<size_t>(capacity), '\0');
  const bool copied = CFStringGetCString(value, storage.data(), capacity, kCFStringEncodingUTF8);
  CFRelease(value);
  if (!copied) return false;
  *out = storage.data();
  return true;
}

bool device_uid_matches(AudioObjectID device, const std::string& expected_uid) {
  if (expected_uid.empty()) return true;
  try {
    std::string actual_uid;
    return copy_device_string(device, kAudioDevicePropertyDeviceUID, &actual_uid) &&
           actual_uid == expected_uid;
  } catch (...) {
    return false;
  }
}

bool copy_device_name(AudioObjectID device, char* out, size_t capacity) {
  if (out == nullptr || capacity == 0) return false;
  std::string name;
  if (!copy_device_string(device, kAudioObjectPropertyName, &name)) return false;
  const size_t copied = utf8_prefix_size(name, capacity);
  std::memcpy(out, name.data(), copied);
  out[copied] = '\0';
  return true;
}

AudioObjectID default_device(AudioObjectPropertySelector selector) noexcept {
  AudioObjectPropertyAddress address{selector, kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain};
  AudioObjectID device = kAudioObjectUnknown;
  UInt32 size = sizeof(device);
  if (!ok(AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size,
                                     &device))) {
    return kAudioObjectUnknown;
  }
  return device;
}

bool set_device_rate(AudioObjectID device, double rate) {
  Float64 value = rate;
  AudioObjectPropertyAddress address{kAudioDevicePropertyNominalSampleRate,
                                     kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain};
  return ok(AudioObjectSetPropertyData(device, &address, 0, nullptr, sizeof(value), &value));
}

bool set_nominal_sample_rate_for_device(AudioObjectID device, double rate,
                                        const std::string& expected_uid) {
  if (device == kAudioObjectUnknown || !std::isfinite(rate) || rate <= 0.0 ||
      !device_uid_matches(device, expected_uid)) {
    return false;
  }
  if (read_device_nominal_sample_rate(device) == rate) {
    return device_uid_matches(device, expected_uid);
  }
  dispatch_queue_t queue = dispatch_queue_create("sonare.coreaudio.rate", DISPATCH_QUEUE_SERIAL);
  if (queue == nullptr) return false;
  dispatch_semaphore_t changed = dispatch_semaphore_create(0);
  if (changed == nullptr) {
    dispatch_release(queue);
    return false;
  }
  AudioObjectPropertyAddress address{kAudioDevicePropertyNominalSampleRate,
                                     kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain};
  AudioObjectPropertyListenerBlock local_block = ^(UInt32, const AudioObjectPropertyAddress*) {
    dispatch_semaphore_signal(changed);
  };
  AudioObjectPropertyListenerBlock listener =
      (AudioObjectPropertyListenerBlock)Block_copy(local_block);
  const bool installed = listener != nullptr &&
                         ok(AudioObjectAddPropertyListenerBlock(device, &address, queue, listener));
  bool accepted =
      installed && device_uid_matches(device, expected_uid) && set_device_rate(device, rate);
  if (accepted && read_device_nominal_sample_rate(device) != rate) {
    const dispatch_time_t deadline = dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC);
    while (read_device_nominal_sample_rate(device) != rate &&
           dispatch_semaphore_wait(changed, deadline) == 0) {
    }
  }
  accepted = accepted && device_uid_matches(device, expected_uid) &&
             read_device_nominal_sample_rate(device) == rate;
  if (installed) AudioObjectRemovePropertyListenerBlock(device, &address, queue, listener);
  dispatch_sync(queue, ^{
                });
  if (listener != nullptr) Block_release(listener);
  dispatch_release(changed);
  dispatch_release(queue);
  return accepted && device_uid_matches(device, expected_uid);
}

}  // namespace

struct CoreAudioDevice::Impl {
  struct ListenerRegistration {
    AudioObjectID object = kAudioObjectUnknown;
    AudioObjectPropertyAddress address{};
    AudioObjectPropertyListenerBlock block = nullptr;
    bool installed = false;
  };

  AudioUnit unit = nullptr;
  AudioObjectID device_id = kAudioObjectUnknown;
  uint32_t bound_device_id = 0;
  std::string expected_device_uid;
  bool has_input = false;
  bool has_output = false;
  AudioDeviceCallback* callback = nullptr;
  bool callback_opened = false;
  AudioStreamConfig config{};
  detail::CoreAudioInputBuffers input_buffers;
  std::vector<const float*> input_ptrs;
  std::vector<float> output_scratch;
  std::vector<float*> output_ptrs;

  std::atomic<bool> running{false};
  std::atomic<uint32_t> xruns{0};
  std::atomic<bool> device_lost_latched{false};
  std::atomic<bool> rate_changed_latched{false};
  std::atomic<bool> format_refused_latched{false};
  int reported_output_latency = 0;
  int reported_input_latency = 0;
  int64_t frame_counter = 0;
  int64_t delivered_frames = 0;
  int64_t device_sample_origin = -1;
  int64_t expected_next_sample_time = -1;
  mach_timebase_info_data_t timebase{};
  MidiHostTimeMapper midi_time_mapper;
  bool sample_clock_domains_match = true;
  double watched_nominal_rate = 0.0;
  dispatch_queue_t listener_queue = nullptr;
  ListenerRegistration listeners[3];

  void observe_device_properties() noexcept {
    if (!device_uid_matches(device_id, expected_device_uid)) {
      device_lost_latched.store(true, std::memory_order_relaxed);
      return;
    }
    bool alive = false;
    if (!read_device_bool(device_id, kAudioDevicePropertyDeviceIsAlive, &alive) || !alive) {
      device_lost_latched.store(true, std::memory_order_relaxed);
      return;
    }
    const double rate = read_device_nominal_sample_rate(device_id);
    if (rate > 0.0 && watched_nominal_rate > 0.0 && rate != watched_nominal_rate) {
      rate_changed_latched.store(true, std::memory_order_relaxed);
    }
  }

  void observe_property(AudioObjectPropertySelector selector) noexcept {
    if (!device_uid_matches(device_id, expected_device_uid)) {
      device_lost_latched.store(true, std::memory_order_relaxed);
      return;
    }
    if (selector == kAudioHardwarePropertyDevices) {
      observe_device_properties();
    } else if (selector == kAudioDevicePropertyDeviceIsAlive) {
      bool alive = false;
      if (!read_device_bool(device_id, selector, &alive) || !alive) {
        device_lost_latched.store(true, std::memory_order_relaxed);
      }
    } else if (selector == kAudioDevicePropertyNominalSampleRate) {
      const double rate = read_device_nominal_sample_rate(device_id);
      if (rate > 0.0 && watched_nominal_rate > 0.0 && rate != watched_nominal_rate) {
        rate_changed_latched.store(true, std::memory_order_relaxed);
      }
    }
  }

  bool add_listener(size_t index, AudioObjectID object, AudioObjectPropertySelector selector) {
    if (index >= 3 || listener_queue == nullptr) return false;
    ListenerRegistration& registration = listeners[index];
    registration.object = object;
    registration.address = {selector, kAudioObjectPropertyScopeGlobal,
                            kAudioObjectPropertyElementMain};
    Impl* self = this;
    AudioObjectPropertySelector watched_selector = selector;
    AudioObjectPropertyListenerBlock local_block = ^(UInt32, const AudioObjectPropertyAddress*) {
      self->observe_property(watched_selector);
    };
    registration.block = (AudioObjectPropertyListenerBlock)Block_copy(local_block);
    if (registration.block == nullptr) return false;
    registration.installed = ok(AudioObjectAddPropertyListenerBlock(
        registration.object, &registration.address, listener_queue, registration.block));
    return registration.installed;
  }

  bool install_fault_listeners() {
    listener_queue = dispatch_queue_create("sonare.coreaudio.device", DISPATCH_QUEUE_SERIAL);
    if (listener_queue == nullptr) return false;
    if (!add_listener(0, device_id, kAudioDevicePropertyDeviceIsAlive) ||
        !add_listener(1, device_id, kAudioDevicePropertyNominalSampleRate) ||
        !add_listener(2, kAudioObjectSystemObject, kAudioHardwarePropertyDevices)) {
      remove_fault_listeners();
      return false;
    }
    observe_device_properties();
    return !device_lost_latched.load(std::memory_order_relaxed);
  }

  void flush_listener_queue() noexcept {
    if (listener_queue != nullptr)
      dispatch_sync(listener_queue, ^{
                    });
  }

  void remove_fault_listeners() noexcept {
    if (listener_queue == nullptr) return;
    for (ListenerRegistration& registration : listeners) {
      if (registration.installed) {
        AudioObjectRemovePropertyListenerBlock(registration.object, &registration.address,
                                               listener_queue, registration.block);
        registration.installed = false;
      }
    }
    flush_listener_queue();
    for (ListenerRegistration& registration : listeners) {
      if (registration.block != nullptr) {
        Block_release(registration.block);
        registration.block = nullptr;
      }
    }
    dispatch_release(listener_queue);
    listener_queue = nullptr;
  }

  static OSStatus render_trampoline(void* ref, AudioUnitRenderActionFlags* flags,
                                    const AudioTimeStamp* ts, UInt32 bus, UInt32 frames,
                                    AudioBufferList* data) noexcept {
    return static_cast<Impl*>(ref)->render(flags, ts, bus, frames, data, false);
  }

  static OSStatus input_trampoline(void* ref, AudioUnitRenderActionFlags* flags,
                                   const AudioTimeStamp* ts, UInt32 bus, UInt32 frames,
                                   AudioBufferList*) noexcept {
    return static_cast<Impl*>(ref)->render(flags, ts, bus, frames, nullptr, true);
  }

  OSStatus render(AudioUnitRenderActionFlags* /*flags*/, const AudioTimeStamp* ts, UInt32,
                  UInt32 frames, AudioBufferList* data, bool input_only) noexcept {
    if (callback == nullptr || frames == 0 || (has_output && !input_only && data == nullptr)) {
      return noErr;
    }
    auto frame_plan = detail::plan_coreaudio_callback(frames, config.max_block_size, frame_counter);
    uint32_t callback_xruns = frame_plan.xrun_delta;
    if (frame_plan.xrun_delta != 0)
      xruns.fetch_add(frame_plan.xrun_delta, std::memory_order_relaxed);

    int64_t sample_time = frame_counter;
    if (ts != nullptr && (ts->mFlags & kAudioTimeStampSampleTimeValid) &&
        sample_clock_domains_match) {
      const int64_t current = static_cast<int64_t>(ts->mSampleTime);
      if (device_sample_origin < 0) device_sample_origin = current;
      sample_time = std::max<int64_t>(0, current - device_sample_origin);
      if (expected_next_sample_time >= 0 && current > expected_next_sample_time) {
        xruns.fetch_add(1, std::memory_order_relaxed);
        ++callback_xruns;
      }
      frame_plan = detail::plan_coreaudio_callback(frames, config.max_block_size, sample_time);
      expected_next_sample_time = current + static_cast<int64_t>(frames);
    }

    if (has_input) {
      AudioUnitRenderActionFlags input_flags = 0;
      const OSStatus status = input_buffers.render(unit, &input_flags, ts,
                                                   static_cast<UInt32>(frame_plan.render_frames));
      if (!ok(status)) {
        xruns.fetch_add(1, std::memory_order_relaxed);
        ++callback_xruns;
      }
    }

    AudioBufferView view;
    view.inputs = has_input ? input_ptrs.data() : nullptr;
    view.num_input_channels = has_input ? config.num_input_channels : 0;
    view.outputs = has_output ? output_ptrs.data() : nullptr;
    view.num_output_channels = has_output ? config.num_output_channels : 0;
    view.num_frames = frame_plan.render_frames;
    view.time.sample_time = sample_time;
    if (ts != nullptr && (ts->mFlags & kAudioTimeStampHostTimeValid)) {
      host_ticks_to_ns(ts->mHostTime, timebase, &view.time.host_time_ns);
    }
    if (view.time.host_time_ns != 0) {
      midi_time_mapper.publish_anchor(view.time.host_time_ns, delivered_frames, config.sample_rate);
    }
    view.time.stream_time_seconds =
        config.sample_rate > 0.0 ? static_cast<double>(sample_time) / config.sample_rate : 0.0;
    view.time.input_xruns = callback_xruns;
    callback->render(view);

    if (has_output && !input_only) copy_output(data, frame_plan.render_frames);
    frame_counter = frame_plan.next_sample_time;
    delivered_frames += frame_plan.render_frames;
    return noErr;
  }

  void copy_output(AudioBufferList* data, int render_frames) noexcept {
    if (data == nullptr) return;
    if (data->mNumberBuffers == 1) {
      auto* destination = static_cast<float*>(data->mBuffers[0].mData);
      const int destination_channels = static_cast<int>(data->mBuffers[0].mNumberChannels);
      const size_t capacity = destination_channels > 0
                                  ? data->mBuffers[0].mDataByteSize /
                                        (sizeof(float) * static_cast<size_t>(destination_channels))
                                  : 0;
      detail::copy_coreaudio_interleaved(destination, capacity, destination_channels,
                                         output_ptrs.data(), config.num_output_channels,
                                         render_frames);
      return;
    }
    const int buffers = static_cast<int>(data->mNumberBuffers);
    for (int channel_index = 0; channel_index < buffers; ++channel_index) {
      auto* destination = static_cast<float*>(data->mBuffers[channel_index].mData);
      const float* source = channel_index < config.num_output_channels
                                ? output_ptrs[static_cast<size_t>(channel_index)]
                                : nullptr;
      const size_t capacity = data->mBuffers[channel_index].mDataByteSize / sizeof(float);
      detail::copy_coreaudio_planar_channel(destination, capacity, source, render_frames);
    }
  }
};

namespace {

bool read_audio_unit_rate(AudioUnit unit, AudioUnitScope scope, AudioUnitElement element,
                          double* out_rate) {
  AudioStreamBasicDescription format{};
  UInt32 size = sizeof(format);
  if (!ok(AudioUnitGetProperty(unit, kAudioUnitProperty_StreamFormat, scope, element, &format,
                               &size)) ||
      !std::isfinite(format.mSampleRate) || format.mSampleRate <= 0.0) {
    return false;
  }
  *out_rate = format.mSampleRate;
  return true;
}

}  // namespace

CoreAudioDevice::CoreAudioDevice() : CoreAudioDevice(0) {}

CoreAudioDevice::CoreAudioDevice(uint32_t device_id) : CoreAudioDevice(device_id, {}) {}

CoreAudioDevice::CoreAudioDevice(uint32_t device_id, std::string uid)
    : impl_(std::make_unique<Impl>()) {
  impl_->bound_device_id = device_id;
  impl_->expected_device_uid = std::move(uid);
}

CoreAudioDevice::~CoreAudioDevice() { close(); }

size_t CoreAudioDevice::output_device_count() {
  const std::vector<AudioObjectID> all = enumerate_device_ids();
  return static_cast<size_t>(std::count_if(all.begin(), all.end(), [](AudioObjectID device) {
    return device != kAudioObjectUnknown &&
           device_channel_count(device, kAudioObjectPropertyScopeOutput) > 0;
  }));
}

uint32_t CoreAudioDevice::output_device_id(size_t index) {
  const std::vector<AudioObjectID> all = enumerate_device_ids();
  size_t output_index = 0;
  for (const AudioObjectID device : all) {
    if (device == kAudioObjectUnknown ||
        device_channel_count(device, kAudioObjectPropertyScopeOutput) == 0) {
      continue;
    }
    if (output_index++ == index) return static_cast<uint32_t>(device);
  }
  return 0;
}

bool CoreAudioDevice::output_device_name(size_t index, char* out, size_t capacity) {
  const uint32_t id = output_device_id(index);
  if (id == 0) return false;
  return copy_device_name(static_cast<AudioObjectID>(id), out, capacity);
}

std::vector<CoreAudioDevice::OutputDevice> CoreAudioDevice::output_devices() {
  const std::vector<AudioObjectID> all = enumerate_device_ids();
  std::vector<OutputDevice> snapshot;
  for (const AudioObjectID device : all) {
    if (device == kAudioObjectUnknown ||
        device_channel_count(device, kAudioObjectPropertyScopeOutput) == 0) {
      continue;
    }
    OutputDevice item;
    item.id = static_cast<uint32_t>(device);
    if (!copy_device_name(device, item.name, sizeof(item.name))) item.name[0] = '\0';
    snapshot.push_back(item);
  }
  return snapshot;
}

std::vector<CoreAudioDevice::DeviceInfo> CoreAudioDevice::devices() {
  const AudioObjectID default_input = default_device(kAudioHardwarePropertyDefaultInputDevice);
  const AudioObjectID default_output = default_device(kAudioHardwarePropertyDefaultOutputDevice);
  const std::vector<AudioObjectID> all = enumerate_device_ids();
  std::vector<DeviceInfo> snapshot;
  snapshot.reserve(all.size());
  for (const AudioObjectID device : all) {
    if (device == kAudioObjectUnknown) continue;
    DeviceInfo info;
    info.id = static_cast<uint32_t>(device);
    copy_device_string(device, kAudioDevicePropertyDeviceUID, &info.uid);
    copy_device_string(device, kAudioObjectPropertyName, &info.name);
    info.input_channels = device_channel_count(device, kAudioObjectPropertyScopeInput);
    info.output_channels = device_channel_count(device, kAudioObjectPropertyScopeOutput);
    if (info.input_channels == 0 && info.output_channels == 0) continue;
    info.is_default_input = device == default_input;
    info.is_default_output = device == default_output;
    info.nominal_sample_rate = read_device_nominal_sample_rate(device);
    info.clock_domain = read_device_uint32(device, kAudioDevicePropertyClockDomain,
                                           kAudioObjectPropertyScopeGlobal);
    snapshot.push_back(std::move(info));
  }
  return snapshot;
}

bool CoreAudioDevice::set_nominal_sample_rate(uint32_t device_id, double rate) {
  return device_id != 0 &&
         set_nominal_sample_rate_for_device(static_cast<AudioObjectID>(device_id), rate, {});
}

bool CoreAudioDevice::set_nominal_sample_rate(uint32_t device_id, double rate,
                                              const std::string& expected_uid) {
  return device_id != 0 && !expected_uid.empty() &&
         set_nominal_sample_rate_for_device(static_cast<AudioObjectID>(device_id), rate,
                                            expected_uid);
}

bool CoreAudioDevice::open(const AudioStreamConfig& config, AudioDeviceCallback* callback) {
  return open_device(impl_->bound_device_id, config, callback);
}

bool CoreAudioDevice::open_device(uint32_t device_id, const AudioStreamConfig& config,
                                  AudioDeviceCallback* callback) {
  if (impl_->unit != nullptr || impl_->callback_opened) return false;
  impl_->format_refused_latched.store(false, std::memory_order_relaxed);
  impl_->device_lost_latched.store(false, std::memory_order_relaxed);
  impl_->rate_changed_latched.store(false, std::memory_order_relaxed);
  if (callback == nullptr || !std::isfinite(config.sample_rate) || config.sample_rate <= 0.0 ||
      config.max_block_size <= 0 || config.num_input_channels < 0 ||
      config.num_output_channels < 0 ||
      (config.num_input_channels == 0 && config.num_output_channels == 0)) {
    return false;
  }

  impl_->has_input = config.num_input_channels > 0;
  impl_->has_output = config.num_output_channels > 0;
  impl_->config = config;
  impl_->callback = callback;
  impl_->xruns.store(0, std::memory_order_relaxed);
  mach_timebase_info(&impl_->timebase);

  AudioObjectID selected_device = static_cast<AudioObjectID>(device_id);
  if (selected_device == 0) {
    const AudioObjectPropertySelector selector = impl_->has_input && !impl_->has_output
                                                     ? kAudioHardwarePropertyDefaultInputDevice
                                                     : kAudioHardwarePropertyDefaultOutputDevice;
    selected_device = default_device(selector);
  }
  impl_->device_id = selected_device;
  bool alive = false;
  if (selected_device == kAudioObjectUnknown ||
      !device_uid_matches(selected_device, impl_->expected_device_uid) ||
      !read_device_bool(selected_device, kAudioDevicePropertyDeviceIsAlive, &alive) || !alive) {
    impl_->device_lost_latched.store(true, std::memory_order_relaxed);
    close();
    return false;
  }

  const int available_inputs =
      device_channel_count(selected_device, kAudioObjectPropertyScopeInput);
  const int available_outputs =
      device_channel_count(selected_device, kAudioObjectPropertyScopeOutput);
  const int input_hardware_channels =
      impl_->has_input ? std::min(config.num_input_channels, available_inputs) : 0;
  const bool mono_duplication = config.num_input_channels == 2 && available_inputs == 1;
  if ((impl_->has_input && (input_hardware_channels == 0 ||
                            (available_inputs < config.num_input_channels && !mono_duplication))) ||
      (impl_->has_output && config.num_output_channels > available_outputs)) {
    impl_->format_refused_latched.store(true, std::memory_order_relaxed);
    close();
    return false;
  }

  AudioComponentDescription description{};
  description.componentType = kAudioUnitType_Output;
  description.componentSubType = kAudioUnitSubType_HALOutput;
  description.componentManufacturer = kAudioUnitManufacturer_Apple;
  AudioComponent component = AudioComponentFindNext(nullptr, &description);
  if (component == nullptr || !ok(AudioComponentInstanceNew(component, &impl_->unit)) ||
      impl_->unit == nullptr) {
    close();
    return false;
  }
  const UInt32 input_enabled = impl_->has_input ? 1u : 0u;
  const UInt32 output_enabled = impl_->has_output ? 1u : 0u;
  if (!device_uid_matches(selected_device, impl_->expected_device_uid)) {
    impl_->device_lost_latched.store(true, std::memory_order_relaxed);
    close();
    return false;
  }
  if (!ok(AudioUnitSetProperty(impl_->unit, kAudioOutputUnitProperty_EnableIO,
                               kAudioUnitScope_Input, 1, &input_enabled, sizeof(input_enabled))) ||
      !ok(AudioUnitSetProperty(impl_->unit, kAudioOutputUnitProperty_EnableIO,
                               kAudioUnitScope_Output, 0, &output_enabled,
                               sizeof(output_enabled))) ||
      !ok(AudioUnitSetProperty(impl_->unit, kAudioOutputUnitProperty_CurrentDevice,
                               kAudioUnitScope_Global, 0, &selected_device,
                               sizeof(selected_device)))) {
    close();
    return false;
  }

  if (impl_->has_output) {
    AudioStreamBasicDescription format{};
    format.mSampleRate = config.sample_rate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    format.mChannelsPerFrame = static_cast<UInt32>(config.num_output_channels);
    format.mBitsPerChannel = 32;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = format.mChannelsPerFrame * sizeof(float);
    format.mBytesPerPacket = format.mBytesPerFrame;
    if (!ok(AudioUnitSetProperty(impl_->unit, kAudioUnitProperty_StreamFormat,
                                 kAudioUnitScope_Input, 0, &format, sizeof(format)))) {
      impl_->format_refused_latched.store(true, std::memory_order_relaxed);
      close();
      return false;
    }
  }
  if (impl_->has_input) {
    AudioStreamBasicDescription format{};
    format.mSampleRate = config.sample_rate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags =
        kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
    format.mChannelsPerFrame = static_cast<UInt32>(input_hardware_channels);
    format.mBitsPerChannel = 32;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = sizeof(float);
    format.mBytesPerPacket = sizeof(float);
    if (!ok(AudioUnitSetProperty(impl_->unit, kAudioUnitProperty_StreamFormat,
                                 kAudioUnitScope_Output, 1, &format, sizeof(format)))) {
      impl_->format_refused_latched.store(true, std::memory_order_relaxed);
      close();
      return false;
    }
  }

  UInt32 maximum_frames = static_cast<UInt32>(config.max_block_size);
  const AudioObjectPropertyScope buffer_scope =
      impl_->has_output ? kAudioDevicePropertyScopeOutput : kAudioDevicePropertyScopeInput;
  AudioObjectPropertyAddress buffer_size{kAudioDevicePropertyBufferFrameSize, buffer_scope,
                                         kAudioObjectPropertyElementMain};
  AudioObjectSetPropertyData(selected_device, &buffer_size, 0, nullptr, sizeof(maximum_frames),
                             &maximum_frames);
  const UInt32 settled_frames =
      read_device_uint32(selected_device, kAudioDevicePropertyBufferFrameSize, buffer_scope);
  if (settled_frames > maximum_frames) maximum_frames = settled_frames;
  if (!ok(AudioUnitSetProperty(impl_->unit, kAudioUnitProperty_MaximumFramesPerSlice,
                               kAudioUnitScope_Global, 0, &maximum_frames,
                               sizeof(maximum_frames)))) {
    impl_->format_refused_latched.store(true, std::memory_order_relaxed);
    close();
    return false;
  }

  AURenderCallbackStruct callback_info{};
  callback_info.inputProcRefCon = impl_.get();
  AudioUnitPropertyID callback_property = kAudioUnitProperty_SetRenderCallback;
  AudioUnitScope callback_scope = kAudioUnitScope_Input;
  AudioUnitElement callback_element = 0;
  if (impl_->has_output) {
    callback_info.inputProc = &Impl::render_trampoline;
  } else {
    callback_info.inputProc = &Impl::input_trampoline;
    callback_property = kAudioOutputUnitProperty_SetInputCallback;
    callback_scope = kAudioUnitScope_Global;
  }
  if (!ok(AudioUnitSetProperty(impl_->unit, callback_property, callback_scope, callback_element,
                               &callback_info, sizeof(callback_info))) ||
      !ok(AudioUnitInitialize(impl_->unit))) {
    close();
    return false;
  }
  if (!device_uid_matches(selected_device, impl_->expected_device_uid)) {
    impl_->device_lost_latched.store(true, std::memory_order_relaxed);
    close();
    return false;
  }

  UInt32 negotiated_frames = 0;
  UInt32 negotiated_size = sizeof(negotiated_frames);
  if (!ok(AudioUnitGetProperty(impl_->unit, kAudioUnitProperty_MaximumFramesPerSlice,
                               kAudioUnitScope_Global, 0, &negotiated_frames, &negotiated_size)) ||
      negotiated_frames == 0 ||
      negotiated_frames > static_cast<UInt32>(std::numeric_limits<int>::max())) {
    impl_->format_refused_latched.store(true, std::memory_order_relaxed);
    close();
    return false;
  }
  impl_->config.max_block_size = static_cast<int>(negotiated_frames);
  const size_t block = static_cast<size_t>(impl_->config.max_block_size);
  if (impl_->has_output) {
    const size_t output_channels = static_cast<size_t>(config.num_output_channels);
    impl_->output_scratch.assign(output_channels * block, 0.0f);
    impl_->output_ptrs.resize(output_channels);
    for (size_t channel = 0; channel < output_channels; ++channel) {
      impl_->output_ptrs[channel] = impl_->output_scratch.data() + channel * block;
    }
  }
  if (impl_->has_input) {
    if (!impl_->input_buffers.allocate(static_cast<UInt32>(input_hardware_channels),
                                       negotiated_frames)) {
      close();
      return false;
    }
    impl_->input_ptrs.resize(static_cast<size_t>(config.num_input_channels), nullptr);
    for (size_t channel = 0; channel < impl_->input_ptrs.size(); ++channel) {
      if (channel < static_cast<size_t>(input_hardware_channels)) {
        impl_->input_ptrs[channel] = impl_->input_buffers.channel(static_cast<UInt32>(channel));
      } else if (input_hardware_channels == 1 && channel == 1) {
        impl_->input_ptrs[channel] = impl_->input_buffers.channel(0);
      }
    }
  }

  double output_rate = 0.0;
  double input_rate = 0.0;
  if ((impl_->has_output &&
       !read_audio_unit_rate(impl_->unit, kAudioUnitScope_Input, 0, &output_rate)) ||
      (impl_->has_input &&
       !read_audio_unit_rate(impl_->unit, kAudioUnitScope_Output, 1, &input_rate))) {
    impl_->format_refused_latched.store(true, std::memory_order_relaxed);
    close();
    return false;
  }
  const double nominal_rate = read_device_nominal_sample_rate(selected_device);
  if (impl_->has_input) {
    const double reference_rate = impl_->has_output ? output_rate : config.sample_rate;
    if (input_rate != reference_rate || input_rate != nominal_rate) {
      impl_->format_refused_latched.store(true, std::memory_order_relaxed);
      close();
      return false;
    }
  }
  impl_->config.sample_rate = impl_->has_output ? output_rate : input_rate;
  if (!std::isfinite(impl_->config.sample_rate) || impl_->config.sample_rate <= 0.0) {
    impl_->format_refused_latched.store(true, std::memory_order_relaxed);
    close();
    return false;
  }

  impl_->watched_nominal_rate = nominal_rate;
  impl_->sample_clock_domains_match = detail::coreaudio_sample_clock_domains_match(
      impl_->watched_nominal_rate, impl_->config.sample_rate);
  if (impl_->has_output) {
    const UInt32 device_latency = read_device_uint32(selected_device, kAudioDevicePropertyLatency,
                                                     kAudioDevicePropertyScopeOutput);
    const UInt32 safety_offset = read_device_uint32(
        selected_device, kAudioDevicePropertySafetyOffset, kAudioDevicePropertyScopeOutput);
    const UInt32 buffer_frames = read_device_uint32(
        selected_device, kAudioDevicePropertyBufferFrameSize, kAudioDevicePropertyScopeOutput);
    const int64_t latency = static_cast<int64_t>(device_latency) + safety_offset + buffer_frames;
    impl_->reported_output_latency = static_cast<int>(detail::scale_coreaudio_device_domain_samples(
        latency, impl_->watched_nominal_rate, impl_->config.sample_rate));
  } else {
    impl_->reported_output_latency = 0;
  }
  if (impl_->has_input) {
    const UInt32 device_latency = read_device_uint32(selected_device, kAudioDevicePropertyLatency,
                                                     kAudioDevicePropertyScopeInput);
    const UInt32 safety_offset = read_device_uint32(
        selected_device, kAudioDevicePropertySafetyOffset, kAudioDevicePropertyScopeInput);
    const UInt32 buffer_frames = read_device_uint32(
        selected_device, kAudioDevicePropertyBufferFrameSize, kAudioDevicePropertyScopeInput);
    const int64_t latency = static_cast<int64_t>(device_latency) + safety_offset + buffer_frames;
    impl_->reported_input_latency = static_cast<int>(detail::scale_coreaudio_device_domain_samples(
        latency, impl_->watched_nominal_rate, impl_->config.sample_rate));
  } else {
    impl_->reported_input_latency = 0;
  }
  impl_->config.input_latency_samples = impl_->reported_input_latency;
  impl_->config.output_latency_samples = impl_->reported_output_latency;

  if (!impl_->install_fault_listeners()) {
    close();
    return false;
  }
  if (read_device_nominal_sample_rate(selected_device) != impl_->watched_nominal_rate) {
    impl_->rate_changed_latched.store(true, std::memory_order_relaxed);
  }
  impl_->flush_listener_queue();
  if (impl_->has_input && impl_->rate_changed_latched.load(std::memory_order_relaxed)) {
    impl_->format_refused_latched.store(true, std::memory_order_relaxed);
    close();
    return false;
  }
  if (!callback->open(impl_->config)) {
    close();
    return false;
  }
  impl_->callback_opened = true;
  return true;
}

bool CoreAudioDevice::start() {
  if (impl_->unit == nullptr || impl_->running.load(std::memory_order_relaxed)) return false;
  impl_->frame_counter = 0;
  impl_->delivered_frames = 0;
  impl_->device_sample_origin = -1;
  impl_->expected_next_sample_time = -1;
  impl_->midi_time_mapper.reset();
  if (!ok(AudioOutputUnitStart(impl_->unit))) return false;
  impl_->running.store(true, std::memory_order_relaxed);
  return true;
}

void CoreAudioDevice::stop() noexcept {
  if (impl_->unit != nullptr && impl_->running.load(std::memory_order_relaxed)) {
    AudioOutputUnitStop(impl_->unit);
    impl_->running.store(false, std::memory_order_relaxed);
  }
  impl_->flush_listener_queue();
}

void CoreAudioDevice::close() noexcept {
  stop();
  impl_->remove_fault_listeners();
  if (impl_->callback != nullptr && impl_->callback_opened) impl_->callback->close();
  impl_->callback_opened = false;
  if (impl_->unit != nullptr) {
    AudioUnitUninitialize(impl_->unit);
    AudioComponentInstanceDispose(impl_->unit);
    impl_->unit = nullptr;
  }
  impl_->callback = nullptr;
  impl_->device_id = kAudioObjectUnknown;
  impl_->has_input = false;
  impl_->has_output = false;
  impl_->input_ptrs.clear();
  impl_->output_ptrs.clear();
  impl_->input_buffers = detail::CoreAudioInputBuffers{};
  impl_->output_scratch.clear();
  impl_->watched_nominal_rate = 0.0;
}

bool CoreAudioDevice::is_running() const noexcept {
  return impl_->running.load(std::memory_order_relaxed);
}

int CoreAudioDevice::input_latency_samples() const noexcept {
  return impl_->reported_input_latency;
}

int CoreAudioDevice::output_latency_samples() const noexcept {
  return impl_->reported_output_latency;
}

uint32_t CoreAudioDevice::xrun_count() const noexcept {
  return impl_->xruns.load(std::memory_order_relaxed);
}

bool CoreAudioDevice::device_lost() const noexcept {
  return impl_->device_lost_latched.load(std::memory_order_relaxed);
}

bool CoreAudioDevice::rate_changed() const noexcept {
  return impl_->rate_changed_latched.load(std::memory_order_relaxed);
}

bool CoreAudioDevice::format_refused() const noexcept {
  return impl_->format_refused_latched.load(std::memory_order_relaxed);
}

MidiHostTimeMapper& CoreAudioDevice::midi_time_mapper() noexcept { return impl_->midi_time_mapper; }

void CoreAudioDevice::note_xrun_for_test() noexcept {
  impl_->xruns.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace sonare::host::backends
