#include <sonare/sonare_c.h>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "core/audio.h"
#include "effects/normalize.h"
#include "filters/iir.h"
#include "util/json.h"
#include "util/system_info.h"

namespace {

namespace json = sonare::util::json;

constexpr int kSampleRate = 22050;

std::vector<float> test_signal() {
  std::vector<float> samples(4096);
  for (size_t i = 0; i < samples.size(); ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(kSampleRate);
    samples[i] = 0.5f * std::sin(2.0f * 3.14159265f * 440.0f * t) +
                 0.25f * std::sin(2.0f * 3.14159265f * 5000.0f * t);
  }
  return samples;
}

/// Owns a float array a C entry returned.
struct Floats {
  float* data = nullptr;
  size_t length = 0;
  ~Floats() { sonare_free_floats(data); }
  std::vector<float> values() const { return std::vector<float>(data, data + length); }
};

std::string last_error() { return sonare_last_error_message(); }

}  // namespace

TEST_CASE("sonare_fade matches the core fades and leaves a zero-length side untouched",
          "[c_api][dsp_entries]") {
  const std::vector<float> samples = test_signal();
  const sonare::Audio audio =
      sonare::Audio::from_buffer(samples.data(), samples.size(), kSampleRate);

  Floats both;
  REQUIRE(sonare_fade(samples.data(), samples.size(), kSampleRate, 0.05f, 0.02f, &both.data,
                      &both.length) == SONARE_OK);
  const sonare::Audio expected = sonare::fade_out(sonare::fade_in(audio, 0.05f), 0.02f);
  CHECK(both.values() == std::vector<float>(expected.data(), expected.data() + expected.size()));

  Floats none;
  REQUIRE(sonare_fade(samples.data(), samples.size(), kSampleRate, 0.0f, 0.0f, &none.data,
                      &none.length) == SONARE_OK);
  CHECK(none.values() == samples);
}

TEST_CASE("sonare_fade refuses a negative length by its parameter name", "[c_api][dsp_entries]") {
  const std::vector<float> samples = test_signal();
  float* out = reinterpret_cast<float*>(0x1);
  size_t out_length = 7;
  CHECK(sonare_fade(samples.data(), samples.size(), kSampleRate, 0.0f, -1.0f, &out, &out_length) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "fade_out_sec must be finite and at least 0");
  CHECK(out == nullptr);
  CHECK(out_length == 0);
}

TEST_CASE("sonare_biquad_filter reproduces each core filter path", "[c_api][dsp_entries]") {
  const std::vector<float> samples = test_signal();
  for (const int zero_phase : {0, 1}) {
    for (const int order : {2, 4}) {
      for (const int type : {SONARE_BIQUAD_FILTER_HIGH_PASS, SONARE_BIQUAD_FILTER_LOW_PASS}) {
        SonareBiquadFilterConfig config;
        sonare_biquad_filter_config_init(&config);
        config.type = type;
        config.order = order;
        config.zero_phase = zero_phase;
        config.cutoff_hz = 1000.0f;
        Floats out;
        REQUIRE(sonare_biquad_filter(samples.data(), samples.size(), kSampleRate, &config,
                                     &out.data, &out.length) == SONARE_OK);
        const bool high = type == SONARE_BIQUAD_FILTER_HIGH_PASS;
        std::vector<float> expected;
        if (order == 4) {
          const sonare::CascadedBiquad cascade =
              high ? sonare::highpass_coeffs_4th(1000.0f, kSampleRate)
                   : sonare::lowpass_coeffs_4th(1000.0f, kSampleRate);
          expected = zero_phase
                         ? sonare::apply_cascade_filtfilt(samples.data(), samples.size(), cascade)
                         : sonare::apply_cascade(samples.data(), samples.size(), cascade);
        } else {
          const sonare::BiquadCoeffs coeffs = high ? sonare::highpass_coeffs(1000.0f, kSampleRate)
                                                   : sonare::lowpass_coeffs(1000.0f, kSampleRate);
          expected = zero_phase
                         ? sonare::apply_biquad_filtfilt(samples.data(), samples.size(), coeffs)
                         : sonare::apply_biquad(samples.data(), samples.size(), coeffs);
        }
        CHECK(out.values() == expected);
      }
    }
    for (const int type : {SONARE_BIQUAD_FILTER_BAND_PASS, SONARE_BIQUAD_FILTER_NOTCH}) {
      SonareBiquadFilterConfig config;
      sonare_biquad_filter_config_init(&config);
      config.type = type;
      config.zero_phase = zero_phase;
      config.center_hz = 2000.0f;
      config.bandwidth_hz = 500.0f;
      Floats out;
      REQUIRE(sonare_biquad_filter(samples.data(), samples.size(), kSampleRate, &config, &out.data,
                                   &out.length) == SONARE_OK);
      const sonare::BiquadCoeffs coeffs =
          type == SONARE_BIQUAD_FILTER_BAND_PASS
              ? sonare::bandpass_coeffs(2000.0f, 500.0f, kSampleRate)
              : sonare::notch_coeffs(2000.0f, 500.0f, kSampleRate);
      const std::vector<float> expected =
          zero_phase ? sonare::apply_biquad_filtfilt(samples.data(), samples.size(), coeffs)
                     : sonare::apply_biquad(samples.data(), samples.size(), coeffs);
      CHECK(out.values() == expected);
    }
  }
}

TEST_CASE("sonare_biquad_filter refuses a field its type does not read and an order it lacks",
          "[c_api][dsp_entries]") {
  const std::vector<float> samples = test_signal();
  Floats out;
  SonareBiquadFilterConfig config;

  sonare_biquad_filter_config_init(&config);
  CHECK(sonare_biquad_filter(samples.data(), samples.size(), kSampleRate, &config, &out.data,
                             &out.length) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "config.cutoff_hz must be given when config.type is highPass or lowPass");

  config.cutoff_hz = 0.0f;
  CHECK(sonare_biquad_filter(samples.data(), samples.size(), kSampleRate, &config, &out.data,
                             &out.length) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "config.cutoff_hz must be finite and in (0, 11025)");

  config.cutoff_hz = 1000.0f;
  config.bandwidth_hz = 10.0f;
  CHECK(sonare_biquad_filter(samples.data(), samples.size(), kSampleRate, &config, &out.data,
                             &out.length) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() ==
        "config.bandwidth_hz does not apply when config.type is highPass or lowPass");

  sonare_biquad_filter_config_init(&config);
  config.type = SONARE_BIQUAD_FILTER_NOTCH;
  config.order = 4;
  config.center_hz = 2000.0f;
  config.bandwidth_hz = 100.0f;
  CHECK(sonare_biquad_filter(samples.data(), samples.size(), kSampleRate, &config, &out.data,
                             &out.length) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "config.order must be 2 when config.type is bandPass or notch");

  config.order = 2;
  config.bandwidth_hz = std::numeric_limits<float>::quiet_NaN();
  CHECK(sonare_biquad_filter(samples.data(), samples.size(), kSampleRate, &config, &out.data,
                             &out.length) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "config.bandwidth_hz must be given when config.type is bandPass or notch");

  SonareBiquadFilterConfig zeroed{};
  CHECK(sonare_biquad_filter(samples.data(), samples.size(), kSampleRate, &zeroed, &out.data,
                             &out.length) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "config.struct_size must be set by sonare_biquad_filter_config_init");
  config.order = 4;
  config.bandwidth_hz = 100.0f;

  config.order = 3;
  CHECK(sonare_biquad_filter(samples.data(), samples.size(), kSampleRate, &config, &out.data,
                             &out.length) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "config.order must be one of 2, 4");

  CHECK(sonare_biquad_filter(nullptr, 1, kSampleRate, nullptr, &out.data, &out.length) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error().find("samples") != std::string::npos);
  CHECK(sonare_biquad_filter(samples.data(), samples.size(), kSampleRate, nullptr, &out.data,
                             &out.length) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "config must not be NULL");
}

TEST_CASE("sonare_system_info_json reports exactly the host facts the platform knows",
          "[c_api][dsp_entries]") {
  char* text = nullptr;
  REQUIRE(sonare_system_info_json(&text) == SONARE_OK);
  REQUIRE(text != nullptr);
  const json::Value doc = json::parse_strict(std::string(text));
  sonare_free_string(text);

  const auto logical = sonare::system_info::logical_cores();
  CHECK(doc["cpu"].contains("logical_cores") == logical.has_value());
  if (logical) CHECK(doc["cpu"]["logical_cores"].as_int() == *logical);
  CHECK(doc["cpu"].contains("physical_cores") == sonare::system_info::physical_cores().has_value());
  CHECK(doc["memory"].contains("total_gb") ==
        sonare::system_info::total_memory_bytes().has_value());
  CHECK(doc["parallel"]["workers"].as_int() == sonare::system_info::parallel_workers());
  CHECK(doc["parallel"]["enabled"].as_bool() == sonare::system_info::parallel_enabled());
  CHECK(doc["parallel"]["strategy"].as_string() == sonare::system_info::parallel_strategy());

  CHECK(sonare_system_info_json(nullptr) == SONARE_ERROR_INVALID_PARAMETER);
}

TEST_CASE("the last parameter refusal re-renders with the subjects a binding supplies",
          "[c_api][dsp_entries]") {
  const std::vector<float> samples = test_signal();
  Floats out;
  SonareBiquadFilterConfig config;
  sonare_biquad_filter_config_init(&config);
  config.type = SONARE_BIQUAD_FILTER_BAND_PASS;
  config.cutoff_hz = 1000.0f;
  config.center_hz = 2000.0f;
  config.bandwidth_hz = 100.0f;
  REQUIRE(sonare_biquad_filter(samples.data(), samples.size(), kSampleRate, &config, &out.data,
                               &out.length) == SONARE_ERROR_INVALID_PARAMETER);
  const std::string message = last_error();
  CHECK(message == "config.cutoff_hz does not apply when config.type is bandPass or notch");

  REQUIRE(sonare_last_refusal_subject_count() == 2);
  CHECK(std::string(sonare_last_refusal_subject(0)) == "config.cutoff_hz");
  CHECK(std::string(sonare_last_refusal_subject(1)) == "config.type");
  CHECK(sonare_last_refusal_subject(2) == nullptr);

  const char* flattened[] = {"cutoff_hz", "type"};
  char* text = reinterpret_cast<char*>(0x1);
  REQUIRE(sonare_last_refusal_render(flattened, nullptr, 2, &text) == SONARE_OK);
  CHECK(std::string(text) == "cutoff_hz does not apply when type is bandPass or notch");
  sonare_free_string(text);

  CHECK(sonare_last_refusal_render(flattened, nullptr, 1, &text) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(text == nullptr);
  const char* with_null[] = {"cutoff_hz", nullptr};
  CHECK(sonare_last_refusal_render(with_null, nullptr, 2, &text) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_last_refusal_render(flattened, nullptr, 2, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == message);
  CHECK(sonare_last_error_code() == SONARE_ERROR_INVALID_PARAMETER);

  REQUIRE(sonare_fade(samples.data(), samples.size(), kSampleRate, 0.0f, 0.0f, &out.data,
                      &out.length) == SONARE_OK);
  CHECK(sonare_last_refusal_subject_count() == 0);
  CHECK(sonare_last_refusal_subject(0) == nullptr);
  text = reinterpret_cast<char*>(0x1);
  CHECK(sonare_last_refusal_render(nullptr, nullptr, 0, &text) == SONARE_ERROR_INVALID_STATE);
  CHECK(text == nullptr);
}
