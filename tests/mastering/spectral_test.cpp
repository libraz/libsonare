#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "mastering/spectral/air_band.h"
#include "mastering/spectral/low_end_focus.h"
#include "mastering/spectral/presence_enhancer.h"
#include "mastering/spectral/spectral_shaper.h"
#include "support/audio_fixtures.h"

using namespace sonare::mastering::spectral;

namespace {
using sonare::test::generate_sine_samples;
using sonare::test::max_abs_difference;
using sonare::test::peak_abs;
using sonare::test::process;
using sonare::test::process_stereo;
using sonare::test::rms;
using sonare::test::rms_tail;

}  // namespace

TEST_CASE("SpectralShaper reduces material above threshold", "[mastering][spectral]") {
  SpectralShaper shaper({0.05f, 1.0f, 500.0f, 8000.0f, 0.0f, 0.0f, 24.0f});
  shaper.prepare(48000.0, 512);

  auto signal = generate_sine_samples(4000.0f, 48000, 512, 0.5f);
  const float before = rms_tail(signal, 128);
  process(shaper, signal);

  REQUIRE(rms_tail(signal, 128) < before * 0.75f);
  REQUIRE(shaper.last_reduction_db() < 0.0f);
}

TEST_CASE("SpectralShaper targets high-band energy more than low-band energy",
          "[mastering][spectral]") {
  SpectralShaper shaper({0.05f, 1.0f, 2000.0f, 6000.0f, 0.0f, 0.0f, 24.0f});
  shaper.prepare(48000.0, 2048);

  auto low = generate_sine_samples(500.0f, 48000, 2048, 0.5f);
  auto target = generate_sine_samples(4000.0f, 48000, 2048, 0.5f);

  const float low_before = rms_tail(low, 512);
  const float target_before = rms_tail(target, 512);
  process(shaper, low);
  shaper.reset();
  process(shaper, target);

  REQUIRE(rms_tail(low, 512) > low_before * 0.85f);
  REQUIRE(rms_tail(target, 512) < target_before * 0.7f);
}

TEST_CASE("SpectralShaper attack slows initial gain reduction", "[mastering][spectral]") {
  SpectralShaper fast({0.05f, 1.0f, 2000.0f, 6000.0f, 0.0f, 20.0f, 24.0f});
  SpectralShaper slow({0.05f, 1.0f, 2000.0f, 6000.0f, 50.0f, 20.0f, 24.0f});
  fast.prepare(48000.0, 256);
  slow.prepare(48000.0, 256);

  auto fast_signal = generate_sine_samples(4000.0f, 48000, 256, 0.5f);
  auto slow_signal = fast_signal;
  const float before = rms(fast_signal);
  process(fast, fast_signal);
  process(slow, slow_signal);

  REQUIRE(rms(fast_signal) < before * 0.8f);
  REQUIRE(rms(slow_signal) > rms(fast_signal) * 1.1f);
}

TEST_CASE("SpectralShaper release controls gain recovery", "[mastering][spectral]") {
  SpectralShaper fast({0.05f, 1.0f, 2000.0f, 6000.0f, 0.0f, 0.0f, 24.0f});
  SpectralShaper slow({0.05f, 1.0f, 2000.0f, 6000.0f, 0.0f, 200.0f, 24.0f});
  fast.prepare(48000.0, 512);
  slow.prepare(48000.0, 512);

  auto loud_fast = generate_sine_samples(4000.0f, 48000, 512, 0.5f);
  auto loud_slow = loud_fast;
  process(fast, loud_fast);
  process(slow, loud_slow);

  std::vector<float> silence_fast(4800, 0.0f);
  std::vector<float> silence_slow(4800, 0.0f);
  process(fast, silence_fast);
  process(slow, silence_slow);

  std::vector<float> probe_fast(1, 0.0f);
  std::vector<float> probe_slow(1, 0.0f);
  process(fast, probe_fast);
  process(slow, probe_slow);

  REQUIRE(fast.last_reduction_db() > -0.001f);
  REQUIRE(slow.last_reduction_db() < -1.0f);
}

TEST_CASE("LowEndFocus narrows low-frequency stereo difference", "[mastering][spectral]") {
  LowEndFocus focus({20000.0f, 0.0f});
  focus.prepare(48000.0, 8);

  std::vector<float> left(32, 1.0f);
  std::vector<float> right(32, -1.0f);
  float* channels[] = {left.data(), right.data()};
  focus.process(channels, 2, static_cast<int>(left.size()));

  REQUIRE(std::abs(left.back() - right.back()) < 2.0f);
}

TEST_CASE("LowEndFocus adds octave-down subharmonic energy", "[mastering][spectral]") {
  LowEndFocus dry({160.0f, 1.0f, 0.0f, 0.0f});
  LowEndFocus sub({160.0f, 1.0f, 0.8f, 0.0f});
  dry.prepare(48000.0, 4096);
  sub.prepare(48000.0, 4096);

  auto dry_signal = generate_sine_samples(80.0f, 48000, 48000, 0.2f);
  auto sub_signal = dry_signal;
  process(dry, dry_signal);
  process(sub, sub_signal);

  REQUIRE(rms_tail(sub_signal, 4096) > rms_tail(dry_signal, 4096) * 1.05f);
}

TEST_CASE("LowEndFocus transient tightness emphasizes low-end attacks", "[mastering][spectral]") {
  LowEndFocus dry({180.0f, 1.0f, 0.0f, 0.0f});
  LowEndFocus tight({180.0f, 1.0f, 0.0f, 0.8f});
  dry.prepare(48000.0, 256);
  tight.prepare(48000.0, 256);

  std::vector<float> dry_signal(256, 0.0f);
  std::vector<float> tight_signal(256, 0.0f);
  for (size_t i = 16; i < dry_signal.size(); ++i) {
    dry_signal[i] = 0.4f;
    tight_signal[i] = 0.4f;
  }

  process(dry, dry_signal);
  process(tight, tight_signal);

  REQUIRE(tight_signal[16] > dry_signal[16]);
  REQUIRE(tight_signal[32] > dry_signal[32]);
}

TEST_CASE("AirBand adds high-frequency detail", "[mastering][spectral]") {
  AirBand air({0.5f});
  air.prepare(48000.0, 16);

  std::vector<float> signal = {0.0f, 0.4f, -0.4f, 0.4f, -0.4f};
  const float before = peak_abs(signal);
  process(air, signal);

  REQUIRE(peak_abs(signal) > before);
}

TEST_CASE("AirBand emphasizes high band more than low band", "[mastering][spectral]") {
  constexpr int sample_rate = 48000;
  AirBand air({0.6f, 10000.0f, -60.0f, 6.0f});
  air.prepare(sample_rate, 1024);

  auto low = generate_sine_samples(500.0f, sample_rate, sample_rate, 0.2f);
  auto high = generate_sine_samples(14000.0f, sample_rate, sample_rate, 0.2f);
  const float low_before = rms_tail(low, 4096);
  const float high_before = rms_tail(high, 4096);

  process(air, low);
  air.reset();
  process(air, high);

  const float low_gain = rms_tail(low, 4096) / low_before;
  const float high_gain = rms_tail(high, 4096) / high_before;
  REQUIRE(high_gain > low_gain);
}

TEST_CASE("AirBand preserves previous sample across process blocks", "[mastering][spectral]") {
  std::vector<float> full = {0.2f, -0.4f, 0.3f, -0.1f, 0.5f};
  std::vector<float> first = {full[0], full[1]};
  std::vector<float> second = {full[2], full[3], full[4]};

  AirBand one_shot({0.4f});
  AirBand split({0.4f});
  one_shot.prepare(48000.0, 16);
  split.prepare(48000.0, 16);

  process(one_shot, full);
  process(split, first);
  process(split, second);

  REQUIRE(first[0] == full[0]);
  REQUIRE(first[1] == full[1]);
  REQUIRE(second[0] == full[2]);
  REQUIRE(second[1] == full[3]);
  REQUIRE(second[2] == full[4]);
}

TEST_CASE("AirBand preserves existing channel state when channel count grows",
          "[mastering][spectral]") {
  AirBand mono_path({0.4f});
  AirBand stereo_path({0.4f});
  mono_path.prepare(48000.0, 1024);
  stereo_path.prepare(48000.0, 1024);

  auto warmup = generate_sine_samples(14000.0f, 48000, 4096, 0.3f);
  auto warmup_copy = warmup;
  process(mono_path, warmup);
  process(stereo_path, warmup_copy);

  auto expected_left = generate_sine_samples(14000.0f, 48000, 512, 0.3f);
  auto actual_left = expected_left;
  auto actual_right = expected_left;
  process(mono_path, expected_left);
  process_stereo(stereo_path, actual_left, actual_right);

  REQUIRE(max_abs_difference(actual_left, expected_left) < 1.0e-6f);
}

TEST_CASE("AirBand does not hard clip when processing is bypassed", "[mastering][spectral]") {
  AirBand air({0.0f});
  air.prepare(48000.0, 16);

  std::vector<float> signal = {2.0f, -2.0f};
  process(air, signal);

  REQUIRE(signal[0] > 1.9f);
  REQUIRE(signal[1] < -1.9f);
}

TEST_CASE("PresenceEnhancer increases RMS with harmonic drive", "[mastering][spectral]") {
  PresenceEnhancer enhancer({0.3f, 3.0f});
  enhancer.prepare(48000.0, 16);

  std::vector<float> signal = {0.1f, -0.1f, 0.2f, -0.2f};
  const float before = rms(signal);
  process(enhancer, signal);

  REQUIRE(rms(signal) > before);
}

TEST_CASE("PresenceEnhancer focuses harmonic drive near presence band", "[mastering][spectral]") {
  constexpr int sample_rate = 48000;
  PresenceEnhancer enhancer({0.4f, 4.0f, 3000.0f, 1.2f});
  enhancer.prepare(sample_rate, 1024);

  auto presence = generate_sine_samples(3000.0f, sample_rate, sample_rate, 0.2f);
  auto low = generate_sine_samples(200.0f, sample_rate, sample_rate, 0.2f);
  const float presence_before = rms_tail(presence, 4096);
  const float low_before = rms_tail(low, 4096);

  process(enhancer, presence);
  enhancer.reset();
  process(enhancer, low);

  REQUIRE(rms_tail(presence, 4096) / presence_before > rms_tail(low, 4096) / low_before + 0.15f);
}

TEST_CASE("PresenceEnhancer preserves existing channel state when channel count grows",
          "[mastering][spectral]") {
  PresenceEnhancer mono_path({0.4f, 4.0f, 3000.0f, 1.2f});
  PresenceEnhancer stereo_path({0.4f, 4.0f, 3000.0f, 1.2f});
  mono_path.prepare(48000.0, 1024);
  stereo_path.prepare(48000.0, 1024);

  auto warmup = generate_sine_samples(3000.0f, 48000, 4096, 0.3f);
  auto warmup_copy = warmup;
  process(mono_path, warmup);
  process(stereo_path, warmup_copy);

  auto expected_left = generate_sine_samples(3000.0f, 48000, 512, 0.3f);
  auto actual_left = expected_left;
  auto actual_right = expected_left;
  process(mono_path, expected_left);
  process_stereo(stereo_path, actual_left, actual_right);

  REQUIRE(max_abs_difference(actual_left, expected_left) < 1.0e-6f);
}

TEST_CASE("PresenceEnhancer does not hard clip when processing is bypassed",
          "[mastering][spectral]") {
  PresenceEnhancer enhancer({0.0f, 3.0f});
  enhancer.prepare(48000.0, 16);

  std::vector<float> signal = {2.0f, -2.0f};
  process(enhancer, signal);

  REQUIRE(signal[0] > 1.9f);
  REQUIRE(signal[1] < -1.9f);
}

TEST_CASE("Spectral processors validate configuration and state", "[mastering][spectral]") {
  REQUIRE_THROWS(SpectralShaper({0.1f, 1.5f}));
  REQUIRE_THROWS(SpectralShaper({0.1f, 0.5f, 6000.0f, 2000.0f}));
  REQUIRE_THROWS(LowEndFocus({0.0f, 0.0f}));
  REQUIRE_THROWS(LowEndFocus({120.0f, 1.0f, -0.1f, 0.0f}));
  REQUIRE_THROWS(LowEndFocus({120.0f, 1.0f, 0.0f, 1.1f}));
  REQUIRE_THROWS(AirBand({-0.1f}));
  REQUIRE_THROWS(AirBand({0.1f, 0.0f}));
  REQUIRE_THROWS(PresenceEnhancer({0.1f, 0.0f}));
  REQUIRE_THROWS(PresenceEnhancer({0.1f, 1.0f, 0.0f}));

  SpectralShaper unprepared;
  std::vector<float> signal(4, 0.0f);
  float* channels[] = {signal.data()};
  REQUIRE_THROWS(unprepared.process(channels, 1, 4));
}
