/// @file c_header_order_mastering_test.cpp
/// @brief The mastering C entries clear every out parameter first and then refuse arguments in
///        public-header parameter order, whatever else is wrong with the call.

#include <sonare/sonare_c.h>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#ifdef SONARE_WITH_MASTERING

namespace {

std::string last_message() {
  const char* message = sonare_last_error_message();
  return message != nullptr ? message : "";
}

constexpr int kRate = 22050;
constexpr size_t kLength = 64;

std::vector<float> make_samples() {
  std::vector<float> samples(kLength);
  for (size_t i = 0; i < kLength; ++i) {
    samples[i] = 0.1f * std::sin(0.05f * static_cast<float>(i));
  }
  return samples;
}

/// A pointer value no entry may dereference or free; a cleared out parameter must not keep it.
template <typename T>
T* garbage_pointer() {
  return reinterpret_cast<T*>(static_cast<std::uintptr_t>(0x10));
}

}  // namespace

TEST_CASE("mastering apply entries refuse in header order", "[c_api][header_order]") {
  const std::vector<float> samples = make_samples();

  SonareMasteringResult result;
  std::memset(&result, 0xAB, sizeof(result));
  REQUIRE(sonare_mastering_apply_processor("gain", nullptr, kLength, kRate, nullptr, 0, &result) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "mastering_apply_processor: samples must be a non-empty buffer");
  CHECK(result.samples == nullptr);
  CHECK(result.length == 0);

  REQUIRE(sonare_mastering_apply_processor("gain", nullptr, kLength, kRate, nullptr, 0, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "mastering_apply_processor: samples must be a non-empty buffer");

  char* json = garbage_pointer<char>();
  REQUIRE(sonare_mastering_analyze_pair_ex("loudness", nullptr, kLength, samples.data(), kLength,
                                           kRate, kRate, nullptr, 0,
                                           &json) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "mastering_analyze_pair_ex: source must be a non-empty buffer");
  CHECK(json == nullptr);

  REQUIRE(sonare_mastering_analyze_pair_ex("loudness", samples.data(), kLength, samples.data(),
                                           kLength, kRate, kRate, nullptr, 1,
                                           nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "params must not be NULL when param_count is not 0");

  float* out = garbage_pointer<float>();
  size_t out_length = 99;
  SonareLoudnessMatch match;
  std::memset(&match, 0xAB, sizeof(match));
  REQUIRE(sonare_mastering_ab_match_loudness(nullptr, kLength, samples.data(), kLength, kRate,
                                             kRate, &out, &out_length,
                                             &match) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "mastering_ab_match_loudness: source must be a non-empty buffer");
  CHECK(out == nullptr);
  CHECK(out_length == 0);
  CHECK(match.applied_gain_db == 0.0f);

  out = garbage_pointer<float>();
  REQUIRE(sonare_mastering_ab_match_loudness(samples.data(), kLength, samples.data(), kLength,
                                             kRate, kRate, &out, nullptr,
                                             nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out_length must not be NULL");
  CHECK(out == nullptr);

  int latency = 77;
  int tail = 77;
  REQUIRE(sonare_mastering_insert_timing("gain", nullptr, 1, &latency, &tail) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(latency == 0);
  CHECK(tail == 0);
  REQUIRE(sonare_mastering_insert_timing("gain", nullptr, kRate, &latency, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out_tail_samples must not be NULL");
  CHECK(latency == 0);
}

TEST_CASE("mastering chain entries refuse in header order", "[c_api][header_order]") {
  const std::vector<float> samples = make_samples();

  SonareMasteringChainResult result;
  std::memset(&result, 0xAB, sizeof(result));
  REQUIRE(sonare_mastering_chain(nullptr, kLength, kRate, nullptr, 0, &result) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "mastering_chain: samples must be a non-empty buffer");
  CHECK(result.samples == nullptr);
  CHECK(result.length == 0);
  CHECK(result.stages == nullptr);

  REQUIRE(sonare_mastering_chain(nullptr, kLength, kRate, nullptr, 0, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "mastering_chain: samples must be a non-empty buffer");

  REQUIRE(sonare_mastering_chain(samples.data(), kLength, kRate, nullptr, 1, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "params must not be NULL when param_count is not 0");

  SonareMasteringChainStereoResult stereo;
  std::memset(&stereo, 0xAB, sizeof(stereo));
  REQUIRE(sonare_mastering_chain_stereo(samples.data(), nullptr, kLength, kRate, nullptr, 0,
                                        &stereo) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "mastering_chain_stereo: right must be a non-empty buffer");
  CHECK(stereo.left == nullptr);
  CHECK(stereo.right == nullptr);
  CHECK(stereo.length == 0);
}

TEST_CASE("mastering eq entries refuse in header order", "[c_api][header_order]") {
  const std::vector<float> samples = make_samples();

  int max_block = 77;
  REQUIRE(sonare_eq_max_block_size(nullptr, &max_block) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "eq must not be NULL");
  CHECK(max_block == 0);

  REQUIRE(sonare_eq_set_band(nullptr, 0, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "eq must not be NULL");

  REQUIRE(sonare_eq_match_ex(nullptr, nullptr, kLength, kRate, samples.data(), kLength, kRate, 0) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "eq must not be NULL");

  REQUIRE(sonare_eq_magnitude_response(nullptr, 0, nullptr, 1, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "eq must not be NULL");

  SonareEq* eq = nullptr;
  REQUIRE(sonare_eq_create(48000.0, 0, &eq) == SONARE_OK);
  REQUIRE(eq != nullptr);
  REQUIRE(sonare_eq_set_band(eq, 0, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "band_json must not be NULL");
  REQUIRE(sonare_eq_match_ex(eq, nullptr, kLength, kRate, samples.data(), kLength, kRate, 0) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "eq_match_ex: source must be a non-empty buffer");
  sonare_eq_destroy(eq);
}

TEST_CASE("mastering preset entries refuse in header order", "[c_api][header_order]") {
  const std::vector<float> samples = make_samples();

  SonareMasteringChainResult result;
  std::memset(&result, 0xAB, sizeof(result));
  REQUIRE(sonare_master_audio(nullptr, nullptr, kLength, kRate, nullptr, 0, &result) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "preset must not be NULL");
  CHECK(result.samples == nullptr);
  CHECK(result.length == 0);

  REQUIRE(sonare_master_audio("streaming", nullptr, kLength, kRate, nullptr, 0, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "master_audio: samples must be a non-empty buffer");

  REQUIRE(sonare_master_audio("streaming", samples.data(), kLength, kRate, nullptr, 1, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "overrides must not be NULL when override_count is not 0");

  char* json = garbage_pointer<char>();
  REQUIRE(sonare_mastering_chain_config_params_json(nullptr, &json) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "document must not be NULL");
  CHECK(json == nullptr);
}

TEST_CASE("mastering repair entries refuse in header order", "[c_api][header_order]") {
  SonareDeclickResult result;
  std::memset(&result, 0xAB, sizeof(result));
  REQUIRE(sonare_mastering_repair_declick(nullptr, kLength, kRate, nullptr, &result) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "mastering_repair_declick: samples must be a non-empty buffer");
  CHECK(result.samples == nullptr);
  CHECK(result.length == 0);

  REQUIRE(sonare_mastering_repair_declick(nullptr, kLength, kRate, nullptr, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "mastering_repair_declick: samples must be a non-empty buffer");

  REQUIRE(sonare_mastering_repair_detect_clicks(nullptr, kLength, kRate, nullptr, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "mastering_repair_detect_clicks: samples must be a non-empty buffer");

  SonareDereverbReport report;
  std::memset(&report, 0xAB, sizeof(report));
  REQUIRE(sonare_mastering_repair_dereverb_classical_linked(nullptr, 2, kLength, kRate, nullptr,
                                                            nullptr, &report) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "channels must not be NULL");
  CHECK(report.mean_reduction_db == 0.0f);

  char* json = garbage_pointer<char>();
  REQUIRE(sonare_mastering_repair_analyze(nullptr, 2, kLength, kRate, nullptr, &json) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "channels must not be NULL");
  CHECK(json == nullptr);
}

TEST_CASE("mastering streaming entries refuse in header order", "[c_api][header_order]") {
  char* json = garbage_pointer<char>();
  REQUIRE(sonare_mastering_assistant_suggest(nullptr, kLength, kRate, nullptr, 0, &json) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "mastering_assistant_suggest: samples must be a non-empty buffer");
  CHECK(json == nullptr);

  REQUIRE(sonare_mastering_assistant_suggest(nullptr, kLength, kRate, nullptr, 0, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "mastering_assistant_suggest: samples must be a non-empty buffer");

  SonareStreamingMasteringChain* handle = garbage_pointer<SonareStreamingMasteringChain>();
  REQUIRE(sonare_streaming_mastering_chain_create_ex(nullptr, 1, 0.0f, 0.0f, &handle) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "params must not be NULL when param_count is not 0");
  CHECK(handle == nullptr);

  size_t written = 99;
  REQUIRE(sonare_streaming_mastering_chain_flush_mono(nullptr, nullptr, 0, &written) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "handle must not be NULL");
  CHECK(written == 0);

  int max_block = 77;
  REQUIRE(sonare_streaming_mastering_chain_max_block_size(nullptr, &max_block) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "handle must not be NULL");
  CHECK(max_block == 0);
}

TEST_CASE("mastering dynamics entries refuse in header order", "[c_api][header_order]") {
  const std::vector<float> samples = make_samples();

  float* out = garbage_pointer<float>();
  size_t out_length = 99;
  int latency = 77;
  REQUIRE(sonare_mastering_dynamics_compressor(nullptr, kLength, kRate, nullptr, &out, &out_length,
                                               &latency) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "mastering_dynamics_compressor: samples must be a non-empty buffer");
  CHECK(out == nullptr);
  CHECK(out_length == 0);
  CHECK(latency == 0);

  REQUIRE(sonare_mastering_dynamics_gate(nullptr, kLength, kRate, nullptr, nullptr, nullptr,
                                         nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "mastering_dynamics_gate: samples must be a non-empty buffer");

  out = garbage_pointer<float>();
  latency = 77;
  REQUIRE(sonare_mastering_dynamics_transient_shaper(samples.data(), kLength, kRate, nullptr, &out,
                                                     nullptr,
                                                     &latency) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out_length must not be NULL");
  CHECK(out == nullptr);
  CHECK(latency == 0);
}

#endif  // SONARE_WITH_MASTERING
