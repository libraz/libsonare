/// @file no_alloc_mastering_eq_test.cpp
/// @brief Mastering EQ no-allocation realtime tests.

#include "no_alloc_test_helpers.h"

TEST_CASE("TruePeakLimiter process performs no heap allocation after prepare",
          "[mastering][maximizer][rt]") {
  constexpr int kBlock = 256;
  sonare::mastering::maximizer::TruePeakLimiter limiter({-1.0f, 1.0f, 20.0f, 4});
  limiter.prepare(48000.0, kBlock);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  for (int i = 0; i < kBlock; ++i) {
    left[static_cast<size_t>(i)] = i == 0 ? 1.2f : 0.25f;
    right[static_cast<size_t>(i)] = i == 1 ? -1.1f : -0.2f;
  }
  float* channels[] = {left.data(), right.data()};

  limiter.process(channels, 2, kBlock);
  limiter.reset();

  AllocationGuard guard;
  limiter.process(channels, 2, kBlock);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("MultibandExciter process performs no heap allocation after prepare",
          "[mastering][saturation][rt]") {
  constexpr int kBlock = 256;
  sonare::mastering::saturation::MultibandExciter exciter;
  exciter.prepare(48000.0, kBlock);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  for (int i = 0; i < kBlock; ++i) {
    left[static_cast<size_t>(i)] = i == 0 ? 0.8f : 0.05f;
    right[static_cast<size_t>(i)] = 0.04f;
  }
  float* channels[] = {left.data(), right.data()};

  exciter.process(channels, 2, kBlock);
  exciter.reset();

  AllocationGuard guard;
  exciter.process(channels, 2, kBlock);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("PultecEq process performs no heap allocation after prepare", "[mastering][eq][rt]") {
  constexpr int kBlock = 256;
  sonare::mastering::eq::PultecEq eq;
  // Engage the WDF component model so the per-channel component state path runs.
  eq.set_component_model(sonare::mastering::eq::PultecComponentModel::Eqp1aWdf);
  eq.set_output_drive(2.0f);
  eq.prepare(48000.0, kBlock);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  for (int i = 0; i < kBlock; ++i) {
    left[static_cast<size_t>(i)] = i == 0 ? 0.6f : 0.05f;
    right[static_cast<size_t>(i)] = 0.04f;
  }
  float* channels[] = {left.data(), right.data()};

  eq.process(channels, 2, kBlock);
  eq.reset();

  AllocationGuard guard;
  eq.process(channels, 2, kBlock);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("MultibandSaturation process performs no heap allocation after prepare",
          "[mastering][saturation][rt]") {
  constexpr int kBlock = 256;
  sonare::mastering::multiband::MultibandSaturation sat;
  sat.prepare(48000.0, kBlock);
  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  for (int i = 0; i < kBlock; ++i) {
    left[static_cast<size_t>(i)] = i == 0 ? 0.8f : 0.05f;
    right[static_cast<size_t>(i)] = 0.04f;
  }
  float* channels[] = {left.data(), right.data()};
  sat.process(channels, 2, kBlock);
  sat.reset();
  AllocationGuard guard;
  sat.process(channels, 2, kBlock);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("EqualizerProcessor process performs no heap allocation after prepare",
          "[mastering][eq][rt]") {
  constexpr int kBlock = 256;
  sonare::mastering::eq::EqualizerProcessor eq({2});
  eq.prepare(48000.0, kBlock);
  eq.set_band(0, {sonare::mastering::eq::EqBandType::Peak, 1000.0f, 3.0f, 1.0f, true});
  eq.set_band(1, {sonare::mastering::eq::EqBandType::HighShelf, 8000.0f, -2.0f, 0.8f, true});

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  for (int i = 0; i < kBlock; ++i) {
    left[static_cast<size_t>(i)] = i == 0 ? 1.0f : 0.02f;
    right[static_cast<size_t>(i)] = 0.01f;
  }
  float* stereo[] = {left.data(), right.data()};

  eq.process(stereo, 2, kBlock);
  eq.reset();

  AllocationGuard stereo_guard;
  eq.process(stereo, 2, kBlock);
  REQUIRE(stereo_guard.count() == 0);

  eq.reset();
  float* mono[] = {left.data()};
  AllocationGuard mono_guard;
  eq.process(mono, 1, kBlock);
  REQUIRE(mono_guard.count() == 0);
}

TEST_CASE("EqualizerProcessor Mid/Side placement performs no heap allocation after prepare",
          "[mastering][eq][rt]") {
  constexpr int kBlock = 256;
  sonare::mastering::eq::EqualizerProcessor eq({2});
  eq.prepare(48000.0, kBlock);
  sonare::mastering::eq::EqBand mid{sonare::mastering::eq::EqBandType::Peak, 1000.0f, 3.0f, 1.0f,
                                    true};
  mid.placement = sonare::mastering::eq::StereoPlacement::Mid;
  sonare::mastering::eq::EqBand side{sonare::mastering::eq::EqBandType::Peak, 3000.0f, -2.0f, 1.2f,
                                     true};
  side.placement = sonare::mastering::eq::StereoPlacement::Side;
  eq.set_band(0, mid);
  eq.set_band(1, side);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  for (int i = 0; i < kBlock; ++i) {
    left[static_cast<size_t>(i)] = i == 0 ? 1.0f : 0.02f;
    right[static_cast<size_t>(i)] = 0.01f;
  }
  float* stereo[] = {left.data(), right.data()};

  eq.process(stereo, 2, kBlock);
  eq.reset();

  AllocationGuard guard;
  eq.process(stereo, 2, kBlock);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("EqualizerProcessor dynamic bands perform no heap allocation after prepare",
          "[mastering][eq][rt]") {
  constexpr int kBlock = 256;
  sonare::mastering::eq::EqualizerProcessor eq({2});
  eq.prepare(48000.0, kBlock);
  sonare::mastering::eq::EqBand band{sonare::mastering::eq::EqBandType::Peak, 1000.0f, 0.0f, 2.0f,
                                     true};
  band.dyn.enabled = true;
  band.dyn.threshold_db = -40.0f;
  band.dyn.ratio = 4.0f;
  band.dyn.range_db = -12.0f;
  band.dyn.attack_ms = 0.0f;
  band.dyn.release_ms = 10.0f;
  eq.set_band(23, band);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  for (int i = 0; i < kBlock; ++i) {
    left[static_cast<size_t>(i)] = 0.5f;
    right[static_cast<size_t>(i)] = 0.5f;
  }
  float* stereo[] = {left.data(), right.data()};

  eq.process(stereo, 2, kBlock);
  eq.reset();

  AllocationGuard guard;
  eq.process(stereo, 2, kBlock);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("EqualizerProcessor external sidechain performs no heap allocation after prepare",
          "[mastering][eq][rt]") {
  constexpr int kBlock = 256;
  sonare::mastering::eq::EqualizerProcessor eq({2});
  eq.prepare(48000.0, kBlock);
  sonare::mastering::eq::EqBand band{sonare::mastering::eq::EqBandType::Peak, 1000.0f, 0.0f, 2.0f,
                                     true};
  band.dyn.enabled = true;
  band.dyn.external_sidechain = true;
  band.dyn.threshold_db = -40.0f;
  band.dyn.ratio = 4.0f;
  band.dyn.range_db = -12.0f;
  band.dyn.attack_ms = 0.0f;
  band.dyn.release_ms = 10.0f;
  eq.set_band(0, band);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  std::array<float, kBlock> key{};
  for (int i = 0; i < kBlock; ++i) {
    left[static_cast<size_t>(i)] = 0.02f;
    right[static_cast<size_t>(i)] = 0.02f;
    key[static_cast<size_t>(i)] = 0.5f;
  }
  float* stereo[] = {left.data(), right.data()};
  const float* sidechain[] = {key.data()};

  eq.set_sidechain(sidechain, 1, kBlock);
  eq.process(stereo, 2, kBlock);
  eq.reset();

  eq.set_sidechain(sidechain, 1, kBlock);
  AllocationGuard guard;
  eq.process(stereo, 2, kBlock);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("EqualizerProcessor LinearPhase bands perform no heap allocation after prepare",
          "[mastering][eq][rt]") {
  constexpr int kBlock = 256;
  sonare::mastering::eq::EqualizerProcessor eq({2});
  eq.prepare(48000.0, kBlock);
  sonare::mastering::eq::EqBand linear{sonare::mastering::eq::EqBandType::Peak, 1000.0f, 4.0f, 1.0f,
                                       true};
  linear.phase = sonare::mastering::eq::PhaseMode::LinearPhase;
  eq.set_band(0, linear);
  eq.set_band(1, {sonare::mastering::eq::EqBandType::HighShelf, 8000.0f, -2.0f, 0.8f, true});

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  for (int i = 0; i < kBlock; ++i) {
    left[static_cast<size_t>(i)] = i == 0 ? 1.0f : 0.02f;
    right[static_cast<size_t>(i)] = 0.01f;
  }
  float* stereo[] = {left.data(), right.data()};

  eq.process(stereo, 2, kBlock);
  eq.reset();

  AllocationGuard guard;
  eq.process(stereo, 2, kBlock);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("EqualizerProcessor E6 features perform no heap allocation after prepare",
          "[mastering][eq][rt]") {
  constexpr int kBlock = 256;
  sonare::mastering::eq::EqualizerProcessor eq({2});
  eq.prepare(48000.0, kBlock);
  eq.set_auto_gain_enabled(true);
  sonare::mastering::eq::EqBand tilt{sonare::mastering::eq::EqBandType::TiltShelf, 1000.0f, 6.0f,
                                     1.0f, true};
  sonare::mastering::eq::EqBand solo{sonare::mastering::eq::EqBandType::Peak, 2500.0f, 9.0f, 3.0f,
                                     true};
  solo.soloed = true;
  solo.proportional_q = true;
  eq.set_band(0, tilt);
  eq.set_band(1, solo);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  for (int i = 0; i < kBlock; ++i) {
    left[static_cast<size_t>(i)] = i == 0 ? 1.0f : 0.02f;
    right[static_cast<size_t>(i)] = 0.01f;
  }
  float* stereo[] = {left.data(), right.data()};

  eq.process(stereo, 2, kBlock);
  eq.reset();

  AllocationGuard guard;
  eq.process(stereo, 2, kBlock);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("SpectrumRegistry publish read and collisions perform no heap allocation",
          "[mastering][eq][rt]") {
  auto& registry = sonare::mastering::eq::SpectrumRegistry::instance();
  registry.reset();

  sonare::mastering::eq::SpectrumProfile first;
  first.instance_id = 9001;
  first.active = true;
  first.seq = 1;
  first.band_db.fill(-120.0f);
  first.band_db[4] = -18.0f;

  sonare::mastering::eq::SpectrumProfile second;
  second.instance_id = 9002;
  second.active = true;
  second.seq = 1;
  second.band_db.fill(-120.0f);
  second.band_db[5] = -16.0f;

  registry.publish(first);
  registry.publish(second);

  AllocationGuard guard;
  registry.publish(first);
  sonare::mastering::eq::SpectrumProfile out;
  REQUIRE(registry.read(9001, out));
  const auto report = registry.collisions(9001, 9002, -60.0f);
  registry.remove(9001);
  REQUIRE(report.count == 1);
  REQUIRE(guard.count() == 0);

  registry.reset();
}

TEST_CASE("EqualizerProcessor spectrum publishing performs no heap allocation after prepare",
          "[mastering][eq][rt]") {
  constexpr int kBlock = 256;
  auto& registry = sonare::mastering::eq::SpectrumRegistry::instance();
  registry.reset();

  sonare::mastering::eq::EqualizerProcessor eq({2, 7007});
  eq.prepare(48000.0, kBlock);
  eq.set_band(0, {sonare::mastering::eq::EqBandType::Peak, 1000.0f, 6.0f, 1.0f, true});

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  for (int i = 0; i < kBlock; ++i) {
    left[static_cast<size_t>(i)] = i == 0 ? 1.0f : 0.02f;
    right[static_cast<size_t>(i)] = 0.01f;
  }
  float* stereo[] = {left.data(), right.data()};

  eq.process(stereo, 2, kBlock);
  eq.reset();

  AllocationGuard guard;
  eq.process(stereo, 2, kBlock);
  sonare::mastering::eq::SpectrumProfile profile;
  REQUIRE(registry.read(7007, profile));
  REQUIRE(profile.active);
  REQUIRE(guard.count() == 0);

  registry.reset();
}

TEST_CASE("CutFilter process performs no heap allocation after prepare", "[mastering][eq][rt]") {
  constexpr int kBlock = 256;
  sonare::mastering::eq::CutFilter eq;
  eq.prepare(48000.0, kBlock);
  eq.set_high_pass(1000.0f, sonare::constants::kButterworthQ,
                   sonare::mastering::eq::CutFilterSlope::Db96PerOct);
  eq.set_low_pass(12000.0f, sonare::constants::kButterworthQ,
                  sonare::mastering::eq::CutFilterSlope::Db96PerOct);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  for (int i = 0; i < kBlock; ++i) {
    left[static_cast<size_t>(i)] = i == 0 ? 1.0f : 0.02f;
    right[static_cast<size_t>(i)] = 0.01f;
  }
  float* stereo[] = {left.data(), right.data()};

  eq.process(stereo, 2, kBlock);
  eq.reset();

  AllocationGuard guard;
  eq.process(stereo, 2, kBlock);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("CutFilter brickwall process performs no heap allocation after prepare",
          "[mastering][eq][rt]") {
  constexpr int kBlock = 256;
  sonare::mastering::eq::CutFilter eq;
  eq.prepare(48000.0, kBlock);
  eq.set_high_pass(1000.0f, sonare::constants::kButterworthQ,
                   sonare::mastering::eq::CutFilterSlope::Brickwall);
  eq.set_low_pass(12000.0f, sonare::constants::kButterworthQ,
                  sonare::mastering::eq::CutFilterSlope::Brickwall);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  for (int i = 0; i < kBlock; ++i) {
    left[static_cast<size_t>(i)] = i == 0 ? 1.0f : 0.02f;
    right[static_cast<size_t>(i)] = 0.01f;
  }
  float* stereo[] = {left.data(), right.data()};

  eq.process(stereo, 2, kBlock);
  eq.reset();

  AllocationGuard guard;
  eq.process(stereo, 2, kBlock);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("MinimumPhaseEq process performs no heap allocation after prepare",
          "[mastering][eq][rt]") {
  constexpr int kBlock = 256;
  sonare::mastering::eq::MinimumPhaseEq eq;
  eq.prepare(48000.0, kBlock);
  eq.prepare_channels(2);
  eq.set_band(0, {sonare::mastering::eq::EqBandType::Peak, 12000.0f, 4.0f, 0.8f, true});
  eq.set_band(1, {sonare::mastering::eq::EqBandType::HighShelf, 9000.0f, -3.0f, 1.0f, true});

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  for (int i = 0; i < kBlock; ++i) {
    left[static_cast<size_t>(i)] = i == 0 ? 1.0f : 0.02f;
    right[static_cast<size_t>(i)] = 0.01f;
  }
  float* stereo[] = {left.data(), right.data()};

  eq.process(stereo, 2, kBlock);
  eq.reset();

  AllocationGuard guard;
  eq.process(stereo, 2, kBlock);
  REQUIRE(guard.count() == 0);
}
