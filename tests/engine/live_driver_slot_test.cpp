#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <thread>
#include <vector>

#include "engine/realtime_engine.h"
#include "util/constants.h"
#include "util/exception.h"

namespace {

using namespace sonare;
using namespace sonare::engine;

class Driver final : public LiveDriver {
 public:
  void stop() noexcept override { stopped = true; }

  bool stopped = false;
};

}  // namespace

TEST_CASE("Device ownership excludes other drivers until the owner releases it",
          "[engine][live_driver_slot]") {
  RealtimeEngine engine;
  Driver first;
  Driver second;
  REQUIRE(engine.live_driver() == nullptr);
  REQUIRE_FALSE(engine.try_attach_live_driver(nullptr));
  REQUIRE_FALSE(engine.detach_live_driver(nullptr));

  std::atomic<bool> start{false};
  bool first_attached = false;
  bool second_attached = false;
  std::thread contender([&] {
    while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
    second_attached = engine.try_attach_live_driver(&second);
  });
  start.store(true, std::memory_order_release);
  first_attached = engine.try_attach_live_driver(&first);
  contender.join();

  REQUIRE(first_attached != second_attached);
  Driver* owner = first_attached ? &first : &second;
  Driver* rival = first_attached ? &second : &first;
  REQUIRE(engine.live_driver() == owner);
  CHECK_FALSE(engine.try_attach_live_driver(owner));
  CHECK_FALSE(engine.detach_live_driver(rival));
  CHECK(engine.live_driver() == owner);
  try {
    engine.require_quiescent();
    FAIL("The device-owned engine must refuse a quiescent operation");
  } catch (const SonareParameterRefusal& error) {
    CHECK(error.code() == ErrorCode::InvalidState);
    CHECK(error.reason() == ErrorReason::EngineDeviceDriven);
    CHECK(error.subjects() == std::vector<std::string>{"engine"});
  }

  engine.live_driver()->stop();
  CHECK(owner->stopped);
  REQUIRE(engine.detach_live_driver(owner));
  CHECK_NOTHROW(engine.require_quiescent());
  REQUIRE(engine.try_attach_live_driver(rival));
  CHECK_FALSE(engine.detach_live_driver(owner));
  CHECK(engine.detach_live_driver(rival));
}

TEST_CASE("Input capture counts dropped frames after capacity is reached",
          "[engine][live_driver_slot][capture]") {
  constexpr int kFrames = 4;
  constexpr int kCapacity = 3;
  constexpr float kSentinel = -0.75f;
  const std::array<float, kFrames> raw{0.125f, 0.25f, 0.5f, 0.75f};
  std::array<float, kCapacity + 1> captured{};
  captured.fill(kSentinel);
  float* destination[]{captured.data()};

  RealtimeEngine engine;
  engine.prepare(constants::kDefaultDawSampleRate, kFrames);
  engine.set_capture_segment({destination, 1, kCapacity});
  engine.set_input_monitor(false, 1.0f);
  engine.set_capture_source(CaptureSource::kInput);
  engine.set_capture_armed(true);

  auto input = raw;
  float* audio[]{input.data()};
  engine.process(audio, 1, kFrames);
  REQUIRE(engine.captured_frames() == kCapacity);
  CHECK(engine.capture_dropped_frames() == kFrames - kCapacity);
  for (int index = 0; index < kCapacity; ++index) {
    CHECK(captured[index] == raw[index]);
  }
  CHECK(captured.back() == kSentinel);

  input = raw;
  engine.process(audio, 1, kFrames);
  CHECK(engine.capture_dropped_frames() == 2 * kFrames - kCapacity);
  CHECK(engine.capture_overflow_count() == 2 * kFrames - kCapacity);
  CHECK(captured.back() == kSentinel);

  engine.set_capture_armed(false);
  engine.process(audio, 1, kFrames);
  CHECK(engine.capture_dropped_frames() == 2 * kFrames - kCapacity);
  engine.reset_capture();
  CHECK(engine.capture_dropped_frames() == 0);
}
