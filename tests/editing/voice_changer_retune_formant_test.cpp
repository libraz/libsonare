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
  REQUIRE(dry_peak == grain);
  REQUIRE(std::abs(halfway_peak - dry_peak) <= 1);
  REQUIRE(std::abs(wet_peak - dry_peak) <= 1);
}

namespace {

constexpr int kRetuneRate = 48000;
constexpr int kRetuneBlock = 128;
constexpr int kRetuneGrain = 512;
constexpr int kRetuneTotal = 12000;  // 0.25 s, several grains past the latency.

// Streams `input` through a unity-pitch retune at the requested mix.
std::vector<float> retune_render(const std::vector<float>& input, float mix) {
  StreamingRetune retune({0.0f, mix, kRetuneGrain});
  retune.prepare(kRetuneRate, kRetuneBlock);
  std::vector<float> output(input.size(), 0.0f);
  const int total = static_cast<int>(input.size());
  for (int pos = 0; pos < total; pos += kRetuneBlock) {
    const int n = std::min(kRetuneBlock, total - pos);
    retune.process_block(input.data() + pos, output.data() + pos, n);
  }
  return output;
}

}  // namespace

TEST_CASE("StreamingRetune at unity pitch reproduces its input after the latency",
          "[voice_changer]") {
  StreamingRetune probe({0.0f, 1.0f, kRetuneGrain});
  probe.prepare(kRetuneRate, kRetuneBlock);
  const std::size_t latency = static_cast<std::size_t>(probe.latency_samples());
  // The drain tap trails the newest grain by a full grain so no output sample
  // is ever normalized against a partial set of the grains overlapping it.
  REQUIRE(latency == static_cast<std::size_t>(kRetuneGrain));

  // A constant input isolates the OLA normalization: past the latency each
  // output sample is the normalized sum of the overlapping grains, so a wrong
  // divisor shows up as a level offset (dividing by sum(w*w) instead of sum(w)
  // is a fixed 2/1.5 = +2.5 dB) and an early drain as a sawtooth at the hop
  // rate (86 Hz for the default grain at 48 kHz).
  const std::vector<float> dc(kRetuneTotal, 0.5f);
  const std::vector<float> dc_out = retune_render(dc, 1.0f);
  float dc_min = dc_out[latency];
  float dc_max = dc_out[latency];
  for (std::size_t i = latency; i < dc_out.size(); ++i) {
    dc_min = std::min(dc_min, dc_out[i]);
    dc_max = std::max(dc_max, dc_out[i]);
  }
  REQUIRE_THAT(dc_min, WithinRel(0.5f, 1.0e-4f));
  REQUIRE_THAT(dc_max, WithinRel(0.5f, 1.0e-4f));
  // No periodic amplitude variation: the level is flat, not swept per hop.
  REQUIRE(20.0f * std::log10(dc_max / dc_min) < 0.01f);

  // A tone additionally exercises the fractional ring read.
  const std::vector<float> tone = sine(1000.0f, kRetuneRate, kRetuneTotal);
  const std::vector<float> tone_out = retune_render(tone, 1.0f);
  float max_error = 0.0f;
  for (std::size_t i = latency; i < tone_out.size(); ++i) {
    max_error = std::max(max_error, std::abs(tone_out[i] - tone[i - latency]));
  }
  INFO("max sample deviation from the delayed input: " << max_error);
  REQUIRE(max_error < 1.0e-5f);
  REQUIRE_THAT(block_rms(tone_out, latency, tone_out.size()),
               WithinRel(block_rms(tone, 0, tone.size() - latency), 1.0e-3f));
}

TEST_CASE("StreamingRetune mix is a level blend at every mix value", "[voice_changer]") {
  // At unity pitch the wet path carries the same signal as the delayed dry
  // path, so an honest level blend leaves peak and RMS untouched for every
  // mix value. A wet path with a gain error would make the mix control a
  // level shift instead of a crossfade.
  const std::vector<float> tone = sine(1000.0f, kRetuneRate, kRetuneTotal);
  const std::size_t latency = static_cast<std::size_t>(kRetuneGrain);
  const auto peak = [](const std::vector<float>& v, std::size_t start, std::size_t end) {
    float best = 0.0f;
    for (std::size_t i = start; i < end; ++i) best = std::max(best, std::abs(v[i]));
    return best;
  };
  const float input_peak = peak(tone, 0, tone.size() - latency);
  const float input_rms = block_rms(tone, 0, tone.size() - latency);
  REQUIRE(input_peak > 0.0f);
  REQUIRE(input_rms > 0.0f);

  for (const float mix : {0.0f, 0.5f, 1.0f}) {
    const std::vector<float> output = retune_render(tone, mix);
    INFO("mix " << mix);
    REQUIRE_THAT(peak(output, latency, output.size()), WithinRel(input_peak, 1.0e-3f));
    REQUIRE_THAT(block_rms(output, latency, output.size()), WithinRel(input_rms, 1.0e-3f));
    float max_error = 0.0f;
    for (std::size_t i = latency; i < output.size(); ++i) {
      max_error = std::max(max_error, std::abs(output[i] - tone[i - latency]));
    }
    REQUIRE(max_error < 1.0e-5f);
  }
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
  REQUIRE(dry_peak == grain);
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

// The requested grain size and the effective one shared a field: set_config()
// overwrote config_.grain_size with what the LAST prepare() resolved, and
// prepare() then read that same field to resolve the next one. A new request
// therefore never survived to the prepare() that was supposed to apply it, and
// re-preparing at a different rate froze the first rate's answer instead of
// re-deriving -- with latency_samples() reporting a delay that no longer
// existed, so a host's PDC compensated for it.
TEST_CASE("StreamingRetune applies a grain size requested after prepare", "[voice_changer]") {
  StreamingRetune retune;
  retune.prepare(48000.0, 256);
  const int derived = retune.grain_size();
  REQUIRE(derived == 2232);

  // The request survives until the prepare() that applies it, and config()
  // keeps reporting the EFFECTIVE grain in the meantime (both halves matter).
  retune.set_config({0.0f, 1.0f, 512});
  REQUIRE(retune.grain_size() == derived);
  REQUIRE(retune.config().grain_size == derived);

  retune.prepare(48000.0, 256);
  REQUIRE(retune.grain_size() == 512);
  REQUIRE(retune.latency_samples() == 512);
}

TEST_CASE("StreamingRetune re-derives an automatic grain at a new sample rate", "[voice_changer]") {
  StreamingRetune retune;
  retune.prepare(48000.0, 256);
  const int at_48k = retune.grain_size();

  // A 0 request means "derive": re-preparing at half the rate must halve the
  // grain, not keep the first rate's resolved value.
  retune.prepare(22050.0, 256);
  const int at_22k = retune.grain_size();
  REQUIRE(at_22k < at_48k);
  REQUIRE(at_22k == 1024);

  // Still true after a live set_config that does not ask for a new grain.
  retune.set_config({2.0f, 0.5f, 0});
  retune.prepare(48000.0, 256);
  REQUIRE(retune.grain_size() == at_48k);
}

TEST_CASE("RealtimeVoiceChanger applies a retune grain requested after prepare",
          "[voice_changer]") {
  RealtimeVoiceChanger changer;
  changer.prepare(48000.0, 256, 1);
  const int derived = changer.config().retune.grain_size;
  REQUIRE(derived > 0);

  RealtimeVoiceChangerConfig config = changer.config();
  config.retune.grain_size = 512;
  changer.set_config(config);
  // Still the effective one until the next prepare, as documented.
  REQUIRE(changer.config().retune.grain_size == derived);

  changer.prepare(48000.0, 256, 1);
  REQUIRE(changer.config().retune.grain_size == 512);
}
