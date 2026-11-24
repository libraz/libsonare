/// @file c_bare_refusal_mastering_repair_test.cpp
/// @brief The mastering repair, apply and equalizer entries state the cause of each argument
/// refusal.

#include <sonare/sonare_c.h>

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <string>
#include <vector>

namespace {

std::string last_message() {
  const char* message = sonare_last_error_message();
  return message != nullptr ? message : "";
}

constexpr int kRate = 48000;

}  // namespace

TEST_CASE("sonare_eq_set_band names the refused band index", "[c_api][mastering]") {
  SonareEq* eq = nullptr;
  REQUIRE(sonare_eq_create(kRate, 512, &eq) == SONARE_OK);
  REQUIRE(sonare_eq_set_band(eq, -1, "{}") == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "index must be an integer in [0, 23]");
  REQUIRE(sonare_eq_set_band(eq, SONARE_EQ_MAX_BANDS, "{}") == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "index must be an integer in [0, 23]");
  sonare_eq_destroy(eq);
}

TEST_CASE("sonare_eq_magnitude_response refuses indexed frequencies without writing output",
          "[c_api][mastering][domain-repair-eq]") {
  SonareEq* eq = nullptr;
  REQUIRE(sonare_eq_create(kRate, 512, &eq) == SONARE_OK);
  const std::array<float, 5> invalid_values = {
      -1.0f, 24001.0f, std::numeric_limits<float>::quiet_NaN(),
      std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()};
  for (const float invalid : invalid_values) {
    const float frequencies[] = {1000.0f, invalid};
    float out_db[] = {17.0f, -23.0f};
    CHECK(sonare_eq_magnitude_response(eq, 0, frequencies, 2, out_db) ==
          SONARE_ERROR_INVALID_PARAMETER);
    CHECK(sonare_last_error_code() == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(last_message() == "frequencies_hz[1] must be finite and in [0, 24000]");
    CHECK(out_db[0] == 17.0f);
    CHECK(out_db[1] == -23.0f);
  }
  sonare_eq_destroy(eq);
}

TEST_CASE("sonare_eq_match names the refused band count", "[c_api][mastering]") {
  SonareEq* eq = nullptr;
  REQUIRE(sonare_eq_create(kRate, 512, &eq) == SONARE_OK);
  const std::vector<float> audio(4800, 0.1f);
  REQUIRE(sonare_eq_match(eq, audio.data(), audio.size(), kRate, audio.data(), audio.size(), kRate,
                          0) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "max_bands must be an integer in [1, 24]");
  REQUIRE(sonare_eq_match(eq, audio.data(), audio.size(), kRate, audio.data(), audio.size(), kRate,
                          SONARE_EQ_MAX_BANDS + 1) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "max_bands must be an integer in [1, 24]");
  sonare_eq_destroy(eq);
}

TEST_CASE("sonare_mastering_repair_noise_band_bins names the refused grid", "[c_api][mastering]") {
  int bins[SONARE_REPAIR_NOISE_BAND_EDGE_COUNT];
  REQUIRE(sonare_mastering_repair_noise_band_bins(1000, kRate, bins) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "n_fft must be a power of two in [1, 2147483647]");
  REQUIRE(sonare_mastering_repair_noise_band_bins(1024, 0, bins) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "sample_rate must be an integer in [1, 2147483647]");
}

TEST_CASE("the channel-count refusals of the repair entries name num_channels",
          "[c_api][mastering]") {
  const std::vector<float> plane(4800, 0.1f);
  const float* planes[2] = {plane.data(), plane.data()};
  char* json = nullptr;

  REQUIRE(sonare_mastering_repair_analyze(planes, 0, plane.size(), kRate, "", &json) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "num_channels must be an integer of at least 1");

  std::vector<float> out0(plane.size()), out1(plane.size());
  float* out_planes[2] = {out0.data(), out1.data()};
  REQUIRE(sonare_mastering_repair_apply(planes, 0, plane.size(), kRate, "", nullptr, nullptr,
                                        out_planes, &json, nullptr,
                                        nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "num_channels must be an integer of at least 1");

  SonareDereverbLinkedResult linked{};
  REQUIRE(sonare_mastering_repair_dereverb_classical_linked(
              planes, 0, plane.size(), kRate, nullptr, &linked) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "num_channels must be an integer of at least 1");
}

TEST_CASE("sonare_mastering_repair_apply names the NULL output plane", "[c_api][mastering]") {
  const std::vector<float> plane(4800, 0.1f);
  const float* planes[2] = {plane.data(), plane.data()};
  std::vector<float> out0(plane.size());
  float* out_planes[2] = {out0.data(), nullptr};
  char* json = nullptr;
  REQUIRE(sonare_mastering_repair_apply(planes, 2, plane.size(), kRate, "", nullptr, nullptr,
                                        out_planes, &json, nullptr,
                                        nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "out_channels[1] must not be NULL");
}

TEST_CASE("the named-processor apply entries refuse an empty processor name",
          "[c_api][mastering]") {
  const std::vector<float> audio(4800, 0.1f);

  SonareMasteringResult mono{};
  REQUIRE(sonare_mastering_apply_processor("", audio.data(), audio.size(), kRate, nullptr, 0,
                                           &mono) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "processor must not be empty");

  SonareMasteringStereoResult stereo{};
  REQUIRE(sonare_mastering_apply_processor_stereo("", audio.data(), audio.data(), audio.size(),
                                                  kRate, nullptr, 0,
                                                  &stereo) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "processor must not be empty");

  REQUIRE(sonare_mastering_apply_pair_processor("", audio.data(), audio.size(), audio.data(),
                                                audio.size(), kRate, kRate, nullptr, 0,
                                                &mono) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "processor must not be empty");

  REQUIRE(sonare_mastering_apply_pair_processor_stereo(
              "", audio.data(), audio.data(), audio.size(), audio.data(), audio.data(),
              audio.size(), kRate, kRate, nullptr, 0, &stereo) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "processor must not be empty");
}
