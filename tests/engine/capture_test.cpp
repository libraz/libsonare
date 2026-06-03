#include "engine/capture.h"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <thread>

TEST_CASE("CaptureSink captures only armed punch range with sample accuracy", "[engine][capture]") {
  std::array<float, 8> in_l{0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f};
  std::array<float, 8> in_r{10.0f, 11.0f, 12.0f, 13.0f, 14.0f, 15.0f, 16.0f, 17.0f};
  const float* input[] = {in_l.data(), in_r.data()};

  std::array<float, 4> cap_l{};
  std::array<float, 4> cap_r{};
  float* capture[] = {cap_l.data(), cap_r.data()};

  sonare::engine::CaptureSink sink;
  sink.prepare({capture, 2, 4});
  sink.set_punch(102, 106, true);
  sink.arm(true);
  sink.process(input, 2, 8, 100);

  REQUIRE(sink.captured_frames() == 4);
  REQUIRE(cap_l[0] == 2.0f);
  REQUIRE(cap_l[3] == 5.0f);
  REQUIRE(cap_r[0] == 12.0f);
  REQUIRE(cap_r[3] == 15.0f);
  REQUIRE(sink.overflow_count() == 0);
}

TEST_CASE("CaptureSink records overflow without writing beyond capacity", "[engine][capture]") {
  std::array<float, 4> input_data{1.0f, 2.0f, 3.0f, 4.0f};
  const float* input[] = {input_data.data()};
  std::array<float, 2> capture_data{};
  float* capture[] = {capture_data.data()};

  sonare::engine::CaptureSink sink;
  sink.prepare({capture, 1, 2});
  sink.arm(true);
  sink.process(input, 1, 4, 0);

  REQUIRE(sink.captured_frames() == 2);
  REQUIRE(sink.overflow_count() == 2);
  REQUIRE(capture_data[0] == 1.0f);
  REQUIRE(capture_data[1] == 2.0f);
}

TEST_CASE("CaptureSink stops recording when disarmed mid-region", "[engine][capture]") {
  std::array<float, 8> in_l{0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f};
  const float* input[] = {in_l.data()};

  std::array<float, 8> cap_l{};
  cap_l.fill(-1.0f);
  float* capture[] = {cap_l.data()};

  sonare::engine::CaptureSink sink;
  sink.prepare({capture, 1, 8});
  sink.arm(true);

  // First sub-block: capture frames 0..3 (timeline 0..3).
  sink.process(input, 1, 4, 0);
  REQUIRE(sink.captured_frames() == 4);

  // Disarm between sub-blocks (control-thread action), then process the rest of
  // the region. Nothing further should be captured.
  sink.arm(false);
  sink.process(input + 0, 1, 4, 4);

  REQUIRE_FALSE(sink.armed());
  REQUIRE(sink.captured_frames() == 4);
  REQUIRE(cap_l[0] == 0.0f);
  REQUIRE(cap_l[3] == 3.0f);
  // The slot after the disarm point was never written.
  REQUIRE(cap_l[4] == -1.0f);
  REQUIRE(sink.overflow_count() == 0);
}

TEST_CASE("CaptureSink publishes each control-thread setter to readers", "[engine][capture]") {
  std::array<float, 8> cap_l{};
  float* capture[] = {cap_l.data()};

  sonare::engine::CaptureSink sink;

  // Defaults before any setter: disarmed, no punch.
  REQUIRE_FALSE(sink.armed());
  REQUIRE_FALSE(sink.punch_enabled());

  sink.prepare({capture, 1, 8});
  sink.arm(true);
  REQUIRE(sink.armed());

  // A coherent punch window must be readable in full after a single setter call
  // (whole-snapshot publish: enabled implies both endpoints are visible).
  sink.set_punch(120, 140, true);
  REQUIRE(sink.punch_enabled());
  REQUIRE(sink.punch_start_sample() == 120);
  REQUIRE(sink.punch_end_sample() == 140);

  // Re-publishing the same fields through a different setter must not disturb the
  // previously published punch window.
  sink.arm(false);
  REQUIRE_FALSE(sink.armed());
  REQUIRE(sink.punch_enabled());
  REQUIRE(sink.punch_start_sample() == 120);
  REQUIRE(sink.punch_end_sample() == 140);

  // Disabling punch with an empty range is normalized to disabled.
  sink.set_punch(200, 200, true);
  REQUIRE_FALSE(sink.punch_enabled());
}

TEST_CASE("CaptureSink hands a coherent snapshot across control/audio threads",
          "[engine][capture]") {
  // The control thread continuously re-publishes the capture control state while
  // the audio thread reads snapshots and processes. The published values must
  // always be observed in-range (no torn 64-bit endpoint, no garbage), and the
  // run must complete without a crash. ThreadSanitizer (if enabled) additionally
  // flags any data race between the writer's store() and the readers' load().
  //
  // Each public accessor is its own seqlock read, so start/end may legitimately
  // come from adjacent generations; we therefore assert each field independently
  // lies within the range the writer ever published rather than a cross-field
  // relation. A torn 64-bit read would surface as an out-of-range value.
  std::array<float, 64> in_l{};
  for (size_t i = 0; i < in_l.size(); ++i) in_l[i] = static_cast<float>(i);
  const float* input[] = {in_l.data()};

  std::array<float, 4096> cap_l{};
  float* capture[] = {cap_l.data()};

  sonare::engine::CaptureSink sink;
  sink.prepare({capture, 1, static_cast<int64_t>(cap_l.size())});

  std::atomic<bool> stop{false};
  std::atomic<bool> torn{false};
  constexpr int kIterations = 20000;
  constexpr int64_t kMinStart = 100;
  constexpr int64_t kMaxStart = 100 + 499;  // i % 500 ranges over [0, 499]

  // Prime an in-range window so the reader never observes the pre-publish zeros.
  sink.set_punch(kMinStart, kMinStart + 20, true);

  std::thread control([&] {
    for (int i = 0; i < kIterations && !stop.load(); ++i) {
      const int64_t start = kMinStart + (i % 500);
      sink.arm((i & 1) != 0);
      sink.set_punch(start, start + 20, (i & 2) != 0);
    }
    stop.store(true);
  });

  std::thread audio([&] {
    while (!stop.load()) {
      const int64_t s = sink.punch_start_sample();
      const int64_t e = sink.punch_end_sample();
      // A torn 64-bit read or a data race would surface as an out-of-range value.
      if (s < kMinStart || s > kMaxStart) torn.store(true);
      if (e < kMinStart + 20 || e > kMaxStart + 20) torn.store(true);
      sink.process(input, 1, static_cast<int>(in_l.size()), 0);
    }
  });

  control.join();
  audio.join();

  REQUIRE_FALSE(torn.load());
}

TEST_CASE("Capture boundaries include punch in and out offsets", "[engine][capture]") {
  sonare::engine::CaptureBoundaryList boundaries;
  sonare::engine::collect_capture_boundaries(1000, 256, 1050, 1200, &boundaries);

  REQUIRE(boundaries.size == 2);
  REQUIRE(boundaries.offsets[0] == 50);
  REQUIRE(boundaries.offsets[1] == 200);
  REQUIRE_FALSE(boundaries.overflowed);
}
