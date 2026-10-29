#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <vector>

#include "mastering/saturation/bitcrusher.h"
#include "mastering/saturation/waveshaper.h"
#include "rt/tail_budget.h"

namespace {

using sonare::mastering::saturation::BitCrusher;
using sonare::mastering::saturation::BitCrusherConfig;
using sonare::mastering::saturation::BitCrusherDiscType;
using sonare::mastering::saturation::QuantizerMode;
using sonare::mastering::saturation::Waveshaper;
using sonare::mastering::saturation::WaveshaperConfig;
using sonare::mastering::saturation::WaveshaperCurve;
using sonare::rt::TailBudget;

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 257;
constexpr size_t kRenderSamples = 48000;
constexpr size_t kLateWindow = 4096;

template <typename Processor>
void process_mono(Processor& processor, std::vector<float>& samples) {
  for (size_t offset = 0; offset < samples.size(); offset += static_cast<size_t>(kBlockSize)) {
    const int count = static_cast<int>(
        std::min<size_t>(static_cast<size_t>(kBlockSize), samples.size() - offset));
    float* channels[] = {samples.data() + offset};
    processor.process(channels, 1, count);
  }
}

template <typename Processor>
std::vector<float> render_silence(Processor& processor, size_t count = kRenderSamples) {
  std::vector<float> samples(count, 0.0f);
  process_mono(processor, samples);
  return samples;
}

float peak(const std::vector<float>& samples, size_t begin) {
  float result = 0.0f;
  for (size_t i = begin; i < samples.size(); ++i) result = std::max(result, std::abs(samples[i]));
  return result;
}

void require_persistent_output(const std::vector<float>& samples) {
  const size_t begin = samples.size() - kLateWindow;
  REQUIRE(std::all_of(samples.begin() + static_cast<std::ptrdiff_t>(begin), samples.end(),
                      [](float sample) { return std::isfinite(sample); }));
  INFO("late window peak=" << peak(samples, begin));
  REQUIRE(peak(samples, begin) > sonare::rt::kTailFloor);
}

void require_transient_output(const std::vector<float>& samples) {
  REQUIRE(std::all_of(samples.begin(), samples.end(),
                      [](float sample) { return std::isfinite(sample); }));
  INFO("transient peak=" << peak(samples, 0));
  REQUIRE(peak(samples, 0) > sonare::rt::kTailFloor);
}

void require_silent_waveshaper_output(WaveshaperConfig config) {
  Waveshaper shaper(config);
  shaper.prepare(kSampleRate, kBlockSize);
  const std::vector<float> samples = render_silence(shaper, 1024);
  REQUIRE(std::all_of(samples.begin(), samples.end(),
                      [](float sample) { return std::isfinite(sample); }));
  REQUIRE(std::all_of(samples.begin(), samples.end(), [](float sample) { return sample == 0.0f; }));
}

struct NoiseCase {
  const char* name;
  void (*enable)(BitCrusherConfig&);
};

constexpr NoiseCase kNoiseSources[] = {
    {"radio", [](BitCrusherConfig& config) { config.radio_noise_level = 0.5f; }},
    {"white", [](BitCrusherConfig& config) { config.wp_noise_level = 0.5f; }},
    {"pink",
     [](BitCrusherConfig& config) {
       config.wp_noise_level = 0.5f;
       config.wp_noise_pink = true;
     }},
    {"disc",
     [](BitCrusherConfig& config) {
       config.disc_noise_level = 0.5f;
       config.disc_type = BitCrusherDiscType::kRnd;
     }},
    {"hum", [](BitCrusherConfig& config) { config.hum_level = 0.5f; }},
};

}  // namespace

TEST_CASE("BitCrusher reports active noise sources as unbounded idle tails", "[idle-source-tail]") {
  for (const NoiseCase& source : kNoiseSources) {
    DYNAMIC_SECTION("source=" << source.name) {
      BitCrusherConfig config;
      config.quantizer_mode = QuantizerMode::kOff;
      source.enable(config);

      BitCrusher crusher(config);
      crusher.prepare(kSampleRate, kBlockSize);
      CHECK(crusher.tail_samples() == TailBudget::kUnbounded);
      require_persistent_output(render_silence(crusher));
    }
  }
}

TEST_CASE("BitCrusher keeps noise tails audible under both mix laws", "[idle-source-tail]") {
  for (const auto law :
       {sonare::effects::common::MixLaw::kCrossfade, sonare::effects::common::MixLaw::kTwoRamps}) {
    DYNAMIC_SECTION("mix law=" << static_cast<int>(law)) {
      BitCrusherConfig config;
      config.quantizer_mode = QuantizerMode::kOff;
      config.wp_noise_level = 0.5f;
      config.mix = 0.25f;
      config.mix_law = law;

      BitCrusher crusher(config);
      crusher.prepare(kSampleRate, kBlockSize);
      CHECK(crusher.tail_samples() == TailBudget::kUnbounded);
      require_persistent_output(render_silence(crusher));
    }
  }
}

TEST_CASE("BitCrusher reports dither as an unbounded idle source", "[idle-source-tail]") {
  for (const auto type : {sonare::mastering::final::DitherType::Tpdf,
                          sonare::mastering::final::DitherType::NoiseShaped}) {
    DYNAMIC_SECTION("dither=" << static_cast<int>(type)) {
      BitCrusherConfig config;
      config.bit_depth = 12;
      config.dither_type = type;
      config.mix_law = sonare::effects::common::MixLaw::kTwoRamps;
      config.mix = 0.25f;

      BitCrusher crusher(config);
      crusher.prepare(kSampleRate, kBlockSize);
      CHECK(crusher.tail_samples() == TailBudget::kUnbounded);
      require_persistent_output(render_silence(crusher));
    }
  }
}

TEST_CASE("BitCrusher treats a stopped hum oscillator as a persistent source",
          "[idle-source-tail]") {
  BitCrusherConfig config;
  config.quantizer_mode = QuantizerMode::kOff;
  config.hum_level = 0.5f;
  BitCrusher crusher(config);
  crusher.prepare(kSampleRate, kBlockSize);
  std::vector<float> warmup(8000, 0.0f);
  process_mono(crusher, warmup);
  REQUIRE(crusher.set_parameter(14, 0.0f));

  CHECK(crusher.tail_samples() == TailBudget::kUnbounded);
  require_persistent_output(render_silence(crusher));
}

TEST_CASE("BitCrusher only reports noise that reaches its wet output", "[idle-source-tail]") {
  const auto finite_tail = [](BitCrusherConfig config) {
    BitCrusher crusher(config);
    crusher.prepare(kSampleRate, kBlockSize);
    return crusher.tail_samples();
  };

  BitCrusherConfig config;
  config.quantizer_mode = QuantizerMode::kOff;
  CHECK(finite_tail(config) < TailBudget::kUnbounded);

  config.radio_noise_level = 0.8f;
  config.noise_detune = 0.0f;
  CHECK(finite_tail(config) < TailBudget::kUnbounded);

  config.radio_noise_level = 0.0f;
  config.wp_noise_level = 0.8f;
  config.mix = 0.0f;
  CHECK(finite_tail(config) < TailBudget::kUnbounded);

  config.mix = 1.0f;
  config.wp_noise_level = 0.0f;
  config.dither_type = sonare::mastering::final::DitherType::Tpdf;
  CHECK(finite_tail(config) < TailBudget::kUnbounded);
}

TEST_CASE("BitCrusher source changes restore finite input history tails", "[idle-source-tail]") {
  BitCrusherConfig parameter_config;
  parameter_config.quantizer_mode = QuantizerMode::kOff;
  parameter_config.wp_noise_level = 0.5f;
  parameter_config.pre_filter_hz = 1000.0f;
  BitCrusher parameter_path(parameter_config);
  parameter_path.prepare(kSampleRate, kBlockSize);
  std::vector<float> input(1, 0.8f);
  process_mono(parameter_path, input);
  CHECK(parameter_path.tail_samples() == TailBudget::kUnbounded);
  REQUIRE(parameter_path.set_parameter(4, 0.0f));
  CHECK(parameter_path.tail_samples() > 0);
  CHECK(parameter_path.tail_samples() < TailBudget::kUnbounded);
  require_transient_output(render_silence(parameter_path, 128));

  BitCrusherConfig config_path_config = parameter_config;
  config_path_config.wp_noise_level = 0.0f;
  config_path_config.disc_noise_level = 0.5f;
  BitCrusher config_path(config_path_config);
  config_path.prepare(kSampleRate, kBlockSize);
  input.assign(1, 0.8f);
  process_mono(config_path, input);
  CHECK(config_path.tail_samples() == TailBudget::kUnbounded);
  config_path_config.disc_noise_level = 0.0f;
  config_path.set_config(config_path_config);
  CHECK(config_path.tail_samples() > 0);
  CHECK(config_path.tail_samples() < TailBudget::kUnbounded);
  require_transient_output(render_silence(config_path, 128));
}

TEST_CASE("Waveshaper bias creates a steady wet output for accepted curves and modes",
          "[idle-source-tail]") {
  struct Case {
    const char* name;
    WaveshaperCurve curve;
    sonare::rt::AliasingControl aliasing;
  };
  constexpr Case cases[] = {
      {"tanh direct", WaveshaperCurve::Tanh, sonare::rt::AliasingControl::None},
      {"arctan adaa", WaveshaperCurve::Arctan, sonare::rt::AliasingControl::Adaa1},
      {"asymmetric oversampled", WaveshaperCurve::Asymmetric,
       sonare::rt::AliasingControl::Oversample4x},
  };

  for (const Case& test : cases) {
    DYNAMIC_SECTION("case=" << test.name) {
      WaveshaperConfig config;
      config.bias = 0.25f;
      config.mix = 0.5f;
      config.output_gain_db = 3.0f;
      config.curve = test.curve;
      config.aliasing = test.aliasing;

      Waveshaper shaper(config);
      shaper.prepare(kSampleRate, kBlockSize);
      CHECK(shaper.tail_samples() == TailBudget::kUnbounded);
      require_persistent_output(render_silence(shaper));
    }
  }
}

TEST_CASE("Waveshaper only reports a tail for audible steady wet bias", "[idle-source-tail]") {
  const auto finite_tail = [](WaveshaperConfig config) {
    Waveshaper shaper(config);
    shaper.prepare(kSampleRate, kBlockSize);
    return shaper.tail_samples();
  };

  WaveshaperConfig config;
  CHECK(finite_tail(config) < TailBudget::kUnbounded);
  require_silent_waveshaper_output(config);

  config.bias = 0.25f;
  config.aliasing = sonare::rt::AliasingControl::Adaa1;
  config.mix = 0.0f;
  CHECK(finite_tail(config) > 0);
  CHECK(finite_tail(config) < TailBudget::kUnbounded);
  require_silent_waveshaper_output(config);

  config.aliasing = sonare::rt::AliasingControl::None;
  config.mix = 1.0f;
  config.output_gain_db = -1000.0f;
  CHECK(finite_tail(config) == 0);
  require_silent_waveshaper_output(config);

  config = WaveshaperConfig{};
  config.curve = WaveshaperCurve::Asymmetric;
  config.bias = -1.0f / 0.35f;
  CHECK(finite_tail(config) == 0);
  require_silent_waveshaper_output(config);
}

TEST_CASE("Waveshaper parameter and config changes retain finite aliasing tails",
          "[idle-source-tail]") {
  WaveshaperConfig adaa_config;
  adaa_config.bias = 0.25f;
  adaa_config.aliasing = sonare::rt::AliasingControl::Adaa1;
  Waveshaper adaa(adaa_config);
  adaa.prepare(kSampleRate, kBlockSize);
  CHECK(adaa.tail_samples() == TailBudget::kUnbounded);
  REQUIRE(adaa.set_parameter(1, 0.0f));
  CHECK(adaa.tail_samples() == 1);

  WaveshaperConfig oversampled_config;
  oversampled_config.bias = 0.25f;
  oversampled_config.aliasing = sonare::rt::AliasingControl::Oversample4x;
  Waveshaper oversampled(oversampled_config);
  oversampled.prepare(kSampleRate, kBlockSize);
  CHECK(oversampled.tail_samples() == TailBudget::kUnbounded);
  oversampled_config.bias = 0.0f;
  oversampled.set_config(oversampled_config);
  CHECK(oversampled.tail_samples() > 0);
  CHECK(oversampled.tail_samples() < TailBudget::kUnbounded);
}
