#include "playback/hrtf_set.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "playback/shrf_fixture.h"
#include "util/exception.h"

using namespace sonare::playback;
using namespace sonare::playback::test;

namespace {

/// Runs @p action and reports the `SonareException` it throws, if any.
struct ThrowResult {
  bool threw = false;
  sonare::ErrorCode code = sonare::ErrorCode::Ok;
  std::string message;
};

ThrowResult run(const std::function<void()>& action) {
  ThrowResult result;
  try {
    action();
  } catch (const sonare::SonareException& e) {
    result.threw = true;
    result.code = e.code();
    result.message = e.what();
  }
  return result;
}

void check_invalid(const std::function<void()>& action, const char* violation_substring) {
  const ThrowResult result = run(action);
  CHECK(result.threw);
  CHECK(result.code == sonare::ErrorCode::InvalidParameter);
  CHECK(result.message.find(violation_substring) != std::string::npos);
}

/// Overwrites 4 bytes at @p offset with @p value's little-endian float32 bits.
void poke_f32(std::vector<uint8_t>& bytes, size_t offset, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  for (int i = 0; i < 4; ++i)
    bytes[offset + static_cast<size_t>(i)] = static_cast<uint8_t>(bits >> (8 * i));
}

std::unique_ptr<HrtfSet> load(const std::vector<uint8_t>& bytes) {
  return std::make_unique<HrtfSet>(HrtfSet::from_memory(bytes.data(), bytes.size()));
}

}  // namespace

TEST_CASE("HRTF set parses the synthetic SHRF fixture", "[playback][hrtf]") {
  const ShrfFixtureSpec spec;
  const std::vector<uint8_t> bytes = make_shrf_fixture(spec);
  std::unique_ptr<HrtfSet> set;
  REQUIRE_NOTHROW(set = load(bytes));
  CHECK(set->sample_rate() == spec.sample_rate);
  CHECK(set->taps() == spec.taps);
  CHECK(set->azimuth_count() == spec.n_az);
  CHECK(set->elevation_count() == spec.n_el);
  CHECK(set->azimuth_step_deg() == spec.az_step_deg);
  CHECK(set->elevation_min_deg() == spec.el_min_deg);
  CHECK(set->elevation_step_deg() == spec.el_step_deg);
  for (int az = 0; az < spec.n_az; ++az) {
    CHECK(std::abs(set->itd_samples(0, az) - fixture_itd_samples(spec, 0, az)) < 1e-6f);
    CHECK(set->hrir(0, az, 0)[0] == 1.0f);
    CHECK(set->hrir(0, az, 1)[0] == 1.0f);
    CHECK(set->hrir(0, az, 0)[1] == 0.0f);
  }
}

TEST_CASE("int16 and float32 fixtures of the same spec produce the same HRTF set",
          "[playback][hrtf]") {
  ShrfFixtureSpec float_spec;
  ShrfFixtureSpec int_spec;
  int_spec.quant = ShrfQuant::Int16;

  const std::unique_ptr<HrtfSet> float_set = load(make_shrf_fixture(float_spec));
  const std::unique_ptr<HrtfSet> int_set = load(make_shrf_fixture(int_spec));

  REQUIRE(int_set->azimuth_count() == float_set->azimuth_count());
  for (int az = 0; az < float_spec.n_az; ++az) {
    CHECK(std::abs(int_set->itd_samples(0, az) - float_set->itd_samples(0, az)) < 1e-6f);
    for (int tap = 0; tap < float_spec.taps; ++tap) {
      CHECK(std::abs(int_set->hrir(0, az, 0)[tap] - float_set->hrir(0, az, 0)[tap]) < 1e-4f);
      CHECK(std::abs(int_set->hrir(0, az, 1)[tap] - float_set->hrir(0, az, 1)[tap]) < 1e-4f);
    }
  }
}

TEST_CASE("SHRF validation rejects a magic mismatch", "[playback][hrtf]") {
  std::vector<uint8_t> bytes = make_shrf_fixture();
  bytes[0] = 'X';
  check_invalid([&] { load(bytes); }, "magic");
}

TEST_CASE("SHRF validation rejects a version mismatch", "[playback][hrtf]") {
  std::vector<uint8_t> bytes = make_shrf_fixture();
  bytes[4] = 2;
  bytes[5] = 0;
  check_invalid([&] { load(bytes); }, "version");
}

TEST_CASE("SHRF validation rejects an azimuth span that does not sum to 360 degrees",
          "[playback][hrtf]") {
  ShrfFixtureSpec spec;
  spec.n_az = 8;
  spec.az_step_deg = 40.0f;  // 8 * 40 = 320, not 360.
  check_invalid([&] { load(make_shrf_fixture(spec)); }, "360 degrees");
}

TEST_CASE("SHRF validation rejects n_az below the minimum", "[playback][hrtf]") {
  ShrfFixtureSpec spec;
  spec.n_az = 3;
  check_invalid([&] { load(make_shrf_fixture(spec)); }, "n_az");
}

TEST_CASE("SHRF validation rejects n_el below the minimum", "[playback][hrtf]") {
  ShrfFixtureSpec spec;
  spec.n_el = 0;
  check_invalid([&] { load(make_shrf_fixture(spec)); }, "n_el");
}

TEST_CASE("SHRF validation rejects taps outside [16, 1024]", "[playback][hrtf]") {
  ShrfFixtureSpec too_few;
  too_few.taps = 8;
  check_invalid([&] { load(make_shrf_fixture(too_few)); }, "taps");

  ShrfFixtureSpec too_many;
  too_many.taps = 2000;
  check_invalid([&] { load(make_shrf_fixture(too_many)); }, "taps");
}

TEST_CASE("SHRF validation rejects sample_rate outside [8000, 384000]", "[playback][hrtf]") {
  ShrfFixtureSpec too_low;
  too_low.sample_rate = 4000;
  check_invalid([&] { load(make_shrf_fixture(too_low)); }, "sample_rate");

  ShrfFixtureSpec too_high;
  too_high.sample_rate = 400000;
  check_invalid([&] { load(make_shrf_fixture(too_high)); }, "sample_rate");
}

TEST_CASE("SHRF validation rejects a non-finite header value", "[playback][hrtf]") {
  ShrfFixtureSpec spec;
  spec.az_step_deg = std::numeric_limits<float>::quiet_NaN();
  check_invalid([&] { load(make_shrf_fixture(spec)); }, "finite");
}

TEST_CASE("SHRF validation rejects a non-finite ITD or HRIR sample", "[playback][hrtf]") {
  std::vector<uint8_t> bad_itd = make_shrf_fixture();
  poke_f32(bad_itd, kShrfHeaderBytes, std::numeric_limits<float>::infinity());
  check_invalid([&] { load(bad_itd); }, "finite");

  const ShrfFixtureSpec spec;
  const size_t hrir_offset =
      kShrfHeaderBytes + static_cast<size_t>(spec.n_el) * static_cast<size_t>(spec.n_az) * 4;
  std::vector<uint8_t> bad_hrir = make_shrf_fixture(spec);
  poke_f32(bad_hrir, hrir_offset, std::numeric_limits<float>::quiet_NaN());
  check_invalid([&] { load(bad_hrir); }, "finite");
}

TEST_CASE("SHRF validation rejects a non-positive scale", "[playback][hrtf]") {
  ShrfFixtureSpec spec;
  spec.quant = ShrfQuant::Int16;
  std::vector<uint8_t> bytes = make_shrf_fixture(spec);
  poke_f32(bytes, 32, 0.0f);
  check_invalid([&] { load(bytes); }, "scale");
}

TEST_CASE("SHRF validation rejects a size mismatch", "[playback][hrtf]") {
  std::vector<uint8_t> bytes = make_shrf_fixture();
  bytes.pop_back();
  check_invalid([&] { load(bytes); }, "size");
}

TEST_CASE("HRTF interpolation reproduces the grid exactly at grid points", "[playback][hrtf]") {
  const ShrfFixtureSpec spec;
  const std::unique_ptr<HrtfSet> set = load(make_shrf_fixture(spec));
  std::vector<float> left(static_cast<size_t>(spec.taps));
  std::vector<float> right(static_cast<size_t>(spec.taps));
  float itd = 0.0f;

  for (int az = 0; az < spec.n_az; ++az) {
    const float az_deg = static_cast<float>(az) * spec.az_step_deg;
    set->interpolate(az_deg, spec.el_min_deg, left.data(), right.data(), &itd);
    CHECK(std::abs(itd - fixture_itd_samples(spec, 0, az)) < 1e-4f);
    CHECK(left[0] == 1.0f);
    CHECK(right[0] == 1.0f);
  }
}

TEST_CASE("HRTF interpolation blends linearly at an azimuth midpoint", "[playback][hrtf]") {
  const ShrfFixtureSpec spec;
  const std::unique_ptr<HrtfSet> set = load(make_shrf_fixture(spec));
  std::vector<float> left(static_cast<size_t>(spec.taps));
  std::vector<float> right(static_cast<size_t>(spec.taps));
  float itd = 0.0f;

  // Midway between grid azimuths 0 and 45 degrees.
  set->interpolate(22.5f, spec.el_min_deg, left.data(), right.data(), &itd);
  const float expected =
      0.5f * fixture_itd_samples(spec, 0, 0) + 0.5f * fixture_itd_samples(spec, 0, 1);
  CHECK(std::abs(itd - expected) < 1e-4f);
}

TEST_CASE("HRTF interpolation wraps azimuth across the 360-degree seam", "[playback][hrtf]") {
  const ShrfFixtureSpec spec;
  const std::unique_ptr<HrtfSet> set = load(make_shrf_fixture(spec));
  std::vector<float> left(static_cast<size_t>(spec.taps));
  std::vector<float> right(static_cast<size_t>(spec.taps));
  float itd_positive = 0.0f;
  float itd_negative = 0.0f;

  // Midway between grid azimuths 315 (index 7) and 0 (index 0, wrapped).
  set->interpolate(337.5f, spec.el_min_deg, left.data(), right.data(), &itd_positive);
  set->interpolate(-22.5f, spec.el_min_deg, left.data(), right.data(), &itd_negative);
  const float expected =
      0.5f * fixture_itd_samples(spec, 0, 7) + 0.5f * fixture_itd_samples(spec, 0, 0);
  CHECK(std::abs(itd_positive - expected) < 1e-4f);
  CHECK(std::abs(itd_negative - expected) < 1e-4f);
}

TEST_CASE("HRTF interpolation clamps elevation to the nearest row", "[playback][hrtf]") {
  ShrfFixtureSpec spec;
  spec.n_el = 5;
  spec.el_min_deg = 0.0f;
  spec.el_step_deg = 15.0f;  // Rows at 0, 15, 30, 45, 60 degrees.
  const std::unique_ptr<HrtfSet> set = load(make_shrf_fixture(spec));
  std::vector<float> left(static_cast<size_t>(spec.taps));
  std::vector<float> right(static_cast<size_t>(spec.taps));
  float itd = 0.0f;

  const float az_deg = 90.0f;  // Grid index 2, no azimuth interpolation.
  set->interpolate(az_deg, -10.0f, left.data(), right.data(), &itd);
  CHECK(std::abs(itd - fixture_itd_samples(spec, 0, 2)) < 1e-4f);

  set->interpolate(az_deg, 75.0f, left.data(), right.data(), &itd);
  CHECK(std::abs(itd - fixture_itd_samples(spec, spec.n_el - 1, 2)) < 1e-4f);
}

TEST_CASE("resampling to the same rate is a no-op", "[playback][hrtf]") {
  const ShrfFixtureSpec spec;
  const std::unique_ptr<HrtfSet> set = load(make_shrf_fixture(spec));
  const HrtfSet same = set->resampled(spec.sample_rate);
  CHECK(same.sample_rate() == spec.sample_rate);
  CHECK(same.taps() == spec.taps);
  for (int az = 0; az < spec.n_az; ++az) {
    CHECK(same.itd_samples(0, az) == set->itd_samples(0, az));
  }
}

TEST_CASE("resampling scales the ITD table by the sample rate ratio", "[playback][hrtf]") {
  const ShrfFixtureSpec spec;
  const std::unique_ptr<HrtfSet> set = load(make_shrf_fixture(spec));
  const HrtfSet half = set->resampled(spec.sample_rate / 2);

  CHECK(half.sample_rate() == spec.sample_rate / 2);
  CHECK(half.taps() > 0);
  CHECK(half.taps() < spec.taps);
  for (int az = 0; az < spec.n_az; ++az) {
    const float expected = set->itd_samples(0, az) * 0.5f;
    CHECK(std::abs(half.itd_samples(0, az) - expected) < 1e-3f);
  }
}
