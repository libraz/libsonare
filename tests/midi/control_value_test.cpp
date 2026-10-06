/// @file control_value_test.cpp
/// @brief Exhaustive tests for the MIDI 2.0-width control value types.

#include "midi/control_value.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <vector>

#include "midi/ump.h"

using sonare::midi::Bend32;
using sonare::midi::Control32;
using sonare::midi::Velocity16;

namespace {

static_assert(std::is_trivially_copyable_v<Velocity16>);
static_assert(std::is_trivially_copyable_v<Control32>);
static_assert(std::is_trivially_copyable_v<Bend32>);
static_assert(!std::is_convertible_v<uint16_t, Velocity16>);
static_assert(!std::is_convertible_v<uint32_t, Control32>);
static_assert(!std::is_convertible_v<uint32_t, Bend32>);
static_assert(!std::is_convertible_v<Velocity16, uint16_t>);
static_assert(!std::is_convertible_v<Control32, uint32_t>);
static_assert(!std::is_convertible_v<Bend32, uint32_t>);

/// Raw sweep over a 32-bit range: a coarse grid plus both neighbours of every upscale point.
template <typename Up>
std::vector<uint32_t> sweep_32(Up up, uint32_t count, uint32_t grid_step) {
  std::vector<uint32_t> raws;
  for (uint64_t r = 0; r <= 0xFFFFFFFFull; r += grid_step) raws.push_back(static_cast<uint32_t>(r));
  raws.push_back(0xFFFFFFFFu);
  for (uint32_t v = 0; v < count; ++v) {
    const uint32_t p = up(v);
    if (p > 0) raws.push_back(p - 1);
    raws.push_back(p);
    if (p < 0xFFFFFFFFu) raws.push_back(p + 1);
  }
  std::sort(raws.begin(), raws.end());
  return raws;
}

}  // namespace

TEST_CASE("control_value Velocity16 7-bit round trip", "[midi][midi2]") {
  for (uint32_t v = 0; v < 128; ++v) {
    const auto v7 = static_cast<uint8_t>(v);
    const Velocity16 vel = Velocity16::from7(v7);
    REQUIRE(vel.raw == sonare::midi::scale_velocity_7_to_16(v7));
    if (v >= 1) {
      REQUIRE(vel.u7() == v7);
      REQUIRE(vel.f7() == static_cast<float>(v));
      REQUIRE(Velocity16::from_f7(static_cast<float>(v)).raw == vel.raw);
    }
  }
  REQUIRE(Velocity16::from7(64).raw == 0x8000u);
  REQUIRE(Velocity16::from7(127).raw == 0xFFFFu);
  REQUIRE(Velocity16::from7(0).raw == 0u);
  REQUIRE(Velocity16::from_raw(0x1234u).raw == 0x1234u);
}

TEST_CASE("control_value Velocity16 floor at 1 and monotone f7", "[midi][midi2]") {
  for (uint32_t r = 0; r < 512; ++r) {
    const Velocity16 vel = Velocity16::from_raw(static_cast<uint16_t>(r));
    REQUIRE(vel.u7() == 1);
    REQUIRE(vel.f7() == 1.0f);
  }
  REQUIRE(Velocity16::from_raw(512).u7() == 1);
  REQUIRE(Velocity16::from_raw(1024).u7() == 2);
  float prev = 0.0f;
  for (uint32_t r = 0; r <= 0xFFFFu; ++r) {
    const Velocity16 vel = Velocity16::from_raw(static_cast<uint16_t>(r));
    REQUIRE(vel.u7() == std::max<uint32_t>(1u, r >> 9));
    const float f = vel.f7();
    REQUIRE(f >= prev);
    REQUIRE(f >= 1.0f);
    REQUIRE(f <= 127.0f);
    prev = f;
  }
}

TEST_CASE("control_value Velocity16 from_f7 inverts f7", "[midi][midi2]") {
  // Every raw at or above up(1) is reachable, so f7 -> from_f7 must return the same raw.
  const uint32_t first = sonare::midi::scale_velocity_7_to_16(1);
  for (uint32_t r = first; r <= 0xFFFFu; ++r) {
    const Velocity16 vel = Velocity16::from_raw(static_cast<uint16_t>(r));
    REQUIRE(Velocity16::from_f7(vel.f7()).raw == vel.raw);
  }
  REQUIRE(Velocity16::from_f7(0.0f).raw == Velocity16::from7(1).raw);
  REQUIRE(Velocity16::from_f7(-5.0f).raw == Velocity16::from7(1).raw);
  REQUIRE(Velocity16::from_f7(200.0f).raw == 0xFFFFu);
  REQUIRE(Velocity16::from_f7(64.5f).raw > Velocity16::from7(64).raw);
  REQUIRE(Velocity16::from_f7(64.5f).raw < Velocity16::from7(65).raw);
}

TEST_CASE("control_value Control32 7-bit min-center-max", "[midi][midi2]") {
  for (uint32_t v = 0; v < 128; ++v) {
    const auto v7 = static_cast<uint8_t>(v);
    const Control32 c = Control32::from7(v7);
    REQUIRE(c.raw == sonare::midi::scale_cc_7_to_32(v7));
    REQUIRE(c.u7() == v7);
    REQUIRE(c.f7() == static_cast<float>(v));
  }
  REQUIRE(Control32::from7(0).raw == 0u);
  REQUIRE(Control32::from7(64).raw == 0x80000000u);
  REQUIRE(Control32::from7(127).raw == 0xFFFFFFFFu);
  REQUIRE(Control32::from_raw(0xDEADBEEFu).raw == 0xDEADBEEFu);
}

TEST_CASE("control_value Control32 fractional encoding preserves MIDI1 points", "[midi][midi2]") {
  for (int value = 0; value <= 127; ++value) {
    REQUIRE(Control32::from_f7(static_cast<float>(value)).raw ==
            Control32::from7(static_cast<uint8_t>(value)).raw);
    if (value == 127) continue;
    for (float fraction : {0.25f, 0.5f, 0.75f}) {
      const float expected = static_cast<float>(value) + fraction;
      const Control32 encoded = Control32::from_f7(expected);
      REQUIRE(encoded.raw > Control32::from7(static_cast<uint8_t>(value)).raw);
      REQUIRE(encoded.raw < Control32::from7(static_cast<uint8_t>(value + 1)).raw);
      REQUIRE(std::abs(encoded.f7() - expected) < 0.00001f);
    }
  }
  REQUIRE(Control32::from_f7(-1.0f).raw == 0u);
  REQUIRE(Control32::from_f7(128.0f).raw == 0xFFFFFFFFu);
  REQUIRE(Control32::from_f7(std::numeric_limits<float>::quiet_NaN()).raw == 0u);
  REQUIRE(Control32::from_f7(std::numeric_limits<float>::infinity()).raw == 0xFFFFFFFFu);
}

TEST_CASE("control_value Control32 monotone f7", "[midi][midi2]") {
  const auto raws =
      sweep_32([](uint32_t v) { return sonare::midi::scale_cc_7_to_32(static_cast<uint8_t>(v)); },
               128u, 1u << 20);
  float prev = 0.0f;
  for (const uint32_t r : raws) {
    const Control32 c = Control32::from_raw(r);
    REQUIRE(c.u7() == (r >> 25));
    REQUIRE(c.u14() == (r >> 18));
    const float f = c.f7();
    REQUIRE(f >= prev);
    REQUIRE(f <= 127.0f);
    prev = f;
  }
}

TEST_CASE("control_value Control32 14-bit families read back by truncation", "[midi][midi2]") {
  for (uint32_t v = 0; v < 16384; ++v) {
    const auto v14 = static_cast<uint16_t>(v);
    const Control32 mcm = Control32::from14_mcm(v14);
    const Control32 zext = Control32::from14_zero_ext(v14);
    REQUIRE(mcm.raw == sonare::midi::scale_cc_14_to_32(v14));
    REQUIRE(zext.raw == sonare::midi::scale_rpn_14_to_32_zero_extend(v14));
    REQUIRE(zext.raw == (v << 18));
    // Truncation recovers the sender's value whichever family it used; CoreMIDI sends
    // structured RPN data min-center-max, so the mcm row is the CoreMIDI case.
    REQUIRE(mcm.u14() == v14);
    REQUIRE(zext.u14() == v14);
  }
  REQUIRE(Control32::from14_mcm(8192).raw == 0x80000000u);
  REQUIRE(Control32::from14_mcm(16383).raw == 0xFFFFFFFFu);
  REQUIRE(Control32::from14_zero_ext(8192).raw == 0x80000000u);
  REQUIRE(Control32::from14_zero_ext(16383).raw == 0xFFFC0000u);
}

TEST_CASE("control_value Control32 q7_25 fixed point", "[midi][midi2]") {
  REQUIRE(Control32::from_raw(0u).q7_25() == 0.0);
  REQUIRE(Control32::from_raw(60u << 25).q7_25() == 60.0);
  REQUIRE(Control32::from_raw(2u << 25).q7_25() == 2.0);
  REQUIRE(Control32::from_raw((12u << 25) | (1u << 24)).q7_25() == 12.5);
  REQUIRE(Control32::from_raw(1u).q7_25() == 1.0 / 33554432.0);
  REQUIRE(Control32::from_raw(0xFFFFFFFFu).q7_25() == 4294967295.0 / 33554432.0);
  // Zero-extended RPN 2 (coarse tuning / pitch-bend sensitivity) lands on whole semitones.
  REQUIRE(Control32::from14_zero_ext(static_cast<uint16_t>(24u << 7)).q7_25() == 24.0);
}

TEST_CASE("control_value Bend32 14-bit min-center-max", "[midi][midi2]") {
  for (uint32_t v = 0; v < 16384; ++v) {
    const auto v14 = static_cast<uint16_t>(v);
    const Bend32 b = Bend32::from14(v14);
    REQUIRE(b.raw == sonare::midi::scale_bend_14_to_32(v14));
    REQUIRE(b.u14() == v14);
    REQUIRE(b.f14() == static_cast<float>(v));
  }
  REQUIRE(Bend32::from14(0).raw == 0u);
  REQUIRE(Bend32::from14(8192).raw == 0x80000000u);
  REQUIRE(Bend32::from14(16383).raw == 0xFFFFFFFFu);
  REQUIRE(Bend32::center().raw == 0x80000000u);
  REQUIRE(Bend32::center().f14() == 8192.0f);
  REQUIRE(Bend32::from_raw(0x12345678u).raw == 0x12345678u);
}

TEST_CASE("control_value Bend32 monotone f14", "[midi][midi2]") {
  const auto raws = sweep_32(
      [](uint32_t v) { return sonare::midi::scale_bend_14_to_32(static_cast<uint16_t>(v)); },
      16384u, 1u << 20);
  float prev = 0.0f;
  for (const uint32_t r : raws) {
    const Bend32 b = Bend32::from_raw(r);
    REQUIRE(b.u14() == (r >> 18));
    const float f = b.f14();
    REQUIRE(f >= prev);
    REQUIRE(f <= 16383.0f);
    prev = f;
  }
}
