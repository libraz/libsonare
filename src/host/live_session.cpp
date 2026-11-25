/// @file live_session.cpp
/// @brief Implementation of host::LiveSession. See live_session.h.

#include "host/live_session.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <utility>
#include <vector>

#include "engine/realtime_engine.h"
#include "util/exception.h"

namespace sonare::host {
namespace {

using DeviceInfo = backends::CoreAudioDevice::DeviceInfo;
using Selection = LiveSession::DeviceSelection;
using Selector = LiveSession::DeviceSelector;

bool valid_selector(const Selector& selector, bool midi) noexcept {
  switch (selector.selection) {
    case Selection::kNone:
      return selector.uid.empty();
    case Selection::kDefault:
      return !midi && selector.uid.empty();
    case Selection::kByUid:
      return !selector.uid.empty();
  }
  return false;
}

bool has_input(const DeviceInfo& info) noexcept { return info.input_channels > 0; }
bool has_output(const DeviceInfo& info) noexcept { return info.output_channels > 0; }

bool info_matches(const DeviceInfo& info, const Selector& selector, bool input) noexcept {
  if (selector.selection == Selection::kByUid) return info.uid == selector.uid;
  if (selector.selection == Selection::kDefault) {
    return input ? info.is_default_input : info.is_default_output;
  }
  return false;
}

const DeviceInfo* find_device(const std::vector<DeviceInfo>& devices, const Selector& selector,
                              bool input) noexcept {
  for (const DeviceInfo& info : devices) {
    if ((input ? has_input(info) : has_output(info)) && info_matches(info, selector, input)) {
      return &info;
    }
  }
  return nullptr;
}

bool same_rate(double lhs, double rhs) noexcept {
  return std::isfinite(lhs) && std::isfinite(rhs) && lhs == rhs;
}

int channels_for(int available) noexcept { return std::min(available, 2); }

bool same_device_identity(const DeviceInfo& lhs, const DeviceInfo& rhs) noexcept {
  return (lhs.id != 0 && lhs.id == rhs.id) || (!lhs.uid.empty() && lhs.uid == rhs.uid);
}

bool live_input_ring_dimensions_valid(int input_frames, int output_frames) noexcept {
  if (input_frames <= 0 || output_frames <= 0) return false;
  const size_t input = static_cast<size_t>(input_frames);
  const size_t output = static_cast<size_t>(output_frames);
  const size_t max_size = std::vector<uint64_t>{}.max_size();
  constexpr size_t kCapacityMultiplier = 4;
  if (input > max_size / kCapacityMultiplier || output > max_size / kCapacityMultiplier - input) {
    return false;
  }

  const size_t minimum_capacity = kCapacityMultiplier * (input + output);
  size_t capacity = 1;
  while (capacity < minimum_capacity) {
    if (capacity > max_size / 2) return false;
    capacity *= 2;
  }
  const size_t target = input + output + std::max(input, output);
  return target + input <= capacity;
}

class CloseOnExit final {
 public:
  explicit CloseOnExit(LiveSession* session) noexcept : session_(session) {}
  ~CloseOnExit() {
    if (active_) session_->close();
  }
  void dismiss() noexcept { active_ = false; }

  CloseOnExit(const CloseOnExit&) = delete;
  CloseOnExit& operator=(const CloseOnExit&) = delete;

 private:
  LiveSession* session_;
  bool active_ = true;
};

}  // namespace

LiveSession::LiveSession() = default;

LiveSession::~LiveSession() { close(); }

size_t LiveSession::audio_output_count() noexcept {
  return backends::CoreAudioDevice::output_device_count();
}

size_t LiveSession::midi_input_count() noexcept { return backends::CoreMidiInput::source_count(); }

bool LiveSession::audio_output_info(size_t index, LiveDeviceInfo* out) noexcept {
  if (out == nullptr) return false;
  return backends::CoreAudioDevice::output_device_name(index, out->name, sizeof(out->name));
}

void LiveSession::audio_outputs(std::vector<LiveDeviceInfo>* out) {
  if (out == nullptr) return;
  static_assert(
      sizeof(LiveDeviceInfo::name) == sizeof(backends::CoreAudioDevice::OutputDevice::name),
      "device name buffers must match");
  const auto devices = backends::CoreAudioDevice::output_devices();
  out->assign(devices.size(), LiveDeviceInfo{});
  for (size_t i = 0; i < devices.size(); ++i) {
    std::memcpy((*out)[i].name, devices[i].name, sizeof((*out)[i].name));
  }
}

bool LiveSession::midi_input_info(size_t index, LiveDeviceInfo* out) noexcept {
  if (out == nullptr) return false;
  return backends::CoreMidiInput::source_name(index, out->name, sizeof(out->name));
}

LiveOpenResult LiveSession::open(engine::RealtimeEngine* engine, const Config& config) {
  prepare_failure_ = nullptr;
  if (engine == nullptr || open_ || !std::isfinite(config.sample_rate) ||
      config.sample_rate <= 0.0 || config.block_size <= 0) {
    return LiveOpenResult::kInvalidArgument;
  }

  const uint32_t device_id =
      config.use_default_audio_output
          ? 0u
          : backends::CoreAudioDevice::output_device_id(config.audio_output_index);
  if (!config.use_default_audio_output && device_id == 0) {
    return LiveOpenResult::kInvalidArgument;
  }

#if defined(SONARE_WITH_ARRANGEMENT)
  if (config.use_midi_input) {
    const auto current = engine->midi_input_binding();
    if (current.source != nullptr && !current.owner) {
      return LiveOpenResult::kMidiInputSourceNotOwned;
    }
  }
#endif

  engine_ = engine;
  destination_id_ = config.destination_id;
  negotiated_ = AudioStreamConfig{};
  negotiated_.sample_rate = config.sample_rate;
  negotiated_.max_block_size = config.block_size;
  output_negotiated_ = AudioStreamConfig{};
  input_negotiated_ = AudioStreamConfig{};
  input_requested_ = false;
  output_requested_ = true;
  input_opened_ = false;
  output_opened_ = false;
  audio_device_open_ = false;
  input_device_open_ = false;
  output_device_open_ = false;
  engine_prepared_ = false;
  device_format_refused_ = false;
  driver_attached_ = false;
  midi_selected_ = config.use_midi_input;
#if defined(SONARE_WITH_ARRANGEMENT)
  midi_bound_ = false;
  previous_midi_ = {};
#endif
  midi_capture_started_ = false;
  require_exact_rate_ = false;
  input_uses_ring_ = false;
  render_calls_.store(0, std::memory_order_relaxed);
  midi_time_mapper_.reset();

  if (!engine_->try_attach_live_driver(this)) {
    engine_ = nullptr;
    midi_selected_ = false;
    return LiveOpenResult::kEngineAlreadyDriven;
  }
  driver_attached_ = true;
  CloseOnExit close_on_failure(this);

#if defined(SONARE_WITH_ARRANGEMENT)
  if (config.use_midi_input) {
    try {
      engine_->start_midi_capture();
      midi_capture_started_ = true;
    } catch (const std::bad_alloc&) {
      return LiveOpenResult::kOutOfMemory;
    }
  }
#endif

  audio_ = std::make_unique<backends::CoreAudioDevice>();
  if (config.use_midi_input) {
#if defined(SONARE_WITH_ARRANGEMENT)
    midi_ = std::make_shared<backends::CoreMidiInput>();
#else
    midi_ = std::make_unique<backends::CoreMidiInput>();
#endif
    auto* core_midi = static_cast<backends::CoreMidiInput*>(midi_.get());
    core_midi->set_time_mapper(&midi_time_mapper_);
    if (!core_midi->open(config.midi_input_index)) return LiveOpenResult::kMidiInputUnavailable;
  }

  AudioStreamConfig stream;
  stream.sample_rate = config.sample_rate;
  stream.max_block_size = config.block_size;
  stream.num_output_channels = 2;
  auto* core_audio = dynamic_cast<backends::CoreAudioDevice*>(audio_.get());
  const bool audio_opened =
      core_audio != nullptr && core_audio->open_device(device_id, stream, &pump_);
  if (!audio_opened) {
    const auto* core = dynamic_cast<const backends::CoreAudioDevice*>(audio_.get());
    const LiveOpenResult result = prepare_failure_ ? LiveOpenResult::kEngineRefusedFormat
                                  : device_format_refused_ || (core && core->format_refused())
                                      ? LiveOpenResult::kAudioFormatRefused
                                      : LiveOpenResult::kAudioOutputUnavailable;
    audio_->close();
    return result;
  }

  audio_device_open_ = true;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (midi_selected_) bind_midi_input();
#endif
  negotiated_ = output_negotiated_;
  open_ = true;
  if (!audio_->start()) {
    return LiveOpenResult::kAudioOutputStartFailed;
  }
  close_on_failure.dismiss();
  return LiveOpenResult::kOk;
}

LiveOpenResult LiveSession::open(engine::RealtimeEngine* engine, const SelectorConfig& config) {
  return open(engine, config, AudioDeviceOverride{}, AudioDeviceOverride{});
}

LiveOpenResult LiveSession::open(engine::RealtimeEngine* engine, const SelectorConfig& config,
                                 AudioDeviceOverride input_override,
                                 AudioDeviceOverride output_override) {
  return open(engine, config, std::move(input_override), std::move(output_override), {});
}

LiveOpenResult LiveSession::open(engine::RealtimeEngine* engine, const SelectorConfig& config,
                                 AudioDeviceOverride input_override,
                                 AudioDeviceOverride output_override,
                                 std::unique_ptr<MidiInputSource> midi_override) {
  prepare_failure_ = nullptr;
  if (engine == nullptr || open_ || !std::isfinite(config.sample_rate) ||
      config.sample_rate < 0.0 || config.block_size <= 0 ||
      !valid_selector(config.audio_input, false) || !valid_selector(config.audio_output, false) ||
      !valid_selector(config.midi_input, true)) {
    return LiveOpenResult::kInvalidArgument;
  }
  const bool wants_input = config.audio_input.selection != DeviceSelection::kNone;
  const bool wants_output = config.audio_output.selection != DeviceSelection::kNone;
  const bool wants_midi = config.midi_input.selection != DeviceSelection::kNone;
  if ((!wants_input && !wants_output) || (wants_midi && !wants_output)) {
    return LiveOpenResult::kInvalidArgument;
  }
#if defined(SONARE_WITH_ARRANGEMENT)
  if (wants_midi) {
    const auto current = engine->midi_input_binding();
    if (current.source != nullptr && !current.owner) {
      return LiveOpenResult::kMidiInputSourceNotOwned;
    }
  }
#endif

  std::vector<DeviceInfo> devices;
  bool have_devices = false;
  const auto resolve = [&](const Selector& selector, bool input,
                           AudioDeviceOverride override_device,
                           ResolvedAudioDevice* resolved) -> LiveOpenResult {
    if (selector.selection == DeviceSelection::kNone) return LiveOpenResult::kOk;
    if (override_device.device) {
      if (!info_matches(override_device.info, selector, input) ||
          (input ? !has_input(override_device.info) : !has_output(override_device.info))) {
        return input ? LiveOpenResult::kAudioInputUnavailable
                     : LiveOpenResult::kAudioOutputUnavailable;
      }
      resolved->info = std::move(override_device.info);
      resolved->device = std::move(override_device.device);
      return LiveOpenResult::kOk;
    }
    if (!have_devices) {
      devices = backends::CoreAudioDevice::devices();
      have_devices = true;
    }
    const DeviceInfo* info = find_device(devices, selector, input);
    if (info == nullptr) {
      return input ? LiveOpenResult::kAudioInputUnavailable
                   : LiveOpenResult::kAudioOutputUnavailable;
    }
    resolved->info = *info;
    resolved->device = std::make_unique<backends::CoreAudioDevice>(info->id, info->uid);
    return LiveOpenResult::kOk;
  };

  ResolvedAudioDevice input;
  ResolvedAudioDevice output;
  LiveOpenResult result = resolve(config.audio_input, true, std::move(input_override), &input);
  if (result != LiveOpenResult::kOk) return result;
  result = resolve(config.audio_output, false, std::move(output_override), &output);
  if (result != LiveOpenResult::kOk) return result;

  if (same_device_identity(input.info, output.info) && input.device && output.device) {
    // One physical endpoint is opened once with both directions enabled.
    output.device.reset();
  }

  double rate = config.sample_rate;
  if (rate == 0.0)
    rate = wants_output ? output.info.nominal_sample_rate : input.info.nominal_sample_rate;
  if (!std::isfinite(rate) || rate <= 0.0) return LiveOpenResult::kAudioFormatRefused;

  return open_devices(engine, config, std::move(input), std::move(output),
                      std::move(midi_override));
}

LiveOpenResult LiveSession::open_devices(engine::RealtimeEngine* engine,
                                         const SelectorConfig& config, ResolvedAudioDevice input,
                                         ResolvedAudioDevice output,
                                         std::unique_ptr<MidiInputSource> midi_override) {
  const bool wants_input = config.audio_input.selection != DeviceSelection::kNone;
  const bool wants_output = config.audio_output.selection != DeviceSelection::kNone;
  const bool same_device =
      wants_input && wants_output && same_device_identity(input.info, output.info);

  engine_ = engine;
  destination_id_ = config.destination_id;
  input_requested_ = wants_input;
  output_requested_ = wants_output;
  input_opened_ = false;
  output_opened_ = false;
  audio_device_open_ = false;
  input_device_open_ = false;
  output_device_open_ = false;
  engine_prepared_ = false;
  device_format_refused_ = false;
  driver_attached_ = false;
  midi_selected_ = false;
#if defined(SONARE_WITH_ARRANGEMENT)
  midi_bound_ = false;
  previous_midi_ = {};
#endif
  midi_capture_started_ = false;
  require_exact_rate_ = true;
  input_uses_ring_ = wants_input && wants_output && !same_device;
  clock_locked_ = input_uses_ring_ && input.info.clock_domain != 0 &&
                  input.info.clock_domain == output.info.clock_domain;
  render_calls_.store(0, std::memory_order_relaxed);
  midi_time_mapper_.reset();

  const double target_rate = config.sample_rate > 0.0 ? config.sample_rate
                             : wants_output           ? output.info.nominal_sample_rate
                                                      : input.info.nominal_sample_rate;
  negotiated_ = AudioStreamConfig{};
  negotiated_.sample_rate = target_rate;
  negotiated_.max_block_size = config.block_size;
  input_negotiated_ = AudioStreamConfig{};
  output_negotiated_ = AudioStreamConfig{};

  if (!engine_->try_attach_live_driver(this)) {
    engine_ = nullptr;
    return LiveOpenResult::kEngineAlreadyDriven;
  }
  driver_attached_ = true;
  CloseOnExit close_on_failure(this);

#if defined(SONARE_WITH_ARRANGEMENT)
  if (config.midi_input.selection == DeviceSelection::kByUid) {
    try {
      engine_->start_midi_capture();
      midi_capture_started_ = true;
    } catch (const std::bad_alloc&) {
      return LiveOpenResult::kOutOfMemory;
    }
  }
#endif

  const auto align_rate = [&](ResolvedAudioDevice* device, bool input_leg) -> LiveOpenResult {
    if (!device->device || same_rate(device->info.nominal_sample_rate, target_rate)) {
      return LiveOpenResult::kOk;
    }
    if (dynamic_cast<backends::CoreAudioDevice*>(device->device.get()) == nullptr) {
      return LiveOpenResult::kAudioFormatRefused;
    }
    const bool changed = backends::CoreAudioDevice::set_nominal_sample_rate(
        device->info.id, target_rate, device->info.uid);
    const auto current = backends::CoreAudioDevice::devices();
    const auto found =
        std::find_if(current.begin(), current.end(), [device, input_leg](const DeviceInfo& item) {
          return item.id == device->info.id && item.uid == device->info.uid &&
                 (input_leg ? has_input(item) : has_output(item));
        });
    if (found == current.end()) {
      return input_leg ? LiveOpenResult::kAudioInputUnavailable
                       : LiveOpenResult::kAudioOutputUnavailable;
    }
    device->info = *found;
    if (!changed || !same_rate(device->info.nominal_sample_rate, target_rate)) {
      return LiveOpenResult::kAudioFormatRefused;
    }
    return LiveOpenResult::kOk;
  };
  LiveOpenResult rate_result = align_rate(&input, true);
  if (rate_result == LiveOpenResult::kOk) rate_result = align_rate(&output, false);
  if (rate_result != LiveOpenResult::kOk) return rate_result;
  if (same_device) output.info = input.info;
  if (wants_input && wants_output && !same_device &&
      !same_rate(input.info.nominal_sample_rate, output.info.nominal_sample_rate)) {
    return LiveOpenResult::kAudioFormatRefused;
  }
  if (config.midi_input.selection == DeviceSelection::kByUid) {
    if (!midi_override) {
      auto selected = std::make_unique<backends::CoreMidiInput>();
      selected->set_time_mapper(&midi_time_mapper_);
      if (!selected->open_uid(config.midi_input.uid)) return LiveOpenResult::kMidiInputUnavailable;
      midi_override = std::move(selected);
    }
#if defined(SONARE_WITH_ARRANGEMENT)
    midi_ = std::shared_ptr<MidiInputSource>(std::move(midi_override));
#else
    midi_ = std::move(midi_override);
#endif
    midi_selected_ = true;
  }

  if (same_device) {
    audio_ = std::move(input.device);
  } else {
    input_audio_ = std::move(input.device);
    output_audio_ = std::move(output.device);
  }

  const auto make_stream = [&](const DeviceInfo& info, bool input_leg, bool output_leg) {
    AudioStreamConfig stream;
    stream.sample_rate = target_rate;
    stream.max_block_size = config.block_size;
    stream.num_input_channels = input_leg ? channels_for(info.input_channels) : 0;
    stream.num_output_channels = output_leg ? channels_for(info.output_channels) : 0;
    return stream;
  };

  bool opened = true;
  if (same_device) {
    const AudioStreamConfig stream = make_stream(input.info, true, true);
    opened = audio_ && audio_->open(stream, &pump_);
    audio_device_open_ = opened;
  } else {
    if (wants_input) {
      const AudioStreamConfig stream = make_stream(input.info, true, false);
      opened = input_audio_ && input_audio_->open(stream, &pump_);
      input_device_open_ = opened;
    }
    if (opened && wants_output) {
      const AudioStreamConfig stream = make_stream(output.info, false, true);
      opened = output_audio_ && output_audio_->open(stream, &pump_);
      output_device_open_ = opened;
    }
  }

  if (!opened || !engine_prepared_) {
    const auto format_refused = [](const AudioDevice* device) {
      const auto* core = dynamic_cast<const backends::CoreAudioDevice*>(device);
      return core != nullptr && core->format_refused();
    };
    const LiveOpenResult failed =
        prepare_failure_ ? LiveOpenResult::kEngineRefusedFormat
        : device_format_refused_ || format_refused(audio_.get()) ||
                format_refused(input_audio_.get()) || format_refused(output_audio_.get())
            ? LiveOpenResult::kAudioFormatRefused
        : wants_input && !input_opened_ ? LiveOpenResult::kAudioInputUnavailable
                                        : LiveOpenResult::kAudioOutputUnavailable;
    return failed;
  }

#if defined(SONARE_WITH_ARRANGEMENT)
  if (midi_selected_) bind_midi_input();
#endif
  open_ = true;

  if (!same_device && wants_input && input_audio_) {
    if (!input_audio_->start()) return LiveOpenResult::kAudioInputUnavailable;
  }
  if (!same_device && wants_output && output_audio_) {
    if (!output_audio_->start()) return LiveOpenResult::kAudioOutputStartFailed;
  }
  if (same_device && audio_) {
    if (!audio_->start()) return LiveOpenResult::kAudioOutputStartFailed;
  }
  close_on_failure.dismiss();
  return LiveOpenResult::kOk;
}

#if defined(SONARE_WITH_ARRANGEMENT)
void LiveSession::bind_midi_input() noexcept {
  previous_midi_ = engine_->midi_input_binding();
  engine_->set_midi_input_source_owned(midi_, destination_id_);
  midi_bound_ = true;
}
#endif

void LiveSession::close_devices() noexcept {
  if (audio_) {
    if (audio_device_open_) {
      audio_->stop();
      audio_->close();
    }
    audio_.reset();
  } else {
    if (output_audio_ && output_device_open_) output_audio_->stop();
    if (input_audio_ && input_device_open_) input_audio_->stop();
    if (output_audio_ && output_device_open_) output_audio_->close();
    if (input_audio_ && input_device_open_) input_audio_->close();
    output_audio_.reset();
    input_audio_.reset();
  }
  audio_device_open_ = false;
  input_device_open_ = false;
  output_device_open_ = false;
}

void LiveSession::close() noexcept {
  close_devices();
  if (auto* selected = dynamic_cast<backends::CoreMidiInput*>(midi_.get())) selected->close();
#if defined(SONARE_WITH_ARRANGEMENT)
  if (engine_ != nullptr && midi_capture_started_) engine_->stop_midi_capture();
  if (engine_ != nullptr && midi_bound_) {
    const auto current = engine_->midi_input_binding();
    if (current.source == midi_.get() && current.destination_id == destination_id_ &&
        current.owner.get() == midi_.get()) {
      if (previous_midi_.source != nullptr && previous_midi_.owner) {
        engine_->set_midi_input_source_owned(previous_midi_.owner, previous_midi_.destination_id);
      } else {
        engine_->set_midi_input_source(nullptr, previous_midi_.destination_id);
      }
    }
  }
  previous_midi_ = {};
  midi_bound_ = false;
#endif
  midi_capture_started_ = false;
  midi_.reset();
  if (engine_ != nullptr && driver_attached_) engine_->detach_live_driver(this);
  driver_attached_ = false;
  engine_ = nullptr;
  destination_id_ = 0;
  negotiated_ = AudioStreamConfig{};
  input_negotiated_ = AudioStreamConfig{};
  output_negotiated_ = AudioStreamConfig{};
  input_requested_ = false;
  output_requested_ = false;
  input_opened_ = false;
  output_opened_ = false;
  audio_device_open_ = false;
  input_device_open_ = false;
  output_device_open_ = false;
  engine_prepared_ = false;
  midi_selected_ = false;
  midi_time_mapper_.reset();
  input_uses_ring_ = false;
  require_exact_rate_ = false;
  open_ = false;
}

bool LiveSession::is_running() const noexcept { return status().running; }

LiveSession::Status LiveSession::status() const noexcept {
  Status result;
  if (!engine_) return result;
  result.device_lost = false;
  result.rate_changed = false;
  const auto add_device_status = [&result](const AudioDevice* device) {
    if (device == nullptr) return;
    result.xrun_count += device->xrun_count();
    if (const auto* core = dynamic_cast<const backends::CoreAudioDevice*>(device)) {
      result.device_lost = result.device_lost || core->device_lost();
      result.rate_changed = result.rate_changed || core->rate_changed();
    }
  };
  if (audio_) {
    add_device_status(audio_.get());
    result.output_latency_ms = negotiated_.sample_rate > 0.0
                                   ? static_cast<double>(audio_->output_latency_samples()) *
                                         1000.0 / negotiated_.sample_rate
                                   : 0.0;
    result.input_latency_ms = negotiated_.sample_rate > 0.0
                                  ? static_cast<double>(audio_->input_latency_samples()) * 1000.0 /
                                        negotiated_.sample_rate
                                  : 0.0;
  } else {
    add_device_status(output_audio_.get());
    add_device_status(input_audio_.get());
    if (output_audio_ && negotiated_.sample_rate > 0.0) {
      result.output_latency_ms = static_cast<double>(output_audio_->output_latency_samples()) *
                                 1000.0 / negotiated_.sample_rate;
    }
    if (input_audio_ && negotiated_.sample_rate > 0.0) {
      result.input_latency_ms = static_cast<double>(input_audio_->input_latency_samples()) *
                                1000.0 / negotiated_.sample_rate;
    }
  }
  result.running = open_ && !result.device_lost &&
                   (!input_requested_ ||
                    (audio_ ? audio_->is_running() : input_audio_ && input_audio_->is_running())) &&
                   (!output_requested_ ||
                    (audio_ ? audio_->is_running() : output_audio_ && output_audio_->is_running()));
  result.sample_rate =
      engine_prepared_ ? static_cast<int>(std::lround(negotiated_.sample_rate)) : 0;
  result.output_block_size = output_requested_ ? output_negotiated_.max_block_size : 0;
  result.input_block_size = input_requested_ ? input_negotiated_.max_block_size : 0;
  if (input_uses_ring_) {
    result.input_overrun_frames = input_ring_.overrun_frames();
    result.input_starvation_frames = input_ring_.starvation_frames();
    result.input_drift_repeat_frames = input_ring_.drift_repeat_frames();
    result.input_underrun_frames =
        result.input_starvation_frames + result.input_drift_repeat_frames;
  }
#if defined(SONARE_WITH_ARRANGEMENT)
  if (midi_capture_started_) result.dropped_midi_events = engine_->dropped_midi_events();
#endif
  return result;
}

#if defined(SONARE_WITH_ARRANGEMENT)
std::vector<engine::CapturedMidiEvent> LiveSession::captured_midi() {
  std::lock_guard<std::mutex> lock(captured_midi_mutex_);
  std::vector<engine::CapturedMidiEvent> result;
  if (engine_ == nullptr || !midi_capture_started_) return result;
  constexpr size_t kBatchSize = 1024;
  std::array<engine::CapturedMidiEvent, kBatchSize> batch{};
  size_t count = 0;
  do {
    count = engine_->drain_captured_midi(batch.data(), batch.size());
    result.insert(result.end(), batch.begin(), batch.begin() + static_cast<std::ptrdiff_t>(count));
  } while (count == batch.size());
  return result;
}
#endif

int LiveSession::actual_block_size() const noexcept {
  if (!open_) return 0;
  return output_requested_ ? output_negotiated_.max_block_size : input_negotiated_.max_block_size;
}

double LiveSession::actual_sample_rate() const noexcept {
  return open_ && engine_prepared_ ? negotiated_.sample_rate : 0.0;
}

double LiveSession::output_latency_ms() const noexcept {
  if (!open_ || negotiated_.sample_rate <= 0.0) return 0.0;
  const AudioDevice* device = audio_ ? audio_.get() : output_audio_.get();
  return device == nullptr ? 0.0
                           : static_cast<double>(device->output_latency_samples()) * 1000.0 /
                                 negotiated_.sample_rate;
}

uint32_t LiveSession::xrun_count() const noexcept {
  const uint64_t total = status().xrun_count;
  return static_cast<uint32_t>(std::min<uint64_t>(total, std::numeric_limits<uint32_t>::max()));
}

uint64_t LiveSession::render_callback_count() const noexcept {
  return render_calls_.load(std::memory_order_relaxed);
}

bool LiveSession::prepare_from_callback(const AudioStreamConfig& config) {
  if (engine_ == nullptr || !std::isfinite(config.sample_rate) || config.sample_rate <= 0.0 ||
      (require_exact_rate_ && !same_rate(config.sample_rate, negotiated_.sample_rate)) ||
      config.max_block_size <= 0) {
    device_format_refused_ = true;
    return false;
  }
  if (config.num_input_channels > 0) {
    input_negotiated_ = config;
    input_opened_ = true;
  }
  if (config.num_output_channels > 0) {
    output_negotiated_ = config;
    output_opened_ = true;
  }
  const bool input_ready = !input_requested_ || input_opened_;
  const bool output_ready = !output_requested_ || output_opened_;
  if (!input_ready || !output_ready || engine_prepared_) return true;

  const int max_block = std::max(input_requested_ ? input_negotiated_.max_block_size : 0,
                                 output_requested_ ? output_negotiated_.max_block_size : 0);
  if (input_uses_ring_ && !live_input_ring_dimensions_valid(input_negotiated_.max_block_size,
                                                            output_negotiated_.max_block_size)) {
    device_format_refused_ = true;
    return false;
  }
  negotiated_.sample_rate = config.sample_rate;
  negotiated_.max_block_size = max_block;
  try {
    engine_->prepare(negotiated_.sample_rate, max_block);
  } catch (const sonare::SonareException& e) {
    if (e.code() != sonare::ErrorCode::InvalidState &&
        e.code() != sonare::ErrorCode::InvalidParameter) {
      throw;
    }
    prepare_failure_ = std::current_exception();
    return false;
  }
  input_scratch_left_.assign(static_cast<size_t>(max_block), 0.0f);
  input_scratch_right_.assign(static_cast<size_t>(max_block), 0.0f);
  input_scratch_[0] = input_scratch_left_.data();
  input_scratch_[1] = input_scratch_right_.data();
  if (input_uses_ring_) {
    input_ring_.prepare(input_negotiated_.max_block_size, output_negotiated_.max_block_size,
                        clock_locked_);
  }
  engine_prepared_ = true;
  return true;
}

bool LiveSession::Pump::open(const AudioStreamConfig& config) {
  return owner_->prepare_from_callback(config);
}

void LiveSession::publish_mapper_anchor(const AudioBufferView& buffers) noexcept {
  if (engine_ == nullptr) return;
  midi_time_mapper_.publish_anchor(buffers.time.host_time_ns, engine_->transport().render_frame(),
                                   negotiated_.sample_rate);
}

void LiveSession::Pump::render(const AudioBufferView& buffers) noexcept {
  LiveSession& session = *owner_;
  session.render_calls_.fetch_add(1, std::memory_order_relaxed);
  if (session.engine_ == nullptr || !session.engine_prepared_ || buffers.num_frames <= 0 ||
      static_cast<size_t>(buffers.num_frames) > session.input_scratch_left_.size()) {
    if (buffers.outputs != nullptr && buffers.num_frames > 0) {
      for (int channel = 0; channel < buffers.num_output_channels; ++channel) {
        if (buffers.outputs[channel] != nullptr) {
          std::fill(buffers.outputs[channel], buffers.outputs[channel] + buffers.num_frames, 0.0f);
        }
      }
    }
    return;
  }

  if (session.input_uses_ring_ && buffers.num_output_channels == 0) {
    if (buffers.inputs != nullptr && buffers.num_input_channels > 0) {
      session.input_ring_.push(buffers.inputs, buffers.num_input_channels, buffers.num_frames);
    }
    return;
  }

  const bool has_input = session.input_requested_;
  if (has_input && !session.input_uses_ring_ && buffers.inputs != nullptr &&
      buffers.num_input_channels > 0) {
    const int first = 0;
    const int second = buffers.num_input_channels > 1 ? 1 : 0;
    for (int frame = 0; frame < buffers.num_frames; ++frame) {
      const float* left = buffers.inputs[first];
      const float* right = buffers.inputs[second];
      session.input_scratch_[0][frame] = left == nullptr ? 0.0f : left[frame];
      session.input_scratch_[1][frame] =
          right == nullptr ? session.input_scratch_[0][frame] : right[frame];
    }
  } else if (has_input && session.input_uses_ring_) {
    session.input_ring_.pop(session.input_scratch_.data(), buffers.num_frames);
  } else {
    std::fill(session.input_scratch_[0], session.input_scratch_[0] + buffers.num_frames, 0.0f);
    std::fill(session.input_scratch_[1], session.input_scratch_[1] + buffers.num_frames, 0.0f);
  }

  session.publish_mapper_anchor(buffers);
  session.engine_->process(session.input_scratch_.data(), 2, buffers.num_frames);

  if (!session.output_requested_ || buffers.outputs == nullptr) return;
  if (buffers.num_output_channels == 1 && buffers.outputs[0] != nullptr) {
    for (int frame = 0; frame < buffers.num_frames; ++frame) {
      buffers.outputs[0][frame] =
          (session.input_scratch_[0][frame] + session.input_scratch_[1][frame]) * 0.5f;
    }
    return;
  }
  const int channels = std::min(buffers.num_output_channels, 2);
  for (int channel = 0; channel < channels; ++channel) {
    if (buffers.outputs[channel] != nullptr) {
      std::memcpy(buffers.outputs[channel], session.input_scratch_[channel],
                  static_cast<size_t>(buffers.num_frames) * sizeof(float));
    }
  }
  for (int channel = channels; channel < buffers.num_output_channels; ++channel) {
    if (buffers.outputs[channel] != nullptr) {
      std::fill(buffers.outputs[channel], buffers.outputs[channel] + buffers.num_frames, 0.0f);
    }
  }
}

void LiveSession::Pump::close() noexcept {}

}  // namespace sonare::host
