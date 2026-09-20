/// @file live_session.cpp
/// @brief Implementation of host::LiveSession. See live_session.h.

#include "host/live_session.h"

#include <cmath>
#include <cstring>

#include "engine/realtime_engine.h"

namespace sonare::host {

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

bool LiveSession::midi_input_info(size_t index, LiveDeviceInfo* out) noexcept {
  if (out == nullptr) return false;
  return backends::CoreMidiInput::source_name(index, out->name, sizeof(out->name));
}

LiveOpenResult LiveSession::open(engine::RealtimeEngine* engine, const Config& config) {
  if (engine == nullptr || open_ || !std::isfinite(config.sample_rate) ||
      config.sample_rate <= 0.0 || config.block_size <= 0) {
    return LiveOpenResult::kInvalidArgument;
  }

  std::unique_ptr<backends::CoreMidiInput> midi;
  if (config.use_midi_input) {
    midi = std::make_unique<backends::CoreMidiInput>();
    if (!midi->open(config.midi_input_index)) return LiveOpenResult::kMidiInputUnavailable;
  }

  auto audio = std::make_unique<backends::CoreAudioDevice>();
  // The mapper has to be attached before the device streams: it is what turns a
  // CoreMIDI host timestamp into a render frame, and an event that arrives
  // before it is attached has no frame to land on.
  if (midi) midi->set_time_mapper(&audio->midi_time_mapper());

  engine_ = engine;
  destination_id_ = config.destination_id;
  render_calls_.store(0, std::memory_order_relaxed);

  AudioStreamConfig stream;
  stream.sample_rate = config.sample_rate;
  stream.max_block_size = config.block_size;
  stream.num_output_channels = 2;
  const uint32_t device_id =
      config.use_default_audio_output
          ? 0u
          : backends::CoreAudioDevice::output_device_id(config.audio_output_index);
  if (!config.use_default_audio_output && device_id == 0) {
    engine_ = nullptr;
    return LiveOpenResult::kInvalidArgument;
  }
  if (!audio->open_device(device_id, stream, &pump_)) {
    engine_ = nullptr;
    return LiveOpenResult::kAudioOutputUnavailable;
  }

  audio_ = std::move(audio);
  midi_ = std::move(midi);
  // Only once the engine is prepared for the format the device negotiated, so
  // the first drained block cannot reach an engine sized for a different one.
  // A session opened without MIDI passes null, which unbinds rather than
  // leaving whatever source the engine was pointed at before.
  engine_->set_midi_input_source(midi_.get(), destination_id_);
  open_ = true;

  if (!audio_->start()) {
    close();
    return LiveOpenResult::kAudioStartFailed;
  }
  return LiveOpenResult::kOk;
}

void LiveSession::close() noexcept {
  if (audio_) {
    audio_->stop();
    audio_->close();
  }
  // The engine must stop reading the input before the input is destroyed, and
  // the device must have stopped before the engine stops reading it.
  if (engine_ != nullptr) engine_->set_midi_input_source(nullptr, 0);
  if (midi_) midi_->close();
  audio_.reset();
  midi_.reset();
  engine_ = nullptr;
  destination_id_ = 0;
  negotiated_ = AudioStreamConfig{};
  open_ = false;
}

bool LiveSession::is_running() const noexcept { return audio_ && audio_->is_running(); }

int LiveSession::actual_block_size() const noexcept {
  // Gated on the device rather than read straight out of the negotiated config,
  // whose own default block size is not zero: a closed session would otherwise
  // report a plausible figure nothing negotiated.
  return audio_ ? negotiated_.max_block_size : 0;
}

double LiveSession::actual_sample_rate() const noexcept {
  return audio_ ? negotiated_.sample_rate : 0.0;
}

double LiveSession::output_latency_ms() const noexcept {
  if (!audio_ || negotiated_.sample_rate <= 0.0) return 0.0;
  return static_cast<double>(audio_->output_latency_samples()) * 1000.0 / negotiated_.sample_rate;
}

uint32_t LiveSession::xrun_count() const noexcept { return audio_ ? audio_->xrun_count() : 0u; }

uint64_t LiveSession::render_callback_count() const noexcept {
  return render_calls_.load(std::memory_order_relaxed);
}

bool LiveSession::Pump::open(const AudioStreamConfig& config) {
  LiveSession& session = *owner_;
  if (session.engine_ == nullptr) return false;
  session.negotiated_ = config;
  session.engine_->prepare(config.sample_rate, config.max_block_size);
  return true;
}

void LiveSession::Pump::render(const AudioBufferView& buffers) noexcept {
  LiveSession& session = *owner_;
  // Counted on entry, not after a successful render: a counter that only the
  // complete path increments cannot tell a device that never called us from
  // one calling us with nothing to fill, and those need different fixes.
  session.render_calls_.fetch_add(1, std::memory_order_relaxed);
  if (session.engine_ == nullptr || buffers.outputs == nullptr) return;
  // The engine mixes into its output planes rather than overwriting them, so a
  // callback with nothing upstream has to clear them first or the device's
  // previous contents are summed back in.
  for (int channel = 0; channel < buffers.num_output_channels; ++channel) {
    if (buffers.outputs[channel] != nullptr) {
      std::memset(buffers.outputs[channel], 0,
                  static_cast<size_t>(buffers.num_frames) * sizeof(float));
    }
  }
  session.engine_->process(buffers.outputs, buffers.num_output_channels, buffers.num_frames);
}

void LiveSession::Pump::close() noexcept {}

}  // namespace sonare::host
