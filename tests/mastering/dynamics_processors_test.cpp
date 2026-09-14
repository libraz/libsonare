/// @file dynamics_processors_test.cpp
/// @brief Transient, parallel, rider, sidechain, and linked behavior tests.

#include <array>
#include <memory>

#include "dynamics_test_helpers.h"
#include "util/db.h"
#include "util/exception.h"

TEST_CASE("DeEsser attenuates sibilant band more than low band", "[mastering][dynamics]") {
  // Split-band de-esser: the gain reduction is applied only to the detected
  // sibilant band, so it cleanly attenuates content AT the detection centre and
  // leaves the low band virtually untouched. (A tone well off-centre is only a
  // small perturbation by design; this is the split-band behaviour, not the old
  // wideband attenuation that pulled down everything.)
  DeEsser deesser({5000.0f, -28.0f, 6.0f, 0.0f, 20.0f, 18.0f});
  deesser.prepare(48000.0, 1024);

  auto high = generate_sine_samples(5000.0f, 48000, 48000, 0.5f);
  auto low = generate_sine_samples(1000.0f, 48000, 48000, 0.5f);
  const float high_before = rms_tail(high, 4096);
  const float low_before = rms_tail(low, 4096);

  process(deesser, high);
  const float high_reduction_db = deesser.last_gain_reduction_db();
  deesser.reset();
  process(deesser, low);

  REQUIRE(rms_tail(high, 4096) / high_before < 0.65f);
  REQUIRE(rms_tail(low, 4096) / low_before > 0.75f);
  REQUIRE(high_reduction_db < -1.0f);
}

TEST_CASE("DeEsser exposes configurable bandpass Q", "[mastering][dynamics]") {
  DeEsser deesser;
  deesser.prepare(48000.0, 1024);
  REQUIRE(deesser.set_parameter(6, 2.5f));
  REQUIRE_THAT(deesser.config().bandpass_q, WithinAbs(2.5f, 1.0e-6f));
  REQUIRE_FALSE(deesser.set_parameter(7, 1.0f));
  REQUIRE_THROWS(DeEsser({6000.0f, -24.0f, 4.0f, 1.0f, 60.0f, 12.0f, 0.0f}));
}

TEST_CASE("DeEsser preserves existing channel filter state when channel count grows",
          "[mastering][dynamics]") {
  DeEsser mono_path({5000.0f, -28.0f, 6.0f, 20.0f, 80.0f, 18.0f});
  DeEsser stereo_path({5000.0f, -28.0f, 6.0f, 20.0f, 80.0f, 18.0f});
  mono_path.prepare(48000.0, 1024);
  stereo_path.prepare(48000.0, 1024);

  auto warmup = generate_sine_samples(8000.0f, 48000, 4096, 0.5f);
  auto warmup_copy = warmup;
  process(mono_path, warmup);
  process(stereo_path, warmup_copy);

  auto expected_left = generate_sine_samples(8000.0f, 48000, 512, 0.5f);
  auto actual_left = expected_left;
  auto actual_right = expected_left;
  process(mono_path, expected_left);
  process_stereo(stereo_path, actual_left, actual_right);

  REQUIRE(max_abs_difference(actual_left, expected_left) < 1.0e-6f);
}

TEST_CASE("DeEsser keeps inactive stereo channel state across mono blocks",
          "[mastering][dynamics]") {
  DeEsser reference({5000.0f, -28.0f, 6.0f, 20.0f, 80.0f, 18.0f});
  DeEsser under_test({5000.0f, -28.0f, 6.0f, 20.0f, 80.0f, 18.0f});
  reference.prepare(48000.0, 1024);
  under_test.prepare(48000.0, 1024);

  auto warm_l = generate_sine_samples(8000.0f, 48000, 4096, 0.5f);
  auto warm_r = generate_sine_samples(8000.0f, 48000, 4096, 0.5f);
  auto test_warm_l = warm_l;
  auto test_warm_r = warm_r;
  process_stereo(reference, warm_l, warm_r);
  process_stereo(under_test, test_warm_l, test_warm_r);

  auto mono = generate_sine_samples(8000.0f, 48000, 512, 0.5f);
  process(under_test, mono);

  auto ref_l = generate_sine_samples(8000.0f, 48000, 512, 0.5f);
  auto ref_r = ref_l;
  auto test_l = ref_l;
  auto test_r = ref_r;
  process_stereo(reference, ref_l, ref_r);
  process_stereo(under_test, test_l, test_r);

  REQUIRE(max_abs_difference(test_r, ref_r) < 1.0e-6f);
}

TEST_CASE("DeEsser validates configuration", "[mastering][dynamics]") {
  REQUIRE_THROWS(DeEsser({0.0f, -24.0f, 4.0f, 1.0f, 60.0f, 12.0f}));
  REQUIRE_THROWS(DeEsser({6000.0f, -24.0f, 0.5f, 1.0f, 60.0f, 12.0f}));
  REQUIRE_THROWS(DeEsser({6000.0f, -24.0f, 4.0f, -1.0f, 60.0f, 12.0f}));
  REQUIRE_THROWS(DeEsser({6000.0f, -24.0f, 4.0f, 1.0f, 60.0f, -1.0f}));
}

TEST_CASE("TransientShaper boosts attacks and can reduce sustain", "[mastering][dynamics]") {
  std::vector<float> impulse(48000, 0.0f);
  impulse[0] = 0.5f;

  auto attack = impulse;
  TransientShaper attack_shaper({6.0f, 0.0f, 0.0f, 20.0f, 20.0f, 200.0f, 1.0f, 12.0f});
  attack_shaper.prepare(48000.0, 1024);
  process(attack_shaper, attack);

  REQUIRE(peak_abs(attack) > peak_abs(impulse) * 1.5f);
  REQUIRE(attack_shaper.last_gain_db() > 4.0f);

  std::vector<float> tail(48000);
  for (size_t i = 0; i < tail.size(); ++i) {
    tail[i] = 0.5f * std::exp(-static_cast<float>(i) / 8000.0f);
  }
  auto shaped_tail = tail;

  TransientShaper sustain_shaper({0.0f, -9.0f, 0.0f, 15.0f, 10.0f, 400.0f, 1.0f, 12.0f});
  sustain_shaper.prepare(48000.0, 1024);
  process(sustain_shaper, shaped_tail);

  REQUIRE(rms_tail(shaped_tail, 4096) < rms_tail(tail, 4096) * 0.8f);
  REQUIRE(sustain_shaper.last_gain_db() < -2.0f);
}

TEST_CASE("TransientShaper validates configuration", "[mastering][dynamics]") {
  REQUIRE_THROWS(TransientShaper({3.0f, 0.0f, -1.0f, 20.0f, 15.0f, 200.0f, 1.0f, 12.0f}));
  REQUIRE_THROWS(TransientShaper({3.0f, 0.0f, 0.0f, 20.0f, 15.0f, 200.0f, -1.0f, 12.0f}));
  REQUIRE_THROWS(TransientShaper({3.0f, 0.0f, 0.0f, 20.0f, 15.0f, 200.0f, 1.0f, -1.0f}));
}

TEST_CASE("TransientShaper supports lookahead and gain smoothing", "[mastering][dynamics]") {
  TransientShaper shaper({6.0f, 0.0f, 0.0f, 20.0f, 20.0f, 200.0f, 1.0f, 12.0f, 1.0f, 1.0f});
  shaper.prepare(1000.0, 8);

  // The lookahead delay is reported as latency for host PDC (1 ms @ 1 kHz = 1 sample).
  REQUIRE(shaper.latency_samples() == 1);

  std::vector<float> impulse(8, 0.0f);
  impulse[1] = 0.5f;
  process(shaper, impulse);

  REQUIRE_THAT(impulse[0], WithinAbs(0.0f, 0.0001f));
  REQUIRE(peak_abs(impulse) > 0.5f);
}

TEST_CASE("TransientShaper reports zero latency without lookahead", "[mastering][dynamics]") {
  TransientShaper shaper;  // lookahead_ms defaults to 0
  shaper.prepare(48000.0, 256);
  REQUIRE(shaper.latency_samples() == 0);
}

TEST_CASE("ParallelComp blends dry and compressed signals", "[mastering][dynamics]") {
  auto dry = generate_sine_samples(1000.0f, 48000, 48000, 0.8f);
  auto half = dry;
  auto wet = dry;

  ParallelComp half_comp({-18.0f, 6.0f, 0.0f, 20.0f, 0.0f, 0.5f});
  ParallelComp wet_comp({-18.0f, 6.0f, 0.0f, 20.0f, 0.0f, 1.0f});
  half_comp.prepare(48000.0, 1024);
  wet_comp.prepare(48000.0, 1024);

  process(half_comp, half);
  process(wet_comp, wet);

  const float dry_rms = rms_tail(dry, 4096);
  const float half_rms = rms_tail(half, 4096);
  const float wet_rms = rms_tail(wet, 4096);
  REQUIRE(wet_rms < dry_rms * 0.5f);
  REQUIRE(half_rms > wet_rms);
  REQUIRE(half_rms < dry_rms);
  REQUIRE(wet_comp.last_gain_reduction_db() < -6.0f);
}

TEST_CASE("ParallelComp attack detector operates in amplitude domain", "[mastering][dynamics]") {
  std::vector<float> transient = {1.0f};
  ParallelComp compressor({-18.0f, 20.0f, 10.0f, 20.0f, 0.0f, 1.0f});
  compressor.prepare(1000.0, 1);

  process(compressor, transient);

  REQUIRE(transient[0] > 0.95f);
  REQUIRE_THAT(compressor.last_gain_reduction_db(), WithinAbs(0.0f, 0.0001f));
}

TEST_CASE("ParallelComp links stereo detection and limits wet output", "[mastering][dynamics]") {
  ParallelComp compressor({-18.0f, 20.0f, 0.0f, 20.0f, 12.0f, 1.0f, true, true, -3.0f});
  compressor.prepare(48000.0, 1024);

  std::vector<float> left(48000, 0.9f);
  std::vector<float> right(48000, 0.05f);
  process_stereo(compressor, left, right);

  REQUIRE(rms_tail(right, 4096) < 0.04f);
  REQUIRE(peak_abs(left, 4096) <= 0.708f);
}

TEST_CASE("ParallelComp output limiter releases gain instead of hard clipping",
          "[mastering][dynamics]") {
  ParallelComp compressor({100.0f, 1.0f, 0.0f, 100.0f, 0.0f, 0.0f, true, true, 0.0f});
  compressor.prepare(1000.0, 2);

  std::vector<float> samples = {2.0f, 0.5f};
  process(compressor, samples);

  REQUIRE_THAT(samples[0], WithinAbs(1.0f, 1e-6f));
  REQUIRE(samples[1] > 0.0f);
  REQUIRE(samples[1] < 0.5f);
}

TEST_CASE("ParallelComp validates configuration", "[mastering][dynamics]") {
  REQUIRE_THROWS(ParallelComp({-18.0f, 0.5f, 10.0f, 100.0f, 0.0f, 0.5f}));
  REQUIRE_THROWS(ParallelComp({-18.0f, 2.0f, -1.0f, 100.0f, 0.0f, 0.5f}));
  REQUIRE_THROWS(ParallelComp({-18.0f, 2.0f, 10.0f, 100.0f, 0.0f, 1.5f}));
}

TEST_CASE("VocalRider boosts quiet material and cuts loud material", "[mastering][dynamics]") {
  VocalRider rider({-18.0f, 9.0f, 9.0f, 0.0f, 50.0f, 0.0f});
  rider.prepare(48000.0, 1024);

  std::vector<float> quiet(48000, 0.05f);
  std::vector<float> loud(48000, 0.8f);
  const float quiet_before = rms_tail(quiet, 4096);
  const float loud_before = rms_tail(loud, 4096);

  process(rider, quiet);
  rider.reset();
  process(rider, loud);

  REQUIRE(rms_tail(quiet, 4096) > quiet_before * 1.7f);
  REQUIRE(rms_tail(loud, 4096) < loud_before * 0.45f);
  REQUIRE(rider.last_gain_db() < -6.0f);
}

TEST_CASE("VocalRider links stereo detection and respects noise floor", "[mastering][dynamics]") {
  VocalRider rider({-18.0f, 9.0f, 9.0f, 0.0f, 50.0f, 0.0f, 0.0f, -50.0f, true});
  rider.prepare(48000.0, 1024);

  std::vector<float> left(48000, 0.8f);
  std::vector<float> right(48000, 0.02f);
  process_stereo(rider, left, right);
  REQUIRE(rms_tail(right, 4096) < 0.01f);

  rider.reset();
  std::vector<float> noise(48000, 0.0001f);
  const float before = rms_tail(noise, 4096);
  process(rider, noise);
  REQUIRE(rms_tail(noise, 4096) < before * 1.1f);
}

TEST_CASE("VocalRider validates configuration", "[mastering][dynamics]") {
  REQUIRE_THROWS(VocalRider({-18.0f, -1.0f, 6.0f, 50.0f, 500.0f, 0.0f}));
  REQUIRE_THROWS(VocalRider({-18.0f, 6.0f, -1.0f, 50.0f, 500.0f, 0.0f}));
  REQUIRE_THROWS(VocalRider({-18.0f, 6.0f, 6.0f, -1.0f, 500.0f, 0.0f}));
}

TEST_CASE("VocalRider unlinked detection allocates per-channel gain state",
          "[mastering][dynamics]") {
  VocalRiderConfig config;
  config.linked_detection = false;
  VocalRider rider(config);
  rider.prepare(48000.0, 128);

  std::vector<float> left(128, 0.2f);
  std::vector<float> right(128, 0.05f);
  float* channels[] = {left.data(), right.data()};

  REQUIRE_NOTHROW(rider.process(channels, 2, 128));
  REQUIRE(std::isfinite(left.front()));
  REQUIRE(std::isfinite(right.front()));
}

TEST_CASE("SidechainRouter ducks from an external detector", "[mastering][dynamics]") {
  SidechainRouter router({-30.0f, 4.0f, 0.0f, 20.0f, 18.0f});
  router.prepare(48000.0, 1024);

  std::vector<float> target(48000, 0.01f);
  std::vector<float> no_sidechain = target;
  std::vector<float> sidechain(48000, 0.8f);
  const float* sidechain_channels[] = {sidechain.data()};

  process(router, no_sidechain);
  router.reset();
  router.set_sidechain(sidechain_channels, 1, static_cast<int>(sidechain.size()));
  process(router, target);

  REQUIRE(rms_tail(no_sidechain, 4096) > 0.009f);
  REQUIRE(rms_tail(target, 4096) < rms_tail(no_sidechain, 4096) * 0.25f);
  REQUIRE(router.last_gain_reduction_db() < -10.0f);
}

TEST_CASE("SidechainRouter validates configuration and sidechain buffers",
          "[mastering][dynamics]") {
  REQUIRE_THROWS(SidechainRouter({-24.0f, 0.5f, 5.0f, 100.0f, 18.0f}));
  REQUIRE_THROWS(SidechainRouter({-24.0f, 4.0f, -1.0f, 100.0f, 18.0f}));
  REQUIRE_THROWS(SidechainRouter({-24.0f, 4.0f, 5.0f, 100.0f, -1.0f}));
  REQUIRE_THROWS(
      SidechainRouter({-24.0f, 4.0f, 5.0f, 100.0f, 18.0f, false, 90.0f, false, false, -1.0f}));
  REQUIRE_NOTHROW(
      SidechainRouter({-24.0f, 4.0f, 5.0f, 100.0f, 18.0f, false, 0.0f, false, false, 0.0f}));

  SidechainRouter router;
  REQUIRE_THROWS(router.set_sidechain(nullptr, 1, 128));
}

TEST_CASE("SidechainRouter lookahead delays main while using current key",
          "[mastering][dynamics]") {
  SidechainRouter router({-18.0f, 8.0f, 0.0f, 0.0f, 18.0f, false, 90.0f, false, false, 2.0f});
  router.prepare(1000.0, 8);

  std::vector<float> main{1.0f, 0.0f, 0.0f, 0.0f};
  std::vector<float> key{1.0f, 0.0f, 0.0f, 0.0f};
  const float* key_channels[] = {key.data()};
  router.set_sidechain(key_channels, 1, static_cast<int>(key.size()));
  process(router, main);

  REQUIRE_THAT(main[0], WithinAbs(0.0f, 0.0001f));
  REQUIRE_THAT(main[1], WithinAbs(0.0f, 0.0001f));
  REQUIRE(main[2] < 0.2f);
}

TEST_CASE("DuckingProcessor wraps SidechainRouter with voiceover defaults",
          "[mastering][dynamics]") {
  DuckingProcessor ducking({-18.0f, 8.0f, 0.0f, 0.0f, 18.0f, 0.0f});
  ducking.prepare(48000.0, 16);

  std::vector<float> main(16, 1.0f);
  std::vector<float> key(16, 1.0f);
  const float* key_channels[] = {key.data()};
  ducking.set_key_input(key_channels, 1, static_cast<int>(key.size()));
  process(ducking, main);

  const float expected_reduction_db = -15.75f;
  const float expected_gain = std::pow(10.0f, expected_reduction_db / 20.0f);
  REQUIRE_THAT(main[15], WithinAbs(expected_gain, 0.0001f));
  REQUIRE_THAT(ducking.last_gain_reduction_db(), WithinAbs(expected_reduction_db, 0.0001f));
}

TEST_CASE("SidechainRouter treats empty sidechain as cleared", "[mastering][dynamics]") {
  SidechainRouter router({-30.0f, 4.0f, 0.0f, 20.0f, 18.0f});
  router.prepare(48000.0, 1024);

  std::vector<float> sidechain(128, 1.0f);
  const float* sidechain_channels[] = {sidechain.data()};
  router.set_sidechain(sidechain_channels, 1, static_cast<int>(sidechain.size()));
  router.set_sidechain(nullptr, 0, 0);

  std::vector<float> target(128, 0.01f);
  process(router, target);

  REQUIRE(rms_tail(target, 0) > 0.009f);
  REQUIRE_THAT(router.last_gain_reduction_db(), WithinAbs(0.0f, 0.0001f));
}

TEST_CASE("SidechainRouter supports HPF mono key and key listen", "[mastering][dynamics]") {
  SidechainRouter listen({-30.0f, 4.0f, 0.0f, 20.0f, 18.0f, true, 120.0f, true, true});
  listen.prepare(48000.0, 1024);

  std::vector<float> target(1024, 0.0f);
  std::vector<float> low = generate_sine_samples(40.0f, 48000, 1024, 1.0f);
  std::vector<float> high = generate_sine_samples(1000.0f, 48000, 1024, 1.0f);
  const float* sidechain[] = {low.data(), high.data()};
  listen.set_sidechain(sidechain, 2, static_cast<int>(target.size()));
  process(listen, target);

  REQUIRE(rms_tail(target, 128) > 0.1f);
}

TEST_CASE("Compressor sidechain HPF attenuates lows in the detector", "[mastering][dynamics]") {
  // With a 1 kHz sidechain HPF the detector should respond more to a 5 kHz tone
  // than to a 100 Hz tone of equal amplitude, so the high tone gets more gain
  // reduction (more negative dB).
  CompressorConfig config{};
  config.threshold_db = -30.0f;
  config.ratio = 4.0f;
  config.attack_ms = 5.0f;
  config.release_ms = 50.0f;
  config.detector = DetectorMode::Peak;
  config.sidechain_hpf_enabled = true;
  config.sidechain_hpf_hz = 1000.0f;

  Compressor low_comp(config);
  Compressor high_comp(config);
  low_comp.prepare(48000.0, 4096);
  high_comp.prepare(48000.0, 4096);

  auto low = generate_sine_samples(100.0f, 48000, 24000, 0.5f);
  auto high = generate_sine_samples(5000.0f, 48000, 24000, 0.5f);
  process(low_comp, low);
  process(high_comp, high);

  const float low_gr = low_comp.last_gain_reduction_db();
  const float high_gr = high_comp.last_gain_reduction_db();

  // The high tone passes through the HPF nearly unattenuated and triggers more
  // gain reduction; the low tone is attenuated in the detector, so less.
  REQUIRE(high_gr < low_gr);
  REQUIRE(low_gr > high_gr + 3.0f);
}

TEST_CASE("Limiter applies identical linked gain to both stereo channels",
          "[mastering][dynamics]") {
  // Regression: a single linked target gain must drive a single shared
  // smoother, so L and R are scaled by the same factor sample-by-sample. The
  // left channel carries a loud transient while both channels share a quiet
  // steady reference; outside the transient the two channels must remain equal
  // (identical scaling => identical values), preserving the stereo image.
  Limiter limiter({-6.0f, 1.0f, 20.0f});
  limiter.prepare(48000.0, 4096);

  std::vector<float> left(4096, 0.1f);
  std::vector<float> right(4096, 0.1f);
  for (size_t i = 1000; i < 1100; ++i) {
    left[i] = 0.95f;  // drives the linked detector hard, left only
  }

  process_stereo(limiter, left, right);

  // After the transient, both channels carry the same reference value; if the
  // same gain was applied they must remain comparable to within float epsilon.
  float max_diff = 0.0f;
  for (size_t i = 1300; i < left.size(); ++i) {
    max_diff = std::max(max_diff, std::abs(left[i] - right[i]));
  }
  REQUIRE(max_diff < 1.0e-6f);
}

TEST_CASE("Expander applies identical linked gain to both stereo channels",
          "[mastering][dynamics]") {
  // Asymmetric drive must still produce identical per-sample gain on both
  // channels: both carry the same constant level so identical linked gain
  // yields identical outputs; a short loud burst on the left only drives the
  // detector.
  Expander expander({-30.0f, 2.0f, 5.0f, 100.0f, -40.0f});
  expander.prepare(48000.0, 4096);

  std::vector<float> left(4096, 0.02f);
  std::vector<float> right(4096, 0.02f);
  for (size_t i = 500; i < 600; ++i) {
    left[i] = 0.5f;
    right[i] = 0.5f;
  }
  process_stereo(expander, left, right);

  REQUIRE(max_abs_difference(left, right) < 1.0e-6f);
}

TEST_CASE("UpwardCompressor applies identical linked gain to both stereo channels",
          "[mastering][dynamics]") {
  UpwardCompressor upward({-20.0f, 2.0f, 10.0f, 100.0f, 12.0f});
  upward.prepare(48000.0, 4096);

  std::vector<float> left(4096, 0.02f);
  std::vector<float> right(4096, 0.02f);
  for (size_t i = 500; i < 600; ++i) {
    left[i] = 0.5f;
    right[i] = 0.5f;
  }
  process_stereo(upward, left, right);

  REQUIRE(max_abs_difference(left, right) < 1.0e-6f);
}

TEST_CASE("Gate opens to unity in the linear domain on a silence-to-loud step",
          "[mastering][dynamics]") {
  // Linear-domain smoothing must converge to unity (gain 1.0) once open; the
  // old dB-domain smoothing toward 0 dB from range_db never converged cleanly.
  const float sr = 48000.0f;
  const float attack_ms = 5.0f;
  Gate gate({-40.0f, attack_ms, 80.0f, -80.0f});
  gate.prepare(static_cast<double>(sr), 8192);

  std::vector<float> signal(8192, 0.0f);
  for (size_t i = 4096; i < signal.size(); ++i) {
    signal[i] = 0.5f;  // loud constant level well above threshold
  }
  process(gate, signal);

  // Roughly attack_ms after the step the gate should have opened to ~unity, so
  // the loud samples pass through near their input value (0.5).
  const size_t attack_samples = static_cast<size_t>(sr * attack_ms * 0.001f);
  const size_t settle = 4096 + attack_samples * 5;
  REQUIRE(settle < signal.size());
  for (size_t i = settle; i < signal.size(); ++i) {
    REQUIRE(std::isfinite(signal[i]));
    REQUIRE(signal[i] > 0.45f);  // near unity, not stuck below
  }
}

TEST_CASE("Gate stays finite with an extreme range", "[mastering][dynamics]") {
  // A very low (effectively -inf) range must not produce NaN/Inf via a dB-domain
  // underflow; the linear floor is 0.
  Gate gate({-40.0f, 2.0f, 80.0f, -240.0f});
  gate.prepare(48000.0, 1024);

  std::vector<float> signal(1024, 0.0001f);  // below threshold -> closes
  process(gate, signal);
  for (float s : signal) {
    REQUIRE(std::isfinite(s));
  }
}

TEST_CASE("Compressor detector mode switch does not spike gain reduction",
          "[mastering][dynamics]") {
  // Switching detector between Rms and LogRms mid-stream must not carry the
  // wrong-window rms_state_ and spike the gain. Process a steady tone in Rms,
  // switch to LogRms, then process the same steady tone; the gain reduction
  // must stay within a sane bound (no large spurious spike).
  CompressorConfig cfg{};
  cfg.threshold_db = -18.0f;
  cfg.ratio = 4.0f;
  cfg.attack_ms = 10.0f;
  cfg.release_ms = 100.0f;
  cfg.detector = DetectorMode::Rms;
  Compressor compressor(cfg);
  compressor.prepare(48000.0, 4096);

  auto block = generate_sine_samples(1000.0f, 48000, 4096, 0.5f);
  process(compressor, block);
  const float gr_before = compressor.last_gain_reduction_db();

  cfg.detector = DetectorMode::LogRms;
  compressor.set_config(cfg);
  auto block2 = generate_sine_samples(1000.0f, 48000, 4096, 0.5f);
  process(compressor, block2);
  const float gr_after = compressor.last_gain_reduction_db();

  // The steady level is unchanged, so the gain reduction after the switch must
  // not jump far beyond the pre-switch value. The reseed keeps it close; allow
  // a small window for the differing time constant settling.
  REQUIRE(std::isfinite(gr_after));
  REQUIRE(gr_after <= 0.0f);
  REQUIRE(gr_after > gr_before - 6.0f);
}

TEST_CASE("DeEsser preserves low-frequency energy while reducing the sibilant band",
          "[mastering][dynamics]") {
  // Split-band: a low-frequency-only signal must pass through nearly unchanged
  // even when the de-esser is configured to reduce the sibilant band. Pre-fix
  // the wideband reduction attenuated everything.
  DeEsser deesser({5000.0f, -28.0f, 6.0f, 1.0f, 60.0f, 18.0f});
  deesser.prepare(48000.0, 4096);

  auto low = generate_sine_samples(120.0f, 48000, 4096, 0.5f);
  const float low_before = rms_tail(low, 1024);
  process(deesser, low);
  const float low_after = rms_tail(low, 1024);

  // The low band sits far below the 5 kHz detection/split band, so it should be
  // virtually untouched.
  REQUIRE(low_after / low_before > 0.97f);
}

// ---------------------------------------------------------------------------
// RT-safe set_config regression tests. Each processor must accept
// set_config() calls from a control thread while an audio thread is calling
// process() back-to-back without producing torn reads (NaN/Inf samples) or
// losing the most recently published configuration.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// A non-finite sample must not outlive the block that carried it. An owner's
// detector state is recursive, so a non-finite value that reaches it comes back
// out of every later block of an otherwise clean stream; the owner applies the
// shared rule (util/non_finite_state.h) once per block over its own cells.
//
// The stranding is not visible as a non-finite output. Every fold these owners
// use -- std::max, std::min, an ordered comparison -- answers false to a
// non-finite operand and returns its other argument, so a stranded detector
// reads as silence or as no reduction at all and the owner quietly stops doing
// its job. The cases below therefore assert on the reduction against a clean-run
// control, never on finiteness.
// ---------------------------------------------------------------------------

namespace {

constexpr int kRecoverySampleRate = 48000;
constexpr int kRecoveryBlockSize = 512;
constexpr int kRecoveryBlocks = 120;
// Block by which a poisoned stream must be bit-identical to its control. A
// discarded cell returns to its post-reset value at once; the stream rejoins
// only once the state behind it has re-converged, so each bound follows the
// slowest time constant in its own path. Both sit at roughly twice their
// measured crossing (28 for the router, 44 and 39 for the compressor's two
// configurations), where the difference passes one float ULP -- a one-ULP
// difference in the platform's math library moved a comparable fixture's by 45%.
constexpr int kSidechainRecoveryBlocks = 60;
constexpr int kCompressorRecoveryBlocks = 90;
constexpr int kRecoveryPoisonBlock = 1;
constexpr int kRecoveryPoisonIndex = 100;

/// @brief One block of the stream the recovery cases are driven with.
/// @details Loud enough to sit above the thresholds configured below, so the
///          control run is reducing gain rather than passing the signal through.
std::vector<float> recovery_block(int block_index) {
  std::vector<float> out(static_cast<size_t>(kRecoveryBlockSize));
  for (int i = 0; i < kRecoveryBlockSize; ++i) {
    const double t =
        static_cast<double>(block_index * kRecoveryBlockSize + i) / kRecoverySampleRate;
    out[static_cast<size_t>(i)] =
        static_cast<float>(0.5 * std::sin(2.0 * sonare::constants::kPiD * 220.0 * t));
  }
  return out;
}

using RecoveryBlocks = std::vector<std::vector<float>>;

/// @brief What one run of the stream produced.
struct RecoveryRun {
  RecoveryBlocks blocks;
  /// The reduction the owner reported for each block. This is the observable a
  /// stranded detector changes, and it stays finite while it is wrong.
  std::vector<float> reduction_db;
};

/// @brief Runs the stream block by block, optionally poisoning one sample of one
///        block, and returns every output block.
template <typename Processor>
RecoveryRun run_recovery_stream(Processor& processor, float poison_value, bool poison) {
  RecoveryRun run;
  run.blocks.reserve(static_cast<size_t>(kRecoveryBlocks));
  run.reduction_db.reserve(static_cast<size_t>(kRecoveryBlocks));
  for (int k = 0; k < kRecoveryBlocks; ++k) {
    std::vector<float> block = recovery_block(k);
    if (poison && k == kRecoveryPoisonBlock) {
      block[static_cast<size_t>(kRecoveryPoisonIndex)] = poison_value;
    }
    float* channels[] = {block.data()};
    processor.process(channels, 1, kRecoveryBlockSize);
    run.blocks.push_back(std::move(block));
    run.reduction_db.push_back(processor.last_gain_reduction_db());
  }
  return run;
}

bool recovery_block_has_non_finite(const std::vector<float>& block) {
  return std::any_of(block.begin(), block.end(), [](float v) { return !std::isfinite(v); });
}

/// @brief Block from which every later block is bit-identical to the control
///        run, or the block count when the stream never rejoins it.
/// @details Scanned from the end: a single coincidentally-identical block is not
///          convergence.
int first_identical_recovery_block(const RecoveryBlocks& control, const RecoveryBlocks& poisoned) {
  int k = static_cast<int>(control.size());
  while (k > kRecoveryPoisonBlock + 1 &&
         control[static_cast<size_t>(k - 1)] == poisoned[static_cast<size_t>(k - 1)]) {
    --k;
  }
  return k;
}

/// @brief The largest change the control run makes to the signal.
/// @details Zero means the owner passed the stream through, and no recovery
///          result read from it would say anything about the state under test.
float recovery_control_effect(const RecoveryBlocks& control) {
  float largest = 0.0f;
  for (size_t k = 0; k < control.size(); ++k) {
    const std::vector<float> source = recovery_block(static_cast<int>(k));
    for (int i = 0; i < kRecoveryBlockSize; ++i) {
      largest = std::max(
          largest, std::abs(control[k][static_cast<size_t>(i)] - source[static_cast<size_t>(i)]));
    }
  }
  return largest;
}

/// @brief Drives one owner twice -- clean and poisoned -- and asserts that the
///        non-finite sample reaches its state and does not outlive it.
/// @param make Builds a prepared, configured owner.
/// @param recovery_blocks Block by which the poisoned stream must be
///        bit-identical to the control again.
template <typename Make>
void check_detector_recovery(const Make& make, int recovery_blocks) {
  const std::array<float, 3> poison_values = {std::numeric_limits<float>::quiet_NaN(),
                                              std::numeric_limits<float>::infinity(),
                                              -std::numeric_limits<float>::infinity()};
  for (const float poison_value : poison_values) {
    INFO("poison value " << poison_value);
    auto control_owner = make();
    const RecoveryRun control = run_recovery_stream(*control_owner, 0.0f, false);
    // Read before any recovery result. An owner that passed the stream through
    // cannot support the claim; neither can one whose control run has stopped
    // reducing, because a stranded detector also reports no reduction and the
    // two would agree.
    REQUIRE(recovery_control_effect(control.blocks) > 0.0f);
    REQUIRE(control.reduction_db.back() < 0.0f);
    REQUIRE_FALSE(
        std::any_of(control.blocks.begin(), control.blocks.end(), recovery_block_has_non_finite));

    auto poisoned_owner = make();
    const RecoveryRun poisoned = run_recovery_stream(*poisoned_owner, poison_value, true);
    // Positive control: an owner that sanitized its input instead of its own
    // state would leave every block finite and satisfy everything below.
    REQUIRE(recovery_block_has_non_finite(poisoned.blocks[kRecoveryPoisonBlock]));
    // The reduction is read separately from the samples: it is what a stranded
    // detector gets wrong while staying finite, and it does not depend on how
    // convergence is scanned for.
    INFO("reduction " << poisoned.reduction_db.back() << " against control "
                      << control.reduction_db.back());
    REQUIRE(poisoned.reduction_db.back() == control.reduction_db.back());
    INFO("first identical " << first_identical_recovery_block(control.blocks, poisoned.blocks)
                            << " of " << control.blocks.size());
    REQUIRE(first_identical_recovery_block(control.blocks, poisoned.blocks) <= recovery_blocks);
  }
}

}  // namespace

TEST_CASE("SidechainRouter bounds a non-finite sample to the block that carried it",
          "[mastering][dynamics]") {
  // Mono summing is what puts the follower in the sample's path: the peak
  // detector folds with std::max, which answers false to a non-finite operand
  // and drops it, while the mono sum carries it into the follower.
  SidechainRouterConfig config;
  config.mono_summing = true;
  // A stranded follower reads as a permanent range_db of reduction, because the
  // fold that derives the reduction drops its non-finite operand and returns the
  // range instead. The threshold and ratio therefore have to put the control run
  // nowhere near that range, or the two agree and the case proves nothing.
  config.threshold_db = -12.0f;
  config.ratio = 2.0f;

  const auto make = [config]() {
    auto router = std::make_unique<SidechainRouter>(config);
    router->prepare(kRecoverySampleRate, kRecoveryBlockSize);
    return router;
  };

  check_detector_recovery(make, kSidechainRecoveryBlocks);
}

TEST_CASE("Compressor bounds a non-finite sample to the block that carried it",
          "[mastering][dynamics]") {
  // The RMS detector is what puts the sample in the owner's path: the peak
  // detector folds with std::max and drops a non-finite operand, while the power
  // sum carries it into rms_state_, which persists across blocks.
  CompressorConfig config;
  config.detector = DetectorMode::Rms;
  // A stranded detector reads as no reduction at all, so the control run has to
  // be reducing by a margin the assertion can see.
  config.threshold_db = -12.0f;
  config.ratio = 2.0f;

  SECTION("through the RMS detector") {
    const auto make = [config]() {
      auto compressor = std::make_unique<Compressor>(config);
      compressor->prepare(kRecoverySampleRate, kRecoveryBlockSize);
      return compressor;
    };
    check_detector_recovery(make, kCompressorRecoveryBlocks);
  }

  SECTION("through the soft knee, which reaches two more cells") {
    // A hard knee derives the reduction through an ordered comparison that
    // answers false for a non-finite level, so the level is laundered to no
    // reduction before it reaches the release state or the smoother. The knee
    // branch is arithmetic instead, so the non-finite value passes into both.
    CompressorConfig knee = config;
    knee.knee_db = 6.0f;
    knee.pdr_time_ms = 50.0f;
    const auto make = [knee]() {
      auto compressor = std::make_unique<Compressor>(knee);
      compressor->prepare(kRecoverySampleRate, kRecoveryBlockSize);
      return compressor;
    };
    check_detector_recovery(make, kCompressorRecoveryBlocks);
  }
}

TEST_CASE("Limiter holds its ceiling across a window it cannot evaluate", "[mastering][dynamics]") {
  // The sliding-window maximum keeps a non-finite entry -- nothing compares
  // greater than it, so no push evicts it -- and hands it to the detector fold
  // when it reaches the front of the window. std::max then drops it and the
  // window reads as SILENCE rather than as a peak of unknown size, so the gain
  // takes one release step toward unity while the owner is blind.
  //
  // That is one sample wide, because the entry pushed just before it is still
  // live and expires one index earlier. This case pins that: it is what keeps
  // the ceiling from breaking, so a change to the window's eviction that widened
  // the blind stretch would surface here rather than in a listening test.
  LimiterConfig config;
  config.threshold_db = -12.0f;
  // The widest window and the steepest release the owner is configured for in
  // practice, which is where a blind stretch would cost the most.
  config.lookahead_ms = 5.0f;
  config.release_ms = 1.0f;

  const float ceiling = sonare::db_to_linear(config.threshold_db);
  const int blocks = 8;
  const int poison_block = 1;
  const int poison_index = 100;
  // Well above the ceiling, so the owner is limiting throughout.
  const float amplitude = 0.9f;

  const auto run = [&](float poison_value, bool poison) {
    Limiter limiter(config);
    limiter.prepare(kRecoverySampleRate, kRecoveryBlockSize);
    float loudest = 0.0f;
    for (int k = 0; k < blocks; ++k) {
      std::vector<float> block(static_cast<size_t>(kRecoveryBlockSize));
      for (int i = 0; i < kRecoveryBlockSize; ++i) {
        const double t = static_cast<double>(k * kRecoveryBlockSize + i) / kRecoverySampleRate;
        block[static_cast<size_t>(i)] =
            static_cast<float>(amplitude * std::sin(2.0 * sonare::constants::kPiD * 220.0 * t));
      }
      if (poison && k == poison_block) {
        block[static_cast<size_t>(poison_index)] = poison_value;
      }
      float* channels[] = {block.data()};
      limiter.process(channels, 1, kRecoveryBlockSize);
      // The non-finite sample itself passes through the delay line and leaves as
      // it arrived; no gain can make it finite. What is read here is what
      // happened to the finite samples around it.
      for (float v : block) {
        if (std::isfinite(v)) loudest = std::max(loudest, std::abs(v));
      }
    }
    return loudest;
  };

  // Non-vacuity, before any ceiling result: the source is above the ceiling and
  // the owner is holding it there rather than attenuating to nothing.
  const float control = run(0.0f, false);
  REQUIRE(amplitude > ceiling);
  INFO("control peak " << control << " against ceiling " << ceiling);
  REQUIRE(control <= ceiling * 1.01f);
  REQUIRE(control > ceiling * 0.5f);

  SECTION("a window carrying a NaN") {
    const float poisoned = run(std::numeric_limits<float>::quiet_NaN(), true);
    INFO("poisoned peak " << poisoned << " against ceiling " << ceiling);
    REQUIRE(poisoned <= ceiling * 1.01f);
  }

  SECTION("a window carrying an infinity, which the detector can evaluate") {
    // Control for the case above: an infinite peak orders normally, so the
    // detector reads it as unbounded and the owner ducks. It is the value that
    // orders against nothing that strands the fold.
    const float poisoned = run(std::numeric_limits<float>::infinity(), true);
    INFO("poisoned peak " << poisoned << " against ceiling " << ceiling);
    REQUIRE(poisoned <= ceiling * 1.01f);
  }
}
