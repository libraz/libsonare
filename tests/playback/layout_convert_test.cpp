#include "playback/layout_convert.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <random>
#include <vector>

#include "mixing/downmix.h"
#include "util/constants.h"
#include "util/exception.h"

using sonare::ChannelLayout;
using sonare::SpeakerRole;
using namespace sonare::playback;

namespace {

constexpr double kRate = 48000.0;
constexpr int kFrames = 4096;

std::vector<std::vector<float>> noise_planes(int channels, int silent_plane, uint32_t seed) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> dist(-0.1f, 0.1f);
  std::vector<std::vector<float>> planes(static_cast<size_t>(channels),
                                         std::vector<float>(kFrames, 0.0f));
  for (int ch = 0; ch < channels; ++ch) {
    if (ch == silent_plane) continue;
    for (float& s : planes[static_cast<size_t>(ch)]) s = dist(rng);
  }
  return planes;
}

std::vector<float> sine_plane(float freq_hz, float amplitude) {
  std::vector<float> plane(kFrames);
  for (int i = 0; i < kFrames; ++i) {
    plane[static_cast<size_t>(i)] =
        amplitude * std::sin(sonare::constants::kTwoPi * freq_hz * static_cast<float>(i) /
                             static_cast<float>(kRate));
  }
  return plane;
}

template <typename Planes>
std::vector<const float*> const_ptrs(const Planes& planes) {
  std::vector<const float*> out;
  for (const auto& p : planes) out.push_back(p.data());
  return out;
}

template <typename Planes>
std::vector<float*> ptrs(Planes& planes) {
  std::vector<float*> out;
  for (auto& p : planes) out.push_back(p.data());
  return out;
}

double energy(const std::vector<float>& plane) {
  double e = 0.0;
  for (float s : plane) e += static_cast<double>(s) * s;
  return e;
}

double rms(const std::vector<float>& plane) { return std::sqrt(energy(plane) / plane.size()); }

}  // namespace

TEST_CASE("5.1 to stereo speakers matches mixing::downmix with LFE silent", "[playback][layout]") {
  auto in = noise_planes(6, 3, 11u);
  const auto in_ptrs = const_ptrs(in);

  std::vector<std::vector<float>> ref(2, std::vector<float>(kFrames, 0.0f));
  auto ref_ptrs = ptrs(ref);
  sonare::mixing::DownmixOptions options;
  options.include_lfe = false;
  sonare::mixing::downmix(ChannelLayout::FivePointOne, ChannelLayout::Stereo, in_ptrs.data(),
                          ref_ptrs.data(), kFrames, options);
  REQUIRE(energy(ref[0]) > 0.0);

  OutputBus bus;
  bus.kind = TargetKind::Speakers;
  bus.speaker_layout = ChannelLayout::Stereo;
  LayoutConverter converter;
  converter.prepare(kRate, kFrames, ChannelLayout::FivePointOne, bus);
  std::vector<std::vector<float>> out(2, std::vector<float>(kFrames, 0.0f));
  auto out_ptrs = ptrs(out);
  converter.process(in_ptrs.data(), out_ptrs.data(), kFrames);

  for (int ch = 0; ch < 2; ++ch) {
    float max_diff = 0.0f;
    for (int i = 0; i < kFrames; ++i) {
      max_diff = std::max(max_diff, std::abs(out[static_cast<size_t>(ch)][static_cast<size_t>(i)] -
                                             ref[static_cast<size_t>(ch)][static_cast<size_t>(i)]));
    }
    CHECK(max_diff == 0.0f);
  }
}

TEST_CASE("7.1 to stereo speakers matches mixing::downmix with LFE silent", "[playback][layout]") {
  auto in = noise_planes(8, 3, 13u);
  const auto in_ptrs = const_ptrs(in);

  std::vector<std::vector<float>> ref(2, std::vector<float>(kFrames, 0.0f));
  auto ref_ptrs = ptrs(ref);
  sonare::mixing::DownmixOptions options;
  options.include_lfe = false;
  sonare::mixing::downmix(ChannelLayout::SevenPointOne, ChannelLayout::Stereo, in_ptrs.data(),
                          ref_ptrs.data(), kFrames, options);
  REQUIRE(energy(ref[0]) > 0.0);

  OutputBus bus;
  bus.kind = TargetKind::Speakers;
  bus.speaker_layout = ChannelLayout::Stereo;
  LayoutConverter converter;
  converter.prepare(kRate, kFrames, ChannelLayout::SevenPointOne, bus);
  std::vector<std::vector<float>> out(2, std::vector<float>(kFrames, 0.0f));
  auto out_ptrs = ptrs(out);
  converter.process(in_ptrs.data(), out_ptrs.data(), kFrames);

  for (int ch = 0; ch < 2; ++ch) {
    float max_diff = 0.0f;
    for (int i = 0; i < kFrames; ++i) {
      max_diff = std::max(max_diff, std::abs(out[static_cast<size_t>(ch)][static_cast<size_t>(i)] -
                                             ref[static_cast<size_t>(ch)][static_cast<size_t>(i)]));
    }
    CHECK(max_diff == 0.0f);
  }
}

TEST_CASE("7.1 to 5.1 speakers matches mixing::downmix with LFE silent", "[playback][layout]") {
  auto in = noise_planes(8, 3, 17u);
  const auto in_ptrs = const_ptrs(in);

  std::vector<std::vector<float>> ref(6, std::vector<float>(kFrames, 0.0f));
  auto ref_ptrs = ptrs(ref);
  sonare::mixing::DownmixOptions options;
  options.include_lfe = false;
  sonare::mixing::downmix(ChannelLayout::SevenPointOne, ChannelLayout::FivePointOne, in_ptrs.data(),
                          ref_ptrs.data(), kFrames, options);
  REQUIRE(energy(ref[4]) > 0.0);

  OutputBus bus;
  bus.kind = TargetKind::Speakers;
  bus.speaker_layout = ChannelLayout::FivePointOne;
  LayoutConverter converter;
  converter.prepare(kRate, kFrames, ChannelLayout::SevenPointOne, bus);
  std::vector<std::vector<float>> out(6, std::vector<float>(kFrames, 0.0f));
  auto out_ptrs = ptrs(out);
  converter.process(in_ptrs.data(), out_ptrs.data(), kFrames);

  for (int ch = 0; ch < 6; ++ch) {
    float max_diff = 0.0f;
    for (int i = 0; i < kFrames; ++i) {
      max_diff = std::max(max_diff, std::abs(out[static_cast<size_t>(ch)][static_cast<size_t>(i)] -
                                             ref[static_cast<size_t>(ch)][static_cast<size_t>(i)]));
    }
    CHECK(max_diff == 0.0f);
  }
}

TEST_CASE("mono to 5.1 speakers feeds only the centre plane", "[playback][layout]") {
  auto in = noise_planes(1, -1, 12u);
  const auto in_ptrs = const_ptrs(in);
  OutputBus bus;
  bus.kind = TargetKind::Speakers;
  bus.speaker_layout = ChannelLayout::FivePointOne;
  LayoutConverter converter;
  converter.prepare(kRate, kFrames, ChannelLayout::Mono, bus);
  std::vector<std::vector<float>> out(6, std::vector<float>(kFrames, 0.0f));
  auto out_ptrs = ptrs(out);
  converter.process(in_ptrs.data(), out_ptrs.data(), kFrames);

  REQUIRE(energy(out[2]) > 0.0);
  for (int ch : {0, 1, 3, 4, 5}) CHECK(energy(out[static_cast<size_t>(ch)]) == 0.0);
}

TEST_CASE("channel map validation rejects a duplicated role", "[playback][layout]") {
  const std::array<SpeakerRole, 6> map = {SpeakerRole::C,  SpeakerRole::L,  SpeakerRole::R,
                                          SpeakerRole::Ls, SpeakerRole::Ls, SpeakerRole::LFE};
  REQUIRE_THROWS_AS(validate_channel_map(ChannelLayout::FivePointOne, map.data(), 6),
                    sonare::SonareException);
}

TEST_CASE("channel map validation rejects a missing role", "[playback][layout]") {
  const std::array<SpeakerRole, 6> map = {SpeakerRole::C,  SpeakerRole::L,  SpeakerRole::R,
                                          SpeakerRole::Ls, SpeakerRole::Ls, SpeakerRole::LFE};
  // Duplicates Ls and never states Rs: also a missing-role violation.
  REQUIRE_THROWS_AS(validate_channel_map(ChannelLayout::FivePointOne, map.data(), 6),
                    sonare::SonareException);
}

TEST_CASE("channel map validation rejects the wrong length", "[playback][layout]") {
  const std::array<SpeakerRole, 5> map = {SpeakerRole::L, SpeakerRole::R, SpeakerRole::C,
                                          SpeakerRole::Ls, SpeakerRole::Rs};
  REQUIRE_THROWS_AS(validate_channel_map(ChannelLayout::FivePointOne, map.data(), 5),
                    sonare::SonareException);
}

TEST_CASE("channel map validation rejects a role outside the layout", "[playback][layout]") {
  const std::array<SpeakerRole, 6> map = {SpeakerRole::C,  SpeakerRole::L,   SpeakerRole::R,
                                          SpeakerRole::Ls, SpeakerRole::Lss, SpeakerRole::LFE};
  REQUIRE_THROWS_AS(validate_channel_map(ChannelLayout::FivePointOne, map.data(), 6),
                    sonare::SonareException);
}

TEST_CASE("AAC-order channel map reorders to the same result as canonical order",
          "[playback][layout]") {
  // AAC channel configuration 6: C L R Ls Rs LFE.
  const std::array<SpeakerRole, 6> aac_map = {SpeakerRole::C,  SpeakerRole::L,  SpeakerRole::R,
                                              SpeakerRole::Ls, SpeakerRole::Rs, SpeakerRole::LFE};
  REQUIRE_NOTHROW(validate_channel_map(ChannelLayout::FivePointOne, aac_map.data(), 6));

  // Content keyed by role, independent of plane order.
  std::mt19937 rng(31u);
  std::uniform_real_distribution<float> dist(-0.1f, 0.1f);
  std::array<std::vector<float>, 6> by_role;
  for (auto& plane : by_role) {
    plane.resize(kFrames);
    for (float& s : plane) s = dist(rng);
  }

  // Delivered in AAC order: plane i carries the role aac_map[i].
  std::vector<std::vector<float>> aac_order(6);
  for (int i = 0; i < 6; ++i) {
    aac_order[static_cast<size_t>(i)] =
        by_role[static_cast<size_t>(aac_map[static_cast<size_t>(i)])];
  }
  // Reordered into canonical order per the declared map (role value == canonical plane index
  // for both 5.1 and 7.1, since SpeakerRole is declared in WAVE_FORMAT_EXTENSIBLE order).
  std::vector<std::vector<float>> canonical(6);
  for (int i = 0; i < 6; ++i) {
    canonical[static_cast<size_t>(aac_map[static_cast<size_t>(i)])] =
        aac_order[static_cast<size_t>(i)];
  }

  OutputBus bus;
  bus.kind = TargetKind::Speakers;
  bus.speaker_layout = ChannelLayout::Stereo;

  LayoutConverter from_canonical;
  from_canonical.prepare(kRate, kFrames, ChannelLayout::FivePointOne, bus);
  std::vector<std::vector<float>> out_canonical(2, std::vector<float>(kFrames, 0.0f));
  auto out_canonical_ptrs = ptrs(out_canonical);
  const auto by_role_ptrs = const_ptrs(by_role);
  from_canonical.process(by_role_ptrs.data(), out_canonical_ptrs.data(), kFrames);

  LayoutConverter from_reordered;
  from_reordered.prepare(kRate, kFrames, ChannelLayout::FivePointOne, bus);
  std::vector<std::vector<float>> out_reordered(2, std::vector<float>(kFrames, 0.0f));
  auto out_reordered_ptrs = ptrs(out_reordered);
  const auto canonical_ptrs = const_ptrs(canonical);
  from_reordered.process(canonical_ptrs.data(), out_reordered_ptrs.data(), kFrames);

  REQUIRE(energy(out_canonical[0]) > 0.0);
  for (int ch = 0; ch < 2; ++ch) {
    for (int i = 0; i < kFrames; ++i) {
      CHECK(out_canonical[static_cast<size_t>(ch)][static_cast<size_t>(i)] ==
            out_reordered[static_cast<size_t>(ch)][static_cast<size_t>(i)]);
    }
  }
}

namespace {

/// One cell of the conversion table, from LayoutConverter's own point of view
/// (the source is already post-upmix, so "stereo" only ever pairs with stereo
/// speakers -- every other target sees a 5.1 or 7.1 bed).
struct MatrixCase {
  const char* name;
  ChannelLayout source;
  TargetKind bus_kind;
  ChannelLayout speaker_layout;  // meaningful when bus_kind == Speakers
  InputLayout headphone_input;   // meaningful when bus_kind == Headphones
  std::vector<int> zero_planes;  // bus planes required to be exactly 0
};

}  // namespace

TEST_CASE("conversion table: supplied planes are non-zero, table zeros are exact",
          "[playback][layout]") {
  const std::vector<MatrixCase> cases = {
      {"mono -> stereo speakers",
       ChannelLayout::Mono,
       TargetKind::Speakers,
       ChannelLayout::Stereo,
       InputLayout::Mono,
       {}},
      {"mono -> 5.1 speakers",
       ChannelLayout::Mono,
       TargetKind::Speakers,
       ChannelLayout::FivePointOne,
       InputLayout::Mono,
       {0, 1, 3, 4, 5}},
      {"mono -> 7.1 speakers",
       ChannelLayout::Mono,
       TargetKind::Speakers,
       ChannelLayout::SevenPointOne,
       InputLayout::Mono,
       {0, 1, 3, 4, 5, 6, 7}},
      {"mono -> headphones",
       ChannelLayout::Mono,
       TargetKind::Headphones,
       ChannelLayout::Stereo,
       InputLayout::Mono,
       {1, 2}},
      {"stereo -> stereo speakers",
       ChannelLayout::Stereo,
       TargetKind::Speakers,
       ChannelLayout::Stereo,
       InputLayout::Stereo,
       {}},
      {"5.1 -> stereo speakers",
       ChannelLayout::FivePointOne,
       TargetKind::Speakers,
       ChannelLayout::Stereo,
       InputLayout::FivePointOne,
       {}},
      {"5.1 -> 5.1 speakers",
       ChannelLayout::FivePointOne,
       TargetKind::Speakers,
       ChannelLayout::FivePointOne,
       InputLayout::FivePointOne,
       {}},
      {"5.1 -> 7.1 speakers",
       ChannelLayout::FivePointOne,
       TargetKind::Speakers,
       ChannelLayout::SevenPointOne,
       InputLayout::FivePointOne,
       {4, 5}},
      {"5.1 -> headphones",
       ChannelLayout::FivePointOne,
       TargetKind::Headphones,
       ChannelLayout::Stereo,
       InputLayout::FivePointOne,
       {}},
      {"7.1 -> stereo speakers",
       ChannelLayout::SevenPointOne,
       TargetKind::Speakers,
       ChannelLayout::Stereo,
       InputLayout::SevenPointOne,
       {}},
      {"7.1 -> 5.1 speakers",
       ChannelLayout::SevenPointOne,
       TargetKind::Speakers,
       ChannelLayout::FivePointOne,
       InputLayout::SevenPointOne,
       {}},
      {"7.1 -> 7.1 speakers",
       ChannelLayout::SevenPointOne,
       TargetKind::Speakers,
       ChannelLayout::SevenPointOne,
       InputLayout::SevenPointOne,
       {}},
      {"7.1 -> headphones",
       ChannelLayout::SevenPointOne,
       TargetKind::Headphones,
       ChannelLayout::Stereo,
       InputLayout::SevenPointOne,
       {}},
  };

  uint32_t seed = 100u;
  for (const auto& c : cases) {
    INFO(c.name);
    OutputBus bus;
    bus.kind = c.bus_kind;
    if (c.bus_kind == TargetKind::Speakers) {
      bus.speaker_layout = c.speaker_layout;
    } else {
      bus.slots = headphone_slots(c.headphone_input);
    }

    const int nsrc = sonare::channel_count(c.source);
    auto in = noise_planes(nsrc, -1, seed++);
    const auto in_ptrs = const_ptrs(in);

    const int nbus = bus.channel_count();
    std::vector<std::vector<float>> out(static_cast<size_t>(nbus),
                                        std::vector<float>(kFrames, 0.0f));
    auto out_ptrs = ptrs(out);

    LayoutConverter converter;
    converter.prepare(kRate, kFrames, c.source, bus);
    converter.process(in_ptrs.data(), out_ptrs.data(), kFrames);

    for (int ch = 0; ch < nbus; ++ch) {
      for (float s : out[static_cast<size_t>(ch)]) REQUIRE(std::isfinite(s));
      const bool expect_zero =
          std::find(c.zero_planes.begin(), c.zero_planes.end(), ch) != c.zero_planes.end();
      if (expect_zero) {
        CHECK(energy(out[static_cast<size_t>(ch)]) == 0.0);
      } else {
        CHECK(energy(out[static_cast<size_t>(ch)]) > 0.0);
      }
    }
  }
}

TEST_CASE("LFE fold-down: in-band pass, treble rejected, tracks lfe_mix_db", "[playback][layout]") {
  constexpr float kLfeMixDb = -6.0f;

  OutputBus bus;
  bus.kind = TargetKind::Speakers;
  bus.speaker_layout = ChannelLayout::Stereo;

  auto run = [&](float freq_hz) {
    std::vector<std::vector<float>> in(6, std::vector<float>(kFrames, 0.0f));
    in[3] = sine_plane(freq_hz, 1.0f);  // LFE plane only
    const auto in_ptrs = const_ptrs(in);

    LayoutConverter converter;
    converter.prepare(kRate, kFrames, ChannelLayout::FivePointOne, bus);
    converter.set_lfe_mix_db(kLfeMixDb);
    std::vector<std::vector<float>> out(2, std::vector<float>(kFrames, 0.0f));
    auto out_ptrs = ptrs(out);
    converter.process(in_ptrs.data(), out_ptrs.data(), kFrames);
    return out;
  };

  // 40 Hz: well inside the 120 Hz LR4 passband.
  const auto low = run(40.0f);
  // Settle past the filter's own transient before measuring steady state.
  const int settle = kFrames / 4;
  std::vector<float> low_l_steady(low[0].begin() + settle, low[0].end());
  std::vector<float> low_r_steady(low[1].begin() + settle, low[1].end());
  const double in_rms = 1.0 / std::sqrt(2.0);                                // unit-amplitude sine
  const double expected_db = kLfeMixDb - 20.0 * std::log10(std::sqrt(2.0));  // lfe_mix_db - 3 dB
  const double measured_l_db = 20.0 * std::log10(rms(low_l_steady) / in_rms);
  const double measured_r_db = 20.0 * std::log10(rms(low_r_steady) / in_rms);
  CHECK(std::abs(measured_l_db - expected_db) < 0.1);
  CHECK(std::abs(measured_r_db - expected_db) < 0.1);
  CHECK(std::abs(measured_l_db - measured_r_db) < 0.01);

  // 1 kHz: well above the 120 Hz LR4 cutoff, expect >= 40 dB of extra attenuation.
  const auto high = run(1000.0f);
  std::vector<float> high_l_steady(high[0].begin() + settle, high[0].end());
  const double high_db = 20.0 * std::log10(rms(high_l_steady) / in_rms);
  CHECK(high_db <= expected_db - 40.0);
}

TEST_CASE("LFE pass-through when the bus has an LFE plane", "[playback][layout]") {
  OutputBus bus;
  bus.kind = TargetKind::Speakers;
  bus.speaker_layout = ChannelLayout::SevenPointOne;

  std::vector<std::vector<float>> in(6, std::vector<float>(kFrames, 0.0f));
  in[3] = sine_plane(1000.0f, 0.5f);
  const auto in_ptrs = const_ptrs(in);

  LayoutConverter converter;
  converter.prepare(kRate, kFrames, ChannelLayout::FivePointOne, bus);
  std::vector<std::vector<float>> out(8, std::vector<float>(kFrames, 0.0f));
  auto out_ptrs = ptrs(out);
  converter.process(in_ptrs.data(), out_ptrs.data(), kFrames);

  for (int i = 0; i < kFrames; ++i) {
    CHECK(out[3][static_cast<size_t>(i)] == in[3][static_cast<size_t>(i)]);
  }
  for (int ch : {0, 1, 2, 4, 5, 6, 7}) CHECK(energy(out[static_cast<size_t>(ch)]) == 0.0);
}

TEST_CASE("headphone slot mapping follows the bus's fixed slot set", "[playback][layout]") {
  OutputBus bus;
  bus.kind = TargetKind::Headphones;
  bus.slots = headphone_slots(InputLayout::SevenPointOne);  // L R C Ls135 Rs135 Lss90 Rss90

  auto in = noise_planes(8, 3, 71u);  // LFE silent: isolates the slot map from the fold path.
  const auto in_ptrs = const_ptrs(in);

  LayoutConverter converter;
  converter.prepare(kRate, kFrames, ChannelLayout::SevenPointOne, bus);
  std::vector<std::vector<float>> out(static_cast<size_t>(bus.channel_count()),
                                      std::vector<float>(kFrames, 0.0f));
  auto out_ptrs = ptrs(out);
  converter.process(in_ptrs.data(), out_ptrs.data(), kFrames);

  // Slot order for InputLayout::SevenPointOne: L R C Ls135 Rs135 Lss90 Rss90 (7 slots).
  for (int ch = 0; ch < 7; ++ch) REQUIRE(energy(out[static_cast<size_t>(ch)]) > 0.0);
  // No LFE energy in this signal, so the direct-ear fold planes stay silent.
  CHECK(energy(out[static_cast<size_t>(bus.direct_left_index())]) == 0.0);
  CHECK(energy(out[static_cast<size_t>(bus.direct_right_index())]) == 0.0);

  // Bit-exact identity: each source plane lands unmixed on its own slot. Slot
  // order is L R C Ls135 Rs135 Lss90 Rss90; source order is L R C LFE Ls Rs
  // Lss Rss, so source planes after LFE (index 3) shift down by one slot.
  const std::array<int, 7> source_plane_for_slot = {0, 1, 2, 4, 5, 6, 7};
  for (int slot = 0; slot < 7; ++slot) {
    const int src = source_plane_for_slot[static_cast<size_t>(slot)];
    for (int f = 0; f < kFrames; ++f) {
      CHECK(out[static_cast<size_t>(slot)][static_cast<size_t>(f)] ==
            in[static_cast<size_t>(src)][static_cast<size_t>(f)]);
    }
  }
}
