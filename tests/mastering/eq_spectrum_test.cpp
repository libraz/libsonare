/// @file eq_spectrum_test.cpp
/// @brief EQ spectrum and registry tests.

#include "eq_test_helpers.h"
#include "mastering/api/named_processor.h"

namespace {

/// Band index holding @p frequency_hz under the profile's geometric spacing
/// across the audible range, derived independently of the analyser.
size_t profile_band_of(double frequency_hz) {
  const double normalized = std::log2(frequency_hz / 20.0) / std::log2(20000.0 / 20.0);
  return static_cast<size_t>(std::floor(normalized * kSpectrumProfileBands));
}

/// Index of the loudest profile band, resolving ties to the lowest band.
size_t peak_profile_band(const std::array<float, kSpectrumProfileBands>& profile) {
  size_t best = 0;
  for (size_t i = 1; i < profile.size(); ++i) {
    if (profile[i] > profile[best]) {
      best = i;
    }
  }
  return best;
}

/// Drives @p eq with a continuous tone split into blocks and returns the
/// published profile. The tone is generated once so it stays phase-continuous
/// across block boundaries.
std::array<float, kSpectrumProfileBands> profile_for_tone(EqualizerProcessor& eq,
                                                          float frequency_hz, int sample_rate,
                                                          int block_size, int blocks) {
  const auto signal = sine(frequency_hz, sample_rate, block_size * blocks, 0.5f);
  for (int b = 0; b < blocks; ++b) {
    std::vector<float> left(signal.begin() + b * block_size, signal.begin() + (b + 1) * block_size);
    std::vector<float> right = left;
    process_stereo(eq, left, right);
  }
  return eq.spectrum_snapshot().profile_db;
}

}  // namespace

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

TEST_CASE("EqualizerProcessor profile_db is a magnitude spectrum that follows the input tone",
          "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  constexpr int block_size = 1024;
  constexpr int blocks = 8;

  // No band is configured: with a flat EQ the profile must still describe the
  // spectral content of the signal rather than its broadband level.
  EqualizerProcessor low_eq({2, 0});
  low_eq.prepare(sample_rate, block_size);
  const auto low_profile = profile_for_tone(low_eq, 250.0f, sample_rate, block_size, blocks);

  EqualizerProcessor high_eq({2, 0});
  high_eq.prepare(sample_rate, block_size);
  const auto high_profile = profile_for_tone(high_eq, 1000.0f, sample_rate, block_size, blocks);

  const size_t low_peak = peak_profile_band(low_profile);
  const size_t high_peak = peak_profile_band(high_profile);

  REQUIRE(low_peak == profile_band_of(250.0));
  REQUIRE(high_peak == profile_band_of(1000.0));
  REQUIRE(low_peak != high_peak);

  // Each tone's band stands clear of the band the other tone occupies.
  REQUIRE(high_profile[high_peak] - high_profile[low_peak] > 20.0f);
  REQUIRE(low_profile[low_peak] - low_profile[high_peak] > 20.0f);

  // Levels are amplitude-referenced, so a 0.5 sine reads near -6 dB in its band.
  REQUIRE_THAT(high_profile[high_peak], WithinAbs(-6.0f, 3.0f));
  REQUIRE_THAT(low_profile[low_peak], WithinAbs(-6.0f, 3.0f));
}

// The offline named-processor route reaches EqualizerProcessor through
// run_processor_mono() rather than the realtime block loop the snapshot tests
// drive, so it prepares and runs the processor on a single channel with a
// block size derived from the whole buffer. It had no direct coverage.
TEST_CASE("eq.equalizer runs through the offline named-processor path", "[mastering][eq]") {
  namespace api = sonare::mastering::api;
  constexpr int sample_rate = 48000;
  const auto input = sine(1000.0f, sample_rate, 4096, 0.25f);

  const auto flat =
      api::apply_named_processor("eq.equalizer", input.data(), input.size(), sample_rate);
  REQUIRE(flat.samples.size() == input.size());

  const std::vector<api::Param> boost{{"band0.enabled", 1.0},
                                      {"band0.frequencyHz", 1000.0},
                                      {"band0.gainDb", 9.0},
                                      {"band0.q", 1.0}};
  const auto boosted =
      api::apply_named_processor("eq.equalizer", input.data(), input.size(), sample_rate, boost);
  REQUIRE(boosted.samples.size() == input.size());
  // A 9 dB peak on the tone's own frequency is a 2.8x rise; require well over
  // half of it so the assertion tracks the band actually being applied.
  REQUIRE(peak_abs(boosted.samples) > peak_abs(flat.samples) * 1.5f);
}
