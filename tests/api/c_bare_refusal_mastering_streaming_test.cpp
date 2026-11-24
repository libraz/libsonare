/// @file c_bare_refusal_mastering_streaming_test.cpp
/// @brief The streaming mastering entries state the cause of every argument refusal.

#include <sonare/sonare_c.h>

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#ifdef SONARE_WITH_MASTERING

#include "mastering/api/chain.h"
#include "util/exception.h"

namespace {

std::string last_message() {
  const char* message = sonare_last_error_message();
  return message != nullptr ? message : "";
}

struct PreparedChain {
  SonareStreamingMasteringChain* chain = nullptr;
  PreparedChain() {
    REQUIRE(sonare_streaming_mastering_chain_create(nullptr, nullptr, &chain) == SONARE_OK);
  }
  ~PreparedChain() { sonare_streaming_mastering_chain_destroy(chain); }
};

}  // namespace

TEST_CASE("the streaming preview names a platform without a name",
          "[c_api][mastering][streaming]") {
  std::vector<float> samples(4800, 0.1f);
  SonareStreamingPlatform platforms[2] = {{"a", -14.0f, -1.0f}, {nullptr, -14.0f, -1.0f}};
  char* json = nullptr;
  REQUIRE(sonare_mastering_streaming_preview(samples.data(), samples.size(), 48000, platforms, 2,
                                             &json) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "platforms[1].name must not be NULL");
  REQUIRE(sonare_mastering_streaming_preview_stereo(samples.data(), samples.data(), samples.size(),
                                                    48000, platforms, 2,
                                                    &json) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "platforms[1].name must not be NULL");
}

TEST_CASE("the streaming chain refuses a NULL handle by name", "[c_api][mastering][streaming]") {
  float sample = 0.0f;
  size_t written = 7;
  int max_block = 7;
  size_t count = 7;

  REQUIRE(sonare_streaming_mastering_chain_prepare(nullptr, 48000, 0, 1) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "handle must not be NULL");
  REQUIRE(sonare_streaming_mastering_chain_process_mono(nullptr, &sample, 1) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "handle must not be NULL");
  REQUIRE(sonare_streaming_mastering_chain_process_stereo(nullptr, &sample, &sample, 1) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "handle must not be NULL");
  REQUIRE(sonare_streaming_mastering_chain_reset(nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "handle must not be NULL");
  REQUIRE(sonare_streaming_mastering_chain_set_parameter(nullptr, "k", 1.0) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "handle must not be NULL");
  REQUIRE(sonare_streaming_mastering_chain_flush_mono(nullptr, &sample, 1, &written) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "handle must not be NULL");
  REQUIRE(sonare_streaming_mastering_chain_flush_stereo(nullptr, &sample, &sample, 1, &written) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "handle must not be NULL");
  REQUIRE(sonare_streaming_mastering_chain_max_block_size(nullptr, &max_block) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "handle must not be NULL");
  REQUIRE(sonare_streaming_mastering_chain_non_finite_substitution_count(nullptr, &count) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "handle must not be NULL");
}

TEST_CASE("the streaming chain process entries state their buffer refusals",
          "[c_api][mastering][streaming]") {
  PreparedChain prepared;
  std::vector<float> block(128, 0.1f);

  REQUIRE(sonare_streaming_mastering_chain_process_mono(prepared.chain, nullptr, 4) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "samples must not be NULL when num_samples is not 0");
  REQUIRE(sonare_streaming_mastering_chain_process_stereo(prepared.chain, nullptr, block.data(),
                                                          4) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "left must not be NULL when num_samples is not 0");
  REQUIRE(sonare_streaming_mastering_chain_process_stereo(prepared.chain, block.data(), nullptr,
                                                          4) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "right must not be NULL when num_samples is not 0");

  REQUIRE(sonare_streaming_mastering_chain_process_mono(prepared.chain, block.data(), 4) ==
          SONARE_ERROR_INVALID_STATE);
  REQUIRE(last_message() == "the chain has not been prepared");

  REQUIRE(sonare_streaming_mastering_chain_prepare(prepared.chain, 48000, 128, 2) == SONARE_OK);
  std::vector<float> over(129, 0.1f);
  REQUIRE(sonare_streaming_mastering_chain_process_mono(prepared.chain, over.data(), over.size()) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "num_samples must be an integer in [0, 128]");
  REQUIRE(sonare_streaming_mastering_chain_process_stereo(prepared.chain, over.data(), over.data(),
                                                          over.size()) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "num_samples must be an integer in [0, 128]");

  std::vector<float> right = block;
  block[5] = std::numeric_limits<float>::quiet_NaN();
  REQUIRE(sonare_streaming_mastering_chain_process_mono(
              prepared.chain, block.data(), block.size()) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "samples contains a non-finite sample at index 5");
  REQUIRE(sonare_streaming_mastering_chain_process_stereo(prepared.chain, block.data(),
                                                          right.data(), block.size()) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "left contains a non-finite sample at index 5");
  REQUIRE(sonare_streaming_mastering_chain_process_stereo(prepared.chain, right.data(),
                                                          block.data(), block.size()) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "right contains a non-finite sample at index 5");
}

TEST_CASE("the streaming chain flush entries state their refusals",
          "[c_api][mastering][streaming]") {
  PreparedChain prepared;
  float sample = 0.0f;
  size_t written = 7;
  constexpr size_t kTooMany = static_cast<size_t>(std::numeric_limits<int>::max()) + 1;

  REQUIRE(sonare_streaming_mastering_chain_flush_mono(prepared.chain, &sample, kTooMany,
                                                      &written) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "capacity must be an integer in [0, 2147483647]");
  REQUIRE(written == 0);
  REQUIRE(sonare_streaming_mastering_chain_flush_stereo(prepared.chain, &sample, &sample, kTooMany,
                                                        &written) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "capacity must be an integer in [0, 2147483647]");
}

TEST_CASE("the streaming chain setter names the refused key or value",
          "[c_api][mastering][streaming]") {
  PreparedChain prepared;
  REQUIRE(sonare_streaming_mastering_chain_set_parameter(prepared.chain, nullptr, 1.0) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "key must not be NULL");
  REQUIRE(sonare_streaming_mastering_chain_set_parameter(prepared.chain, "", 1.0) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "key must not be empty");
  REQUIRE(sonare_streaming_mastering_chain_set_parameter(prepared.chain, "k",
                                                         std::numeric_limits<double>::infinity()) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "value must be finite and in [-3.4028235e38, 3.4028235e38]");
  REQUIRE(sonare_streaming_mastering_chain_set_parameter(prepared.chain, "k", 1.0e39) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "value must be finite and in [-3.4028235e38, 3.4028235e38]");
}

TEST_CASE("the streaming chain names the supplied planes on a prepared-width mismatch",
          "[c_api][mastering][streaming]") {
  PreparedChain prepared;
  float left = 0.0f;
  float right = 0.0f;

  SECTION("mono input to a stereo chain") {
    REQUIRE(sonare_streaming_mastering_chain_prepare(prepared.chain, 48000, 128, 2) == SONARE_OK);
    REQUIRE(sonare_streaming_mastering_chain_process_stereo(prepared.chain, &left, &right, 1) ==
            SONARE_OK);
    REQUIRE(sonare_streaming_mastering_chain_process_mono(prepared.chain, &left, 1) ==
            SONARE_ERROR_INVALID_PARAMETER);
    CHECK(last_message() == "samples must match the prepared channel count");
    REQUIRE(sonare_last_refusal_entry() != nullptr);
    CHECK(std::string(sonare_last_refusal_entry()) == "streaming_mastering_chain_process_mono");
    REQUIRE(sonare_last_refusal_subject_count() == 1);
    CHECK(std::string(sonare_last_refusal_subject(0)) == "samples");
  }

  SECTION("stereo input to a mono chain") {
    REQUIRE(sonare_streaming_mastering_chain_prepare(prepared.chain, 48000, 128, 1) == SONARE_OK);
    REQUIRE(sonare_streaming_mastering_chain_process_mono(prepared.chain, &left, 1) == SONARE_OK);
    REQUIRE(sonare_streaming_mastering_chain_process_stereo(prepared.chain, &left, &right, 1) ==
            SONARE_ERROR_INVALID_PARAMETER);
    CHECK(last_message() == "left and right must match the prepared channel count");
    REQUIRE(sonare_last_refusal_entry() != nullptr);
    CHECK(std::string(sonare_last_refusal_entry()) == "streaming_mastering_chain_process_stereo");
    REQUIRE(sonare_last_refusal_subject_count() == 2);
    CHECK(std::string(sonare_last_refusal_subject(0)) == "left");
    CHECK(std::string(sonare_last_refusal_subject(1)) == "right");
  }
}

TEST_CASE("the streaming chain names value when a direct setter cannot narrow it",
          "[c_api][mastering][streaming]") {
  using namespace sonare::mastering::api;
  StreamingMasteringChain chain(streaming_chain_config_from_json(
      R"({"params":{"eq.tilt.enabled":true,"eq.tilt.tiltDb":1}})"));
  chain.prepare(48000, 128, 1);
  REQUIRE_NOTHROW(chain.set_parameter("eq.tilt.tiltDb", 2.0));
  try {
    chain.set_parameter("eq.tilt.tiltDb", std::numeric_limits<double>::max());
    FAIL("the value must be refused before narrowing");
  } catch (const sonare::SonareParameterRefusal& error) {
    CHECK(error.code() == sonare::ErrorCode::InvalidParameter);
    CHECK(error.subject() == "value");
  }
}

#endif  // SONARE_WITH_MASTERING
