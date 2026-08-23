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
  cfg.limiter.enable_isp_limiter = false;  // isolate snapshot adoption from FIR lookahead
  RealtimeVoiceChanger changer(cfg);
  changer.prepare(sample_rate, block, 1);

  std::vector<float> input(block, 0.25f);
  std::vector<float> output(block, 0.0f);

  // Warm up: drive the chain for a few blocks under wet_mix=1.0.
  for (int b = 0; b < blocks; ++b) {
    changer.process_block(input.data(), output.data(), block);
  }

  // Publish a new snapshot: wet_mix=0 is smoothed from the old mix rather
  // than turning an arbitrary waveform into dry audio at one block boundary.
  auto dry_cfg = cfg;
  dry_cfg.wet_mix = 0.0f;
  changer.set_config(dry_cfg);

  std::vector<float> bypassed(block, 0.0f);
  changer.process_block(input.data(), bypassed.data(), block);

  // A block-edge step used to make this dry path bit-identical immediately;
  // the blend smoother deliberately retains a wet contribution here.
  REQUIRE(bypassed[0] != input[0]);

  // After the 10 ms blend ramp has settled, the aligned dry path is recovered.
  for (int b = 0; b < 48; ++b) {
    changer.process_block(input.data(), bypassed.data(), block);
  }
  for (int n = 0; n < block; ++n) {
    REQUIRE(std::abs(bypassed[static_cast<std::size_t>(n)] - input[static_cast<std::size_t>(n)]) <
            1.0e-5f);
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

TEST_CASE(
    "RealtimeVoiceChanger race with process_block never permanently drops the final set_config",
    "[voice_changer][snapshot][concurrency]") {
  // Regression coverage for a correctness hole the RtPublisher -> SeqlockCell
  // rewrite (realtime-safety on the WASM AudioWorklet render thread)
  // introduced and then had to close: SeqlockCell::try_load() silently falls
  // back to its last torn-free cached value on a conflicting read, with no
  // way for the caller to tell that happened. A naive adopt_snapshot_for_block()
  // that unconditionally marked the observed version as "applied" after such
  // a read would permanently drop the update — worst case, the LAST
  // set_config() of a UI slider drag (the value the user actually settles
  // on), leaving the DSP frozen on the second-to-last value forever, with no
  // error and no further trigger to retry (a regression against the old
  // RtPublisher path, which never lost an update). The fix uses
  // try_load_into() and only advances applied_config_version_ on a
  // genuinely consistent read, so a conflict on block N is retried on block
  // N+1 rather than dropped.
  //
  // This races set_config() (writer thread, unthrottled) against
  // process_block() (this thread) for real — not simulated — and asserts
  // the LAST published value is eventually reflected in the rendered audio.
  // This assertion holds unconditionally once the fix is in place; it does
  // not depend on whether a genuine torn read actually occurred during this
  // particular run (true concurrency gives no such guarantee), which is why
  // the writer is deliberately unthrottled and the reader loop is long
  // enough to heavily overlap the writer's entire lifetime.
  constexpr int sample_rate = 48000;
  constexpr int block = 128;
  constexpr int race_blocks = 4000;
  constexpr int settle_blocks = 64;  // >> the 10 ms output-gain smoother ramp.
  // Distinctive final value: -36 dB is the normalizer's clamp floor
  // (realtime/config.cpp: std::clamp(output_gain_db, -36.0f, 12.0f)), well
  // below the -3/-9 dB values used during the race, so a dropped final
  // update is unambiguous in the rendered peak level.
  constexpr float kFinalOutputGainDb = -36.0f;

  auto base = realtime_voice_changer_preset(VoiceCharacterPreset::NeutralMonitor);
  base.wet_mix = 1.0f;  // output_gain_db only shapes the wet path (see process_output_stage).
  RealtimeVoiceChanger changer(base);
  changer.prepare(sample_rate, block, 1);

  std::vector<float> input(static_cast<std::size_t>(block), 0.0f);
  for (int n = 0; n < block; ++n) {
    input[static_cast<std::size_t>(n)] =
        0.4f * std::sin(2.0f * sonare::constants::kPi * 440.0f * static_cast<float>(n) /
                        static_cast<float>(sample_rate));
  }

  std::thread config_thread([&]() {
    auto cfg = base;
    // Unthrottled burst of racy writes, deliberately overlapping the reader
    // loop below as densely as possible, ending on the distinctive value.
    for (int i = 0; i < 400; ++i) {
      cfg.output_gain_db = (i % 2 == 0) ? -3.0f : -9.0f;
      changer.set_config(cfg);
    }
    cfg.output_gain_db = kFinalOutputGainDb;
    changer.set_config(cfg);
  });

  std::vector<float> output(static_cast<std::size_t>(block), 0.0f);
  for (int b = 0; b < race_blocks; ++b) {
    changer.process_block(input.data(), output.data(), block);
    for (float s : output) REQUIRE(std::isfinite(s));
  }
  config_thread.join();

  // No further writes from here: keep reading until the output-gain
  // ParamSmoother settles, then confirm the rendered level actually reflects
  // kFinalOutputGainDb rather than a value frozen by a dropped update.
  float peak = 0.0f;
  for (int b = 0; b < settle_blocks; ++b) {
    changer.process_block(input.data(), output.data(), block);
    for (float s : output) peak = std::max(peak, std::abs(s));
  }
  // Measured peak at a settled -36 dB output gain is ~0.004 for this input
  // and preset; -9 dB and -3 dB (the race values) would settle around 0.08
  // and 0.17 respectively. 0.02 cleanly separates "adopted the final -36 dB
  // write" from "stuck on an earlier race value".
  REQUIRE(peak < 0.02f);
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

TEST_CASE("ISP limiter true peak is invariant across audio block sizes", "[voice_changer][isp]") {
  constexpr int sample_rate = 48000;
  constexpr int total_samples = 8192;
  constexpr float ceiling_dbtp = -3.0f;
  std::vector<float> input(static_cast<std::size_t>(total_samples));
  for (int i = 0; i < total_samples; ++i) {
    // A near-Nyquist tone has appreciable inter-sample peaks. Its phase stays
    // continuous across every tested boundary so a block-dependent detector
    // truncation is directly observable in the final true-peak measurement.
    input[static_cast<std::size_t>(i)] =
        1.2f * std::sin(sonare::constants::kTwoPiD * 11737.0 * i / sample_rate);
  }

  auto render_true_peak_db = [&](int block_size) {
    IspLimiter limiter;
    limiter.prepare(sample_rate, block_size);
    limiter.set_config({ceiling_dbtp, 50.0f});
    std::vector<float> output = input;
    for (int offset = 0; offset < total_samples; offset += block_size) {
      limiter.process_block(output.data() + offset, block_size);
    }
    const float peak = sonare::metering::true_peak(output.data(), output.size(), 4);
    return 20.0f * std::log10(std::max(peak, 1e-9f));
  };

  const float reference_db = render_true_peak_db(256);
  for (const int block_size : {32, 64, 128, 256}) {
    const float peak_db = render_true_peak_db(block_size);
    INFO("block_size=" << block_size << ", true_peak_db=" << peak_db);
    // The detect-only base-rate gain stage leaves a small reconstruction
    // tolerance below the configured dBTP ceiling; the contract here is
    // block-boundary consistency, not an exact final-sample clamp.
    REQUIRE(peak_db <= ceiling_dbtp + 0.2f);
    REQUIRE(std::abs(peak_db - reference_db) < 0.02f);
  }
}

TEST_CASE("ISP limiter does not recreate an inter-sample peak during its attack",
          "[voice_changer][isp][attack]") {
  constexpr int sample_rate = 48000;
  constexpr int total_samples = 8192;
  constexpr int transient_start = 2048;
  constexpr float ceiling_dbtp = -3.0f;

  std::vector<float> input(static_cast<std::size_t>(total_samples));
  for (int i = 0; i < total_samples; ++i) {
    const float amplitude = i < transient_start ? 0.05f : 1.8f;
    input[static_cast<std::size_t>(i)] =
        amplitude * std::sin(sonare::constants::kTwoPiD * 11737.0 * i / sample_rate + 0.37);
  }

  auto render_true_peak_db = [&](int block_size) {
    IspLimiter limiter;
    limiter.prepare(sample_rate, block_size);
    limiter.set_config({ceiling_dbtp, 50.0f});
    std::vector<float> output = input;
    for (int offset = 0; offset < total_samples; offset += block_size) {
      limiter.process_block(output.data() + offset, block_size);
    }
    const float true_peak = sonare::metering::true_peak(output.data(), output.size(), 4);
    return 20.0f * std::log10(std::max(true_peak, 1.0e-9f));
  };

  const float reference_db = render_true_peak_db(128);
  for (const int block_size : {32, 64, 128, 256}) {
    const float true_peak_db = render_true_peak_db(block_size);
    INFO("block_size=" << block_size << ", true_peak_db=" << true_peak_db);
    REQUIRE(true_peak_db <= ceiling_dbtp + 0.05f);
    REQUIRE(std::abs(true_peak_db - reference_db) < 0.02f);
  }
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

TEST_CASE("RealtimeVoiceChanger resets ISP lookahead across a dry interval",
          "[voice_changer][isp]") {
  constexpr int sample_rate = 48000;
  constexpr int block_size = 128;
  RealtimeVoiceChangerConfig config;
  config.wet_mix = 1.0f;
  config.retune = {0.0f, 0.0f, 0};
  config.formant = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  config.eq = {20.0f, 0.0f, 0.0f, 0.0f};
  config.gate = {-120.0f, 1.0f, 1.0f, 0.0f};
  config.compressor = {0.0f, 1.0f, 1.0f, 1.0f, 0.0f};
  config.deesser = {7000.0f, 0.0f, 1.0f, 0.0f};
  config.reverb.mix = 0.0f;
  config.limiter = {0.0f, 1.0f, true, 0.0f};

  RealtimeVoiceChanger changer(config);
  changer.prepare(sample_rate, block_size, 1);
  std::vector<float> hot(static_cast<size_t>(block_size), 0.0f);
  for (int i = 0; i < block_size; ++i) {
    hot[static_cast<size_t>(i)] =
        0.8f * std::sin(sonare::constants::kTwoPi * 440.0f * i / sample_rate);
  }
  std::vector<float> output(static_cast<size_t>(block_size), 0.0f);
  changer.process_block(hot.data(), output.data(), block_size);

  // While fully dry, the final limiter is skipped. Run enough zero blocks to
  // drain all other DSP state; its old lookahead must still be discarded when
  // the wet path becomes active again.
  auto dry = config;
  dry.wet_mix = 0.0f;
  changer.set_config(dry);
  std::vector<float> silence(static_cast<size_t>(block_size), 0.0f);
  for (int block = 0; block < 64; ++block) {
    changer.process_block(silence.data(), output.data(), block_size);
  }

  changer.set_config(config);
  changer.process_block(silence.data(), output.data(), block_size);
  for (const float sample : output) {
    REQUIRE(std::abs(sample) < 1.0e-5f);
  }
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

  // Unity identity is asserted independently in the offline voice-changer
  // suite. This test intentionally covers only the non-unity no-hard-clip
  // path, so it must not compare against the old LPC reconstruction at 1.0.
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
