/// @file eq_spectrum_test.cpp
/// @brief EQ spectrum and registry tests.

#include "eq_test_helpers.h"

TEST_CASE("spectrum_grab_band chooses the nearest enabled EQ band or an empty slot",
          "[mastering][eq]") {
  std::array<EqBand, EqualizerProcessor::kMaxBands> bands{};
  bands[0] = {EqBandType::Peak, 200.0f, 0.0f, 1.0f, true};
  bands[1] = {EqBandType::Peak, 2000.0f, 0.0f, 1.0f, true};
  bands[2] = {EqBandType::Peak, 8000.0f, 0.0f, 1.0f, true};

  const auto existing = spectrum_grab_band(2600.0f, bands.data(), bands.size());
  REQUIRE(existing.use_existing);
  REQUIRE(existing.index == 1);

  bands[0].enabled = false;
  bands[1].enabled = false;
  bands[2].enabled = false;
  const auto empty = spectrum_grab_band(2600.0f, bands.data(), bands.size());
  REQUIRE_FALSE(empty.use_existing);
  REQUIRE(empty.index == 0);
}

TEST_CASE("SpectrumRegistry stores fixed profiles and reports overlapping bands",
          "[mastering][eq]") {
  auto& registry = SpectrumRegistry::instance();
  registry.reset();

  SpectrumProfile kick;
  kick.instance_id = 101;
  kick.active = true;
  kick.seq = 3;
  kick.band_db.fill(-120.0f);
  kick.band_db[2] = -18.0f;
  kick.band_db[8] = -42.0f;

  SpectrumProfile bass;
  bass.instance_id = 202;
  bass.active = true;
  bass.seq = 4;
  bass.band_db.fill(-120.0f);
  bass.band_db[2] = -12.0f;
  bass.band_db[6] = -20.0f;

  registry.publish(kick);
  registry.publish(bass);

  SpectrumProfile read_back;
  REQUIRE(registry.read(101, read_back));
  REQUIRE(read_back.seq == 3);
  REQUIRE_THAT(read_back.band_db[2], WithinAbs(-18.0f, 0.0001f));

  const auto report = registry.collisions(101, 202, -60.0f);
  REQUIRE(report.count == 1);
  REQUIRE(report.bands[0].band == 2);
  REQUIRE_THAT(report.bands[0].score_db, WithinAbs(-18.0f, 0.0001f));

  bass.band_db.fill(-120.0f);
  bass.band_db[3] = -14.0f;
  registry.publish(bass);
  const auto adjacent_report = registry.collisions(101, 202, -60.0f);
  REQUIRE(adjacent_report.count == 1);
  REQUIRE(adjacent_report.bands[0].band == 2);
  REQUIRE_THAT(adjacent_report.bands[0].score_db, WithinAbs(-18.0f, 0.0001f));

  registry.remove(101);
  REQUIRE_FALSE(registry.read(101, read_back));
}

TEST_CASE("SpectrumRegistry keeps fixed capacity entries stable when full", "[mastering][eq]") {
  auto& registry = SpectrumRegistry::instance();
  registry.reset();

  SpectrumProfile profile;
  profile.active = true;
  profile.seq = 1;
  profile.band_db.fill(-120.0f);
  for (uint64_t id = 1; id <= 64; ++id) {
    profile.instance_id = id;
    profile.band_db[0] = -60.0f + static_cast<float>(id);
    registry.publish(profile);
  }

  profile.instance_id = 65;
  profile.band_db[0] = 0.0f;
  registry.publish(profile);

  SpectrumProfile read_back;
  REQUIRE_FALSE(registry.read(65, read_back));
  REQUIRE(registry.read(1, read_back));
  REQUIRE_THAT(read_back.band_db[0], WithinAbs(-59.0f, 0.0001f));
  REQUIRE(registry.read(64, read_back));
  REQUIRE_THAT(read_back.band_db[0], WithinAbs(4.0f, 0.0001f));

  registry.reset();
}

TEST_CASE("EqualizerProcessor exposes pre/post spectrum snapshots and publishes a registry profile",
          "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  SpectrumRegistry::instance().reset();
  {
    EqualizerProcessor eq({2, 303});
    eq.prepare(sample_rate, 1024);
    EqBand boost{EqBandType::Peak, 1000.0f, 9.0f, 1.0f, true};
    eq.set_band(0, boost);

    auto left = sine(1000.0f, sample_rate, 1024, 0.25f);
    auto right = sine(2000.0f, sample_rate, 1024, 0.1f);
    const float first_left = left[4];
    process_stereo(eq, left, right);

    const auto snapshot = eq.spectrum_snapshot();
    REQUIRE(snapshot.seq == 1);
    REQUIRE(snapshot.pre_count == kSpectrumStreamCapacity);
    REQUIRE(snapshot.post_count == kSpectrumStreamCapacity);
    REQUIRE_THAT(snapshot.pre[1].left, WithinAbs(first_left, 0.000001f));
    REQUIRE(std::abs(snapshot.post[1].left - snapshot.pre[1].left) > 0.000001f);
    REQUIRE_THAT(snapshot.band_gain_db[0], WithinAbs(9.0f, 0.0001f));

    SpectrumProfile profile;
    REQUIRE(SpectrumRegistry::instance().read(303, profile));
    REQUIRE(profile.seq == 1);
    bool has_activity = false;
    for (float db : profile.band_db) {
      has_activity = has_activity || db > -120.0f;
    }
    REQUIRE(has_activity);
  }

  SpectrumProfile removed;
  REQUIRE_FALSE(SpectrumRegistry::instance().read(303, removed));
}
