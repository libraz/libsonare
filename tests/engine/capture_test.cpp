#include "engine/capture.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

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

TEST_CASE("Capture boundaries include punch in and out offsets", "[engine][capture]") {
  sonare::engine::CaptureBoundaryList boundaries;
  sonare::engine::collect_capture_boundaries(1000, 256, 1050, 1200, &boundaries);

  REQUIRE(boundaries.size == 2);
  REQUIRE(boundaries.offsets[0] == 50);
  REQUIRE(boundaries.offsets[1] == 200);
  REQUIRE_FALSE(boundaries.overflowed);
}
