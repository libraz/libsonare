/// @file insert_bit_identity_test.cpp
/// @brief Every GS-EFX-driven insert (chorus / flanger / phaser / stereo
///        delay / rotary / pitch shifter / bitcrusher) runs through
///        insert_factory's `"{}"` path for the entire GM default rig
///        (gm_fallback_map.cpp's kRigStages) and every GM bounce, and no
///        golden covers it (gm_program_golden_test.cpp never injects a
///        factory). This is the only net for a config field whose default
///        silently moves that path: (a) `"{}"` must render bit-identical to
///        every parameter spelled out at its current default, and (b) two
///        scalars of the `"{}"` render are pinned against today's measured
///        values.
///
/// None of these seven defaults produces a static notch or null the fixed
/// FFT window here can resolve -- each is either swept (phaser, rotary,
/// chorus/flanger LFOs) or a comb spaced too narrowly for the bin resolution
/// (the stereo delay's 250 ms tap). RMS plus spectral centroid is what is
/// actually readable and is used uniformly instead.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <string>
#include <vector>

#include "mastering/api/insert_factory.h"
#include "support/audio_fixtures.h"

namespace {

using sonare::mastering::api::make_insert;
using sonare::test::generate_sine;
using sonare::test::kRate;
using sonare::test::process_stereo;
using sonare::test::rms;
using sonare::test::spectral_centroid;

constexpr int kNumSamples = 14400;  // 0.3 s at 48000 Hz.
// Skips each processor's startup transient before the scalars are read, the
// same discipline the sibling effects tests use.
constexpr std::size_t kMeasureSkip = 2000;

/// A fixed two-tone signal (a mono source panned centre, identical on both
/// channels) -- deterministic, no RNG.
std::vector<float> test_signal() {
  const std::vector<float> a = generate_sine(kNumSamples, 220.0f, static_cast<int>(kRate), 0.25f);
  const std::vector<float> b = generate_sine(kNumSamples, 330.0f, static_cast<int>(kRate), 0.15f);
  std::vector<float> sum(a.size());
  for (std::size_t i = 0; i < sum.size(); ++i) sum[i] = a[i] + b[i];
  return sum;
}

struct RenderResult {
  std::vector<float> left;
  std::vector<float> right;
};

RenderResult render(const std::string& name, const std::string& json) {
  auto processor = make_insert(name, json);
  REQUIRE(processor != nullptr);
  processor->prepare(kRate, kNumSamples);
  RenderResult out{test_signal(), test_signal()};
  process_stereo(*processor, out.left, out.right);
  return out;
}

struct Scalars {
  float rms_value;
  float centroid_hz;
};

Scalars measure(const std::vector<float>& buf) {
  return {rms(buf, kMeasureSkip), static_cast<float>(spectral_centroid(buf, kMeasureSkip))};
}

// Measured "{}" behaviour: RMS and spectral centroid (Hz) of the left channel
// after the two-tone test signal, skip and FFT window above. Pins today's
// default DSP so a later config-field addition cannot move it silently.
constexpr float kPhaserRms = 0.142598107f;
constexpr float kPhaserCentroidHz = 232.994003f;
constexpr float kRotaryRms = 0.176608101f;
constexpr float kRotaryCentroidHz = 249.350067f;
constexpr float kChorusRms = 0.136745155f;
constexpr float kChorusCentroidHz = 241.363144f;
constexpr float kFlangerRms = 0.150896922f;
constexpr float kFlangerCentroidHz = 275.08844f;
constexpr float kStereoDelayRms = 0.121026672f;
constexpr float kStereoDelayCentroidHz = 249.117661f;
constexpr float kPitchShifterRms = 0.206451163f;
constexpr float kPitchShifterCentroidHz = 249.117661f;
constexpr float kBitcrusherRms = 0.206448466f;
constexpr float kBitcrusherCentroidHz = 249.123016f;

void require_pinned(const Scalars& s, float expected_rms, float expected_centroid_hz) {
  REQUIRE(s.rms_value == Catch::Approx(expected_rms).epsilon(1e-6));
  REQUIRE(s.centroid_hz == Catch::Approx(expected_centroid_hz).epsilon(1e-6));
}

}  // namespace

TEST_CASE(
    "GS-EFX-driven inserts are bit-identical under \"{}\" and today's scalar "
    "behaviour is pinned",
    "[insert-bit-identity]") {
  SECTION("effects.modulation.phaser") {
    const RenderResult empty = render("effects.modulation.phaser", "{}");
    const RenderResult explicit_defaults = render(
        "effects.modulation.phaser",
        R"({"rateHz":0.4,"minHz":300.0,"maxHz":1600.0,"stages":4,"dryWet":0.5,"feedback":0.0,"mixMode":0})");
    REQUIRE(empty.left == explicit_defaults.left);
    REQUIRE(empty.right == explicit_defaults.right);
    const Scalars s = measure(empty.left);
    require_pinned(s, kPhaserRms, kPhaserCentroidHz);
  }

  SECTION("effects.modulation.rotary") {
    const RenderResult empty = render("effects.modulation.rotary", "{}");
    const RenderResult explicit_defaults = render(
        "effects.modulation.rotary",
        R"({"rateHz":6.0,"drumRateHz":4.44,"depthMs":1.2,"tremolo":0.5,"stereoSpread":1.0,"dryWet":1.0,"accelTauS":0.0,"decelTauS":0.0,"undershootHz":0.0,"drumUndershootHz":0.0})");
    REQUIRE(empty.left == explicit_defaults.left);
    REQUIRE(empty.right == explicit_defaults.right);
    const Scalars s = measure(empty.left);
    require_pinned(s, kRotaryRms, kRotaryCentroidHz);
  }

  SECTION("effects.modulation.chorus") {
    const RenderResult empty = render("effects.modulation.chorus", "{}");
    const RenderResult explicit_defaults = render(
        "effects.modulation.chorus",
        R"({"rateHz":0.8,"depthMs":6.0,"centerDelayMs":14.0,"dryWet":0.5,"preFilterHz":0.0,"preFilterMode":0})");
    REQUIRE(empty.left == explicit_defaults.left);
    REQUIRE(empty.right == explicit_defaults.right);
    const Scalars s = measure(empty.left);
    require_pinned(s, kChorusRms, kChorusCentroidHz);
  }

  SECTION("effects.modulation.flanger") {
    const RenderResult empty = render("effects.modulation.flanger", "{}");
    const RenderResult explicit_defaults = render(
        "effects.modulation.flanger",
        R"({"rateHz":0.25,"depthMs":2.0,"centerDelayMs":3.0,"feedback":0.3,"dryWet":0.5,"preFilterHz":0.0,"preFilterMode":0})");
    REQUIRE(empty.left == explicit_defaults.left);
    REQUIRE(empty.right == explicit_defaults.right);
    const Scalars s = measure(empty.left);
    require_pinned(s, kFlangerRms, kFlangerCentroidHz);
  }

  SECTION("effects.delay.stereo") {
    const RenderResult empty = render("effects.delay.stereo", "{}");
    const RenderResult explicit_defaults = render(
        "effects.delay.stereo",
        R"({"delayTimeLMs":250.0,"delayTimeRMs":250.0,"feedback":0.25,"pingPong":0.0,"dryWet":0.5,"dampingHz":0.0})");
    REQUIRE(empty.left == explicit_defaults.left);
    REQUIRE(empty.right == explicit_defaults.right);
    const Scalars s = measure(empty.left);
    require_pinned(s, kStereoDelayRms, kStereoDelayCentroidHz);
  }

  SECTION("effects.modulation.pitchShifter") {
    const RenderResult empty = render("effects.modulation.pitchShifter", "{}");
    const RenderResult explicit_defaults = render(
        "effects.modulation.pitchShifter", R"({"semitones":0.0,"dryWet":1.0,"windowMs":22.5})");
    REQUIRE(empty.left == explicit_defaults.left);
    REQUIRE(empty.right == explicit_defaults.right);
    const Scalars s = measure(empty.left);
    require_pinned(s, kPitchShifterRms, kPitchShifterCentroidHz);
  }

  SECTION("saturation.bitcrusher") {
    const RenderResult empty = render("saturation.bitcrusher", "{}");
    const RenderResult explicit_defaults = render(
        "saturation.bitcrusher",
        R"({"bitDepth":12,"downsampleFactor":1,"mix":1.0,"ditherType":0,"ditherSeed":5351397,"holdHz":0.0,"quantizerMode":0})");
    REQUIRE(empty.left == explicit_defaults.left);
    REQUIRE(empty.right == explicit_defaults.right);
    const Scalars s = measure(empty.left);
    require_pinned(s, kBitcrusherRms, kBitcrusherCentroidHz);
  }
}
