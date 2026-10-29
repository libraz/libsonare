#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

#include "playback/binaural.h"
#include "playback/shrf_fixture.h"
#include "util/exception.h"

#if defined(SONARE_WITH_MIXING)
#include "core/audio.h"
#include "support/alloc_guard.h"
#endif

using namespace sonare::playback;
using namespace sonare::playback::test;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 64;

#if defined(SONARE_WITH_MIXING)
constexpr std::size_t kLargeAllocationThreshold = 64 * 1024;
#endif

HrtfSet make_hrtf(float first_itd = 0.0f, bool replace_first_itd = false) {
  std::vector<std::uint8_t> bytes = make_shrf_fixture();
  if (replace_first_itd) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &first_itd, sizeof(bits));
    for (std::size_t byte = 0; byte < sizeof(bits); ++byte) {
      bytes[kShrfHeaderBytes + byte] = static_cast<std::uint8_t>(bits >> (8 * byte));
    }
  }
  return HrtfSet::from_memory(bytes.data(), bytes.size());
}

template <typename Operation>
bool rejects_invalid_parameter(Operation&& operation) {
  try {
    operation();
  } catch (const sonare::SonareException& error) {
    return error.code() == sonare::ErrorCode::InvalidParameter;
  } catch (...) {
    return false;
  }
  return false;
}

struct RenderedBlock {
  std::array<float, kBlockSize> left{};
  std::array<float, kBlockSize> right{};
};

RenderedBlock process_block(BinauralRenderer& renderer, const HeadphoneSlotSet& slots,
                            bool with_impulse) {
  std::array<std::array<float, kBlockSize>, kVirtualSlotCount + 2> bus{};
  if (with_impulse) bus[0][kBlockSize - 1] = 0.5f;
  std::array<const float*, kVirtualSlotCount + 2> planes{};
  for (int plane = 0; plane < slots.count + 2; ++plane) {
    planes[static_cast<std::size_t>(plane)] = bus[static_cast<std::size_t>(plane)].data();
  }

  RenderedBlock output;
  renderer.process(planes.data(), output.left.data(), output.right.data(), kBlockSize);
  return output;
}

float max_abs(const RenderedBlock& output) noexcept {
  float peak = 0.0f;
  for (float sample : output.left) peak = std::max(peak, std::fabs(sample));
  for (float sample : output.right) peak = std::max(peak, std::fabs(sample));
  return peak;
}

bool outputs_match(const RenderedBlock& actual, const RenderedBlock& expected) noexcept {
  const auto matches = [](float lhs, float rhs) {
    if (!std::isfinite(lhs) || !std::isfinite(rhs)) return false;
    const float scale = std::max({1.0f, std::fabs(lhs), std::fabs(rhs)});
    return std::fabs(lhs - rhs) <= 1.0e-6f * scale;
  };
  for (std::size_t sample = 0; sample < actual.left.size(); ++sample) {
    if (!matches(actual.left[sample], expected.left[sample]) ||
        !matches(actual.right[sample], expected.right[sample])) {
      return false;
    }
  }
  return true;
}

#if defined(SONARE_WITH_MIXING)
bool allocation_guard_blocks_large_audio_copy() {
  std::vector<float> samples(kLargeAllocationThreshold / sizeof(float) + 1, 0.25f);
  sonare::Audio copied;
  bool allocation_was_blocked = false;
  {
    sonare::test::AllocationFailureGuard guard(kLargeAllocationThreshold);
    try {
      copied = sonare::Audio::from_buffer(samples.data(), samples.size(), 48000);
    } catch (const std::bad_alloc&) {
      allocation_was_blocked = true;
    }
  }
  return allocation_was_blocked && copied.empty();
}
#endif

bool invalid_rate_preserves_output(double rate, const HrtfSet& hrtf,
                                   const HeadphoneSlotSet& slots) {
  BinauralRenderer actual;
  BinauralRenderer control;
  actual.prepare(kSampleRate, kBlockSize, hrtf, slots, RoomPreset::None);
  control.prepare(kSampleRate, kBlockSize, hrtf, slots, RoomPreset::None);
  (void)process_block(actual, slots, true);
  (void)process_block(control, slots, true);

  const bool rejected = rejects_invalid_parameter(
      [&] { actual.prepare(rate, kBlockSize, hrtf, slots, RoomPreset::None); });
  const RenderedBlock actual_output = process_block(actual, slots, false);
  const RenderedBlock control_output = process_block(control, slots, false);
  return rejected && max_abs(control_output) > 1.0e-6f &&
         outputs_match(actual_output, control_output);
}

#if defined(SONARE_WITH_MIXING)
bool invalid_block_preserves_output(int block_size, const HrtfSet& hrtf,
                                    const HeadphoneSlotSet& slots) {
  BinauralRenderer actual;
  BinauralRenderer control;
  actual.prepare(kSampleRate, kBlockSize, hrtf, slots, RoomPreset::None);
  control.prepare(kSampleRate, kBlockSize, hrtf, slots, RoomPreset::None);
  (void)process_block(actual, slots, true);
  (void)process_block(control, slots, true);

  bool rejected = false;
  {
    sonare::test::AllocationFailureGuard guard(kLargeAllocationThreshold);
    rejected = rejects_invalid_parameter(
        [&] { actual.prepare(kSampleRate, block_size, hrtf, slots, RoomPreset::None); });
  }
  const RenderedBlock actual_output = process_block(actual, slots, false);
  const RenderedBlock control_output = process_block(control, slots, false);
  return rejected && max_abs(control_output) > 1.0e-6f &&
         outputs_match(actual_output, control_output);
}

bool derived_block_limit_rejects(const HrtfSet& hrtf, const HeadphoneSlotSet& slots) {
  BinauralRenderer renderer;
  bool rejected = false;
  {
    sonare::test::AllocationFailureGuard guard(kLargeAllocationThreshold);
    rejected = rejects_invalid_parameter(
        [&] { renderer.prepare(kSampleRate, INT_MAX / 2, hrtf, slots, RoomPreset::None); });
  }
  return rejected;
}

bool oversized_itd_rejects(float itd, const HeadphoneSlotSet& slots) {
  const HrtfSet oversized_hrtf = make_hrtf(itd, true);
  BinauralRenderer renderer;

  bool rejected = false;
  {
    sonare::test::AllocationFailureGuard guard(kLargeAllocationThreshold);
    rejected = rejects_invalid_parameter([&] {
      renderer.prepare(kSampleRate, kBlockSize, oversized_hrtf, slots, RoomPreset::None);
    });
  }
  return rejected;
}
#endif

}  // namespace

TEST_CASE("Binaural prepare accepts ordinary and fractional sample rates",
          "[playback][binaural][validation][binaural_prepare_domain]") {
  const HrtfSet hrtf = make_hrtf();
  const HeadphoneSlotSet slots = headphone_slots(InputLayout::Stereo);
  for (const double rate : {44100.0, 48000.0, 48000.25}) {
    CAPTURE(rate);
    BinauralRenderer renderer;
    renderer.prepare(rate, kBlockSize, hrtf, slots, RoomPreset::None);
    CHECK(max_abs(process_block(renderer, slots, true)) > 1.0e-6f);
  }
}

TEST_CASE("Binaural prepare rejects invalid sample rates before changing state",
          "[playback][binaural][validation][binaural_prepare_domain]") {
  const HrtfSet hrtf = make_hrtf();
  const HeadphoneSlotSet slots = headphone_slots(InputLayout::Stereo);
  const std::array<double, 4> invalid_rates{
      std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN(),
      std::numeric_limits<double>::max(), 4294967296.0 + 48000.0};
  for (const double rate : invalid_rates) {
    CAPTURE(rate);
    CHECK(invalid_rate_preserves_output(rate, hrtf, slots));
  }
}

TEST_CASE("Binaural prepare rejects extreme block sizes in the signed history domain",
          "[playback][binaural][validation][binaural_prepare_domain]") {
  const HrtfSet hrtf = make_hrtf();
  const HeadphoneSlotSet slots = headphone_slots(InputLayout::Stereo);
  BinauralRenderer unprepared;
  CHECK(rejects_invalid_parameter(
      [&] { unprepared.prepare(kSampleRate, 0, hrtf, slots, RoomPreset::None); }));

#if defined(SONARE_WITH_MIXING)
  REQUIRE(allocation_guard_blocks_large_audio_copy());
  CHECK(invalid_block_preserves_output(INT_MAX, hrtf, slots));
  CHECK(derived_block_limit_rejects(hrtf, slots));
#endif
}

#if defined(SONARE_WITH_MIXING)
TEST_CASE("Binaural allocation guard blocks a large copy through a public API",
          "[playback][binaural][validation][binaural_prepare_domain]") {
  REQUIRE(allocation_guard_blocks_large_audio_copy());
}

TEST_CASE("Binaural prepare rejects ITD values before integer conversion or history allocation",
          "[playback][binaural][validation][binaural_prepare_domain]") {
  const HeadphoneSlotSet slots = headphone_slots(InputLayout::Stereo);
  REQUIRE(allocation_guard_blocks_large_audio_copy());
  CHECK(oversized_itd_rejects(std::numeric_limits<float>::max(), slots));
  CHECK(oversized_itd_rejects(1073741824.0f, slots));
}
#endif
