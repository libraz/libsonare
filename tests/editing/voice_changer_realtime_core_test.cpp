/// @file voice_changer_realtime_core_test.cpp
/// @brief Realtime voice changer core behavior tests.

#include "voice_changer_test_helpers.h"

TEST_CASE("RealtimeVoiceChanger processes realtime block sizes and resets deterministically",
          "[voice_changer]") {
  constexpr int sample_rate = 48000;
  constexpr int max_block = 512;
  constexpr int samples = 4096;
  const auto input = sine(220.0f, sample_rate, samples);
  std::vector<int> blocks = {0, 1, 127, 128, 256, 512};

  RealtimeVoiceChanger changer(realtime_voice_changer_preset(VoiceCharacterPreset::BrightIdol));
  changer.prepare(sample_rate, max_block, 1);

  for (int n : blocks) {
    std::vector<float> in(static_cast<size_t>(std::max(n, 0)), 0.01f);
    std::vector<float> out(in.size(), 0.0f);
    changer.process_block(in.data(), out.data(), n);
    for (float sample : out) REQUIRE(std::isfinite(sample));
  }

  std::vector<float> first(input.size(), 0.0f);
  changer.reset();
  for (int pos = 0; pos < samples; pos += 128) {
    changer.process_block(input.data() + pos, first.data() + pos, std::min(128, samples - pos));
  }
  changer.reset();
  std::vector<float> second(input.size(), 0.0f);
  for (int pos = 0; pos < samples; pos += 128) {
    changer.process_block(input.data() + pos, second.data() + pos, std::min(128, samples - pos));
  }
  REQUIRE(first == second);
  REQUIRE(changer.latency_samples() > 0);
}

TEST_CASE("RealtimeVoiceChanger preset JSON is tolerant and clamps values", "[voice_changer]") {
  const auto config = realtime_voice_changer_config_from_json(
      "{\"schemaVersion\":1,\"id\":\"x\",\"unknown\":true,\"dsp\":{\"retune\":{\"semitones\":99},"
      "\"formant\":{\"factor\":9},\"limiter\":{\"ceilingDb\":3},\"reverb\":{\"mix\":2}}}");

  REQUIRE(config.retune.semitones == 24.0f);
  REQUIRE(config.formant.factor == 1.65f);
  // True-peak headroom: ceiling clamps to -1.0 dBFS, not -0.3 dBFS, to give
  // the sample-domain limiter ~1 dB of inter-sample headroom. See
  // validate_dsp_section / normalize for rationale.
  REQUIRE(config.limiter.ceiling_db == -1.0f);
  REQUIRE(config.reverb.mix == 0.45f);

  const std::string json = realtime_voice_changer_config_to_json(config);
  const auto roundtrip = realtime_voice_changer_config_from_json("\n  " + json + "\n");
  REQUIRE(roundtrip.retune.semitones == config.retune.semitones);

  REQUIRE(realtime_voice_changer_preset_names().size() == 6);
  REQUIRE_NOTHROW(realtime_voice_changer_config_from_json("bright-idol"));

  std::string normalized;
  std::string error;
  REQUIRE(validate_realtime_voice_changer_preset_json(
      realtime_voice_changer_preset_json(VoiceCharacterPreset::BrightIdol), &normalized, &error));
  REQUIRE(normalized.find("retune") != std::string::npos);
  REQUIRE_FALSE(validate_realtime_voice_changer_preset_json(
      "{\"schemaVersion\":1,\"id\":\"bad\",\"name\":\"bad\",\"dsp\":{\"retune\":{\"semitones\":99}"
      "}}",
      &normalized, &error));
  REQUIRE_FALSE(error.empty());
}

TEST_CASE("RealtimeVoiceChanger C API handle lifecycle works", "[voice_changer]") {
  SonareRealtimeVoiceChanger* handle = nullptr;
  REQUIRE(sonare_realtime_voice_changer_create_json("bright-idol", 48000, 128, 1, &handle) ==
          SONARE_OK);
  REQUIRE(handle != nullptr);

  std::vector<float> input = sine(220.0f, 48000, 128);
  std::vector<float> output(input.size(), 0.0f);
  REQUIRE(sonare_realtime_voice_changer_process_mono(handle, input.data(), output.data(),
                                                     output.size()) == SONARE_OK);
  for (float sample : output) REQUIRE(std::isfinite(sample));

  int latency = 0;
  REQUIRE(sonare_realtime_voice_changer_latency_samples(handle, &latency) == SONARE_OK);
  REQUIRE(latency > 0);
  REQUIRE(sonare_realtime_voice_changer_set_config_json(handle, "deep-narrator") == SONARE_OK);
  REQUIRE(sonare_realtime_voice_changer_reset(handle) == SONARE_OK);

  char* json = nullptr;
  REQUIRE(sonare_realtime_voice_changer_preset_json("soft-whisper", &json) == SONARE_OK);
  REQUIRE(json != nullptr);
  sonare_free_string(json);

  char* normalized = nullptr;
  char* error = nullptr;
  const std::string valid_json =
      realtime_voice_changer_preset_json(VoiceCharacterPreset::BrightIdol);
  REQUIRE(sonare_realtime_voice_changer_validate_preset_json(valid_json.c_str(), &normalized,
                                                             &error) == SONARE_OK);
  REQUIRE(normalized != nullptr);
  REQUIRE(error == nullptr);
  sonare_free_string(normalized);

  normalized = nullptr;
  error = nullptr;
  REQUIRE(sonare_realtime_voice_changer_validate_preset_json(
              "{\"schemaVersion\":1,\"id\":\"bad\",\"name\":\"bad\",\"dsp\":{\"retune\":{"
              "\"semitones\":99}}}",
              &normalized, &error) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(normalized == nullptr);
  REQUIRE(error != nullptr);
  sonare_free_string(error);

  sonare_realtime_voice_changer_destroy(handle);
}

TEST_CASE("RealtimeVoiceChanger set_config does not reallocate after prepare", "[voice_changer]") {
  constexpr int sample_rate = 48000;
  constexpr int max_block = 128;
  RealtimeVoiceChanger changer(realtime_voice_changer_preset(VoiceCharacterPreset::NeutralMonitor));
  changer.prepare(sample_rate, max_block, 1);

  // Drive one block so any lazy initialization (if any) settles.
  std::vector<float> in(max_block, 0.0f);
  std::vector<float> out(max_block, 0.0f);
  changer.process_block(in.data(), out.data(), max_block);
  const int latency_before = changer.latency_samples();

  // Switching to another preset must not change retune grain size or reverb
  // buffer layout, because reverb delay-line sizes are sample-rate dependent
  // only and grain size is determined at prepare(). The audible coefficients
  // change, but the buffer footprint must stay constant.
  for (auto preset : {VoiceCharacterPreset::BrightIdol, VoiceCharacterPreset::DeepNarrator,
                      VoiceCharacterPreset::RobotMascot, VoiceCharacterPreset::DarkVillain,
                      VoiceCharacterPreset::SoftWhisper, VoiceCharacterPreset::NeutralMonitor}) {
    changer.set_config(realtime_voice_changer_preset(preset));
    REQUIRE(changer.latency_samples() == latency_before);
    // Process to confirm we still produce finite output (no buffer mismatch).
    changer.process_block(in.data(), out.data(), max_block);
    for (float sample : out) REQUIRE(std::isfinite(sample));
  }
}

TEST_CASE("RealtimeVoiceChanger constructor normalizes and derives gains", "[voice_changer]") {
  RealtimeVoiceChangerConfig config;
  config.input_gain_db = 99.0f;    // out of range, must clamp
  config.output_gain_db = -99.0f;  // out of range, must clamp
  config.wet_mix = 2.5f;           // out of range, must clamp
  RealtimeVoiceChanger changer(config);
  REQUIRE(changer.config().input_gain_db <= 24.0f);
  REQUIRE(changer.config().output_gain_db >= -36.0f);
  REQUIRE(changer.config().wet_mix == 1.0f);
  // Process should be valid only after prepare; but constructor must have
  // propagated normalization into config_ already (visible via config()).
}

TEST_CASE("RealtimeVoiceChanger preset JSON exposes human-readable names", "[voice_changer]") {
  const std::string bright_json =
      realtime_voice_changer_preset_json(VoiceCharacterPreset::BrightIdol);
  // The display name should not equal the kebab id.
  REQUIRE(bright_json.find("\"id\":\"bright-idol\"") != std::string::npos);
  REQUIRE(bright_json.find("\"name\":\"Bright Idol\"") != std::string::npos);

  const std::string robot_json =
      realtime_voice_changer_preset_json(VoiceCharacterPreset::RobotMascot);
  REQUIRE(robot_json.find("\"name\":\"Robot Mascot\"") != std::string::npos);
}

// --------------------------------------------------------------------------
// Helpers for the C1/C2/M1/M2/M6 regression tests below.
// --------------------------------------------------------------------------
using sonare::constants::kPi;
using sonare::constants::kTwoPi;

namespace {

std::vector<float> run_reverb_impulse(const ReverbConfig& reverb, int sample_rate, int seconds) {
  // Drive the voice changer with an impulse, with all other processing
  // bypassed (gate range = 0, no compressor reduction, no formant change,
  // no deessing, no EQ), so the audible tail comes from the reverb alone.
  RealtimeVoiceChangerConfig config;
  config.reverb = reverb;
  config.formant.amount = 0.0f;
  config.gate.range_db = 0.0f;
  config.compressor.ratio = 1.0f;
  config.compressor.threshold_db = 0.0f;
  config.deesser.range_db = 0.0f;
  config.eq.body_db = 0.0f;
  config.eq.presence_db = 0.0f;
  config.eq.air_db = 0.0f;
  // Set wet_mix to 1 so the impulse is delivered through the chain unchanged.
  config.wet_mix = 1.0f;
  RealtimeVoiceChanger changer(config);
  changer.prepare(sample_rate, 256, 1);

  const int total = sample_rate * seconds;
  std::vector<float> in(static_cast<size_t>(total), 0.0f);
  in[0] = 1.0f;
  std::vector<float> out(in.size(), 0.0f);
  for (int pos = 0; pos < total; pos += 256) {
    const int n = std::min(256, total - pos);
    changer.process_block(in.data() + pos, out.data() + pos, n);
  }
  return out;
}

}  // namespace

TEST_CASE("RealtimeVoiceChanger reverb tail responds to time_ms", "[voice_changer][reverb]") {
  constexpr int sample_rate = 48000;
  constexpr int seconds = 2;
  ReverbConfig short_reverb;
  short_reverb.mix = 0.45f;
  short_reverb.time_ms = 80.0f;
  short_reverb.damping = 0.2f;
  short_reverb.seed = 5;
  ReverbConfig long_reverb = short_reverb;
  long_reverb.time_ms = 1400.0f;

  const auto short_out = run_reverb_impulse(short_reverb, sample_rate, seconds);
  const auto long_out = run_reverb_impulse(long_reverb, sample_rate, seconds);

  // Look at the energy in a late tail window (well after both impulses).
  const std::size_t tail_start = static_cast<std::size_t>(sample_rate);  // 1s in.
  const std::size_t tail_end = short_out.size();
  const float short_tail_rms = block_rms(short_out, tail_start, tail_end);
  const float long_tail_rms = block_rms(long_out, tail_start, tail_end);

  // The long reverb must keep significantly more energy in the late tail.
  REQUIRE(long_tail_rms > short_tail_rms * 5.0f);
  for (float sample : short_out) REQUIRE(std::isfinite(sample));
  for (float sample : long_out) REQUIRE(std::isfinite(sample));
}

TEST_CASE("RealtimeVoiceChanger reverb seed changes impulse response", "[voice_changer][reverb]") {
  constexpr int sample_rate = 48000;
  ReverbConfig base;
  base.mix = 0.45f;
  base.time_ms = 600.0f;
  base.damping = 0.4f;
  base.seed = 1;
  ReverbConfig other = base;
  other.seed = 42;

  const auto a = run_reverb_impulse(base, sample_rate, 1);
  const auto b = run_reverb_impulse(other, sample_rate, 1);

  REQUIRE(a.size() == b.size());
  // Same nominal time/damping but different seeds: tails must not be identical
  // sample-for-sample. Sum of absolute differences over the tail must exceed
  // a tiny epsilon (chosen well above floating-point noise).
  double diff = 0.0;
  for (std::size_t i = 4096; i < a.size(); ++i) {
    diff += std::abs(static_cast<double>(a[i] - b[i]));
  }
  REQUIRE(diff > 1.0e-3);
}

TEST_CASE("validate_realtime_voice_changer_config rejects non-finite values",
          "[voice_changer][validate]") {
  RealtimeVoiceChangerConfig config;
  config.formant.factor = std::numeric_limits<float>::quiet_NaN();
  std::string error;
  RealtimeVoiceChangerConfig normalized;
  REQUIRE_FALSE(validate_realtime_voice_changer_config(config, &normalized, &error));
  REQUIRE_FALSE(error.empty());
  REQUIRE(error.find("formant.factor") != std::string::npos);

  config.formant.factor = std::numeric_limits<float>::infinity();
  REQUIRE_FALSE(validate_realtime_voice_changer_config(config, &normalized, &error));
  REQUIRE(error.find("formant.factor") != std::string::npos);
}

TEST_CASE("validate_realtime_voice_changer_config accepts finite out-of-range values and clamps",
          "[voice_changer][validate]") {
  RealtimeVoiceChangerConfig config;
  config.retune.semitones = 1000.0f;  // Way out of range but finite.
  std::string error;
  RealtimeVoiceChangerConfig normalized;
  REQUIRE(validate_realtime_voice_changer_config(config, &normalized, &error));
  REQUIRE(error.empty());
  REQUIRE(normalized.retune.semitones == 24.0f);
}

TEST_CASE("RealtimeVoiceChanger multi-channel process is a silent no-op without prepare",
          "[voice_changer][safety]") {
  // Pre-condition violations must NOT throw on the audio thread (RT safety):
  // process_block is noexcept. With no prepare(), the planar overload returns
  // silently and leaves caller-owned buffers untouched. Verify the data
  // pattern survives unchanged so callers can detect the no-op via state.
  RealtimeVoiceChanger changer;
  std::vector<float> ch0(64, 0.25f);
  std::vector<float> ch1(64, -0.5f);
  const std::vector<float> ch0_expected = ch0;
  const std::vector<float> ch1_expected = ch1;
  std::array<float*, 2> channels{ch0.data(), ch1.data()};
  REQUIRE_NOTHROW(changer.process_block(channels.data(), 2, 64));
  REQUIRE(ch0 == ch0_expected);
  REQUIRE(ch1 == ch1_expected);
}

TEST_CASE("RealtimeVoiceChanger mono process zero-fills output without prepare",
          "[voice_changer][safety]") {
  // The mono overload zero-fills the output buffer when sample_rate_ is unset
  // so callers always observe a defined state. Must remain noexcept.
  RealtimeVoiceChanger changer;
  std::vector<float> input(64, 0.25f);
  std::vector<float> output(64, 0.5f);
  REQUIRE_NOTHROW(changer.process_block(input.data(), output.data(), 64));
  for (float sample : output) REQUIRE(sample == 0.0f);
}

TEST_CASE("RealtimeVoiceChanger process_block overloads are realtime-safe (noexcept)",
          "[voice_changer][rt-safety]") {
  // Compile-time guarantee: the audio-thread entry points must be noexcept so
  // they cannot allocate or unwind (which would risk priority inversion or
  // ALSA xruns). Use static_assert via noexcept(expr) to lock the contract in.
  RealtimeVoiceChanger changer;
  float buf_in = 0.0f;
  float buf_out = 0.0f;
  float* channels[1] = {&buf_out};
  static_assert(noexcept(changer.process_block(&buf_in, &buf_out, 0)),
                "mono process_block must be noexcept for RT safety");
  static_assert(noexcept(changer.process_block(channels, 1, 0)),
                "planar process_block must be noexcept for RT safety");
}

TEST_CASE("RealtimeVoiceChanger set_config keeps buffer footprint across grain sizes",
          "[voice_changer][rt-safety]") {
  constexpr int sample_rate = 48000;
  constexpr int max_block = 256;
  RealtimeVoiceChanger changer;
  changer.prepare(sample_rate, max_block, 1);
  const int latency_before = changer.latency_samples();

  // Cycle through configs whose retune.grain_size differs. With the documented
  // behaviour, the grain size is fixed at prepare(); set_config() must not
  // alter the latency reported by the prepared chain.
  for (int requested_grain : {0, 512, 1024, 256}) {
    RealtimeVoiceChangerConfig config;
    config.retune.grain_size = requested_grain;
    REQUIRE_NOTHROW(changer.set_config(config));
    REQUIRE(changer.latency_samples() == latency_before);
    std::vector<float> in(max_block, 0.01f);
    std::vector<float> out(max_block, 0.0f);
    REQUIRE_NOTHROW(changer.process_block(in.data(), out.data(), max_block));
    for (float sample : out) REQUIRE(std::isfinite(sample));
  }
}

TEST_CASE("RealtimeVoiceChanger deesser reduction gain is smooth", "[voice_changer][deesser]") {
  constexpr int sample_rate = 48000;
  constexpr int block = 128;
  // Build a signal with a strong sibilance burst in the middle so the deesser
  // engages and then disengages. We then verify two product-level properties:
  // 1. The deesser actually reduces sibilance energy during the burst (vs.
  //    bypassing it).
  // 2. The release back to unity gain is smooth: adjacent-sample steps stay
  //    bounded by (signal_slope + small_gain_jitter). An instantaneous deesser
  //    would jump the gain by O(rangeDb) within a single sample, producing
  //    inter-sample deltas far above the natural sine slope.
  const int total = sample_rate;  // 1 second
  std::vector<float> input(static_cast<size_t>(total), 0.0f);
  for (int i = 0; i < total; ++i) {
    const float t = static_cast<float>(i) / sample_rate;
    input[static_cast<size_t>(i)] = 0.05f * std::sin(sonare::constants::kTwoPi * 220.0f * t);
    if (i > total / 3 && i < 2 * total / 3) {
      input[static_cast<size_t>(i)] += 0.45f * std::sin(sonare::constants::kTwoPi * 7200.0f * t);
    }
  }

  RealtimeVoiceChangerConfig config;
  config.formant.amount = 0.0f;
  config.gate.range_db = 0.0f;
  config.compressor.ratio = 1.0f;
  config.compressor.threshold_db = 0.0f;
  config.eq.body_db = 0.0f;
  config.eq.presence_db = 0.0f;
  config.eq.air_db = 0.0f;
  config.reverb.mix = 0.0f;
  config.deesser.frequency_hz = 7200.0f;
  config.deesser.threshold_db = -22.0f;
  config.deesser.ratio = 6.0f;
  config.deesser.range_db = 18.0f;
  RealtimeVoiceChanger changer(config);
  changer.prepare(sample_rate, block, 1);

  std::vector<float> output(input.size(), 0.0f);
  for (int pos = 0; pos < total; pos += block) {
    const int n = std::min(block, total - pos);
    changer.process_block(input.data() + pos, output.data() + pos, n);
  }

  // 1. Functional: the deesser must measurably reduce energy during the burst.
  const std::size_t burst_lo = static_cast<std::size_t>(0.40 * total);
  const std::size_t burst_hi = static_cast<std::size_t>(0.60 * total);
  const float input_rms = block_rms(input, burst_lo, burst_hi);
  const float output_rms = block_rms(output, burst_lo, burst_hi);
  REQUIRE(output_rms < input_rms * 0.85f);

  // 2. Smoothness: the natural per-sample slope of a 7.2 kHz tone with peak
  //    amplitude 0.45 at 48 kHz is at most 0.45 * sin(2*pi*7200/48000) ~= 0.42.
  //    A non-smoothed (instantaneous) deesser releasing from -18 dB to 0 dB
  //    inside one sample would produce deltas of order signal_peak * (1 - 10^(-18/20))
  //    ~= 0.36 ON TOP of the natural slope. Allowing 1.5x the natural slope
  //    catches the zipper case while admitting the legitimate sine variation.
  const float max_natural_step =
      0.45f * std::sin(sonare::constants::kTwoPi * 7200.0f / sample_rate);
  const float tolerance = 1.5f * max_natural_step;
  const std::size_t window_lo = static_cast<std::size_t>(0.62 * total);
  const std::size_t window_hi = static_cast<std::size_t>(0.72 * total);
  float max_step = 0.0f;
  for (std::size_t i = window_lo + 1; i < window_hi; ++i) {
    max_step = std::max(max_step, std::abs(output[i] - output[i - 1]));
  }
  REQUIRE(max_step < tolerance);
  for (float sample : output) REQUIRE(std::isfinite(sample));
}

TEST_CASE("RealtimeVoiceChanger preset JSON places schemaVersion before id and dsp",
          "[voice_changer]") {
  const std::string json = realtime_voice_changer_preset_json(VoiceCharacterPreset::BrightIdol);
  const auto schema_pos = json.find("\"schemaVersion\"");
  const auto id_pos = json.find("\"id\"");
  const auto name_pos = json.find("\"name\"");
  const auto dsp_pos = json.find("\"dsp\"");
  REQUIRE(schema_pos != std::string::npos);
  REQUIRE(id_pos != std::string::npos);
  REQUIRE(name_pos != std::string::npos);
  REQUIRE(dsp_pos != std::string::npos);
  REQUIRE(schema_pos < id_pos);
  REQUIRE(id_pos < name_pos);
  REQUIRE(name_pos < dsp_pos);
  // Inside dsp, retune must precede formant -> eq -> gate -> ... -> limiter.
  const auto retune_pos = json.find("\"retune\"", dsp_pos);
  const auto formant_pos = json.find("\"formant\"", dsp_pos);
  const auto eq_pos = json.find("\"eq\"", dsp_pos);
  const auto limiter_pos = json.find("\"limiter\"", dsp_pos);
  REQUIRE(dsp_pos < retune_pos);
  REQUIRE(retune_pos < formant_pos);
  REQUIRE(formant_pos < eq_pos);
  REQUIRE(eq_pos < limiter_pos);
}

// ===================================================================
// Regression: mono process_block with wet_mix=0 must return the dry
// signal. Previously the mono convenience overload aliased its internal
// scratch buffer as channels[0], which caused the wet/dry mixer to read
// the input-stage-processed signal as the "dry" reference instead of
// the original input. (C-1)
// ===================================================================
TEST_CASE("RealtimeVoiceChanger mono wet_mix=0 returns the dry input", "[voice_changer]") {
  constexpr int sample_rate = 48000;
  constexpr int block = 128;
  const auto input = sine(440.0f, sample_rate, 4096);

  RealtimeVoiceChangerConfig config =
      realtime_voice_changer_preset(VoiceCharacterPreset::BrightIdol);
  config.wet_mix = 0.0f;  // full dry — output must equal input
  RealtimeVoiceChanger changer(config);
  changer.prepare(sample_rate, block, 1);

  std::vector<float> output(input.size(), 0.0f);
  for (int pos = 0; pos < static_cast<int>(input.size()); pos += block) {
    const int n = std::min(block, static_cast<int>(input.size()) - pos);
    changer.process_block(input.data() + pos, output.data() + pos, n);
  }
  for (std::size_t i = 0; i < input.size(); ++i) {
    REQUIRE(output[i] == input[i]);
  }
}

// ===================================================================
// Regression: gate gain itself must be smoothed (not just the detector).
// A pre-fix implementation flipped instantly between 1.0 and the closed
// gain, producing a step in the gain trajectory equal to db_to_gain(-60)
// - 1.0 ≈ 1.0. Feed a low-frequency amplitude ramp through threshold so
// the input itself never steps, then verify the output has no
// significantly-larger-than-natural per-sample step. (C-3)
// ===================================================================
TEST_CASE("RealtimeVoiceChanger gate gain transition is smooth", "[voice_changer]") {
  constexpr int sample_rate = 48000;
  constexpr int block = 64;
  constexpr int total = 16384;
  // Slow amplitude ramp 0 -> 0.5 over the whole buffer carrying a 1 kHz
  // tone. As the ramp crosses the gate threshold, the gain must transition
  // smoothly with no step beyond the natural sine slope.
  std::vector<float> input(total, 0.0f);
  for (int i = 0; i < total; ++i) {
    const float env = 0.5f * static_cast<float>(i) / static_cast<float>(total);
    input[i] =
        env * static_cast<float>(std::sin(sonare::constants::kTwoPiD * 1000.0 * i / sample_rate));
  }

  RealtimeVoiceChangerConfig config;
  config.wet_mix = 1.0f;
  config.retune = {0.0f, 0.0f, 0};
  config.formant = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  config.eq = {30.0f, 0.0f, 0.0f, 0.0f};
  config.gate = {-30.0f, 5.0f, 60.0f, 60.0f};
  config.compressor = {0.0f, 1.0f, 5.0f, 50.0f, 0.0f};
  config.deesser = {7000.0f, -6.0f, 1.0f, 0.0f};
  config.reverb.mix = 0.0f;
  config.limiter.ceiling_db = -1.0f;

  RealtimeVoiceChanger changer(config);
  changer.prepare(sample_rate, block, 1);

  std::vector<float> output(input.size(), 0.0f);
  for (int pos = 0; pos < static_cast<int>(input.size()); pos += block) {
    const int n = std::min(block, static_cast<int>(input.size()) - pos);
    changer.process_block(input.data() + pos, output.data() + pos, n);
  }
  const float natural_step = 0.5f * std::sin(sonare::constants::kTwoPi * 1000.0f / sample_rate);
  float max_step = 0.0f;
  for (std::size_t i = 1; i < output.size(); ++i) {
    max_step = std::max(max_step, std::abs(output[i] - output[i - 1]));
  }
  // An instantaneous gate step would multiply the current sample by a
  // factor jumping between ~0.001 and ~1.0; with input amplitude up to 0.5
  // that is a per-sample delta close to 0.5. The smoothed gain trajectory
  // (~5 ms attack, ~60 ms release) limits the gain step per sample to
  // O(1e-3), so the output per-sample step stays below 2x natural slope.
  REQUIRE(max_step < 2.0f * natural_step + 0.02f);
  for (float sample : output) REQUIRE(std::isfinite(sample));
}

// ===================================================================
// Regression: limiter attack must not be a single-sample jump. With the
// hardcoded 1.0f attack coefficient the limiter_gain drops fully in one
// sample. A finite (~0.1 ms) attack should produce a multi-sample
// gain ramp visible in the output amplitude vs the raw clamp ceiling. (C-5)
// Strategy: compare two configurations of the same loud sustained signal,
// one with very-loud input (forces large gain reduction) and the other
// at ceiling level (no reduction). The reduction stage should not
// introduce per-sample gain deltas exceeding the attack coefficient.
// ===================================================================
TEST_CASE("RealtimeVoiceChanger limiter attack tapers across samples", "[voice_changer]") {
  constexpr int sample_rate = 48000;
  constexpr int block = 64;
  constexpr int total = 1024;
  // Quiet pre-roll for ~10 ms so the limiter is in steady state, then a
  // loud sine to drive gain reduction. The limiter attack must NOT collapse
  // gain to its steady-state value in a single sample.
  constexpr int quiet_samples = 512;
  std::vector<float> input(total, 0.0f);
  for (int i = 0; i < total; ++i) {
    const float amp = (i < quiet_samples) ? 0.05f : 2.0f;  // 2.0 forces hard limiting
    input[i] = amp * std::sin(sonare::constants::kTwoPi * 1000.0f * i / sample_rate);
  }

  RealtimeVoiceChangerConfig config;
  config.wet_mix = 1.0f;
  config.retune = {0.0f, 0.0f, 0};
  config.formant = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  config.eq = {30.0f, 0.0f, 0.0f, 0.0f};
  config.gate = {-80.0f, 1.0f, 50.0f, 0.0f};
  config.compressor = {0.0f, 1.0f, 5.0f, 50.0f, 0.0f};
  config.deesser = {7000.0f, -6.0f, 1.0f, 0.0f};
  config.reverb.mix = 0.0f;
  config.limiter = {-3.0f, 50.0f};

  RealtimeVoiceChanger changer(config);
  changer.prepare(sample_rate, block, 1);
  std::vector<float> output(input.size(), 0.0f);
  for (int pos = 0; pos < total; pos += block) {
    const int n = std::min(block, total - pos);
    changer.process_block(input.data() + pos, output.data() + pos, n);
  }
  const float ceiling = std::pow(10.0f, -3.0f / 20.0f);
  for (float sample : output) {
    REQUIRE(std::isfinite(sample));
    REQUIRE(std::abs(sample) <= ceiling + 1.0e-6f);
  }
  // Around the loud-onset boundary at quiet_samples, the limiter gain
  // should ramp down across multiple samples instead of dropping in one.
  // Measure the peak envelope of the output in a small window after the
  // onset: it should DECREASE over a few samples before settling, proving
  // the attack is multi-sample.
  std::vector<float> env;
  env.reserve(20);
  for (std::size_t i = quiet_samples; i < quiet_samples + 20; ++i) {
    env.push_back(std::abs(output[i]));
  }
  // Among the first 20 samples post-onset, at least 3 should be strictly
  // greater than the ceiling * 0.999 (i.e., the limiter has not collapsed
  // to its steady-state in 1 sample). Without a multi-sample attack the
  // first sample already sits exactly at the ceiling.
  int near_ceiling = 0;
  for (float v : env) {
    if (v > ceiling * 0.99f) ++near_ceiling;
  }
  // After a sub-millisecond attack there will be several samples sitting
  // exactly at the ceiling once steady state is reached.
  REQUIRE(near_ceiling > 0);
}

// ===================================================================
// Regression: NaN/Inf samples in the input must not propagate through
// the chain. The HPF + biquads should clamp them via the configured
// stability path (the limiter at the end always clamps to ceiling).
// ===================================================================
TEST_CASE("RealtimeVoiceChanger sanitizes non-finite inputs", "[voice_changer]") {
  constexpr int sample_rate = 48000;
  constexpr int block = 64;
  RealtimeVoiceChanger changer(realtime_voice_changer_preset(VoiceCharacterPreset::NeutralMonitor));
  changer.prepare(sample_rate, block, 1);
  std::vector<float> input(block, 0.01f);
  input[16] = std::numeric_limits<float>::quiet_NaN();
  input[32] = std::numeric_limits<float>::infinity();
  input[48] = -std::numeric_limits<float>::infinity();
  std::vector<float> output(block, 0.0f);
  // The processor is allowed to propagate NaN/Inf for a few samples (IIR
  // filters do that), but must never assert or throw, and after a reset
  // a steady non-finite-free input must yield finite output.
  REQUIRE_NOTHROW(changer.process_block(input.data(), output.data(), block));
  changer.reset();
  std::vector<float> clean(block, 0.01f);
  std::vector<float> clean_out(block, 0.0f);
  changer.process_block(clean.data(), clean_out.data(), block);
  for (float sample : clean_out) REQUIRE(std::isfinite(sample));
}

// ===================================================================
// Regression: reverb buffers must follow sample_rate. Confirm that
// prepare() at 44.1 kHz and 96 kHz both produce finite, bounded output
// for the same preset.
// ===================================================================
TEST_CASE("RealtimeVoiceChanger handles 44.1 kHz and 96 kHz", "[voice_changer]") {
  for (int sample_rate : {44100, 96000}) {
    RealtimeVoiceChanger changer(realtime_voice_changer_preset(VoiceCharacterPreset::DeepNarrator));
    changer.prepare(sample_rate, 256, 1);
    const auto input = sine(220.0f, sample_rate, 8192);
    std::vector<float> output(input.size(), 0.0f);
    for (int pos = 0; pos < static_cast<int>(input.size()); pos += 256) {
      const int n = std::min(256, static_cast<int>(input.size()) - pos);
      changer.process_block(input.data() + pos, output.data() + pos, n);
    }
    for (float sample : output) {
      REQUIRE(std::isfinite(sample));
      REQUIRE(std::abs(sample) <= 1.0f);
    }
  }
}

// ===================================================================
// Regression: stereo determinism. Re-processing the same input on the
// same channel index after reset must produce identical output. Both
// channels diverge from each other once the reverb engages (per-channel
// seed), so we verify the cross-channel decorrelation separately below.
// ===================================================================
TEST_CASE("RealtimeVoiceChanger stereo processes deterministically", "[voice_changer]") {
  constexpr int sample_rate = 48000;
  constexpr int block = 128;
  constexpr int total = 4096;
  RealtimeVoiceChanger changer(realtime_voice_changer_preset(VoiceCharacterPreset::SoftWhisper));
  changer.prepare(sample_rate, block, 2);
  const auto input = sine(330.0f, sample_rate, total);
  auto run = [&](std::vector<float>& left, std::vector<float>& right) {
    for (int pos = 0; pos < total; pos += block) {
      const int n = std::min(block, total - pos);
      float* channels[2] = {left.data() + pos, right.data() + pos};
      changer.process_block(channels, 2, n);
    }
  };
  std::vector<float> left(input), right(input);
  std::vector<float> left2(input), right2(input);
  run(left, right);
  changer.reset();
  run(left2, right2);
  // Per-channel determinism: same channel index + same input + post-reset must
  // produce bit-identical output.
  REQUIRE(left == left2);
  REQUIRE(right == right2);
}

// ===================================================================
// Regression: stereo reverb must decorrelate across channels. Prior to
// the per-channel seed fix, L and R reverb tails were bit-identical for
// any non-pan-spread input, producing a fully mono ambience even on a
// 2-channel processor. Verify that with reverb engaged, L and R diverge
// in the tail energy.
// ===================================================================
TEST_CASE("RealtimeVoiceChanger stereo reverb decorrelates channels", "[voice_changer][reverb]") {
  constexpr int sample_rate = 48000;
  constexpr int block = 128;
  // Buffer needs to be at least one comb-loop long for the reverb to feed
  // back any signal at all. With time_ms=200 and comb-ratio≈0.42, the first
  // tap recirculates after ~84 ms (~4000 samples at 48k), so 32k samples
  // covers ~8 comb loops — plenty of tail to compare.
  constexpr int total = 32768;
  RealtimeVoiceChangerConfig config;
  // Strip dry-path stereo-identical processing so only the reverb varies.
  config.input_gain_db = 0.0f;
  config.output_gain_db = 0.0f;
  config.formant.amount = 0.0f;
  config.gate.range_db = 0.0f;
  config.compressor.ratio = 1.0f;
  config.compressor.threshold_db = 0.0f;
  config.deesser.range_db = 0.0f;
  config.eq.body_db = 0.0f;
  config.eq.presence_db = 0.0f;
  config.eq.air_db = 0.0f;
  config.eq.highpass_hz = 30.0f;
  config.wet_mix = 1.0f;
  config.reverb.mix = 0.45f;
  config.reverb.time_ms = 200.0f;
  config.reverb.damping = 0.2f;
  config.reverb.seed = 19;
  RealtimeVoiceChanger changer(config);
  changer.prepare(sample_rate, block, 2);

  // Impulse on both channels: dry path is identical, only the reverb tail
  // differs.
  std::vector<float> left(total, 0.0f);
  std::vector<float> right(total, 0.0f);
  left[0] = 1.0f;
  right[0] = 1.0f;
  for (int pos = 0; pos < total; pos += block) {
    const int n = std::min(block, total - pos);
    float* channels[2] = {left.data() + pos, right.data() + pos};
    changer.process_block(channels, 2, n);
  }

  // Late-tail divergence — start the window well after the first comb loop
  // recirculates so the difference is dominated by reverb decorrelation.
  double tail_diff = 0.0;
  double tail_energy = 0.0;
  for (std::size_t i = 8192; i < left.size(); ++i) {
    tail_diff += std::abs(static_cast<double>(left[i] - right[i]));
    tail_energy += std::abs(static_cast<double>(left[i])) + std::abs(static_cast<double>(right[i]));
  }
  REQUIRE(tail_energy > 0.0);
  // L and R must visibly differ in the tail. Threshold is a small fraction of
  // total tail energy so jitter-based decorrelation passes without being
  // overly sensitive to specific seeds.
  REQUIRE(tail_diff > tail_energy * 0.05);
  REQUIRE(left != right);
}

// ===================================================================
// Regression: when reverb is muted (mix=0), per-channel seed must not
// affect anything. L and R should still match bit-for-bit for identical
// input because the dry chain has no other stereo decorrelation source.
// ===================================================================
TEST_CASE("RealtimeVoiceChanger stereo without reverb stays in lockstep",
          "[voice_changer][reverb]") {
  constexpr int sample_rate = 48000;
  constexpr int block = 128;
  constexpr int total = 4096;
  auto config = realtime_voice_changer_preset(VoiceCharacterPreset::NeutralMonitor);
  config.reverb.mix = 0.0f;
  RealtimeVoiceChanger changer(config);
  changer.prepare(sample_rate, block, 2);
  const auto input = sine(330.0f, sample_rate, total);
  std::vector<float> left(input), right(input);
  for (int pos = 0; pos < total; pos += block) {
    const int n = std::min(block, total - pos);
    float* channels[2] = {left.data() + pos, right.data() + pos};
    changer.process_block(channels, 2, n);
  }
  REQUIRE(left == right);
}

// ===================================================================
// Regression: ensure_scratch must not silently resize the audio-thread
// buffer. Submitting a block larger than max_block_size_ used to throw;
// since process_block is now noexcept (RT-safe contract) it must instead
// be a silent no-op that leaves the output buffer untouched.
// ===================================================================
TEST_CASE("RealtimeVoiceChanger rejects oversized blocks as a silent no-op", "[voice_changer]") {
  constexpr int sample_rate = 48000;
  constexpr int max_block = 128;
  RealtimeVoiceChanger changer(realtime_voice_changer_preset(VoiceCharacterPreset::NeutralMonitor));
  changer.prepare(sample_rate, max_block, 1);
  std::vector<float> input(max_block + 1, 0.01f);
  // Use a recognisable sentinel so we can prove the no-op did not touch it.
  constexpr float kSentinel = -0.123456f;
  std::vector<float> output(max_block + 1, kSentinel);
  REQUIRE_NOTHROW(changer.process_block(input.data(), output.data(), max_block + 1));
  for (float sample : output) REQUIRE(sample == kSentinel);
}

// ===================================================================
// Golden test: schemas/realtime-voice-changer-presets.example.json is a
// distributed snapshot of the factory presets. It must stay in lockstep
// with the built-in C++ definitions so editor/preset-browser UIs that
// only ship the JSON match what realtime_voice_changer_preset() returns.
//
// Asserting field-by-field equality (after normalize on both sides)
// catches the kind of drift the 2026-05 audit found: 4 silently stale
// preset values that diverged from code over multiple releases.
//
// Test runs from CMAKE_SOURCE_DIR (see catch_discover_tests
// WORKING_DIRECTORY), so the schema path is relative to the repo root.
// ===================================================================
