#include "playback/binaural.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <random>
#include <vector>

#include "playback/room_presets.h"
#include "playback/shrf_fixture.h"
#include "util/constants.h"

using namespace sonare::playback;
using namespace sonare::playback::test;
using sonare::constants::kPi;
using sonare::constants::kTwoPi;

namespace {

constexpr double kRate = 48000.0;
constexpr float kDegToRad = kPi / 180.0f;
constexpr float kRadToDeg = 180.0f / kPi;

/// Lag (right relative to left, in samples) of the cross-correlation peak,
/// refined by a parabola through the peak and its neighbours.
double xcorr_peak_lag(const std::vector<float>& left, const std::vector<float>& right,
                      int max_lag) {
  const int n = static_cast<int>(left.size());
  std::vector<double> c(static_cast<size_t>(2 * max_lag + 1), 0.0);
  for (int lag = -max_lag; lag <= max_lag; ++lag) {
    double acc = 0.0;
    for (int i = max_lag; i < n - max_lag; ++i) {
      acc +=
          static_cast<double>(left[static_cast<size_t>(i)]) * right[static_cast<size_t>(i + lag)];
    }
    c[static_cast<size_t>(lag + max_lag)] = acc;
  }
  int best = 1;
  for (int k = 1; k < 2 * max_lag; ++k) {
    if (c[static_cast<size_t>(k)] > c[static_cast<size_t>(best)]) best = k;
  }
  const double a = c[static_cast<size_t>(best - 1)];
  const double b = c[static_cast<size_t>(best)];
  const double d = c[static_cast<size_t>(best + 1)];
  const double denom = a - 2.0 * b + d;
  const double frac = denom != 0.0 ? 0.5 * (a - d) / denom : 0.0;
  return static_cast<double>(best - max_lag) + frac;
}

/// Positive when the left ear lags (left[i] ~ right[i - itd]).
double measured_itd(const std::vector<float>& left, const std::vector<float>& right) {
  return -xcorr_peak_lag(left, right, 40);
}

/// Table ITD at an arbitrary azimuth of one fixture row: linear between the two
/// neighbouring grid columns, wrapping around.
double row_itd(const ShrfFixtureSpec& spec, int el_index, double azimuth_deg) {
  double wrapped = std::fmod(azimuth_deg, 360.0);
  if (wrapped < 0.0) wrapped += 360.0;
  const double units = wrapped / spec.az_step_deg;
  const int low = static_cast<int>(std::floor(units)) % spec.n_az;
  const int high = (low + 1) % spec.n_az;
  const double frac = units - std::floor(units);
  return (1.0 - frac) * fixture_itd_samples(spec, el_index, low) +
         frac * fixture_itd_samples(spec, el_index, high);
}

HrtfSet make_set(const std::vector<uint8_t>& bytes) {
  return HrtfSet::from_memory(bytes.data(), bytes.size());
}

/// Fixture with an interaural level difference: tap 0 is sqrt((1 -/+ sin az)/2)
/// on the left/right ear instead of 1, so energy reveals the direction.
std::vector<uint8_t> make_ild_fixture(const ShrfFixtureSpec& spec) {
  std::vector<uint8_t> bytes = make_shrf_fixture(spec);
  const size_t hrir_base =
      kShrfHeaderBytes + static_cast<size_t>(spec.n_el * spec.n_az) * sizeof(float);
  for (int el = 0; el < spec.n_el; ++el) {
    for (int az = 0; az < spec.n_az; ++az) {
      const float s = std::sin(static_cast<float>(az) * spec.az_step_deg * kDegToRad);
      const float gains[2] = {std::sqrt(0.5f * (1.0f - s)), std::sqrt(0.5f * (1.0f + s))};
      for (int ear = 0; ear < 2; ++ear) {
        const size_t direction = static_cast<size_t>(el * spec.n_az + az);
        const size_t offset =
            hrir_base + (direction * 2 + static_cast<size_t>(ear)) * spec.taps * sizeof(float);
        std::memcpy(&bytes[offset], &gains[ear], sizeof(float));
      }
    }
  }
  return bytes;
}

/// Low-passed Gaussian noise: the broad autocorrelation peak keeps the
/// parabolic lag estimate unbiased.
std::vector<float> lowpass_noise(int frames, unsigned seed) {
  std::mt19937 rng(seed);
  std::normal_distribution<float> dist(0.0f, 0.1f);
  std::vector<float> out(static_cast<size_t>(frames));
  float s1 = 0.0f, s2 = 0.0f;
  for (auto& v : out) {
    s1 = 0.6f * s1 + 0.4f * dist(rng);
    s2 = 0.6f * s2 + 0.4f * s1;
    v = s2;
  }
  return out;
}

/// Drives a renderer block by block. @p fill writes the bus planes of the block
/// starting at @p start; @p before_block runs ahead of each process() call.
struct Run {
  std::vector<float> left;
  std::vector<float> right;
};

Run render(BinauralRenderer& renderer, const HeadphoneSlotSet& slots, int frames, int block,
           const std::function<void(std::vector<std::vector<float>>&, int, int)>& fill,
           const std::function<void(int)>& before_block = {}) {
  const int planes = slots.count + 2;
  std::vector<std::vector<float>> bus(static_cast<size_t>(planes), std::vector<float>(block));
  std::vector<const float*> ptrs;
  for (auto& p : bus) ptrs.push_back(p.data());
  Run run{std::vector<float>(static_cast<size_t>(frames)),
          std::vector<float>(static_cast<size_t>(frames))};
  for (int start = 0, index = 0; start < frames; start += block, ++index) {
    const int n = std::min(block, frames - start);
    for (auto& p : bus) std::fill(p.begin(), p.end(), 0.0f);
    fill(bus, start, n);
    if (before_block) before_block(index);
    renderer.process(ptrs.data(), run.left.data() + start, run.right.data() + start, n);
  }
  return run;
}

/// ITD measured with low-passed noise on bus plane @p plane only.
double plane_itd(BinauralRenderer& renderer, const HeadphoneSlotSet& slots, int plane) {
  constexpr int kFrames = 8192;
  const std::vector<float> noise = lowpass_noise(kFrames, 3u);
  renderer.reset();
  const Run run = render(
      renderer, slots, kFrames, 256, [&](std::vector<std::vector<float>>& bus, int start, int n) {
        std::copy_n(noise.begin() + start, n, bus[static_cast<size_t>(plane)].begin());
      });
  return measured_itd(run.left, run.right);
}

int plane_of(const HeadphoneSlotSet& slots, VirtualSlot slot) {
  for (int i = 0; i < slots.count; ++i) {
    if (slots.slots[i] == slot) return i;
  }
  return -1;
}

float max_step(const std::vector<float>& x, int from) {
  float m = 0.0f;
  for (size_t i = static_cast<size_t>(from) + 1; i < x.size(); ++i) {
    m = std::max(m, std::fabs(x[i] - x[i - 1]));
  }
  return m;
}

/// First sample above -40 dB of the peak.
int onset(const std::vector<float>& x) {
  float peak = 0.0f;
  for (float v : x) peak = std::max(peak, std::fabs(v));
  for (size_t i = 0; i < x.size(); ++i) {
    if (std::fabs(x[i]) >= 0.01f * peak) return static_cast<int>(i);
  }
  return -1;
}

std::vector<float> sine(int frames, float hz) {
  std::vector<float> out(static_cast<size_t>(frames));
  for (int i = 0; i < frames; ++i) {
    out[static_cast<size_t>(i)] =
        0.5f * std::sin(kTwoPi * hz * static_cast<float>(i) / static_cast<float>(kRate));
  }
  return out;
}

}  // namespace

TEST_CASE("binaural ITD of a grid-point speaker matches the table", "[playback][binaural]") {
  const ShrfFixtureSpec spec;
  const HrtfSet set = make_set(make_shrf_fixture(spec));
  const HeadphoneSlotSet slots = headphone_slots(InputLayout::SevenPointOne);
  BinauralRenderer renderer;
  renderer.prepare(kRate, 256, set, slots, RoomPreset::None);

  // Slot -> grid column of the 45-degree fixture.
  const struct {
    VirtualSlot slot;
    int az_index;
  } cases[] = {{VirtualSlot::C, 0},
               {VirtualSlot::Lss90, 6},
               {VirtualSlot::Rss90, 2},
               {VirtualSlot::Ls135, 5},
               {VirtualSlot::Rs135, 3}};
  for (const auto& c : cases) {
    const double expected = fixture_itd_samples(spec, 0, c.az_index);
    const double itd = plane_itd(renderer, slots, plane_of(slots, c.slot));
    CAPTURE(static_cast<int>(c.slot), expected, itd);
    CHECK(std::abs(itd - expected) <= 0.1);
  }
}

TEST_CASE("binaural ITD between grid points is the linear interpolation, 9-slot auto set",
          "[playback][binaural]") {
  const ShrfFixtureSpec spec;
  const HrtfSet set = make_set(make_shrf_fixture(spec));
  const HeadphoneSlotSet slots = headphone_slots(InputLayout::Auto);
  REQUIRE(slots.count == kVirtualSlotCount);
  BinauralRenderer renderer;
  renderer.prepare(kRate, 256, set, slots, RoomPreset::None);

  for (int plane = 0; plane < slots.count; ++plane) {
    const SpeakerDirection d = virtual_slot_direction(slots.slots[plane]);
    const double expected = row_itd(spec, 0, d.azimuth_deg);
    const double itd = plane_itd(renderer, slots, plane);
    CAPTURE(plane, d.azimuth_deg, expected, itd);
    CHECK(std::abs(itd - expected) <= 0.1);
  }
}

TEST_CASE("binaural head yaw brings a speaker to the frontal ITD", "[playback][binaural]") {
  const ShrfFixtureSpec spec;
  const HrtfSet set = make_set(make_shrf_fixture(spec));
  const HeadphoneSlotSet slots = headphone_slots(InputLayout::SevenPointOne);
  BinauralRenderer renderer;
  renderer.prepare(kRate, 256, set, slots, RoomPreset::None);
  const double frontal = fixture_itd_samples(spec, 0, 0);

  for (VirtualSlot slot : {VirtualSlot::Rss90, VirtualSlot::R, VirtualSlot::Ls135}) {
    const float azimuth = virtual_slot_direction(slot).azimuth_deg;
    renderer.set_head_pose({azimuth, 0.0f, 0.0f});
    const double itd = plane_itd(renderer, slots, plane_of(slots, slot));
    CAPTURE(azimuth, itd);
    CHECK(std::abs(itd - frontal) <= 0.1);
  }

  // Tracking off ignores the pose.
  renderer.set_head_pose({90.0f, 0.0f, 0.0f});
  renderer.set_params({true, -6.0f, false});
  const double itd = plane_itd(renderer, slots, plane_of(slots, VirtualSlot::Rss90));
  CHECK(std::abs(itd - fixture_itd_samples(spec, 0, 2)) <= 0.1);
}

TEST_CASE("binaural head-relative direction follows the yaw-pitch-roll convention",
          "[playback][binaural]") {
  auto near = [](SpeakerDirection d, float az, float el) {
    return std::abs(d.azimuth_deg - az) < 1e-3f && std::abs(d.elevation_deg - el) < 1e-3f;
  };
  CHECK(near(head_relative_direction({90.0f, 0.0f}, {90.0f, 0.0f, 0.0f}), 0.0f, 0.0f));
  CHECK(near(head_relative_direction({0.0f, 0.0f}, {-30.0f, 0.0f, 0.0f}), 30.0f, 0.0f));
  // Looking up lowers a frontal source; rolling the right ear down raises a right source.
  CHECK(near(head_relative_direction({0.0f, 0.0f}, {0.0f, 30.0f, 0.0f}), 0.0f, -30.0f));
  CHECK(near(head_relative_direction({90.0f, 0.0f}, {0.0f, 0.0f, 20.0f}), 90.0f, 20.0f));
  // Intrinsic order: yaw first, then pitch about the turned right axis.
  CHECK(near(head_relative_direction({90.0f, 0.0f}, {90.0f, 30.0f, 0.0f}), 0.0f, -30.0f));
}

TEST_CASE("binaural yaw changing every block does not click", "[playback][binaural]") {
  const ShrfFixtureSpec spec;
  const HrtfSet set = make_set(make_shrf_fixture(spec));
  const HeadphoneSlotSet slots = headphone_slots(InputLayout::FivePointOne);
  constexpr int kBlock = 128;
  constexpr int kFrames = 64 * kBlock;
  const std::vector<float> tone = sine(kFrames, 1000.0f);
  const int plane = plane_of(slots, VirtualSlot::R);
  auto fill = [&](std::vector<std::vector<float>>& bus, int start, int n) {
    std::copy_n(tone.begin() + start, n, bus[static_cast<size_t>(plane)].begin());
  };

  BinauralRenderer renderer;
  renderer.prepare(kRate, kBlock, set, slots, RoomPreset::None);
  const Run steady = render(renderer, slots, kFrames, kBlock, fill);

  renderer.reset();
  const Run moving = render(renderer, slots, kFrames, kBlock, fill, [&](int index) {
    renderer.set_head_pose({3.0f * static_cast<float>(index), 0.0f, 0.0f});
  });

  // The yaw sweeps 0..189 degrees, so the source crosses to the other ear.
  for (int ear = 0; ear < 2; ++ear) {
    const float reference = max_step(ear == 0 ? steady.left : steady.right, 0);
    const float step = max_step(ear == 0 ? moving.left : moving.right, 0);
    CAPTURE(ear, reference, step, step / reference);
    CHECK(reference > 0.0f);
    CHECK(step <= 1.5f * reference);
  }
}

TEST_CASE("binaural pitch beyond the grid clamps the elevation and does not click",
          "[playback][binaural]") {
  ShrfFixtureSpec spec;
  spec.n_el = 5;
  spec.el_min_deg = -30.0f;
  spec.el_step_deg = 15.0f;
  const HrtfSet set = make_set(make_shrf_fixture(spec));
  const HeadphoneSlotSet slots = headphone_slots(InputLayout::FivePointOne);
  const int plane = plane_of(slots, VirtualSlot::L);
  BinauralRenderer renderer;
  renderer.prepare(kRate, 128, set, slots, RoomPreset::None);

  // L (-30, 0) under pitch +45: front 0.612, right -0.5, up -0.612, so the
  // elevation (-37.8) lies below the lowest row and clamps to it.
  const float cos45 = std::cos(45.0f * kDegToRad);
  const float cos30 = std::cos(30.0f * kDegToRad);
  const float sin30 = std::sin(30.0f * kDegToRad);
  const double head_az = std::atan2(-sin30, cos45 * cos30) * kRadToDeg;
  const double head_el = std::asin(-cos45 * cos30) * kRadToDeg;
  REQUIRE(head_el < spec.el_min_deg);
  renderer.set_head_pose({0.0f, 45.0f, 0.0f});
  const double itd = plane_itd(renderer, slots, plane);
  const double clamped = row_itd(spec, 0, head_az);
  CAPTURE(head_az, head_el, clamped, itd);
  CHECK(std::abs(itd - clamped) <= 0.1);

  constexpr int kBlock = 128;
  constexpr int kFrames = 48 * kBlock;
  const std::vector<float> tone = sine(kFrames, 1000.0f);
  auto fill = [&](std::vector<std::vector<float>>& bus, int start, int n) {
    std::copy_n(tone.begin() + start, n, bus[static_cast<size_t>(plane)].begin());
  };
  renderer.reset();
  const Run steady = render(renderer, slots, kFrames, kBlock, fill);
  renderer.reset();
  // Yaw sweeps under the off-grid pitch, so the elevation moves across the grid edge.
  const Run moving = render(renderer, slots, kFrames, kBlock, fill, [&](int index) {
    renderer.set_head_pose({-3.0f * static_cast<float>(index), 45.0f, 0.0f});
  });
  for (int ear = 0; ear < 2; ++ear) {
    const float reference = max_step(ear == 0 ? steady.left : steady.right, 0);
    const float step = max_step(ear == 0 ? moving.left : moving.right, 0);
    CAPTURE(ear, reference, step, step / reference);
    CHECK(step <= 1.5f * reference);
  }
}

TEST_CASE("binaural room keeps the direct sound in place with zero latency",
          "[playback][binaural]") {
  const ShrfFixtureSpec spec;
  const HrtfSet set = make_set(make_shrf_fixture(spec));
  const HeadphoneSlotSet slots = headphone_slots(InputLayout::FivePointOne);
  constexpr int kFrames = 4096;
  constexpr int kImpulseAt = 1000;

  auto impulse_on = [&](VirtualSlot slot) {
    const int plane = plane_of(slots, slot);
    return [plane](std::vector<std::vector<float>>& bus, int start, int n) {
      if (kImpulseAt >= start && kImpulseAt < start + n) {
        bus[static_cast<size_t>(plane)][static_cast<size_t>(kImpulseAt - start)] = 1.0f;
      }
    };
  };

  BinauralRenderer dry;
  dry.prepare(kRate, 100, set, slots, RoomPreset::None);
  BinauralRenderer room;
  room.prepare(kRate, 100, set, slots, RoomPreset::LivingRoom);
  CHECK(room.latency_samples() == 0);

  // Odd block length so the late FIFO runs out of phase with the blocks.
  const Run dry_c = render(dry, slots, kFrames, 100, impulse_on(VirtualSlot::C));
  const Run room_c = render(room, slots, kFrames, 100, impulse_on(VirtualSlot::C));
  CHECK(onset(dry_c.left) == kImpulseAt);
  CHECK(onset(room_c.left) == onset(dry_c.left));
  CHECK(onset(room_c.right) == onset(dry_c.right));
  // The room did add something after the direct sound.
  double tail = 0.0;
  for (int i = kImpulseAt + 1; i < kFrames; ++i) {
    tail += std::fabs(room_c.left[static_cast<size_t>(i)]);
  }
  CHECK(tail > 0.0);

  // R at +30: the right ear is the near ear and carries no extra delay.
  room.reset();
  const Run room_r = render(room, slots, kFrames, 100, impulse_on(VirtualSlot::R));
  CHECK(onset(room_r.right) == kImpulseAt);
  CHECK(onset(room_r.left) > kImpulseAt);
}

TEST_CASE("binaural reflection ring turns with the head", "[playback][binaural]") {
  const ShrfFixtureSpec spec;
  const HrtfSet set = make_set(make_ild_fixture(spec));
  const HeadphoneSlotSet slots = headphone_slots(InputLayout::FivePointOne);
  constexpr int kBlock = 128;
  constexpr int kFrames = 2048;
  constexpr int kImpulseAt = 256;
  // Late reverberation starts at the mixing time sqrt(V) ms; stay before it.
  RoomPresetSpec room_spec;
  REQUIRE(room_preset_spec(RoomPreset::LivingRoom, &room_spec));
  const double volume =
      static_cast<double>(room_spec.dims.length) * room_spec.dims.width * room_spec.dims.height;
  const int window = static_cast<int>(std::sqrt(volume) * 1e-3 * kRate) - 8;

  const int plane = plane_of(slots, VirtualSlot::C);
  auto fill = [&](std::vector<std::vector<float>>& bus, int start, int n) {
    if (kImpulseAt >= start && kImpulseAt < start + n) {
      bus[static_cast<size_t>(plane)][static_cast<size_t>(kImpulseAt - start)] = 1.0f;
    }
  };
  BinauralRenderer dry;
  dry.prepare(kRate, kBlock, set, slots, RoomPreset::None);
  BinauralRenderer room;
  room.prepare(kRate, kBlock, set, slots, RoomPreset::LivingRoom);

  // Left-over-right energy of the room's contribution, in dB.
  auto balance_db = [&](float yaw) {
    dry.set_head_pose({yaw, 0.0f, 0.0f});
    room.set_head_pose({yaw, 0.0f, 0.0f});
    dry.reset();
    room.reset();
    const Run a = render(dry, slots, kFrames, kBlock, fill);
    const Run b = render(room, slots, kFrames, kBlock, fill);
    double el = 0.0, er = 0.0;
    for (int i = kImpulseAt; i < kImpulseAt + window; ++i) {
      const double l =
          static_cast<double>(b.left[static_cast<size_t>(i)]) - a.left[static_cast<size_t>(i)];
      const double r =
          static_cast<double>(b.right[static_cast<size_t>(i)]) - a.right[static_cast<size_t>(i)];
      el += l * l;
      er += r * r;
    }
    REQUIRE(el + er > 0.0);
    return 10.0 * std::log10(el / er);
  };

  // The room is mirror-symmetric about the listener and C sits on that plane.
  const double straight = balance_db(0.0f);
  // Facing right puts the front wall, floor and ceiling images on the left ear.
  const double right_turn = balance_db(90.0f);
  const double left_turn = balance_db(-90.0f);
  CAPTURE(straight, right_turn, left_turn);
  CHECK(std::abs(straight) < 0.1);
  CHECK(right_turn > 3.0);
  CHECK(left_turn < -3.0);
}

TEST_CASE("binaural direct-ear planes pass unfiltered", "[playback][binaural]") {
  const ShrfFixtureSpec spec;
  const HrtfSet set = make_set(make_shrf_fixture(spec));
  const HeadphoneSlotSet slots = headphone_slots(InputLayout::Auto);
  BinauralRenderer renderer;
  renderer.prepare(kRate, 128, set, slots, RoomPreset::LivingRoom);
  renderer.set_head_pose({40.0f, 10.0f, -5.0f});

  constexpr int kFrames = 1024;
  const std::vector<float> a = lowpass_noise(kFrames, 7u);
  const std::vector<float> b = lowpass_noise(kFrames, 8u);
  const Run run = render(renderer, slots, kFrames, 128,
                         [&](std::vector<std::vector<float>>& bus, int start, int n) {
                           std::copy_n(a.begin() + start, n, bus[9].begin());
                           std::copy_n(b.begin() + start, n, bus[10].begin());
                         });
  CHECK(run.left == a);
  CHECK(run.right == b);
}

TEST_CASE("binaural room presets place the listener and speakers inside the room",
          "[playback][binaural]") {
  RoomPresetSpec spec;
  CHECK_FALSE(room_preset_spec(RoomPreset::None, &spec));
  REQUIRE(room_preset_spec(RoomPreset::LivingRoom, &spec));
  CHECK(spec.dims.length == 5.0f);
  CHECK(spec.dims.width == 4.0f);
  CHECK(spec.dims.height == 2.6f);
  CHECK(spec.absorption == 0.35f);
  CHECK(spec.speaker_distance_m == 2.0f);
  const auto listener = listener_position(spec);
  CHECK(std::abs(listener.x - (2.5f - 5.0f / 6.0f)) < 1e-5f);
  CHECK(listener.y == 2.0f);
  CHECK(listener.z == kListenerEarHeightM);
  const auto front = virtual_speaker_position(spec, {0.0f, 0.0f});
  CHECK(std::abs(front.x - (listener.x + 2.0f)) < 1e-5f);
  CHECK(std::abs(front.y - listener.y) < 1e-5f);

  REQUIRE(room_preset_spec(RoomPreset::ScreeningRoom, &spec));
  CHECK(spec.speaker_distance_m == 4.0f);
  for (int slot = 0; slot < kVirtualSlotCount; ++slot) {
    const auto p =
        virtual_speaker_position(spec, virtual_slot_direction(static_cast<VirtualSlot>(slot)));
    CHECK(p.x > 0.0f);
    CHECK(p.x < spec.dims.length);
    CHECK(p.y > 0.0f);
    CHECK(p.y < spec.dims.width);
  }
}
