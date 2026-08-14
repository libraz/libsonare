/// @file voice_changer_retune_formant_test.cpp
/// @brief Retune and formant voice changer tests.

#include "voice_changer_test_helpers.h"

TEST_CASE("StreamingRetune shifts block pitch up an octave", "[voice_changer]") {
  constexpr int sample_rate = 48000;
  constexpr int samples = 32768;  // Long enough to flush the grain latency.
  constexpr float f0 = 220.0f;
  constexpr int block = 512;
  const auto input = sine(f0, sample_rate, samples);
  std::vector<float> output(static_cast<size_t>(samples), 0.0f);

  StreamingRetune retune({12.0f, 1.0f});  // +1 octave, fully wet.
  retune.prepare(sample_rate, block);

  // Stream block-by-block, respecting max_block_size from prepare().
  for (int pos = 0; pos < samples; pos += block) {
    const int n = std::min(block, samples - pos);
    retune.process_block(input.data() + pos, output.data() + pos, n);
  }

  for (float sample : output) {
    REQUIRE(std::isfinite(sample));
  }
  REQUIRE(zero_crossings(output) > zero_crossings(input));

  // Estimate the dominant output frequency past the initial latency region
  // (~grain_size). It should be about 2 * f0 (one octave up).
  const std::vector<float> steady(output.begin() + 8192, output.end());
  const float dominant = dominant_frequency(steady, sample_rate, 200.0f, 800.0f);
  REQUIRE_THAT(dominant, WithinRel(2.0f * f0, 0.08f));
}

TEST_CASE("StreamingRetune derives grain size from sample rate unless configured",
          "[voice_changer]") {
  StreamingRetune low_rate;
  low_rate.prepare(24000.0, 256);
  StreamingRetune high_rate;
  high_rate.prepare(96000.0, 256);

  REQUIRE(low_rate.grain_size() >= 256);
  REQUIRE(high_rate.grain_size() > low_rate.grain_size());
  REQUIRE(high_rate.grain_size() % 4 == 0);

  StreamingRetune configured({0.0f, 1.0f, 1024});
  configured.prepare(96000.0, 256);
  REQUIRE(configured.grain_size() == 1024);

  // Grain structure is independent of the host callback size. Both a caller's
  // explicit grain and the automatic sample-rate-derived grain must survive a
  // block that is larger than either value.
  StreamingRetune small_grain({0.0f, 1.0f, 64});
  small_grain.prepare(48000.0, 4096);
  REQUIRE(small_grain.grain_size() == 64);
  StreamingRetune large_block_auto;
  large_block_auto.prepare(48000.0, 4096);
  REQUIRE(large_block_auto.grain_size() == 2232);

  // grain_size is structural and fixed at prepare(): a runtime set_config()
  // (which runs on the audio thread and must not reallocate) keeps the
  // effective grain size and reports it back through config(), rather than
  // silently storing a value that never takes effect.
  configured.set_config({3.0f, 0.5f, 4096});
  REQUIRE(configured.grain_size() == 1024);
  REQUIRE(configured.config().grain_size == 1024);
}

TEST_CASE("StreamingRetune aligns dry and wet impulse peaks across mix values", "[voice_changer]") {
  constexpr int sample_rate = 48000;
  constexpr int block = 128;
  constexpr int grain = 512;
  constexpr int total = grain * 4;
  std::vector<float> input(total, 0.0f);
  input[0] = 1.0f;

  auto peak_position = [&](float mix) {
    StreamingRetune retune({0.0f, mix, grain});
    retune.prepare(sample_rate, block);
    std::vector<float> output(total, 0.0f);
    for (int pos = 0; pos < total; pos += block) {
      retune.process_block(input.data() + pos, output.data() + pos, block);
    }
    const auto peak = std::max_element(output.begin(), output.end(),
                                       [](float a, float b) { return std::abs(a) < std::abs(b); });
    REQUIRE(std::abs(*peak) > 1.0e-4f);
    return static_cast<int>(std::distance(output.begin(), peak));
  };

  const int dry_peak = peak_position(0.0f);
  const int halfway_peak = peak_position(0.5f);
  const int wet_peak = peak_position(1.0f);
  REQUIRE(dry_peak == grain * 3 / 4);
  REQUIRE(std::abs(halfway_peak - dry_peak) <= 1);
  REQUIRE(std::abs(wet_peak - dry_peak) <= 1);
}

TEST_CASE("RealtimeVoiceChanger aligns whole-chain dry and wet impulse peaks", "[voice_changer]") {
  constexpr int sample_rate = 48000;
  constexpr int block = 128;
  constexpr int grain = 512;
  constexpr int total = grain * 4;
  std::vector<float> input(total, 0.0f);
  input[0] = 1.0f;

  auto peak_position = [&](float wet_mix) {
    RealtimeVoiceChangerConfig config;
    config.wet_mix = wet_mix;
    config.retune = {0.0f, 1.0f, grain};
    config.formant = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    config.eq = {1.0f, 0.0f, 0.0f, 0.0f};
    config.gate = {-120.0f, 1.0f, 50.0f, 0.0f};
    config.compressor = {0.0f, 1.0f, 1.0f, 50.0f, 0.0f};
    config.deesser = {7000.0f, -6.0f, 1.0f, 0.0f};
    config.reverb.mix = 0.0f;
    config.limiter = {0.0f, 50.0f, false, -1.0f};
    RealtimeVoiceChanger changer(config);
    changer.prepare(sample_rate, block, 1);
    std::vector<float> output(total, 0.0f);
    for (int pos = 0; pos < total; pos += block) {
      changer.process_block(input.data() + pos, output.data() + pos, block);
    }
    const auto peak = std::max_element(output.begin(), output.end(),
                                       [](float a, float b) { return std::abs(a) < std::abs(b); });
    REQUIRE(std::abs(*peak) > 1.0e-4f);
    return static_cast<int>(std::distance(output.begin(), peak));
  };

  const int dry_peak = peak_position(0.0f);
  const int halfway_peak = peak_position(0.5f);
  const int wet_peak = peak_position(1.0f);
  REQUIRE(dry_peak == grain * 3 / 4);
  REQUIRE(std::abs(halfway_peak - dry_peak) <= 1);
  REQUIRE(std::abs(wet_peak - dry_peak) <= 1);
}

TEST_CASE("StreamingRetune process_block is noexcept on the audio thread",
          "[voice_changer][rt-safety]") {
  // Compile-time guarantee: noexcept is part of the contract because the
  // immediate caller (RealtimeVoiceChanger::process_block) is noexcept.
  // Throwing here would call std::terminate and crash the audio thread.
  StreamingRetune retune;
  float buf_in = 0.0f;
  float buf_out = 0.0f;
  static_assert(noexcept(retune.process_block(&buf_in, &buf_out, 0)),
                "StreamingRetune::process_block must be noexcept for RT safety");
}

TEST_CASE("StreamingRetune passes input through without prepare", "[voice_changer][rt-safety]") {
  // Without prepare() the retune has no ring buffer / grain state to drive
  // the OLA path. Passing through the input keeps the chain audible (vs.
  // emitting silence) without invoking any throwing branch.
  StreamingRetune retune;
  std::vector<float> input(64, 0.25f);
  std::vector<float> output(64, -1.0f);
  REQUIRE_NOTHROW(retune.process_block(input.data(), output.data(), 64));
  for (std::size_t i = 0; i < input.size(); ++i) {
    REQUIRE(output[i] == input[i]);
  }
}

TEST_CASE("StreamingRetune rejects oversized blocks as a silent no-op",
          "[voice_changer][rt-safety]") {
  // The audio thread cannot reallocate the ring/accumulator buffers. Blocks
  // larger than the prepare()-time max must be ignored rather than throwing.
  StreamingRetune retune;
  retune.prepare(48000.0, 128);
  std::vector<float> input(129, 0.5f);
  constexpr float kSentinel = -0.987654f;
  std::vector<float> output(129, kSentinel);
  REQUIRE_NOTHROW(retune.process_block(input.data(), output.data(), 129));
  for (float sample : output) REQUIRE(sample == kSentinel);
}

TEST_CASE("StreamingRetune ignores null buffers without throwing", "[voice_changer][rt-safety]") {
  // Defensive: even with prepare() done, a buggy caller passing null must
  // be a no-op (not an exception). This keeps the noexcept contract honest.
  StreamingRetune retune;
  retune.prepare(48000.0, 64);
  std::vector<float> input(64, 0.1f);
  std::vector<float> output(64, 0.0f);
  REQUIRE_NOTHROW(retune.process_block(nullptr, output.data(), 64));
  REQUIRE_NOTHROW(retune.process_block(input.data(), nullptr, 64));
  REQUIRE_NOTHROW(retune.process_block(nullptr, nullptr, 0));
  REQUIRE_NOTHROW(retune.process_block(input.data(), output.data(), -1));
}

TEST_CASE("FormantWarp raises the spectral envelope when factor > 1", "[voice_changer]") {
  constexpr int sample_rate = 22050;
  constexpr int n = sample_rate / 2;
  constexpr float f0 = 150.0f;
  // Vowel-like source: harmonics of f0 with a formant-shaped magnitude envelope
  // peaking near 900 Hz. This gives a clear spectral envelope to warp.
  std::vector<float> samples(static_cast<size_t>(n), 0.0f);
  constexpr float formant_hz = 900.0f;
  constexpr float bandwidth_hz = 600.0f;
  for (int h = 1; h * f0 < static_cast<float>(n); ++h) {
    const float harm_hz = h * f0;
    const float env = 1.0f / (1.0f + std::pow((harm_hz - formant_hz) / bandwidth_hz, 2.0f));
    for (int i = 0; i < n; ++i) {
      samples[static_cast<size_t>(i)] +=
          0.2f * env *
          static_cast<float>(std::sin(sonare::constants::kTwoPiD * harm_hz *
                                      static_cast<double>(i) / sample_rate));
    }
  }
  const sonare::Audio audio = sonare::Audio::from_vector(std::vector<float>(samples), sample_rate);

  FormantWarp warp({1.3f, 12, 1.0f});  // Raise formants.
  const sonare::Audio warped = warp.process(audio);

  REQUIRE(warped.size() == audio.size());
  REQUIRE(warped.sample_rate() == audio.sample_rate());
  for (float sample : warped) {
    REQUIRE(std::isfinite(sample));
  }

  // Measure spectral centroid over a steady mid-signal segment.
  const int start = n / 4;
  const std::vector<float> in_seg(samples.begin() + start, samples.end());
  std::vector<float> out_vec(warped.data(), warped.data() + warped.size());
  const std::vector<float> out_seg(out_vec.begin() + start, out_vec.end());

  const float centroid_in = spectral_centroid(in_seg, sample_rate);
  const float centroid_out = spectral_centroid(out_seg, sample_rate);
  REQUIRE(centroid_in > 0.0f);
  // Raising formants pushes spectral energy upward.
  REQUIRE(centroid_out > centroid_in);
}

TEST_CASE("FormantWarp preserves signal level across warp factors", "[voice_changer]") {
  constexpr int sample_rate = 32000;
  constexpr int n = sample_rate;  // 1 s, long enough for a steady interior.
  const std::vector<float> samples = vowel(150.0f, 900.0f, sample_rate, n, 0.2f);
  const sonare::Audio audio = sonare::Audio::from_vector(samples, sample_rate);

  // Skip the first and last frame's worth of OLA edge so only steady-state
  // frames are measured.
  constexpr std::size_t kEdge = 2048;
  const float in_rms = block_rms(samples, kEdge, samples.size() - kEdge);
  REQUIRE(in_rms > 0.0f);

  // Warping reshapes the envelope, so the level moves a little; it must not
  // collapse. A gain error of the LPC prediction gain shows up as tens of dB.
  for (float factor : {0.8f, 1.2f, 1.5f}) {
    const sonare::Audio warped = FormantWarp({factor, 12, 1.0f}).process(audio);
    const std::vector<float> out(warped.begin(), warped.end());
    const float out_rms = block_rms(out, kEdge, out.size() - kEdge);
    const float delta_db = 20.0f * std::log10(out_rms / in_rms);
    INFO("factor " << factor << " level delta " << delta_db << " dB");
    REQUIRE(std::abs(delta_db) < 4.0f);
  }
}

TEST_CASE("FormantWarp gain is independent of input level", "[voice_changer]") {
  constexpr int sample_rate = 32000;
  constexpr int n = sample_rate / 2;
  const std::vector<float> loud = vowel(150.0f, 900.0f, sample_rate, n, 0.2f);
  std::vector<float> quiet(loud.size());
  for (std::size_t i = 0; i < loud.size(); ++i) quiet[i] = 0.25f * loud[i];

  auto render = [&](const std::vector<float>& in) {
    const sonare::Audio out =
        FormantWarp({1.3f, 12, 1.0f}).process(sonare::Audio::from_vector(in, sample_rate));
    return std::vector<float>(out.begin(), out.end());
  };

  const std::vector<float> loud_out = render(loud);
  const std::vector<float> quiet_out = render(quiet);
  REQUIRE(loud_out.size() == quiet_out.size());

  // Recolouring the residual with an envelope that still carries the frame's
  // excitation gain makes the transfer quadratic in input level; the warp must
  // stay homogeneous instead.
  constexpr std::size_t kEdge = 2048;
  const float loud_rms = block_rms(loud_out, kEdge, loud_out.size() - kEdge);
  const float quiet_rms = block_rms(quiet_out, kEdge, quiet_out.size() - kEdge);
  REQUIRE(loud_rms > 0.0f);
  REQUIRE_THAT(quiet_rms, WithinRel(0.25f * loud_rms, 0.02f));
}

TEST_CASE("FormantWarp clamps finite formant factors to realtime bounds", "[voice_changer]") {
  constexpr int sample_rate = 22050;
  const sonare::Audio audio =
      sonare::Audio::from_vector(sine(220.0f, sample_rate, sample_rate / 3), sample_rate);

  auto render = [&](float factor) {
    const sonare::Audio out = FormantWarp({factor, 12, 1.0f}).process(audio);
    return std::vector<float>(out.begin(), out.end());
  };

  const auto very_low = render(-2.0f);
  const auto low_bound = render(kFormantFactorMin);
  const auto very_high = render(9.0f);
  const auto high_bound = render(kFormantFactorMax);

  REQUIRE(very_low.size() == low_bound.size());
  REQUIRE(very_high.size() == high_bound.size());
  for (std::size_t i = 0; i < low_bound.size(); ++i) {
    REQUIRE(very_low[i] == low_bound[i]);
    REQUIRE(very_high[i] == high_bound[i]);
  }
}

TEST_CASE("FormantWarp and VoiceChanger bypass a unity formant factor", "[voice_changer]") {
  constexpr int sample_rate = 22050;
  const std::vector<float> samples = sine(220.0f, sample_rate, sample_rate / 3);
  const sonare::Audio audio = sonare::Audio::from_vector(samples, sample_rate);

  const sonare::Audio warped = FormantWarp({1.0f, 12, 1.0f}).process(audio);
  VoiceChanger changer({0.0f, 1.0f});
  const sonare::Audio changed = changer.process(audio);

  REQUIRE(std::vector<float>(warped.begin(), warped.end()) == samples);
  REQUIRE(std::vector<float>(changed.begin(), changed.end()) == samples);
}

TEST_CASE("VoiceChanger combines pitch and formant controls", "[voice_changer]") {
  constexpr int sample_rate = 22050;
  auto samples = sine(220.0f, sample_rate, sample_rate / 2);
  const sonare::Audio audio = sonare::Audio::from_vector(std::move(samples), sample_rate);

  VoiceChangerConfig config;
  config.pitch_semitones = 7.0f;
  config.formant_factor = 1.15f;
  VoiceChanger changer(config);
  const sonare::Audio changed = changer.process(audio);

  REQUIRE(!changed.empty());
  REQUIRE(changed.sample_rate() == audio.sample_rate());
  REQUIRE_THAT(changed.duration(), WithinRel(audio.duration(), 0.05f));
}

TEST_CASE("StreamingFormant changes spectral color without changing duration", "[voice_changer]") {
  constexpr int sample_rate = 48000;
  constexpr int block = 128;
  constexpr int samples = 8192;
  auto input = sine(180.0f, sample_rate, samples);
  for (int i = 0; i < samples; ++i) {
    input[static_cast<size_t>(i)] +=
        0.25f * std::sin(sonare::constants::kTwoPiD * 720.0 * i / sample_rate);
  }
  std::vector<float> output(input.size(), 0.0f);

  StreamingFormant formant({1.35f, 1.0f, -0.3f, 0.8f, 0.2f});
  formant.prepare(sample_rate, block);
  for (int pos = 0; pos < samples; pos += block) {
    const int n = std::min(block, samples - pos);
    formant.process_block(input.data() + pos, output.data() + pos, n);
  }

  for (float sample : output) REQUIRE(std::isfinite(sample));
  REQUIRE(output.size() == input.size());
  REQUIRE(spectral_centroid(output, sample_rate) > spectral_centroid(input, sample_rate));
}

TEST_CASE("StreamingFormant applies amount once and preserves tonal controls at zero",
          "[voice_changer]") {
  constexpr int sample_rate = 48000;
  constexpr int block = 128;
  const auto input = sine(440.0f, sample_rate, 1024);
  std::vector<float> half_shift(input.size(), 0.0f);
  std::vector<float> full_equivalent(input.size(), 0.0f);
  std::vector<float> brightness_only(input.size(), 0.0f);
  std::vector<float> neutral(input.size(), 0.0f);

  // factor 1.5 at amount 0.5 resolves to the same 1.25 effective factor as a
  // full-strength 1.25 setting. This catches the former amount-squared path.
  StreamingFormant half({1.5f, 0.5f, 0.0f, 0.0f, 0.0f});
  StreamingFormant full({1.25f, 1.0f, 0.0f, 0.0f, 0.0f});
  StreamingFormant bright({1.0f, 0.0f, 0.0f, 0.7f, 0.0f});
  StreamingFormant plain({1.0f, 0.0f, 0.0f, 0.0f, 0.0f});
  for (StreamingFormant* formant : {&half, &full, &bright, &plain}) {
    formant->prepare(sample_rate, block);
  }
  for (size_t offset = 0; offset < input.size(); offset += block) {
    const int count = static_cast<int>(std::min(static_cast<size_t>(block), input.size() - offset));
    half.process_block(input.data() + offset, half_shift.data() + offset, count);
    full.process_block(input.data() + offset, full_equivalent.data() + offset, count);
    bright.process_block(input.data() + offset, brightness_only.data() + offset, count);
    plain.process_block(input.data() + offset, neutral.data() + offset, count);
  }

  float max_shift_difference = 0.0f;
  float max_brightness_difference = 0.0f;
  for (size_t i = 0; i < input.size(); ++i) {
    max_shift_difference =
        std::max(max_shift_difference, std::abs(half_shift[i] - full_equivalent[i]));
    max_brightness_difference =
        std::max(max_brightness_difference, std::abs(brightness_only[i] - neutral[i]));
  }
  REQUIRE(max_shift_difference < 1.0e-5f);
  REQUIRE(max_brightness_difference > 1.0e-4f);
}

TEST_CASE("StreamingFormant live updates are invariant to caller chunking",
          "[voice_changer][chunk]") {
  constexpr int sample_rate = 48000;
  constexpr int max_block = 256;
  constexpr int pre_roll = 19;  // Deliberately leaves the 32-sample cadence unaligned.
  constexpr int rendered = 192;
  const int total = pre_roll + rendered;

  const auto input = [&] {
    std::vector<float> signal(static_cast<size_t>(total), 0.0f);
    for (int i = 0; i < total; ++i) {
      signal[static_cast<size_t>(i)] =
          0.45f * std::sin(sonare::constants::kTwoPiD * 180.0 * i / sample_rate) +
          0.20f * std::sin(sonare::constants::kTwoPiD * 900.0 * i / sample_rate) +
          0.10f * std::sin(sonare::constants::kTwoPiD * 5200.0 * i / sample_rate);
    }
    return signal;
  }();

  const StreamingFormantConfig initial{1.0f, 1.0f, 0.0f, 0.0f, 0.0f};
  const StreamingFormantConfig updated{1.35f, 1.0f, -0.6f, 0.7f, 0.4f};
  StreamingFormant one_block(initial);
  StreamingFormant ragged(initial);
  StreamingFormant unchanged(initial);
  one_block.prepare(sample_rate, max_block);
  ragged.prepare(sample_rate, max_block);
  unchanged.prepare(sample_rate, max_block);
  std::vector<float> pre_output(static_cast<size_t>(pre_roll), 0.0f);
  one_block.process_block(input.data(), pre_output.data(), pre_roll);
  ragged.process_block(input.data(), pre_output.data(), pre_roll);
  unchanged.process_block(input.data(), pre_output.data(), pre_roll);

  one_block.set_config(updated);
  ragged.set_config(updated);
  std::vector<float> one_block_output(static_cast<size_t>(rendered), 0.0f);
  std::vector<float> ragged_output(static_cast<size_t>(rendered), 0.0f);
  std::vector<float> unchanged_output(static_cast<size_t>(rendered), 0.0f);
  one_block.process_block(input.data() + pre_roll, one_block_output.data(), rendered);
  unchanged.process_block(input.data() + pre_roll, unchanged_output.data(), rendered);

  constexpr int kChunkSizes[] = {17, 31, 64, 80};
  int offset = 0;
  for (const int chunk : kChunkSizes) {
    ragged.process_block(input.data() + pre_roll + offset, ragged_output.data() + offset, chunk);
    offset += chunk;
  }
  REQUIRE(offset == rendered);
  REQUIRE(one_block_output == ragged_output);
  REQUIRE(one_block_output != unchanged_output);
}
