#include "mastering/saturation/bitcrusher.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "mastering/api/insert_factory.h"
#include "rt/processor_base.h"

namespace {

template <typename Processor>
void process_mono(Processor& processor, std::vector<float>& samples, int block_size = 256) {
  for (size_t offset = 0; offset < samples.size(); offset += static_cast<size_t>(block_size)) {
    const int count = static_cast<int>(
        std::min<size_t>(static_cast<size_t>(block_size), samples.size() - offset));
    float* channels[] = {samples.data() + offset};
    processor.process(channels, 1, count);
  }
}

float peak_abs(const std::vector<float>& samples) {
  float peak = 0.0f;
  for (float sample : samples) peak = std::max(peak, std::abs(sample));
  return peak;
}

float max_difference(const std::vector<float>& first, const std::vector<float>& second) {
  REQUIRE(first.size() == second.size());
  float difference = 0.0f;
  for (size_t i = 0; i < first.size(); ++i) {
    difference = std::max(difference, std::abs(first[i] - second[i]));
  }
  return difference;
}

}  // namespace

struct NoiseLpfCase {
  const char* name;
  unsigned int level_parameter;
  unsigned int lpf_parameter;
};

TEST_CASE("BitCrusher noise LPF re-entry does not replay any source history",
          "[mastering][saturation][boundary]") {
  using namespace sonare::mastering::saturation;
  for (const NoiseLpfCase test :
       {NoiseLpfCase{"radio", 3, 8}, NoiseLpfCase{"white", 4, 9}, NoiseLpfCase{"pink", 4, 9},
        NoiseLpfCase{"disc", 5, 10}, NoiseLpfCase{"hum", 6, 11}}) {
    BitCrusherConfig subject_config;
    subject_config.quantizer_mode = QuantizerMode::kOff;
    subject_config.noise_lpf_hz = 5.0f;
    subject_config.wp_noise_lpf_hz = 5.0f;
    subject_config.disc_noise_lpf_hz = 5.0f;
    subject_config.hum_lpf_hz = 5.0f;
    subject_config.disc_type = BitCrusherDiscType::kRnd;
    subject_config.hum_hz = 50.0f;
    if (test.name == std::string("radio")) subject_config.radio_noise_level = 1.0f;
    if (test.name == std::string("white")) subject_config.wp_noise_level = 1.0f;
    if (test.name == std::string("pink")) {
      subject_config.wp_noise_level = 1.0f;
      subject_config.wp_noise_pink = true;
    }
    if (test.name == std::string("disc")) subject_config.disc_noise_level = 1.0f;
    if (test.name == std::string("hum")) subject_config.hum_level = 1.0f;

    auto reset_control_config = subject_config;
    if (test.lpf_parameter == 8) reset_control_config.noise_lpf_hz = 0.0f;
    if (test.lpf_parameter == 9) reset_control_config.wp_noise_lpf_hz = 0.0f;
    if (test.lpf_parameter == 10) reset_control_config.disc_noise_lpf_hz = 0.0f;
    if (test.lpf_parameter == 11) reset_control_config.hum_lpf_hz = 0.0f;

    BitCrusher subject(subject_config);
    BitCrusher reset_control(reset_control_config);
    subject.prepare(48000.0, 257);
    reset_control.prepare(48000.0, 257);
    std::vector<float> warm_subject(96000, 0.0f);
    auto warm_control = warm_subject;
    process_mono(subject, warm_subject, 257);
    process_mono(reset_control, warm_control, 257);
    REQUIRE(subject.set_parameter(test.lpf_parameter, 0.0f));

    std::vector<float> gap_subject(48000, 0.0f);
    auto gap_control = gap_subject;
    process_mono(subject, gap_subject, 257);
    process_mono(reset_control, gap_control, 257);
    REQUIRE(subject.set_parameter(test.lpf_parameter, 5.0f));
    REQUIRE(reset_control.set_parameter(test.lpf_parameter, 5.0f));

    std::vector<float> reentry_subject(257, 0.0f);
    auto reentry_control = reentry_subject;
    process_mono(subject, reentry_subject, 257);
    process_mono(reset_control, reentry_control, 257);
    INFO("source=" << test.name);
    REQUIRE(std::isfinite(peak_abs(reentry_subject)));
    REQUIRE(max_difference(reentry_subject, reentry_control) < 1.0e-5f);
    REQUIRE(subject.non_finite_discard_count() == 0u);
    REQUIRE(reset_control.non_finite_discard_count() == 0u);
  }
}

TEST_CASE("BitCrusher is silent once its reported tail has elapsed",
          "[mastering][saturation][tail]") {
  struct Case {
    const char* name;
    const char* json;
  };
  const Case cases[] = {
      {"count 4", R"({"downsampleFactor": 4})"},
      {"hold 80 Hz", R"({"holdHz": 80})"},
      {"pre 100 Hz", R"({"preFilterHz": 100})"},
      {"post 100 Hz", R"({"postFilterHz": 100})"},
  };
  // One finite input frame, then silence: anything left after the reported tail is owed state.
  const auto render = [](sonare::rt::ProcessorBase& processor, double rate, float* residual,
                         float* inside) {
    processor.prepare(rate, 256);
    const int tail = processor.tail_samples();
    const int silent_from = 1 + processor.latency_samples() + tail;
    std::vector<float> samples(static_cast<size_t>(silent_from) + 4096, 0.0f);
    samples[0] = 0.8f;
    process_mono(processor, samples);
    *residual = 0.0f;
    *inside = 0.0f;
    for (size_t i = 0; i < samples.size(); ++i) {
      const float magnitude = std::abs(samples[i]);
      if (static_cast<int>(i) >= silent_from) {
        *residual = std::max(*residual, magnitude);
      } else if (i > 0) {
        *inside = std::max(*inside, magnitude);
      }
    }
    return tail;
  };
  for (const double rate : {8000.0, 48000.0, 96000.0}) {
    for (const Case& c : cases) {
      CAPTURE(rate, c.name);
      std::unique_ptr<sonare::rt::ProcessorBase> processor =
          sonare::mastering::api::make_insert("saturation.bitcrusher", c.json);
      REQUIRE(processor != nullptr);
      float residual = 0.0f;
      float inside = 0.0f;
      const int tail = render(*processor, rate, &residual, &inside);
      CHECK(tail > 0);
      CHECK(inside > 1.0e-5f);
      CHECK(residual <= 1.0e-5f);
    }
    // Pass-through settings own no state past the input frame.
    for (const char* json : {"{}", R"({"mix": 0, "downsampleFactor": 4, "postFilterHz": 100})"}) {
      CAPTURE(rate, json);
      std::unique_ptr<sonare::rt::ProcessorBase> processor =
          sonare::mastering::api::make_insert("saturation.bitcrusher", json);
      REQUIRE(processor != nullptr);
      float residual = 0.0f;
      float inside = 0.0f;
      CHECK(render(*processor, rate, &residual, &inside) == 0);
      CHECK(residual == 0.0f);
    }
  }
}
