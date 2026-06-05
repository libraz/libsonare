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

  // grain_size is structural and fixed at prepare(): a runtime set_config()
  // (which runs on the audio thread and must not reallocate) keeps the
  // effective grain size and reports it back through config(), rather than
  // silently storing a value that never takes effect.
  configured.set_config({3.0f, 0.5f, 4096});
  REQUIRE(configured.grain_size() == 1024);
  REQUIRE(configured.config().grain_size == 1024);
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
