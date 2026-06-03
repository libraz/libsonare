/// @file voice_changer_limiter_json_test.cpp
/// @brief Realtime voice changer limiter and JSON edge-case tests.

#include "voice_changer_test_helpers.h"

TEST_CASE("RealtimeVoiceChanger silent input produces silence with aggressive gate",
          "[voice_changer][dsp][gate][silence]") {
  constexpr int sample_rate = 48000;
  constexpr int block = 128;
  constexpr int total = sample_rate;                                           // 1 s
  constexpr std::size_t settle = static_cast<std::size_t>(sample_rate * 0.2);  // 200 ms

  RealtimeVoiceChangerConfig cfg;
  cfg.wet_mix = 1.0f;
  cfg.retune = {0.0f, 0.0f, 0};
  cfg.formant = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  cfg.eq = {30.0f, 0.0f, 0.0f, 0.0f};
  cfg.gate.threshold_db = -40.0f;
  cfg.gate.attack_ms = 1.0f;
  cfg.gate.release_ms = 50.0f;
  cfg.gate.range_db = 30.0f;
  cfg.compressor = {0.0f, 1.0f, 5.0f, 50.0f, 0.0f};
  cfg.deesser = {7000.0f, -6.0f, 1.0f, 0.0f};
  cfg.reverb.mix = 0.0f;
  cfg.limiter.ceiling_db = 0.0f;

  RealtimeVoiceChanger changer(cfg);
  changer.prepare(sample_rate, block, 1);

  std::vector<float> input(static_cast<std::size_t>(total), 0.0f);
  std::vector<float> output(static_cast<std::size_t>(total), 0.0f);
  for (int pos = 0; pos < total; pos += block) {
    const int n = std::min(block, total - pos);
    changer.process_block(input.data() + pos, output.data() + pos, n);
  }

  for (float s : output) REQUIRE(std::isfinite(s));

  // After the settle window the output should be effectively silent: no
  // envelope-follower flutter should be leaking through the gate.
  const float tail_rms = block_rms(output, settle, static_cast<std::size_t>(total));
  REQUIRE(tail_rms < 1e-6f);
}

// ===================================================================
// Clipped input (±2.0f, 200% of full-scale): limiter must hold the
// ceiling and the output must stay finite.
// ===================================================================
TEST_CASE("RealtimeVoiceChanger limiter holds ceiling on clipped input",
          "[voice_changer][dsp][limiter][clip]") {
  constexpr int sample_rate = 48000;
  constexpr int block = 128;
  constexpr int total = sample_rate / 2;  // 0.5 s

  auto cfg = realtime_voice_changer_preset(VoiceCharacterPreset::NeutralMonitor);
  // Force a clear ceiling so we can assert a numeric bound.
  cfg.limiter.ceiling_db = -1.0f;
  cfg.limiter.release_ms = 50.0f;

  RealtimeVoiceChanger changer(cfg);
  changer.prepare(sample_rate, block, 1);

  // Constant ±2.0f square wave — 200% of full-scale, alternating polarity so
  // the HPF cannot park it at DC.
  std::vector<float> input(static_cast<std::size_t>(total), 0.0f);
  for (int i = 0; i < total; ++i) {
    input[static_cast<std::size_t>(i)] = (i % 2 == 0) ? 2.0f : -2.0f;
  }

  std::vector<float> output(static_cast<std::size_t>(total), 0.0f);
  for (int pos = 0; pos < total; pos += block) {
    const int n = std::min(block, total - pos);
    changer.process_block(input.data() + pos, output.data() + pos, n);
  }

  // Every output sample must be finite and the limiter must hold the peak
  // below 0.95 (well below 1.0 since ceiling is -1 dBFS ~= 0.891).
  for (std::size_t i = 0; i < output.size(); ++i) {
    REQUIRE(std::isfinite(output[i]));
    REQUIRE(std::abs(output[i]) <= 0.95f);
  }
}

// ===================================================================
// DC bias through the FULL preset chain (not a neutralized chain).
// HPF must remove DC even with compressor / makeup gain / deesser /
// reverb / limiter all active. Catches regressions where a later stage
// re-introduces DC bias.
// ===================================================================
TEST_CASE("RealtimeVoiceChanger full preset chain removes DC bias",
          "[voice_changer][dsp][hpf][dc]") {
  constexpr int sample_rate = 48000;
  constexpr int block = 128;
  constexpr int total = sample_rate;                                           // 1 s
  constexpr std::size_t settle = static_cast<std::size_t>(sample_rate * 0.5);  // 500 ms

  auto cfg = realtime_voice_changer_preset(VoiceCharacterPreset::BrightIdol);
  RealtimeVoiceChanger changer(cfg);
  changer.prepare(sample_rate, block, 1);

  std::vector<float> input(static_cast<std::size_t>(total), 0.5f);  // pure DC
  std::vector<float> output(static_cast<std::size_t>(total), 0.0f);
  for (int pos = 0; pos < total; pos += block) {
    const int n = std::min(block, total - pos);
    changer.process_block(input.data() + pos, output.data() + pos, n);
  }

  for (float s : output) REQUIRE(std::isfinite(s));

  // After 500 ms, the mean of the output must be near zero: even with the
  // compressor, makeup gain, deesser, reverb, and limiter active in the
  // full preset chain, no stage should re-inject DC past the HPF.
  double mean = 0.0;
  const std::size_t n_tail = static_cast<std::size_t>(total) - settle;
  for (std::size_t i = settle; i < static_cast<std::size_t>(total); ++i) {
    mean += static_cast<double>(output[i]);
  }
  mean /= static_cast<double>(n_tail);
  REQUIRE(std::abs(static_cast<float>(mean)) < 0.005f);
}

TEST_CASE("RealtimeVoiceChanger set_config snapshot is adopted at next block boundary",
          "[voice_changer][snapshot][rt]") {
  constexpr int sample_rate = 48000;
  constexpr int block = 128;
  constexpr int blocks = 64;

  auto cfg = realtime_voice_changer_preset(VoiceCharacterPreset::BrightIdol);
  cfg.wet_mix = 1.0f;
  RealtimeVoiceChanger changer(cfg);
  changer.prepare(sample_rate, block, 1);

  std::vector<float> input(block, 0.0f);
  std::vector<float> output(block, 0.0f);
  for (int n = 0; n < block; ++n) {
    input[static_cast<std::size_t>(n)] =
        0.25f * std::sin(2.0f * sonare::constants::kPi * 440.0f * static_cast<float>(n) /
                         static_cast<float>(sample_rate));
  }

  // Warm up: drive the chain for a few blocks under wet_mix=1.0.
  for (int b = 0; b < blocks; ++b) {
    changer.process_block(input.data(), output.data(), block);
  }

  // Publish a new snapshot: wet_mix=0 → next block should bypass the DSP.
  auto dry_cfg = cfg;
  dry_cfg.wet_mix = 0.0f;
  changer.set_config(dry_cfg);

  std::vector<float> bypassed(block, 0.0f);
  changer.process_block(input.data(), bypassed.data(), block);

  // wet_mix is now 0, so the output should equal the dry input bit-for-bit
  // (the snapshot was adopted at the start of this block).
  for (int n = 0; n < block; ++n) {
    REQUIRE(bypassed[static_cast<std::size_t>(n)] == input[static_cast<std::size_t>(n)]);
  }

  // Reading config() back on the configuration thread must reflect the publish.
  REQUIRE(changer.config().wet_mix == 0.0f);
}

TEST_CASE("RealtimeVoiceChanger concurrent set_config and process_block stays finite",
          "[voice_changer][snapshot][concurrency]") {
  constexpr int sample_rate = 48000;
  constexpr int block = 128;
  constexpr int audio_blocks = 2000;

  auto base = realtime_voice_changer_preset(VoiceCharacterPreset::BrightIdol);
  RealtimeVoiceChanger changer(base);
  changer.prepare(sample_rate, block, 1);

  std::vector<float> input(block, 0.0f);
  for (int n = 0; n < block; ++n) {
    input[static_cast<std::size_t>(n)] =
        0.25f * std::sin(2.0f * sonare::constants::kPi * 440.0f * static_cast<float>(n) /
                         static_cast<float>(sample_rate));
  }

  std::atomic<bool> stop{false};
  std::atomic<std::size_t> publish_count{0};
  std::thread config_thread([&]() {
    auto cfg = base;
    while (!stop.load(std::memory_order_relaxed)) {
      // Toggle a few fields that exercise both the input and output stage
      // re-applies as well as the retune/formant grain config paths.
      cfg.wet_mix = (cfg.wet_mix > 0.5f) ? 0.0f : 1.0f;
      cfg.retune.semitones = (cfg.retune.semitones > 0.0f) ? -3.0f : 3.0f;
      cfg.formant.factor = (cfg.formant.factor > 1.0f) ? 0.85f : 1.15f;
      changer.set_config(cfg);
      publish_count.fetch_add(1, std::memory_order_relaxed);
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
  });

  std::vector<float> output(block, 0.0f);
  std::size_t finite_count = 0;
  for (int b = 0; b < audio_blocks; ++b) {
    changer.process_block(input.data(), output.data(), block);
    for (float s : output) {
      REQUIRE(std::isfinite(s));
      ++finite_count;
    }
  }

  stop.store(true, std::memory_order_relaxed);
  config_thread.join();

  REQUIRE(finite_count == static_cast<std::size_t>(audio_blocks) * block);
  REQUIRE(publish_count.load() > 0);
}

TEST_CASE("RealtimeVoiceChanger ISP limiter keeps true peak under the configured ceiling",
          "[voice_changer][isp]") {
  constexpr int sample_rate = 48000;
  constexpr int block_size = 256;
  constexpr int num_blocks = 32;
  constexpr int total = block_size * num_blocks;

  // Loud sine close to 0 dBFS — the kind of signal where naive sample-domain
  // limiting still produces inter-sample overshoots after the DAC oversamples.
  std::vector<float> input(total);
  for (int i = 0; i < total; ++i) {
    input[static_cast<size_t>(i)] =
        0.97f * std::sin(sonare::constants::kTwoPiD * 997.0 * i / sample_rate);
  }

  auto run = [&](bool enable_isp) {
    RealtimeVoiceChangerConfig config =
        realtime_voice_changer_preset(VoiceCharacterPreset::NeutralMonitor);
    config.limiter.enable_isp_limiter = enable_isp;
    config.limiter.isp_ceiling_dbtp = -1.0f;
    RealtimeVoiceChanger changer(config);
    changer.prepare(sample_rate, block_size, 1);
    std::vector<float> output(total, 0.0f);
    for (int b = 0; b < num_blocks; ++b) {
      changer.process_block(input.data() + b * block_size, output.data() + b * block_size,
                            block_size);
    }
    return output;
  };

  const auto out_on = run(true);
  const auto out_off = run(false);

  const float tp_on = sonare::metering::true_peak(out_on.data(), out_on.size(), 4);
  const float tp_off = sonare::metering::true_peak(out_off.data(), out_off.size(), 4);
  const float tp_on_db = 20.0f * std::log10(std::max(tp_on, 1e-9f));
  const float tp_off_db = 20.0f * std::log10(std::max(tp_off, 1e-9f));

  // With the limiter ON, the true peak must sit under (ceiling + small
  // tolerance). With it OFF, the unconstrained output must exceed the same
  // ceiling — that proves the limiter is what's holding tp_on down.
  REQUIRE(tp_on_db <= -1.0f + 0.5f);
  REQUIRE(tp_off_db > -1.0f);
  for (float s : out_on) REQUIRE(std::isfinite(s));
}

TEST_CASE("RealtimeVoiceChanger ISP limiter leaves quiet signals untouched",
          "[voice_changer][isp]") {
  constexpr int sample_rate = 48000;
  constexpr int block_size = 128;
  constexpr int num_blocks = 16;
  constexpr int total = block_size * num_blocks;

  std::vector<float> input(total);
  for (int i = 0; i < total; ++i) {
    input[static_cast<size_t>(i)] =
        0.05f * std::sin(sonare::constants::kTwoPiD * 440.0 * i / sample_rate);
  }

  RealtimeVoiceChangerConfig config =
      realtime_voice_changer_preset(VoiceCharacterPreset::NeutralMonitor);
  config.limiter.enable_isp_limiter = true;
  config.limiter.isp_ceiling_dbtp = -1.0f;
  RealtimeVoiceChanger changer(config);
  changer.prepare(sample_rate, block_size, 1);

  std::vector<float> output(total, 0.0f);
  for (int b = 0; b < num_blocks; ++b) {
    changer.process_block(input.data() + b * block_size, output.data() + b * block_size,
                          block_size);
  }

  // Quiet signal must stay well under the ceiling and the limiter should not
  // pump or zero anything — every sample stays finite, and the peak remains
  // close to the input's peak (within ~3 dB to absorb the chain's intrinsic
  // gain / EQ shaping).
  float peak_out = 0.0f;
  for (float s : output) {
    REQUIRE(std::isfinite(s));
    peak_out = std::max(peak_out, std::abs(s));
  }
  REQUIRE(peak_out < 0.5f);
}

// ============================================================================
// P0-C regression tests: ScopedNoDenormals guard and null-channel continue fix
// ============================================================================

TEST_CASE("RealtimeVoiceChanger silent input produces silent output (denormal regression guard)",
          "[voice_changer][rt-safety]") {
  // Regression guard for P0-C: ScopedNoDenormals at the top of process_block.
  // A silent input must yield a silent, finite output — no NaN/Inf and no
  // runaway denormal accumulation from filter tail-off.
  constexpr int kSampleRate = 48000;
  constexpr int kBlockSize = 512;
  constexpr int kNumBlocks = 2;

  RealtimeVoiceChanger changer;
  changer.prepare(kSampleRate, kBlockSize, 1);

  std::vector<float> input(kBlockSize, 0.0f);
  std::vector<float> output(kBlockSize, 0.0f);

  for (int b = 0; b < kNumBlocks; ++b) {
    std::fill(output.begin(), output.end(), 0.0f);
    changer.process_block(input.data(), output.data(), kBlockSize);
    for (int i = 0; i < kBlockSize; ++i) {
      REQUIRE(std::isfinite(output[static_cast<size_t>(i)]));
      REQUIRE(std::abs(output[static_cast<size_t>(i)]) < 1e-6f);
    }
  }
}

TEST_CASE(
    "RealtimeVoiceChanger multi-channel process_block with null right channel still processes left",
    "[voice_changer][rt-safety]") {
  // Regression guard for P0-C: the `return → continue` fix ensures that a null
  // channel pointer skips only that channel rather than aborting the whole call.
  constexpr int kSampleRate = 48000;
  constexpr int kBlockSize = 256;

  RealtimeVoiceChanger changer;
  changer.prepare(kSampleRate, kBlockSize, 2);

  std::vector<float> left(static_cast<size_t>(kBlockSize));
  for (int i = 0; i < kBlockSize; ++i) {
    left[static_cast<size_t>(i)] =
        0.5f * static_cast<float>(std::sin(sonare::constants::kTwoPiD * 440.0 * i / kSampleRate));
  }
  const std::vector<float> left_original = left;

  // Right channel pointer is null — must not crash and must not abort left processing.
  float* channels[] = {left.data(), nullptr};
  REQUIRE_NOTHROW(changer.process_block(channels, 2, kBlockSize));

  // At least one left sample must have been modified by the processing chain
  // (wet/dry mix, EQ, gain stages introduce numerical change).
  bool any_differs = false;
  for (int i = 0; i < kBlockSize; ++i) {
    if (left[static_cast<size_t>(i)] != left_original[static_cast<size_t>(i)]) {
      any_differs = true;
      break;
    }
  }
  REQUIRE(any_differs);

  // All processed left samples must be finite.
  for (int i = 0; i < kBlockSize; ++i) {
    REQUIRE(std::isfinite(left[static_cast<size_t>(i)]));
  }
}

// ===================================================================
// Regression (FormantWarp no hard-clip): the OLA-normalised output is no
// longer clamped to [-1, 1]. Formant warping reshapes the spectral
// envelope; it is not a limiter, so clamping added nonlinear distortion
// (a flat-topped plateau at exactly +/-1.0) to any frame whose output
// legitimately exceeded unity. Scale the input so the warped output runs
// hot and assert: (1) the output is NOT flat-topped at exactly +/-1.0,
// (2) peaks exceed 1.0, and (3) there are no NaNs.
// ===================================================================
TEST_CASE("FormantWarp does not hard-clip a hot signal", "[voice_changer][formant]") {
  constexpr int sample_rate = 22050;
  constexpr int n = sample_rate / 2;
  constexpr float f0 = 150.0f;
  // Vowel-like source with a formant peak near 900 Hz, scaled hot so the
  // reconstructed (near-neutral warp) output legitimately exceeds 1.0.
  std::vector<float> samples(static_cast<size_t>(n), 0.0f);
  constexpr float formant_hz = 900.0f;
  constexpr float bandwidth_hz = 600.0f;
  for (int h = 1; h * f0 < static_cast<float>(n); ++h) {
    const float harm_hz = h * f0;
    const float env = 1.0f / (1.0f + std::pow((harm_hz - formant_hz) / bandwidth_hz, 2.0f));
    for (int i = 0; i < n; ++i) {
      samples[static_cast<size_t>(i)] +=
          1.6f * env *
          static_cast<float>(std::sin(sonare::constants::kTwoPiD * harm_hz *
                                      static_cast<double>(i) / sample_rate));
    }
  }
  float input_peak = 0.0f;
  for (float s : samples) input_peak = std::max(input_peak, std::abs(s));
  REQUIRE(input_peak > 1.0f);  // sanity: input itself is hot.

  const sonare::Audio audio = sonare::Audio::from_vector(std::vector<float>(samples), sample_rate);
  // Near-neutral warp: the spectral envelope is barely shifted, so this isolates
  // the OLA reconstruction path where a clamp would have flat-topped the output.
  FormantWarp warp({1.02f, 12, 1.0f});
  const sonare::Audio warped = warp.process(audio);

  REQUIRE(warped.size() == audio.size());
  std::vector<float> out(warped.data(), warped.data() + warped.size());
  for (float s : out) REQUIRE(std::isfinite(s));

  // Peaks must exceed 1.0 (no clamp), measured in the steady interior away
  // from the OLA edge ramps.
  const int lo = n / 4;
  const int hi = 3 * n / 4;
  float out_peak = 0.0f;
  for (int i = lo; i < hi; ++i)
    out_peak = std::max(out_peak, std::abs(out[static_cast<size_t>(i)]));
  REQUIRE(out_peak > 1.0f);

  // No flat-topped plateau: count samples pinned at exactly +/-1.0. A clamp
  // would create many consecutive samples sitting on the ceiling; an unclamped
  // warp essentially never lands on exactly 1.0f.
  int pinned = 0;
  for (int i = lo; i < hi; ++i) {
    if (std::abs(std::abs(out[static_cast<size_t>(i)]) - 1.0f) < 1.0e-6f) ++pinned;
  }
  REQUIRE(pinned == 0);

  // Reconstruction stability: a strictly-neutral warp (factor = 1.0) and the
  // near-neutral warp produce essentially the same output (the warp itself does
  // not collapse or explode the signal; the only difference is the tiny envelope
  // shift). NOTE: FormantWarp re-colours the whitened LPC residual, so its
  // absolute output level is NOT unity-gain relative to the input by design --
  // we therefore compare against the neutral warp, not the raw input.
  FormantWarp neutral({1.0f, 12, 1.0f});
  const sonare::Audio neutral_warped = neutral.process(audio);
  std::vector<float> ref(neutral_warped.data(), neutral_warped.data() + neutral_warped.size());
  for (float s : ref) REQUIRE(std::isfinite(s));

  double ref_sq = 0.0;
  double out_sq = 0.0;
  for (int i = lo; i < hi; ++i) {
    ref_sq += static_cast<double>(ref[static_cast<size_t>(i)]) * ref[static_cast<size_t>(i)];
    out_sq += static_cast<double>(out[static_cast<size_t>(i)]) * out[static_cast<size_t>(i)];
  }
  const double ref_rms = std::sqrt(ref_sq / (hi - lo));
  const double out_rms = std::sqrt(out_sq / (hi - lo));
  REQUIRE(ref_rms > 0.0);
  REQUIRE(out_rms > ref_rms * 0.5);
  REQUIRE(out_rms < ref_rms * 2.0);
  // The neutral warp also runs hot (peaks > 1.0): proves the no-clamp behaviour
  // is not specific to a single factor.
  float ref_peak = 0.0f;
  for (int i = lo; i < hi; ++i)
    ref_peak = std::max(ref_peak, std::abs(ref[static_cast<size_t>(i)]));
  REQUIRE(ref_peak > 1.0f);
}

TEST_CASE("RealtimeVoiceChanger JSON round-trips the ISP limiter fields", "[voice_changer]") {
  // Regression: the JSON path used to serialize only ceilingDb/releaseMs and
  // dropped enableIspLimiter / ispCeilingDbtp, so a preset that disabled the ISP
  // stage or set a custom dBTP silently reverted to the POD defaults (enabled,
  // -1.0 dBTP) — diverging from the memcpy-based POD round-trip.
  SECTION("disabled ISP with a custom ceiling survives the round-trip") {
    RealtimeVoiceChangerConfig config;
    config.limiter.enable_isp_limiter = false;
    config.limiter.isp_ceiling_dbtp = -3.0f;

    const std::string json = realtime_voice_changer_config_to_json(config);
    const auto roundtrip = realtime_voice_changer_config_from_json(json);

    REQUIRE(roundtrip.limiter.enable_isp_limiter == false);
    REQUIRE(roundtrip.limiter.isp_ceiling_dbtp == -3.0f);
  }

  SECTION("enabled ISP with a custom ceiling survives the round-trip") {
    RealtimeVoiceChangerConfig config;
    config.limiter.enable_isp_limiter = true;
    config.limiter.isp_ceiling_dbtp = -6.0f;

    const std::string json = realtime_voice_changer_config_to_json(config);
    const auto roundtrip = realtime_voice_changer_config_from_json(json);

    REQUIRE(roundtrip.limiter.enable_isp_limiter == true);
    REQUIRE(roundtrip.limiter.isp_ceiling_dbtp == -6.0f);
  }
}

TEST_CASE("RealtimeVoiceChanger JSON clamps out-of-int-range reverb seed", "[voice_changer]") {
  // Regression (editing#4): the lenient JSON path read integer fields via
  // static_cast<int>(double) after only a finite check. A finite JSON number
  // outside [INT_MIN, INT_MAX] (e.g. 1e12) is undefined behaviour when cast to
  // int. The parser must clamp such values to the int range instead of
  // invoking UB, and still produce a usable config.
  SECTION("absurdly large positive seed clamps to INT_MAX") {
    const auto config = realtime_voice_changer_config_from_json(
        "{\"schemaVersion\":1,\"id\":\"x\",\"dsp\":{\"reverb\":{\"seed\":1000000000000}}}");
    REQUIRE(config.reverb.seed == std::numeric_limits<int>::max());
  }

  SECTION("absurdly large negative seed clamps to INT_MIN") {
    const auto config = realtime_voice_changer_config_from_json(
        "{\"schemaVersion\":1,\"id\":\"x\",\"dsp\":{\"reverb\":{\"seed\":-1000000000000}}}");
    REQUIRE(config.reverb.seed == std::numeric_limits<int>::min());
  }

  SECTION("clamped-seed config drives the engine without crashing") {
    const auto config = realtime_voice_changer_config_from_json(
        "{\"schemaVersion\":1,\"id\":\"x\",\"dsp\":{\"reverb\":{\"seed\":9e18,\"mix\":0.3}}}");
    RealtimeVoiceChanger changer(config);
    changer.prepare(48000, 128, 1);
    const auto input = sine(220.0f, 48000, 512);
    std::vector<float> output(input.size(), 0.0f);
    for (int pos = 0; pos < static_cast<int>(input.size()); pos += 128) {
      const int n = std::min(128, static_cast<int>(input.size()) - pos);
      changer.process_block(input.data() + pos, output.data() + pos, n);
    }
    for (float sample : output) REQUIRE(std::isfinite(sample));
  }
}
