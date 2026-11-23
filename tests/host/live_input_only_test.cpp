#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "engine/realtime_engine.h"
#include "host/live_session.h"
#include "host/midi_io.h"
#include "support/alloc_guard.h"

namespace {

using sonare::engine::CaptureSegment;
using sonare::engine::CaptureSource;
using sonare::engine::RealtimeEngine;
using sonare::host::AudioBufferView;
using sonare::host::AudioDevice;
using sonare::host::AudioDeviceCallback;
using sonare::host::AudioStreamConfig;
using sonare::host::LiveOpenResult;
using sonare::host::LiveSession;

constexpr std::array<float, 8> kLeft{0.125f, -0.25f, 0.375f, -0.5f, 0.625f, -0.75f, 0.875f, -1.0f};
constexpr std::array<float, 8> kRight{-0.875f, 0.75f, -0.625f, 0.5f, -0.375f, 0.25f, -0.125f, 1.0f};
constexpr std::array<float, 12> kDuplexLeft{0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f,
                                            0.7f, 0.8f, 0.9f, 1.0f, 1.1f, 1.2f};
constexpr std::array<float, 12> kDuplexRight{-0.1f, -0.2f, -0.3f, -0.4f, -0.5f, -0.6f,
                                             -0.7f, -0.8f, -0.9f, -1.0f, -1.1f, -1.2f};

class FakeInputDevice final : public AudioDevice {
 public:
  bool open(const AudioStreamConfig& config, AudioDeviceCallback* callback) override {
    if (config.num_input_channels != 2 || config.num_output_channels != 0 || callback == nullptr) {
      return false;
    }
    config_ = config;
    callback_ = callback;
    return callback_->open(config_);
  }

  bool start() override {
    if (callback_ == nullptr) return false;
    running_ = true;
    for (int offset = 0; offset < static_cast<int>(kLeft.size());
         offset += config_.max_block_size) {
      const int frames = std::min(config_.max_block_size, static_cast<int>(kLeft.size()) - offset);
      std::array<const float*, 2> inputs{kLeft.data() + offset, kRight.data() + offset};
      AudioBufferView buffers;
      buffers.inputs = inputs.data();
      buffers.num_input_channels = 2;
      buffers.num_frames = frames;
      buffers.time.sample_time = offset;
      buffers.time.host_time_ns =
          1'000'000'000u + static_cast<uint64_t>(offset) * 1'000'000'000u / 48000u;
      callback_->render(buffers);
    }
    return true;
  }

  void stop() noexcept override { running_ = false; }

  void close() noexcept override {
    if (callback_ != nullptr) callback_->close();
    callback_ = nullptr;
  }

  bool is_running() const noexcept override { return running_; }

 private:
  AudioStreamConfig config_{};
  AudioDeviceCallback* callback_ = nullptr;
  bool running_ = false;
};

class FakeOutputDevice final : public AudioDevice {
 public:
  explicit FakeOutputDevice(int* open_count = nullptr) noexcept : open_count_(open_count) {}

  bool open(const AudioStreamConfig& config, AudioDeviceCallback* callback) override {
    if (open_count_ != nullptr) ++*open_count_;
    if (config.num_input_channels != 0 || config.num_output_channels <= 0 || callback == nullptr) {
      return false;
    }
    callback_ = callback;
    return callback_->open(config);
  }
  bool start() override {
    running_ = true;
    return true;
  }
  void stop() noexcept override { running_ = false; }
  void close() noexcept override {
    if (callback_ != nullptr) callback_->close();
  }
  bool is_running() const noexcept override { return running_; }

 private:
  int* open_count_ = nullptr;
  AudioDeviceCallback* callback_ = nullptr;
  bool running_ = false;
};

struct DuplexState {
  std::mutex mutex;
  std::condition_variable changed;
  bool first_input_ready = false;
  bool first_output_done = false;
  bool input_done = false;
  bool failed = false;
  std::vector<std::string> order;
};

void record(DuplexState& state, const char* event) {
  std::lock_guard<std::mutex> lock(state.mutex);
  state.order.emplace_back(event);
}

class ClockedInputDevice final : public AudioDevice {
 public:
  explicit ClockedInputDevice(std::shared_ptr<DuplexState> state) : state_(std::move(state)) {}

  bool open(const AudioStreamConfig& config, AudioDeviceCallback* callback) override {
    if (config.num_input_channels != 2 || config.num_output_channels != 0 || callback == nullptr) {
      return false;
    }
    config_ = config;
    callback_ = callback;
    return callback_->open(config_);
  }

  bool start() override {
    if (callback_ == nullptr) return false;
    running_ = true;
    record(*state_, "input-start");
    producer_ = std::thread([this] {
      emit_block(0);
      {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->first_input_ready = true;
      }
      state_->changed.notify_all();

      bool output_seen = false;
      {
        std::unique_lock<std::mutex> lock(state_->mutex);
        output_seen = state_->changed.wait_for(lock, std::chrono::seconds(5),
                                               [this] { return state_->first_output_done; });
        if (!output_seen) state_->failed = true;
      }
      if (output_seen) {
        emit_block(4);
        emit_block(8);
      }
      {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->input_done = true;
      }
      state_->changed.notify_all();
    });
    return true;
  }

  void stop() noexcept override {
    record(*state_, "input-stop");
    running_ = false;
    if (producer_.joinable()) producer_.join();
  }

  void close() noexcept override {
    record(*state_, "input-close");
    if (callback_ != nullptr) callback_->close();
    callback_ = nullptr;
  }

  bool is_running() const noexcept override { return running_; }

 private:
  void emit_block(int offset) {
    std::array<const float*, 2> inputs{kDuplexLeft.data() + offset, kDuplexRight.data() + offset};
    AudioBufferView buffers;
    buffers.inputs = inputs.data();
    buffers.num_input_channels = 2;
    buffers.num_frames = 4;
    buffers.time.sample_time = offset;
    buffers.time.host_time_ns =
        2'000'000'000u + static_cast<uint64_t>(offset) * 1'000'000'000u / 48000u;
    callback_->render(buffers);
  }

  std::shared_ptr<DuplexState> state_;
  AudioStreamConfig config_{};
  AudioDeviceCallback* callback_ = nullptr;
  std::thread producer_;
  bool running_ = false;
};

class ClockedOutputDevice final : public AudioDevice {
 public:
  explicit ClockedOutputDevice(std::shared_ptr<DuplexState> state) : state_(std::move(state)) {}

  bool open(const AudioStreamConfig& config, AudioDeviceCallback* callback) override {
    if (config.num_input_channels != 0 || config.num_output_channels != 2 || callback == nullptr) {
      return false;
    }
    config_ = config;
    callback_ = callback;
    return callback_->open(config_);
  }

  bool start() override {
    if (callback_ == nullptr) return false;
    running_ = true;
    record(*state_, "output-start");
    std::thread consumer([this] {
      bool input_ready = false;
      {
        std::unique_lock<std::mutex> lock(state_->mutex);
        input_ready = state_->changed.wait_for(lock, std::chrono::seconds(5),
                                               [this] { return state_->first_input_ready; });
        if (!input_ready) state_->failed = true;
      }
      if (input_ready) emit_block(0);
      {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->first_output_done = true;
      }
      state_->changed.notify_all();

      bool input_done = false;
      {
        std::unique_lock<std::mutex> lock(state_->mutex);
        input_done = state_->changed.wait_for(lock, std::chrono::seconds(5),
                                              [this] { return state_->input_done; });
        if (!input_done) state_->failed = true;
      }
      if (input_done) emit_block(4);
    });
    consumer.join();
    return true;
  }

  void stop() noexcept override {
    record(*state_, "output-stop");
    running_ = false;
  }

  void close() noexcept override {
    record(*state_, "output-close");
    if (callback_ != nullptr) callback_->close();
    callback_ = nullptr;
  }

  bool is_running() const noexcept override { return running_; }

 private:
  void emit_block(int input_offset) {
    std::array<float, 4> left{};
    std::array<float, 4> right{};
    std::array<float*, 2> outputs{left.data(), right.data()};
    AudioBufferView buffers;
    buffers.outputs = outputs.data();
    buffers.num_output_channels = 2;
    buffers.num_frames = 4;
    buffers.time.sample_time = input_offset;
    buffers.time.host_time_ns =
        2'000'000'000u + static_cast<uint64_t>(input_offset) * 1'000'000'000u / 48000u;
    callback_->render(buffers);
  }

  std::shared_ptr<DuplexState> state_;
  AudioStreamConfig config_{};
  AudioDeviceCallback* callback_ = nullptr;
  bool running_ = false;
};

#if defined(SONARE_WITH_ARRANGEMENT)
sonare::midi::Ump test_note() {
  sonare::midi::Ump ump;
  ump.words[0] = 0x20903C7Fu;
  ump.word_count = 1;
  return ump;
}
#endif

}  // namespace

TEST_CASE("LiveSession captures an input-only fake stream", "[host][live_input_only]") {
  constexpr int kCapacity = 6;
  std::array<float, kCapacity> captured_left{};
  std::array<float, kCapacity> captured_right{};
  std::array<float*, 2> capture_channels{captured_left.data(), captured_right.data()};

  RealtimeEngine engine;
  engine.set_capture_source(CaptureSource::kInput);
  engine.set_capture_segment(CaptureSegment{capture_channels.data(), 2, kCapacity});
  engine.set_capture_armed(true);
#if defined(SONARE_WITH_ARRANGEMENT)
  sonare::host::FixedMidiInputSource<8> existing_source;
  REQUIRE(existing_source.push_event(test_note(), 0));
  engine.set_midi_input_source(&existing_source, 7);
  engine.start_midi_capture();
#endif

  LiveSession session;
  LiveSession::SelectorConfig config;
  config.audio_input = {LiveSession::DeviceSelection::kByUid, "fake-input"};
  config.sample_rate = 0.0;
  config.block_size = 4;

  LiveSession::AudioDeviceOverride input;
  input.info.id = 31;
  input.info.uid = "fake-input";
  input.info.name = "Input fixture";
  input.info.input_channels = 2;
  input.info.nominal_sample_rate = 48000.0;
  input.device = std::make_unique<FakeInputDevice>();

  REQUIRE(session.open(&engine, config, std::move(input), {}) == LiveOpenResult::kOk);
  REQUIRE(session.is_running());
  REQUIRE(session.actual_sample_rate() == 48000.0);
  REQUIRE(session.actual_block_size() == 4);
  REQUIRE(session.status().input_block_size == 4);
  REQUIRE(session.status().output_block_size == 0);
  REQUIRE(engine.captured_frames() == kCapacity);
  REQUIRE(engine.capture_dropped_frames() == kLeft.size() - kCapacity);
  for (int frame = 0; frame < kCapacity; ++frame) {
    REQUIRE(captured_left[frame] == kLeft[static_cast<size_t>(frame)]);
    REQUIRE(captured_right[frame] == kRight[static_cast<size_t>(frame)]);
  }
#if defined(SONARE_WITH_ARRANGEMENT)
  REQUIRE(existing_source.pending_count() == 0);
  REQUIRE(session.captured_midi().empty());
  REQUIRE(session.status().dropped_midi_events == 0);
#endif

  session.close();
  REQUIRE_FALSE(session.is_running());
#if defined(SONARE_WITH_ARRANGEMENT)
  std::array<sonare::engine::CapturedMidiEvent, 4> stale_capture{};
  REQUIRE(engine.drain_captured_midi(stale_capture.data(), stale_capture.size()) == 1);
  engine.stop_midi_capture();
  REQUIRE(existing_source.push_event(test_note(), 0));
#endif
  engine.set_capture_armed(false);
#if defined(SONARE_WITH_ARRANGEMENT)
  std::array<float, 4> output_left{};
  std::array<float, 4> output_right{};
  std::array<float*, 2> outputs{output_left.data(), output_right.data()};
  engine.process(outputs.data(), 2, static_cast<int>(output_left.size()));
  REQUIRE(existing_source.pending_count() == 0);
#endif
}

TEST_CASE("LiveSession requires output before opening a MIDI source", "[host][live_input_only]") {
  RealtimeEngine engine;
  LiveSession session;
  LiveSession::SelectorConfig config;
  config.audio_input = {LiveSession::DeviceSelection::kByUid, "fake-input"};
  config.midi_input = {LiveSession::DeviceSelection::kByUid, "fake-midi"};

  LiveSession::AudioDeviceOverride input;
  input.info.id = 31;
  input.info.uid = "fake-input";
  input.info.input_channels = 2;
  input.info.nominal_sample_rate = 48000.0;
  input.device = std::make_unique<FakeInputDevice>();

  REQUIRE(session.open(&engine, config, std::move(input), {}) == LiveOpenResult::kInvalidArgument);
  REQUIRE(engine.live_driver() == nullptr);
}

#if defined(SONARE_WITH_ARRANGEMENT)
TEST_CASE("LiveSession restores the owned prior MIDI source and keeps it alive",
          "[host][live_input_only][live_midi]") {
  constexpr uint32_t kPriorDestination = 7;
  constexpr uint32_t kSessionDestination = 11;
  auto prior = std::make_shared<sonare::host::FixedMidiInputSource<8>>();
  std::weak_ptr<sonare::host::FixedMidiInputSource<8>> prior_weak = prior;
  auto selected = std::make_unique<sonare::host::FixedMidiInputSource<8>>();
  const auto* selected_pointer = selected.get();
  RealtimeEngine engine;
  engine.set_midi_input_source_owned(prior, kPriorDestination);
  prior.reset();
  REQUIRE_FALSE(prior_weak.expired());

  LiveSession::SelectorConfig config;
  config.audio_output = {LiveSession::DeviceSelection::kByUid, "fake-output"};
  config.midi_input = {LiveSession::DeviceSelection::kByUid, "fake-midi"};
  config.destination_id = kSessionDestination;

  LiveSession::AudioDeviceOverride output;
  output.info.id = 32;
  output.info.uid = "fake-output";
  output.info.output_channels = 2;
  output.info.nominal_sample_rate = sonare::constants::kDefaultDawSampleRate;
  output.device = std::make_unique<FakeOutputDevice>();

  LiveSession session;
  REQUIRE(session.open(&engine, config, {}, std::move(output), std::move(selected)) ==
          LiveOpenResult::kOk);
  const auto selected_binding = engine.midi_input_binding();
  CHECK(selected_binding.source == selected_pointer);
  CHECK(selected_binding.destination_id == kSessionDestination);
  CHECK(selected_binding.owner.get() == selected_pointer);
  CHECK(selected_binding.source != prior_weak.lock().get());
  CHECK(selected_binding.destination_id != kPriorDestination);
  prior.reset();
  CHECK_FALSE(prior_weak.expired());
  session.close();
  {
    const auto restored = engine.midi_input_binding();
    const auto prior_owner = prior_weak.lock();
    CHECK(restored.source == prior_owner.get());
    CHECK(restored.destination_id == kPriorDestination);
    CHECK(restored.owner.get() == restored.source);
  }
  CHECK_FALSE(prior_weak.expired());
  CHECK(engine.live_driver() == nullptr);
  engine.set_midi_input_source(nullptr);
  CHECK(prior_weak.expired());
}

TEST_CASE("LiveSession rejects an unowned prior MIDI source before device open",
          "[host][live_input_only][live_midi]") {
  constexpr uint32_t kPriorDestination = 7;
  sonare::host::FixedMidiInputSource<8> prior;
  RealtimeEngine engine;
  engine.set_midi_input_source(&prior, kPriorDestination);

  LiveSession::SelectorConfig config;
  config.audio_output = {LiveSession::DeviceSelection::kByUid, "fake-output"};
  config.midi_input = {LiveSession::DeviceSelection::kByUid, "fake-midi"};
  LiveSession::AudioDeviceOverride output;
  output.info.id = 32;
  output.info.uid = "fake-output";
  output.info.output_channels = 2;
  output.info.nominal_sample_rate = sonare::constants::kDefaultDawSampleRate;
  int open_calls = 0;
  output.device = std::make_unique<FakeOutputDevice>(&open_calls);
  auto selected = std::make_unique<sonare::host::FixedMidiInputSource<8>>();

  LiveSession session;
  CHECK(session.open(&engine, config, {}, std::move(output), std::move(selected)) ==
        LiveOpenResult::kMidiInputSourceNotOwned);
  CHECK(open_calls == 0);
  const auto binding = engine.midi_input_binding();
  CHECK(binding.source == &prior);
  CHECK(binding.destination_id == kPriorDestination);
  CHECK(engine.live_driver() == nullptr);
}

TEST_CASE("LiveSession preserves a source replacement made while it is open",
          "[host][live_input_only][live_midi]") {
  constexpr uint32_t kSessionDestination = 11;
  constexpr uint32_t kReplacementDestination = 13;
  RealtimeEngine engine;
  LiveSession::SelectorConfig config;
  config.audio_output = {LiveSession::DeviceSelection::kByUid, "fake-output"};
  config.midi_input = {LiveSession::DeviceSelection::kByUid, "fake-midi"};
  config.destination_id = kSessionDestination;
  LiveSession::AudioDeviceOverride output;
  output.info.id = 32;
  output.info.uid = "fake-output";
  output.info.output_channels = 2;
  output.info.nominal_sample_rate = sonare::constants::kDefaultDawSampleRate;
  output.device = std::make_unique<FakeOutputDevice>();
  auto selected = std::make_unique<sonare::host::FixedMidiInputSource<8>>();
  LiveSession session;
  REQUIRE(session.open(&engine, config, {}, std::move(output), std::move(selected)) ==
          LiveOpenResult::kOk);

  auto replacement = std::make_shared<sonare::host::FixedMidiInputSource<8>>();
  engine.set_midi_input_source_owned(replacement, kReplacementDestination);
  session.close();
  const auto current = engine.midi_input_binding();
  CHECK(current.source == replacement.get());
  CHECK(current.destination_id == kReplacementDestination);
  CHECK(current.owner == replacement);
}

TEST_CASE("LiveSession leaves a raw MIDI binding alone when MIDI is not selected",
          "[host][live_input_only][live_midi]") {
  constexpr uint32_t kPriorDestination = 7;
  sonare::host::FixedMidiInputSource<8> prior;
  RealtimeEngine engine;
  engine.set_midi_input_source(&prior, kPriorDestination);
  LiveSession::SelectorConfig config;
  config.audio_output = {LiveSession::DeviceSelection::kByUid, "fake-output"};
  LiveSession::AudioDeviceOverride output;
  output.info.id = 32;
  output.info.uid = "fake-output";
  output.info.output_channels = 2;
  output.info.nominal_sample_rate = sonare::constants::kDefaultDawSampleRate;
  output.device = std::make_unique<FakeOutputDevice>();

  LiveSession session;
  REQUIRE(session.open(&engine, config, {}, std::move(output)) == LiveOpenResult::kOk);
  session.close();
  const auto binding = engine.midi_input_binding();
  CHECK(binding.source == &prior);
  CHECK(binding.destination_id == kPriorDestination);
}

TEST_CASE("LiveSession reserves MIDI capture before opening devices",
          "[host][live_input_only][live_midi][allocation]") {
  RealtimeEngine engine;
  LiveSession::SelectorConfig config;
  config.audio_output = {LiveSession::DeviceSelection::kByUid, "fake-output"};
  config.midi_input = {LiveSession::DeviceSelection::kByUid, "fake-midi"};
  LiveSession::AudioDeviceOverride output;
  output.info.id = 32;
  output.info.uid = "fake-output";
  output.info.output_channels = 2;
  output.info.nominal_sample_rate = sonare::constants::kDefaultDawSampleRate;
  int open_calls = 0;
  output.device = std::make_unique<FakeOutputDevice>(&open_calls);
  auto selected = std::make_unique<sonare::host::FixedMidiInputSource<8>>();

  LiveSession session;
  LiveOpenResult result = LiveOpenResult::kOk;
  {
    sonare::test::AllocationFailureAtGuard fail_first(1);
    result = session.open(&engine, config, {}, std::move(output), std::move(selected));
  }
  CHECK(result == LiveOpenResult::kOutOfMemory);
  CHECK(open_calls == 0);
  CHECK(engine.live_driver() == nullptr);
}
#endif  // defined(SONARE_WITH_ARRANGEMENT)

TEST_CASE("LiveSession coordinates independent audio devices", "[host][live][live_duplex]") {
  constexpr int kCapacity = 8;
  std::array<float, kCapacity> captured_left{};
  std::array<float, kCapacity> captured_right{};
  std::array<float*, 2> capture_channels{captured_left.data(), captured_right.data()};

  RealtimeEngine engine;
  engine.set_capture_source(CaptureSource::kInput);
  engine.set_capture_segment(CaptureSegment{capture_channels.data(), 2, kCapacity});
  engine.set_capture_armed(true);

  auto state = std::make_shared<DuplexState>();
  LiveSession::SelectorConfig config;
  config.audio_input = {LiveSession::DeviceSelection::kByUid, "clocked-input"};
  config.audio_output = {LiveSession::DeviceSelection::kByUid, "clocked-output"};
  config.block_size = 4;

  LiveSession::AudioDeviceOverride input;
  input.info.id = 41;
  input.info.uid = "clocked-input";
  input.info.input_channels = 2;
  input.info.nominal_sample_rate = 48000.0;
  input.info.clock_domain = 7;
  input.device = std::make_unique<ClockedInputDevice>(state);

  LiveSession::AudioDeviceOverride output;
  output.info.id = 42;
  output.info.uid = "clocked-output";
  output.info.output_channels = 2;
  output.info.nominal_sample_rate = 48000.0;
  output.info.clock_domain = 7;
  output.device = std::make_unique<ClockedOutputDevice>(state);

  LiveSession session;
  REQUIRE(session.open(&engine, config, std::move(input), std::move(output)) ==
          LiveOpenResult::kOk);
  REQUIRE(session.is_running());
  const LiveSession::Status active = session.status();
  REQUIRE(active.input_block_size == 4);
  REQUIRE(active.output_block_size == 4);
  REQUIRE(active.input_overrun_frames == 0);
  REQUIRE(active.input_underrun_frames == 0);
  REQUIRE(engine.captured_frames() == kCapacity);
  for (int frame = 0; frame < 4; ++frame) {
    REQUIRE(captured_left[frame] == 0.0f);
    REQUIRE(captured_right[frame] == 0.0f);
    REQUIRE(captured_left[frame + 4] == kDuplexLeft[static_cast<size_t>(frame)]);
    REQUIRE(captured_right[frame + 4] == kDuplexRight[static_cast<size_t>(frame)]);
  }
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    REQUIRE_FALSE(state->failed);
    REQUIRE(state->order.size() >= 2);
    REQUIRE(state->order[0] == "input-start");
    REQUIRE(state->order[1] == "output-start");
  }

  LiveSession second;
  LiveSession::AudioDeviceOverride second_input;
  second_input.info.id = 41;
  second_input.info.uid = "clocked-input";
  second_input.info.input_channels = 2;
  second_input.info.nominal_sample_rate = 48000.0;
  second_input.info.clock_domain = 7;
  second_input.device = std::make_unique<FakeInputDevice>();
  LiveSession::AudioDeviceOverride second_output;
  second_output.info.id = 42;
  second_output.info.uid = "clocked-output";
  second_output.info.output_channels = 2;
  second_output.info.nominal_sample_rate = 48000.0;
  second_output.info.clock_domain = 7;
  second_output.device = std::make_unique<ClockedOutputDevice>(state);
  REQUIRE(second.open(&engine, config, std::move(second_input), std::move(second_output)) ==
          LiveOpenResult::kEngineAlreadyDriven);

  session.close();
  REQUIRE_FALSE(session.is_running());
  REQUIRE(engine.live_driver() == nullptr);
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    const auto output_stop = std::find(state->order.begin(), state->order.end(), "output-stop");
    const auto input_stop = std::find(state->order.begin(), state->order.end(), "input-stop");
    REQUIRE(output_stop != state->order.end());
    REQUIRE(input_stop != state->order.end());
    REQUIRE(output_stop < input_stop);
    const auto output_close = std::find(state->order.begin(), state->order.end(), "output-close");
    const auto input_close = std::find(state->order.begin(), state->order.end(), "input-close");
    REQUIRE(output_close != state->order.end());
    REQUIRE(input_close != state->order.end());
    REQUIRE(output_close < input_close);
  }

  engine.set_capture_armed(false);
  LiveSession after_close;
  LiveSession::SelectorConfig input_only;
  input_only.audio_input = {LiveSession::DeviceSelection::kByUid, "fake-input"};
  LiveSession::AudioDeviceOverride released_input;
  released_input.info.id = 31;
  released_input.info.uid = "fake-input";
  released_input.info.input_channels = 2;
  released_input.info.nominal_sample_rate = 48000.0;
  released_input.device = std::make_unique<FakeInputDevice>();
  REQUIRE(after_close.open(&engine, input_only, std::move(released_input), {}) ==
          LiveOpenResult::kOk);
  after_close.close();
}

TEST_CASE("LiveSession keeps distinct id-zero audio overrides separate",
          "[host][live][live_duplex]") {
  RealtimeEngine engine;
  LiveSession::SelectorConfig config;
  config.audio_input = {LiveSession::DeviceSelection::kByUid, "id-zero-input"};
  config.audio_output = {LiveSession::DeviceSelection::kByUid, "id-zero-output"};
  config.block_size = 4;

  LiveSession::AudioDeviceOverride input;
  input.info.id = 0;
  input.info.uid = "id-zero-input";
  input.info.input_channels = 2;
  input.info.nominal_sample_rate = 48000.0;
  input.device = std::make_unique<FakeInputDevice>();

  LiveSession::AudioDeviceOverride output;
  output.info.id = 0;
  output.info.uid = "id-zero-output";
  output.info.output_channels = 2;
  output.info.nominal_sample_rate = 48000.0;
  output.device = std::make_unique<FakeOutputDevice>();

  LiveSession session;
  REQUIRE(session.open(&engine, config, std::move(input), std::move(output)) ==
          LiveOpenResult::kOk);
  CHECK(session.status().input_block_size == 4);
  CHECK(session.status().output_block_size == 4);
  session.close();
}
