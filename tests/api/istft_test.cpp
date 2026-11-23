#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "core/spectrum.h"
#include "sonare_c_test_helpers.h"

TEST_CASE("sonare_stft exposes a phase plane only when requested", "[c_api][features][stft]") {
  constexpr int n_fft = 512;
  constexpr int hop_length = 128;
  constexpr int tone_bin = 7;
  constexpr int first_full_frame = n_fft / 2 / hop_length;
  std::vector<float> samples(2048);
  for (size_t i = 0; i < samples.size(); ++i) {
    samples[i] = static_cast<float>(std::sin(sonare::constants::kTwoPiD * tone_bin * i / n_fft));
  }

  SonareStftResult plain{};
  REQUIRE(sonare_stft(samples.data(), samples.size(), sonare::constants::kDefaultSampleRate, n_fft,
                      hop_length, nullptr, &plain) == SONARE_OK);
  REQUIRE(plain.phase == nullptr);

  SonareStftOptions options{};
  sonare_stft_options_init(&options);
  REQUIRE(options.phase == 0);
  options.phase = 1;
  options.db = 1;
  SonareStftResult with_phase{};
  REQUIRE(sonare_stft(samples.data(), samples.size(), sonare::constants::kDefaultSampleRate, n_fft,
                      hop_length, &options, &with_phase) == SONARE_OK);
  REQUIRE(with_phase.phase != nullptr);
  REQUIRE(with_phase.db != nullptr);
  REQUIRE(with_phase.n_bins == plain.n_bins);
  REQUIRE(with_phase.n_frames == plain.n_frames);
  const int tone_index = tone_bin * with_phase.n_frames + first_full_frame;
  REQUIRE(with_phase.magnitude[tone_index] > 1.0f);
  CHECK(with_phase.phase[tone_index] == Catch::Approx(-sonare::constants::kPi / 2.0f).margin(1e-5));
  const size_t total = static_cast<size_t>(plain.n_bins) * plain.n_frames;
  for (size_t i = 0; i < total; ++i) {
    CHECK(with_phase.magnitude[i] == Catch::Approx(plain.magnitude[i]).margin(1e-6));
    CHECK(std::isfinite(with_phase.phase[i]));
    CHECK(with_phase.phase[i] >= -sonare::constants::kPi);
    CHECK(with_phase.phase[i] <= sonare::constants::kPi);
  }

  sonare_free_stft_result(&plain);
  sonare_free_stft_result(&with_phase);
}

TEST_CASE("sonare_stft returns positive zero phase for silent bins", "[c_api][features][stft]") {
  const std::vector<float> silence(1024, 0.0f);
  SonareStftOptions options{};
  sonare_stft_options_init(&options);
  options.phase = 1;

  SonareStftResult result{};
  REQUIRE(sonare_stft(silence.data(), silence.size(), sonare::constants::kDefaultSampleRate, 512,
                      128, &options, &result) == SONARE_OK);
  REQUIRE(result.phase != nullptr);
  const size_t total = static_cast<size_t>(result.n_bins) * result.n_frames;
  for (size_t i = 0; i < total; ++i) {
    CHECK(result.magnitude[i] == 0.0f);
    CHECK(result.phase[i] == 0.0f);
    CHECK_FALSE(std::signbit(result.phase[i]));
  }

  sonare_free_stft_result(&result);
}

TEST_CASE("sonare_istft reconstructs a centered STFT and honors length_samples",
          "[c_api][features][istft]") {
  constexpr int n_fft = 512;
  constexpr size_t hop_length = 128;
  constexpr int tone_bin = 7;
  std::vector<float> samples(2048);
  for (size_t i = 0; i < samples.size(); ++i) {
    samples[i] = static_cast<float>(std::sin(sonare::constants::kTwoPiD * tone_bin * i / n_fft));
  }

  SonareStftOptions stft_options{};
  sonare_stft_options_init(&stft_options);
  stft_options.phase = 1;
  SonareStftResult stft{};
  REQUIRE(sonare_stft(samples.data(), samples.size(), sonare::constants::kDefaultSampleRate, n_fft,
                      hop_length, &stft_options, &stft) == SONARE_OK);
  REQUIRE(stft.phase != nullptr);

  float* output = nullptr;
  size_t output_length = 0;
  REQUIRE(sonare_istft(stft.magnitude, stft.phase, stft.n_bins, stft.n_frames, n_fft, hop_length,
                       nullptr, &output, &output_length) == SONARE_OK);
  REQUIRE(output_length == samples.size());
  for (size_t i = 0; i < output_length; ++i) {
    CHECK(output[i] == Catch::Approx(samples[i]).margin(2e-5));
  }
  sonare_free_floats(output);

  SonareIstftOptions istft_options{};
  sonare_istft_options_init(&istft_options);
  CHECK(istft_options.struct_size == sizeof(istft_options));
  CHECK(istft_options.length_samples == 0);
  istft_options.length_samples = 1;
  output = nullptr;
  output_length = 0;
  REQUIRE(sonare_istft(stft.magnitude, stft.phase, stft.n_bins, stft.n_frames, n_fft, hop_length,
                       &istft_options, &output, &output_length) == SONARE_OK);
  REQUIRE(output_length == 1);
  CHECK(output[0] == Catch::Approx(samples[0]).margin(2e-5));
  sonare_free_floats(output);
  sonare_free_stft_result(&stft);
}

TEST_CASE("sonare_istft names refused inputs and bounds output length",
          "[c_api][features][istft]") {
  constexpr int n_fft = 16;
  constexpr int n_bins = n_fft / 2 + 1;
  constexpr int n_frames = 2;
  constexpr size_t hop_length = 4;
  std::vector<float> magnitude(static_cast<size_t>(n_bins) * n_frames, 1.0f);
  std::vector<float> phase(magnitude.size(), 0.0f);
  SonareIstftOptions options{};
  sonare_istft_options_init(&options);

  const auto expect_refusal =
      [&](const float* input_magnitude, const float* input_phase, int bins, int frames, int fft,
          size_t hop, const SonareIstftOptions* istft_options, const std::string& subject) {
        float* output = non_null_sentinel_float_ptr();
        size_t output_length = 1;
        CHECK(sonare_istft(input_magnitude, input_phase, bins, frames, fft, hop, istft_options,
                           &output, &output_length) == SONARE_ERROR_INVALID_PARAMETER);
        const std::string message = sonare_last_error_message();
        CHECK(message.find(subject) != std::string::npos);
        CHECK(output == nullptr);
        CHECK(output_length == 0);
      };

  SECTION("the one-sided bin count must match n_fft") {
    expect_refusal(magnitude.data(), phase.data(), n_bins - 1, n_frames, n_fft, hop_length,
                   &options, "n_bins");
    CHECK(std::string(sonare_last_error_message()).find("n_fft") != std::string::npos);
  }

  SECTION("n_frames must be positive") {
    expect_refusal(magnitude.data(), phase.data(), n_bins, 0, n_fft, hop_length, &options,
                   "n_frames");
  }

  SECTION("a single frame is accepted") {
    options.length_samples = 1;
    float* output = nullptr;
    size_t output_length = 0;
    REQUIRE(sonare_istft(magnitude.data(), phase.data(), n_bins, 1, n_fft, hop_length, &options,
                         &output, &output_length) == SONARE_OK);
    CHECK(output_length == 1);
    sonare_free_floats(output);
  }

  SECTION("the half-window hop is accepted") {
    options.length_samples = 1;
    float* output = nullptr;
    size_t output_length = 0;
    REQUIRE(sonare_istft(magnitude.data(), phase.data(), n_bins, n_frames, n_fft, n_fft / 2,
                         &options, &output, &output_length) == SONARE_OK);
    CHECK(output_length == 1);
    sonare_free_floats(output);
  }

  SECTION("n_fft uses the shared Hann resynthesis domain") {
    expect_refusal(magnitude.data(), phase.data(), 2, n_frames, 2, 1, &options, "n_fft");
    CHECK(std::string(sonare_last_error_message()).find("hop_length") != std::string::npos);
    expect_refusal(magnitude.data(), phase.data(), 8, n_frames, 15, hop_length, &options, "n_fft");
    CHECK(std::string(sonare_last_error_message()).find("hop_length") != std::string::npos);
    constexpr int above_cap = sonare::kMaxStftNFft + 2;
    expect_refusal(magnitude.data(), phase.data(), above_cap / 2 + 1, n_frames, above_cap,
                   hop_length, &options, "n_fft");
    CHECK(std::string(sonare_last_error_message()).find("hop_length") != std::string::npos);
  }

  SECTION("COLA geometry names both pair members") {
    expect_refusal(magnitude.data(), phase.data(), n_bins, n_frames, n_fft, n_fft / 2 + 1, &options,
                   "n_fft");
    CHECK(std::string(sonare_last_error_message()).find("hop_length") != std::string::npos);
  }

  SECTION("hop_length must be positive") {
    expect_refusal(magnitude.data(), phase.data(), n_bins, n_frames, n_fft, 0, &options, "n_fft");
    CHECK(std::string(sonare_last_error_message()).find("hop_length") != std::string::npos);
  }

  SECTION("magnitude and phase values are validated by name") {
    magnitude[0] = std::numeric_limits<float>::quiet_NaN();
    expect_refusal(magnitude.data(), phase.data(), n_bins, n_frames, n_fft, hop_length, &options,
                   "magnitude");
    magnitude[0] = -1.0f;
    expect_refusal(magnitude.data(), phase.data(), n_bins, n_frames, n_fft, hop_length, &options,
                   "magnitude");
    magnitude[0] = 1.0f;
    phase[0] = std::numeric_limits<float>::infinity();
    expect_refusal(magnitude.data(), phase.data(), n_bins, n_frames, n_fft, hop_length, &options,
                   "phase");
  }

  SECTION("magnitude and phase pointers are required") {
    expect_refusal(nullptr, phase.data(), n_bins, n_frames, n_fft, hop_length, &options,
                   "magnitude");
    expect_refusal(magnitude.data(), nullptr, n_bins, n_frames, n_fft, hop_length, &options,
                   "phase");
  }

  SECTION("requested lengths above the Audio cap are refused") {
    const size_t lengths[] = {500000001u, size_t{1} << 31, size_t{1} << 32};
    for (const size_t length : lengths) {
      options.length_samples = length;
      expect_refusal(magnitude.data(), phase.data(), n_bins, n_frames, n_fft, hop_length, &options,
                     "length_samples");
    }
    CHECK(std::string(sonare_last_error_message()).find("500000000") != std::string::npos);
  }
}
