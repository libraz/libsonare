// Every sonare_<type>_create returns SonareError and writes the handle through
// its last parameter, which it leaves NULL on any failure.

#include <sonare/sonare_c.h>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <string>

namespace {

template <typename T>
T* sentinel() {
  return reinterpret_cast<T*>(static_cast<uintptr_t>(0x1));
}

}  // namespace

TEST_CASE("sonare_eq_create returns its error and leaves out NULL on failure",
          "[c_api][c_create]") {
  CHECK(sonare_eq_create(48000, 512, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  SonareEq* eq = sentinel<SonareEq>();
  CHECK(sonare_eq_create(0, 512, &eq) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(eq == nullptr);
  REQUIRE(sonare_eq_create(48000, 512, &eq) == SONARE_OK);
  CHECK(eq != nullptr);
  sonare_eq_destroy(eq);
}

TEST_CASE("sonare_mixer_create returns its error and leaves out NULL on failure",
          "[c_api][c_create]") {
  CHECK(sonare_mixer_create(48000, 512, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  SonareMixer* mixer = sentinel<SonareMixer>();
  CHECK(sonare_mixer_create(0, 512, &mixer) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mixer == nullptr);
  REQUIRE(sonare_mixer_create(48000, 512, &mixer) == SONARE_OK);
  CHECK(mixer != nullptr);
  sonare_mixer_destroy(mixer);
}

TEST_CASE(
    "sonare_streaming_mastering_chain_create returns its error and leaves out NULL on "
    "failure",
    "[c_api][c_create]") {
  CHECK(sonare_streaming_mastering_chain_create(nullptr, nullptr, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  SonareStreamingMasteringChain* chain = sentinel<SonareStreamingMasteringChain>();
  CHECK(sonare_streaming_mastering_chain_create("{", nullptr, &chain) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(chain == nullptr);
  chain = sentinel<SonareStreamingMasteringChain>();
  SonareStreamingMasteringChainOptions options;
  sonare_streaming_mastering_chain_options_init(&options);
  CHECK(sonare_streaming_mastering_chain_create("{", &options, &chain) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(chain == nullptr);
  REQUIRE(sonare_streaming_mastering_chain_create(nullptr, nullptr, &chain) == SONARE_OK);
  CHECK(chain != nullptr);
  sonare_streaming_mastering_chain_destroy(chain);
}

TEST_CASE("sonare_sample_bank_create returns its error and leaves out NULL on failure",
          "[c_api][c_create]") {
  CHECK(sonare_sample_bank_create(nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  SonareSampleBank* bank = sentinel<SonareSampleBank>();
  const SonareError err = sonare_sample_bank_create(&bank);
  if (err == SONARE_ERROR_NOT_SUPPORTED) {
    CHECK(bank == nullptr);
    return;
  }
  REQUIRE(err == SONARE_OK);
  CHECK(bank != nullptr);
  sonare_sample_bank_destroy(bank);
}

TEST_CASE("sonare_streaming_retune_create returns its error and leaves out NULL on failure",
          "[c_api][c_create]") {
  CHECK(sonare_streaming_retune_create(0.0f, 1.0f, 0, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  SonareStreamingRetune* retune = sentinel<SonareStreamingRetune>();
  const SonareError err = sonare_streaming_retune_create(NAN, 1.0f, 0, &retune);
  CHECK(retune == nullptr);
  if (err == SONARE_ERROR_NOT_SUPPORTED) return;
  CHECK(err == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_streaming_retune_create(0.0f, 1.0f, 0, &retune) == SONARE_OK);
  CHECK(retune != nullptr);
  sonare_streaming_retune_destroy(retune);
}

#if defined(SONARE_WITH_MASTERING)
TEST_CASE("chain configuration retains the structured name of a refused stage field",
          "[c_api][c_create][mastering]") {
  struct Row {
    const char* params;
    const char* field;
  };
  const Row rows[] = {
      {R"({"dynamics.compressor.ratio":0.5})", "dynamics.compressor.ratio"},
      {R"({"dynamics.deesser.bandPassQ":0})", "dynamics.deesser.bandPassQ"},
  };
  for (const Row& row : rows) {
    CAPTURE(row.field);
    SonareStreamingMasteringChain* chain = nullptr;
    REQUIRE(sonare_streaming_mastering_chain_create(row.params, nullptr, &chain) ==
            SONARE_ERROR_INVALID_PARAMETER);
    CHECK(chain == nullptr);
    REQUIRE(sonare_last_refusal_subject_count() == 1);
    CHECK(std::string(sonare_last_refusal_subject(0)) == std::string("config.") + row.field);

    const std::string doc = std::string(R"({"version":2,"params":)") + row.params + "}";
    char* output = nullptr;
    REQUIRE(sonare_mastering_chain_config_from_json(doc.c_str(), &output) ==
            SONARE_ERROR_INVALID_FORMAT);
    CHECK(output == nullptr);
    REQUIRE(sonare_last_refusal_subject_count() == 1);
    CHECK(std::string(sonare_last_refusal_subject(0)) == std::string("params.") + row.field);
  }
}
#endif
