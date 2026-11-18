#include <sonare/sonare_c.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

#if defined(SONARE_WITH_VOICE_CHANGER)
#include "editing/voice_changer/streaming_retune.h"
#endif
#include "feature/inverse.h"
#include "util/exception.h"
#include "util/json.h"

// The core function owns each check below; the C entry, the Node addon and the
// WASM embind reach it directly, so a refusal is asserted at the core and at the C
// entry here and on each binding by the matching conformance test.

namespace {

using sonare::util::json::Value;

Value load_refusal_corpus() {
  std::ifstream input("tests/conformance/core_validation_refusals.json");
  REQUIRE(input.good());
  const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  return sonare::util::json::parse_strict(text);
}

std::vector<float> filled_matrix(const Value& input) {
  const int rows = static_cast<int>(input["n_mfcc"].as_number());
  const int cols = static_cast<int>(input["n_frames"].as_number());
  return std::vector<float>(static_cast<size_t>(rows) * cols,
                            static_cast<float>(input["fill"].as_number()));
}

std::string last_error_message() {
  const char* message = sonare_last_error_message();
  REQUIRE(message != nullptr);
  return message;
}

void require_argument_refusal(const std::function<void()>& call, const std::string& message) {
  try {
    call();
    FAIL("expected a refusal naming: " << message);
  } catch (const sonare::SonareException& e) {
    CHECK(e.code() == sonare::ErrorCode::InvalidParameter);
    CHECK_THAT(std::string(e.what()), Catch::Matchers::ContainsSubstring(message));
  }
}

}  // namespace

TEST_CASE("mfcc_to_mel refusals are owned by the core function", "[conformance][core_validation]") {
  const Value corpus = load_refusal_corpus();
  const Value& route = corpus["mfcc_to_mel"];
  const Value& input = route["input"];
  const int n_mfcc = static_cast<int>(input["n_mfcc"].as_number());
  const int n_frames = static_cast<int>(input["n_frames"].as_number());
  const std::vector<float> mfcc = filled_matrix(input);

  for (const auto& item : route["accepted"].as_array()) {
    const int n_mels = static_cast<int>(item["n_mels"].as_number());
    INFO(item["id"].as_string());
    CHECK(sonare::mfcc_to_mel(mfcc.data(), n_mfcc, n_frames, n_mels).size() ==
          static_cast<size_t>(n_mels) * n_frames);
    SonareInverseResult out{};
    REQUIRE(sonare_mfcc_to_mel_ex(mfcc.data(), n_mfcc, n_frames, n_mels, 0.0f, &out) == SONARE_OK);
    sonare_free_inverse_result(&out);
  }

  for (const auto& item : route["refused"].as_array()) {
    const int n_mels = static_cast<int>(item["n_mels"].as_number());
    const std::string message = item["message"].as_string();
    INFO(item["id"].as_string());
    REQUIRE(item["cause"].as_string() == "argument");

    require_argument_refusal([&] { sonare::mfcc_to_mel(mfcc.data(), n_mfcc, n_frames, n_mels); },
                             message);
    sonare::MelConfig config;
    config.n_mels = n_mels;
    require_argument_refusal(
        [&] { sonare::mfcc_to_audio(mfcc.data(), n_mfcc, n_frames, config, 1, 22050); }, message);

    SonareInverseResult out{};
    CHECK(sonare_mfcc_to_mel_ex(mfcc.data(), n_mfcc, n_frames, n_mels, 0.0f, &out) ==
          SONARE_ERROR_INVALID_PARAMETER);
    CHECK_THAT(last_error_message(), Catch::Matchers::ContainsSubstring(message));
    float* audio = nullptr;
    size_t audio_length = 0;
    CHECK(sonare_mfcc_to_audio_ex2(mfcc.data(), n_mfcc, n_frames, n_mels, 22050, 2048, 512, 0.0f,
                                   0.0f, 0, 0.0f, 1, &audio,
                                   &audio_length) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK_THAT(last_error_message(), Catch::Matchers::ContainsSubstring(message));
  }
}

#if defined(SONARE_WITH_VOICE_CHANGER)
TEST_CASE("streaming retune prepare refusals are owned by the core function",
          "[conformance][core_validation]") {
  const Value corpus = load_refusal_corpus();
  const Value& route = corpus["streaming_retune_prepare"];

  for (const auto& item : route["accepted"].as_array()) {
    const double rate = item["sample_rate"].as_number();
    const int block = static_cast<int>(item["max_block_size"].as_number());
    INFO(item["id"].as_string());
    sonare::editing::voice_changer::StreamingRetune core;
    CHECK_NOTHROW(core.prepare(rate, block));
    SonareStreamingRetune* handle = nullptr;
    REQUIRE(sonare_streaming_retune_create(0.0f, 1.0f, 0, &handle) == SONARE_OK);
    CHECK(sonare_streaming_retune_prepare(handle, rate, block) == SONARE_OK);
    sonare_streaming_retune_destroy(handle);
  }

  for (const auto& item : route["refused"].as_array()) {
    const double rate = item["sample_rate"].as_number();
    const int block = static_cast<int>(item["max_block_size"].as_number());
    const std::string message = item["message"].as_string();
    INFO(item["id"].as_string());
    REQUIRE(item["cause"].as_string() == "argument");

    sonare::editing::voice_changer::StreamingRetune core;
    require_argument_refusal([&] { core.prepare(rate, block); }, message);

    SonareStreamingRetune* handle = nullptr;
    REQUIRE(sonare_streaming_retune_create(0.0f, 1.0f, 0, &handle) == SONARE_OK);
    CHECK(sonare_streaming_retune_prepare(handle, rate, block) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK_THAT(last_error_message(), Catch::Matchers::ContainsSubstring(message));
    sonare_streaming_retune_destroy(handle);
  }
}
#endif  // defined(SONARE_WITH_VOICE_CHANGER)
