/// @file no_alloc_mastering_eq_test.cpp
/// @brief Mastering EQ no-allocation realtime tests.

#include "mastering/api/insert_factory.h"
#include "mastering/multiband/multiband_compressor.h"
#include "mastering/saturation/bitcrusher.h"
#include "mastering/saturation/exciter.h"
#include "mastering/saturation/hard_clipper.h"
#include "mastering/saturation/soft_clipper.h"
#include "mastering/saturation/transformer.h"
#include "mastering/saturation/waveshaper.h"
#include "mastering/spectral/air_band.h"
#include "mastering/spectral/low_end_focus.h"
#include "mastering/spectral/presence_enhancer.h"
#include "mastering/spectral/spectral_shaper.h"
#include "no_alloc_test_helpers.h"

namespace {

// Verifies a processor allocates nothing on the audio thread once prepared —
// including the FIRST process() block and a stereo->mono->stereo channel-count
// change (which previously reallocated, and for some processors wiped state).
template <typename Proc>
void require_no_audio_thread_alloc(Proc& proc) {
  constexpr int kBlock = 256;
  proc.prepare(48000.0, kBlock);
  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  for (int i = 0; i < kBlock; ++i) {
    left[static_cast<size_t>(i)] = i == 0 ? 0.8f : 0.05f;
    right[static_cast<size_t>(i)] = 0.04f;
  }
  float* stereo[] = {left.data(), right.data()};
  float* mono[] = {left.data()};

  AllocationGuard guard;
  proc.process(stereo, 2, kBlock);  // first block must not allocate
  proc.process(mono, 1, kBlock);    // channel-count change must not allocate
  proc.process(stereo, 2, kBlock);
  REQUIRE(guard.count() == 0);
}

template <typename Proc, typename Config>
void require_no_spectral_state_alloc(const Config& updated_config) {
  constexpr int kBlock = 64;
  for (const int num_channels : {1, 2, 6}) {
    Proc proc;
    proc.prepare(48000.0, kBlock);
    std::array<std::array<float, kBlock>, 6> storage{};
    std::array<float*, 6> channels{};
    for (int ch = 0; ch < num_channels; ++ch) {
      storage[static_cast<size_t>(ch)][0] = 0.5f;
      channels[static_cast<size_t>(ch)] = storage[static_cast<size_t>(ch)].data();
    }

    size_t first_block_allocations = 0;
    {
      AllocationGuard guard;
      proc.process(channels.data(), num_channels, kBlock);
      first_block_allocations = guard.count();
    }
    INFO("first block channel count=" << num_channels);
    REQUIRE(first_block_allocations == 0);

    proc.set_config(updated_config);
    size_t changed_config_allocations = 0;
    {
      AllocationGuard guard;
      proc.process(channels.data(), num_channels, kBlock);
      changed_config_allocations = guard.count();
    }
    INFO("config-change block channel count=" << num_channels);
    REQUIRE(changed_config_allocations == 0);
  }

  Proc over_capacity;
  over_capacity.prepare(48000.0, kBlock);
  std::array<float, kBlock> samples{};
  std::array<float*, 65> too_many_channels{};
  too_many_channels.fill(samples.data());
  REQUIRE_THROWS_AS(over_capacity.process(too_many_channels.data(), 65, kBlock),
                    sonare::SonareException);
}

}  // namespace

TEST_CASE("Exciter process is allocation free from the first block",
          "[mastering][saturation][rt]") {
  sonare::mastering::saturation::Exciter proc{sonare::mastering::saturation::ExciterConfig{}};
  require_no_audio_thread_alloc(proc);
}

TEST_CASE("BitCrusher process is allocation free from the first block",
          "[mastering][saturation][rt]") {
  sonare::mastering::saturation::BitCrusher proc{sonare::mastering::saturation::BitCrusherConfig{}};
  require_no_audio_thread_alloc(proc);
}

TEST_CASE("SoftClipper process is allocation free from the first block",
          "[mastering][saturation][rt]") {
  sonare::mastering::saturation::SoftClipper proc{
      sonare::mastering::saturation::SoftClipperConfig{}};
  require_no_audio_thread_alloc(proc);
}

TEST_CASE("HardClipper process is allocation free from the first block",
          "[mastering][saturation][rt]") {
  sonare::mastering::saturation::HardClipper proc{
      sonare::mastering::saturation::HardClipperConfig{}};
  require_no_audio_thread_alloc(proc);
}

TEST_CASE("Waveshaper process is allocation free from the first block",
          "[mastering][saturation][rt]") {
  sonare::mastering::saturation::Waveshaper proc{sonare::mastering::saturation::WaveshaperConfig{}};
  require_no_audio_thread_alloc(proc);
}

TEST_CASE("Transformer process is allocation free from the first block",
          "[mastering][saturation][rt]") {
  sonare::mastering::saturation::Transformer proc{
      sonare::mastering::saturation::TransformerConfig{}};
  require_no_audio_thread_alloc(proc);
}

TEST_CASE("AirBand process is allocation free from the first block", "[mastering][spectral][rt]") {
  sonare::mastering::spectral::AirBand proc{sonare::mastering::spectral::AirBandConfig{}};
  require_no_audio_thread_alloc(proc);
}

TEST_CASE("PresenceEnhancer process is allocation free from the first block",
          "[mastering][spectral][rt]") {
  sonare::mastering::spectral::PresenceEnhancer proc{
      sonare::mastering::spectral::PresenceEnhancerConfig{}};
  require_no_audio_thread_alloc(proc);
}

TEST_CASE("LowEndFocus mono stereo and surround first blocks are allocation free",
          "[mastering][spectral][rt]") {
  sonare::mastering::spectral::LowEndFocusConfig updated;
  updated.cutoff_hz = 180.0f;
  updated.subharmonic_amount = 0.2f;
  require_no_spectral_state_alloc<sonare::mastering::spectral::LowEndFocus>(updated);
}

TEST_CASE("SpectralShaper mono stereo and surround first blocks are allocation free",
          "[mastering][spectral][rt]") {
  sonare::mastering::spectral::SpectralShaperConfig updated;
  updated.attack_ms = 5.0f;
  updated.release_ms = 120.0f;
  require_no_spectral_state_alloc<sonare::mastering::spectral::SpectralShaper>(updated);
}

TEST_CASE("every realtime insert factory processor is allocation free on its first block",
          "[mastering][rt][catalog]") {
  constexpr int kBlock = 64;
  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  left[0] = 0.5f;
  right[0] = -0.25f;
  float* channels[] = {left.data(), right.data()};

  const std::vector<std::string> insert_ids = sonare::mastering::api::insert_factory_names();
  REQUIRE_FALSE(insert_ids.empty());
  for (const std::string& id : insert_ids) {
    INFO("realtime insert id=" << id);
    auto processor = sonare::mastering::api::make_insert(id, "{}");
    REQUIRE(processor != nullptr);
    processor->prepare(48000.0, kBlock);
    size_t allocations = 0;
    {
      AllocationGuard guard;
      processor->process(channels, 2, kBlock);
      allocations = guard.count();
    }
    REQUIRE(allocations == 0);
  }
}

TEST_CASE("TruePeakLimiter mono first block is allocation free after prepare",
          "[mastering][maximizer][rt]") {
  // The existing "no heap allocation after prepare" case below always measures
  // a STEREO block and warms up with one unguarded process() call first, so it
  // cannot see an allocation that only happens on the very first block, or one
  // that only happens for a channel count other than 2 -- exactly the scenario
  // a preset change or a device restart into mono monitoring hits.
  constexpr int kBlock = 256;
  sonare::mastering::maximizer::TruePeakLimiter limiter({-1.0f, 1.0f, 20.0f, 4});
  limiter.prepare(48000.0, kBlock);

  std::array<float, kBlock> mono{};
  for (int i = 0; i < kBlock; ++i) {
    mono[static_cast<size_t>(i)] = i == 0 ? 1.2f : 0.25f;
  }
  float* channels[] = {mono.data()};

  AllocationGuard guard;
  limiter.process(channels, 1, kBlock);
  REQUIRE(guard.count() == 0);
}

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

TEST_CASE("TruePeakLimiter mono stereo alternation is allocation free after prepare",
          "[mastering][maximizer][rt]") {
  constexpr int kBlock = 64;
  sonare::mastering::maximizer::TruePeakLimiter limiter({-6.0f, 1.0f, 50.0f, 4});
  limiter.prepare(48000.0, kBlock);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  left[0] = 1.2f;
  right[0] = -1.1f;
  float* stereo[] = {left.data(), right.data()};
  float* mono[] = {left.data()};

  limiter.process(stereo, 2, kBlock);
  limiter.process(mono, 1, kBlock);
  limiter.reset();

  AllocationGuard guard;
  limiter.process(mono, 1, kBlock);
  limiter.process(stereo, 2, kBlock);
  limiter.process(mono, 1, kBlock);
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

TEST_CASE("MultibandCompressor mono first block is allocation free after prepare",
          "[mastering][multiband][rt]") {
  // The two-argument prepare() uses the full realtime channel bound, so a
  // mono FIRST block must not resize crossover scratch on the audio thread.
  constexpr int kBlock = 256;
  sonare::mastering::multiband::MultibandCompressor compressor;
  compressor.prepare(48000.0, kBlock);

  std::array<float, kBlock> mono{};
  for (int i = 0; i < kBlock; ++i) {
    mono[static_cast<size_t>(i)] = i == 0 ? 0.8f : 0.05f;
  }
  float* channels[] = {mono.data()};

  AllocationGuard guard;
  compressor.process(channels, 1, kBlock);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("MultibandCompressor 7.1 first block is allocation free", "[mastering][multiband][rt]") {
  constexpr int kBlock = 64;
  constexpr int kChannels = 8;
  sonare::mastering::multiband::MultibandCompressor compressor;
  compressor.prepare(48000.0, kBlock, kChannels);

  std::array<std::array<float, kBlock>, kChannels> storage{};
  std::array<float*, kChannels> channels{};
  for (int ch = 0; ch < kChannels; ++ch) {
    storage[static_cast<size_t>(ch)][0] = 0.1f * static_cast<float>(ch + 1);
    channels[static_cast<size_t>(ch)] = storage[static_cast<size_t>(ch)].data();
  }

  AllocationGuard guard;
  compressor.process(channels.data(), kChannels, kBlock);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("MultibandCompressor config-change block is allocation free",
          "[mastering][multiband][rt]") {
  constexpr int kBlock = 64;
  sonare::mastering::multiband::MultibandCompressor compressor;
  compressor.prepare(48000.0, kBlock, 2);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  left[0] = 0.8f;
  right[0] = 0.4f;
  float* channels[] = {left.data(), right.data()};
  compressor.process(channels, 2, kBlock);

  auto updated = compressor.config();
  updated.crossover.cutoffs_hz[0] = 240.0f;
  updated.bands[0].threshold_db = -12.0f;
  compressor.set_config(updated);

  AllocationGuard guard;
  compressor.process(channels, 2, kBlock);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("MultibandCompressor FIR first and second blocks are allocation free",
          "[mastering][multiband][rt]") {
  constexpr int kBlock = 64;
  sonare::mastering::multiband::MultibandCompressorConfig config;
  config.crossover.mode = sonare::mastering::multiband::CrossoverMode::FirLinearPhase;
  config.crossover.fir_kernel_size = 65;
  sonare::mastering::multiband::MultibandCompressor compressor(config);
  compressor.prepare(48000.0, kBlock, 2);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  left[0] = 0.8f;
  right[0] = -0.4f;
  float* channels[] = {left.data(), right.data()};

  AllocationGuard first_block_guard;
  compressor.process(channels, 2, kBlock);
  REQUIRE(first_block_guard.count() == 0);

  AllocationGuard second_block_guard;
  compressor.process(channels, 2, kBlock);
  REQUIRE(second_block_guard.count() == 0);
}

TEST_CASE("MultibandCompressor rejects blocks outside explicit prepare bounds",
          "[mastering][multiband][rt]") {
  constexpr int kBlock = 64;
  constexpr int kChannels = 2;
  sonare::mastering::multiband::MultibandCompressor compressor;
  REQUIRE_THROWS_AS(compressor.prepare(48000.0, kBlock, 0), sonare::SonareException);
  compressor.prepare(48000.0, kBlock, kChannels);

  std::array<std::array<float, kBlock + 1>, kChannels + 1> storage{};
  std::array<float*, kChannels + 1> channels{};
  for (int ch = 0; ch < kChannels + 1; ++ch) {
    channels[static_cast<size_t>(ch)] = storage[static_cast<size_t>(ch)].data();
  }

  REQUIRE_THROWS_AS(compressor.process(channels.data(), kChannels + 1, kBlock),
                    sonare::SonareException);
  REQUIRE_THROWS_AS(compressor.process(channels.data(), kChannels, kBlock + 1),
                    sonare::SonareException);
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
