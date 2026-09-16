/// @file effects_state_non_finite_recovery_test.cpp
/// @brief One non-finite sample must not outlive the block that carried it, at
///        the effects inserts whose recursive cells nothing returned to rest:
///        the reverb tanks, the two feedback delays, the modulation filters and
///        the room morph's tail expander.
///
/// The claim is read against the INPUT rather than against a clean run, because
/// none of these cells converges back to a clean run's trajectory: a discarded
/// state re-enters its loop from rest, not from wherever the control had
/// reached. What a caller can hold an insert to is that its output tracks the
/// input again, to within what the insert does to a clean stream. The bound is
/// therefore taken FROM the clean run instead of written down, so it cannot be
/// set loose enough to pass a degraded stream by accident.
///
/// Where an insert holds a delay line, contamination may legitimately outlast
/// the carrying block, so the sample at which recovery is read comes from the
/// insert's own structure -- its declared tail, its declared latency plus the
/// target impulse response, or its configured delay length -- never from a
/// written-down block count. The lines that carry a reverb's feedback are
/// scrubbed with the cells that feed them, so an insert may recover well before
/// its own bound; nothing here asserts that it may not.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

#include "effects/common/dc_blocker.h"
#include "effects/delay/stereo_delay.h"
#include "effects/modulation/auto_wah.h"
#include "effects/modulation/ensemble.h"
#include "effects/modulation/flanger.h"
#include "effects/modulation/phaser.h"
#include "effects/modulation/rotary.h"
#include "effects/modulation/wah.h"
#include "effects/reverb/dattorro_reverb.h"
#include "effects/reverb/fdn_reverb.h"
#include "effects/reverb/velvet_reverb.h"
#include "util/constants.h"

#ifdef SONARE_WITH_ACOUSTIC_SIM
#include "acoustic/room_model.h"
#include "effects/acoustic/room_morph.h"
#endif

namespace {

using sonare::constants::kPiD;

constexpr int kSampleRate = 48000;
constexpr int kBlockSize = 256;
constexpr int kPoisonBlock = 1;
constexpr int kPoisonIndex = 100;
/// Blocks read past the structural recovery point, so the comparison window is
/// never empty and a late relapse is still seen.
constexpr int kTrailingBlocks = 12;
/// How far past the clean run's own worst deviation a recovered stream may sit.
/// The recovered run re-enters its loop from rest while the control is
/// mid-trajectory, so the two are not required to coincide -- only to stay on
/// the same scale.
constexpr double kDeviationMargin = 4.0;
/// A cell that holds one sample of history needs one block to flush.
constexpr int kSingleSampleHistory = 1;

/// Fixture well inside full scale: the inserts colour it rather than clip it.
float source_sample(int index) {
  const double t = static_cast<double>(index) / kSampleRate;
  return static_cast<float>(0.35 * std::sin(2.0 * kPiD * 110.0 * t) +
                            0.20 * std::sin(2.0 * kPiD * 1970.0 * t));
}

int ms_to_samples(float milliseconds) {
  return static_cast<int>(std::ceil(static_cast<double>(milliseconds) * 0.001 * kSampleRate));
}

struct Stream {
  std::vector<float> left;
  std::vector<float> right;
};

std::array<float, 3> poison_values() {
  return {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
          -std::numeric_limits<float>::infinity()};
}

/// First block whose output may no longer carry the poison: the block after the
/// carrying one, pushed out by however long the insert's own structure lets a
/// sample stay audible.
int recovery_block(int contamination_samples) {
  return kPoisonBlock + 1 + (contamination_samples + kBlockSize - 1) / kBlockSize;
}

template <typename Processor>
Stream run_stream(Processor& processor, int blocks, float poison_value, bool poison) {
  Stream out;
  out.left.reserve(static_cast<size_t>(blocks) * kBlockSize);
  out.right.reserve(static_cast<size_t>(blocks) * kBlockSize);
  std::vector<float> left(static_cast<size_t>(kBlockSize));
  std::vector<float> right(static_cast<size_t>(kBlockSize));
  for (int k = 0; k < blocks; ++k) {
    for (int i = 0; i < kBlockSize; ++i) {
      const float sample = source_sample(k * kBlockSize + i);
      left[static_cast<size_t>(i)] = sample;
      right[static_cast<size_t>(i)] = sample;
    }
    // One channel only: the other channel's cells must come back too, which is
    // what the group rule is for in a cross-coupled tank.
    if (poison && k == kPoisonBlock) left[static_cast<size_t>(kPoisonIndex)] = poison_value;
    float* channels[] = {left.data(), right.data()};
    processor.process(channels, 2, kBlockSize);
    out.left.insert(out.left.end(), left.begin(), left.end());
    out.right.insert(out.right.end(), right.begin(), right.end());
  }
  return out;
}

/// One block of the same source, optionally poisoned in EVERY channel. That is
/// the difference from run_stream, and it is what separates a per-block count
/// from a per-channel one: while only one channel carries the poison the two
/// produce the same number.
template <typename Processor>
void run_one_block(Processor& processor, int block_index, float poison_value, bool poison) {
  std::vector<float> left(static_cast<size_t>(kBlockSize));
  std::vector<float> right(static_cast<size_t>(kBlockSize));
  for (int i = 0; i < kBlockSize; ++i) {
    const float sample = source_sample(block_index * kBlockSize + i);
    left[static_cast<size_t>(i)] = sample;
    right[static_cast<size_t>(i)] = sample;
  }
  if (poison) {
    left[static_cast<size_t>(kPoisonIndex)] = poison_value;
    right[static_cast<size_t>(kPoisonIndex)] = poison_value;
  }
  float* channels[] = {left.data(), right.data()};
  processor.process(channels, 2, kBlockSize);
}

/// Largest distance between one output channel and the latency-aligned source
/// from sample @p from onward, or infinity when any distance is non-finite.
/// @note The non-finite return is load-bearing. std::max returns its first
///       argument when the second is non-finite, so folding a NaN distance into
///       a running maximum skips it and a wholly degraded run reads as a small
///       deviation -- passing the bound on the worst possible result.
double channel_deviation(const std::vector<float>& channel, int from, int latency) {
  double worst = 0.0;
  const int last = static_cast<int>(channel.size()) - latency;
  for (int i = from; i < last; ++i) {
    const double output = static_cast<double>(channel[static_cast<size_t>(i + latency)]);
    const double distance = std::abs(output - static_cast<double>(source_sample(i)));
    if (!std::isfinite(distance)) {
      return std::numeric_limits<double>::infinity();
    }
    worst = std::max(worst, distance);
  }
  return worst;
}

double deviation_from_input(const Stream& stream, int from, int latency) {
  return std::max(channel_deviation(stream.left, from, latency),
                  channel_deviation(stream.right, from, latency));
}

int last_non_finite_sample(const Stream& stream) {
  int last = -1;
  for (size_t i = 0; i < stream.left.size(); ++i) {
    if (!std::isfinite(stream.left[i]) || !std::isfinite(stream.right[i])) {
      last = static_cast<int>(i);
    }
  }
  return last;
}

/// Drives one insert twice -- clean and poisoned -- and asserts the invariant.
/// @param make                  Builds a fresh, prepared insert.
/// @param contamination_samples How long this insert's structure lets a sample
///                              stay audible after it arrives.
/// @param latency               The insert's declared output latency.
template <typename Make>
void check_owner(const Make& make, int contamination_samples, int latency, float poison_value) {
  const int recovery = recovery_block(contamination_samples + latency);
  const int blocks = recovery + kTrailingBlocks;
  const int recovery_sample = recovery * kBlockSize;

  auto control_insert = make();
  const Stream control = run_stream(*control_insert, blocks, 0.0f, false);

  // Non-vacuity, read before any recovery result. The first says the insert is
  // not a passthrough; the second says it is still doing something where the
  // recovery bound is read, which is what a stream that goes quiet late would
  // otherwise pass on.
  const double control_deviation = deviation_from_input(control, 0, latency);
  const double control_deviation_late = deviation_from_input(control, recovery_sample, latency);
  INFO("control deviation " << control_deviation << ", from the recovery sample "
                            << control_deviation_late);
  REQUIRE(control_deviation > 0.0);
  REQUIRE(control_deviation_late > 0.0);
  REQUIRE(last_non_finite_sample(control) < 0);

  auto poisoned_insert = make();
  const Stream poisoned = run_stream(*poisoned_insert, blocks, poison_value, true);

  INFO("last non-finite sample " << last_non_finite_sample(poisoned) << " against recovery sample "
                                 << recovery_sample);
  REQUIRE(last_non_finite_sample(poisoned) < recovery_sample);

  // Finiteness alone is not recovery: an insert whose feedback path substitutes
  // an in-domain value for a non-finite one stays finite and stops tracking its
  // input. The bound is the clean run's own worst deviation.
  const double recovered = deviation_from_input(poisoned, recovery_sample, latency);
  INFO("recovered deviation " << recovered << " against bound "
                              << control_deviation * kDeviationMargin);
  REQUIRE(recovered <= control_deviation * kDeviationMargin);
}

/// A section per poison value, so an assertion that stops the case cannot hide
/// the other two. The three reach a cell by different routes -- a NaN arrives as
/// one, an infinity becomes one inside a difference -- and only running all
/// three says so.
template <typename Make>
void check_recovery(const Make& make, int contamination_samples, int latency = 0) {
  for (const float poison_value : poison_values()) {
    DYNAMIC_SECTION("poison " << poison_value) {
      check_owner(make, contamination_samples, latency, poison_value);
    }
  }
}

template <typename Processor, typename Config>
auto prepared(const Config& config) {
  return [config]() {
    auto processor = std::make_shared<Processor>(config);
    processor->prepare(kSampleRate, kBlockSize);
    return processor;
  };
}

/// The insert reports that it discarded, with a clean run as the control. Every
/// case above reads whether the insert came back; none of them can tell a caller
/// that it ever left, and a recovery nobody can observe is one nobody can act on.
///
/// @p contamination_samples is the same window the matching recovery case
/// allows: an insert whose recursive cells sit behind a long line does not see
/// the poison in the block that carried it, so a shorter run reads zero from an
/// insert that counts correctly.
template <typename Make>
void check_discard_is_counted(const Make& make, int contamination_samples) {
  const int kBlocks = recovery_block(contamination_samples) + kTrailingBlocks;
  for (const float poison_value : poison_values()) {
    DYNAMIC_SECTION("poison " << poison_value) {
      auto control = make();
      run_stream(*control, kBlocks, poison_value, false);
      // Without this, an insert that counted every block would satisfy the
      // assertion below while reporting nothing about this sample.
      CHECK(control->non_finite_discard_count() == 0);

      auto poisoned = make();
      run_stream(*poisoned, kBlocks, poison_value, true);
      CHECK(poisoned->non_finite_discard_count() > 0);

      // The unit, not merely its presence. Stepped block by block with every
      // channel poisoned, the first block that counts must count exactly one: a
      // processor bumping per channel records two for that block, and the
      // channel count is a property of the buffer the caller passed rather than
      // of the work they asked for.
      auto stepped = make();
      uint32_t previous = 0;
      bool counted = false;
      for (int k = 0; k < kBlocks && !counted; ++k) {
        run_one_block(*stepped, k, poison_value, k == kPoisonBlock);
        const uint32_t now = stepped->non_finite_discard_count();
        if (now != previous) {
          CHECK(now - previous == 1u);
          counted = true;
        }
      }
      // Without this the loop above passes by never counting at all.
      CHECK(counted);
    }
  }
}

}  // namespace

TEST_CASE("the DC blocker's history is returned to rest", "[effects][non_finite]") {
  using sonare::effects::common::DcBlocker;

  const auto make = []() {
    auto processor = std::make_shared<DcBlocker>();
    // A corner well above the fixture's low tone, so the clean run's own
    // deviation is the filter rather than a near-unity pass.
    processor->set_cutoff_hz(400.0f);
    processor->prepare(kSampleRate, kBlockSize);
    return processor;
  };

  check_recovery(make, kSingleSampleHistory);
}

TEST_CASE("the FDN reverb's network is returned to rest", "[effects][non_finite]") {
  using sonare::effects::reverb::FdnReverb;
  using sonare::effects::reverb::FdnReverbConfig;

  // A short decay keeps the declared tail -- and with it the window this test
  // has to allow and then outrun -- to a fraction of a second.
  FdnReverbConfig config;
  config.decay = 0.05f;
  config.dry_wet = 0.5f;

  const auto make = prepared<FdnReverb>(config);
  check_recovery(make, make()->tail_samples());
}

TEST_CASE("the Dattorro reverb's tank is returned to rest", "[effects][non_finite]") {
  using sonare::effects::reverb::DattorroReverb;
  using sonare::effects::reverb::DattorroReverbConfig;

  DattorroReverbConfig config;
  config.decay = 0.3f;
  config.dry_wet = 0.5f;

  const auto make = prepared<DattorroReverb>(config);
  check_recovery(make, make()->tail_samples());
}

TEST_CASE("the velvet reverb's post filters are returned to rest", "[effects][non_finite]") {
  using sonare::effects::reverb::VelvetReverb;
  using sonare::effects::reverb::VelvetReverbConfig;

  VelvetReverbConfig config;
  config.reverb_time_s = 0.4f;
  config.dry_wet = 0.5f;

  // The tap span, plus the late convolution's own staging and overlap-add
  // partitions, which hold a sample past the span the tail declares.
  const auto make = prepared<VelvetReverb>(config);
  check_recovery(make, make()->tail_samples() + 2 * VelvetReverb::kEarlyPartitionSamples);
}

TEST_CASE("the stereo delay's feedback path is returned to rest", "[effects][non_finite]") {
  using sonare::effects::delay::StereoDelay;
  using sonare::effects::delay::StereoDelayConfig;

  StereoDelayConfig config;
  config.delay_time_l_ms = 40.0f;
  config.delay_time_r_ms = 55.0f;
  config.feedback = 0.5f;
  config.ping_pong = 0.5f;
  config.dry_wet = 0.5f;

  const auto make = prepared<StereoDelay>(config);
  check_recovery(make, make()->tail_samples());
}

TEST_CASE("the flanger's feedback path is returned to rest", "[effects][non_finite]") {
  using sonare::effects::modulation::Flanger;
  using sonare::effects::modulation::FlangerConfig;

  FlangerConfig config;
  config.feedback = 0.8f;
  config.dry_wet = 0.5f;

  // The line is read at most centre + depth behind the write head, and the
  // insert declares no tail of its own.
  const int contamination = ms_to_samples(config.center_delay_ms + config.depth_ms);
  check_recovery(prepared<Flanger>(config), contamination);
}

TEST_CASE("the phaser's allpass sections are returned to rest", "[effects][non_finite]") {
  using sonare::effects::modulation::Phaser;
  using sonare::effects::modulation::PhaserConfig;

  PhaserConfig config;
  config.dry_wet = 0.5f;

  check_recovery(prepared<Phaser>(config), kSingleSampleHistory);
}

TEST_CASE("the rotary's crossover filter is returned to rest", "[effects][non_finite]") {
  using sonare::effects::modulation::Rotary;
  using sonare::effects::modulation::RotaryConfig;

  RotaryConfig config;
  config.dry_wet = 1.0f;

  // The rotor delays are centred on depth_ms and swing by the same amount.
  check_recovery(prepared<Rotary>(config), ms_to_samples(2.0f * config.depth_ms));
}

TEST_CASE("the ensemble's tone filter is returned to rest", "[effects][non_finite]") {
  using sonare::effects::modulation::Ensemble;
  using sonare::effects::modulation::EnsembleConfig;

  EnsembleConfig config;
  config.dry_wet = 1.0f;

  const int contamination =
      ms_to_samples(config.center_delay_ms + config.depth_slow_ms + config.depth_fast_ms);
  check_recovery(prepared<Ensemble>(config), contamination);
}

TEST_CASE("the auto-wah's follower and filters are returned to rest", "[effects][non_finite]") {
  using sonare::effects::modulation::AutoWah;
  using sonare::effects::modulation::AutoWahConfig;

  AutoWahConfig config;
  config.dry_wet = 1.0f;

  check_recovery(prepared<AutoWah>(config), kSingleSampleHistory);
}

TEST_CASE("the wah's filters are returned to rest", "[effects][non_finite]") {
  using sonare::effects::modulation::Wah;
  using sonare::effects::modulation::WahConfig;

  WahConfig config;
  config.dry_wet = 1.0f;

  check_recovery(prepared<Wah>(config), kSingleSampleHistory);
}

TEST_CASE("an insert counts the state it discarded", "[effects][non_finite]") {
  using namespace sonare::effects;

  SECTION("dc blocker") {
    check_discard_is_counted(
        []() {
          auto processor = std::make_shared<common::DcBlocker>();
          processor->set_cutoff_hz(400.0f);
          processor->prepare(kSampleRate, kBlockSize);
          return processor;
        },
        kSingleSampleHistory);
  }
  SECTION("fdn reverb") {
    reverb::FdnReverbConfig config;
    config.decay = 0.05f;
    config.dry_wet = 0.5f;
    const auto make = prepared<reverb::FdnReverb>(config);
    check_discard_is_counted(make, make()->tail_samples());
  }
  SECTION("dattorro reverb") {
    reverb::DattorroReverbConfig config;
    config.decay = 0.3f;
    config.dry_wet = 0.5f;
    const auto make = prepared<reverb::DattorroReverb>(config);
    check_discard_is_counted(make, make()->tail_samples());
  }
  SECTION("velvet reverb") {
    reverb::VelvetReverbConfig config;
    config.reverb_time_s = 0.4f;
    config.dry_wet = 0.5f;
    const auto make = prepared<reverb::VelvetReverb>(config);
    check_discard_is_counted(
        make, make()->tail_samples() + 2 * reverb::VelvetReverb::kEarlyPartitionSamples);
  }
  SECTION("stereo delay") {
    delay::StereoDelayConfig config;
    config.delay_time_l_ms = 40.0f;
    config.delay_time_r_ms = 55.0f;
    config.feedback = 0.5f;
    config.ping_pong = 0.5f;
    config.dry_wet = 0.5f;
    const auto make = prepared<delay::StereoDelay>(config);
    check_discard_is_counted(make, make()->tail_samples());
  }
  SECTION("flanger") {
    modulation::FlangerConfig config;
    config.feedback = 0.8f;
    config.dry_wet = 0.5f;
    check_discard_is_counted(prepared<modulation::Flanger>(config),
                             ms_to_samples(config.center_delay_ms + config.depth_ms));
  }
  SECTION("phaser") {
    modulation::PhaserConfig config;
    config.dry_wet = 0.5f;
    check_discard_is_counted(prepared<modulation::Phaser>(config), kSingleSampleHistory);
  }
  SECTION("rotary") {
    modulation::RotaryConfig config;
    config.dry_wet = 1.0f;
    check_discard_is_counted(prepared<modulation::Rotary>(config),
                             ms_to_samples(2.0f * config.depth_ms));
  }
  SECTION("ensemble") {
    modulation::EnsembleConfig config;
    config.dry_wet = 1.0f;
    check_discard_is_counted(
        prepared<modulation::Ensemble>(config),
        ms_to_samples(config.center_delay_ms + config.depth_slow_ms + config.depth_fast_ms));
  }
  SECTION("auto-wah") {
    modulation::AutoWahConfig config;
    config.dry_wet = 1.0f;
    check_discard_is_counted(prepared<modulation::AutoWah>(config), kSingleSampleHistory);
  }
  SECTION("wah") {
    modulation::WahConfig config;
    config.dry_wet = 1.0f;
    check_discard_is_counted(prepared<modulation::Wah>(config), kSingleSampleHistory);
  }
}

#ifdef SONARE_WITH_ACOUSTIC_SIM
TEST_CASE("the room morph's tail expander is returned to rest", "[effects][non_finite][acoustic]") {
  using sonare::RoomDimensions;
  using sonare::acoustic::uniform_shoebox;
  using sonare::effects::acoustic::RoomMorphConfig;
  using sonare::effects::acoustic::RoomMorphProcessor;

  RoomMorphConfig config;
  const RoomDimensions dims{4.0f, 3.0f, 2.5f};
  config.target = uniform_shoebox(dims, 0.4f);
  config.placement.source = {1.0f, 1.0f, 1.2f};
  config.placement.listener = {2.6f, 2.0f, 1.2f};
  // A short, low-order target RIR: the convolution is fed the suppressed signal,
  // so its length is the window a poisoned sample stays audible for.
  config.ism_order = 1;
  config.max_seconds = 0.2f;
  config.source_tail_suppression = 1.0f;
  config.wet = 0.5f;

  const auto make = prepared<RoomMorphProcessor>(config);
  const auto probe = make();
  // The target impulse response, plus the convolution's staging and overlap-add
  // partitions, each one reported latency long.
  check_recovery(make, probe->target_ir_size() + 2 * probe->latency_samples(),
                 probe->latency_samples());
}
#endif  // SONARE_WITH_ACOUSTIC_SIM
