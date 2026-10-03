/// @file sonare_c_mastering_streaming_test.cpp
/// @brief Streaming mastering parameter-control C API tests.

#include <sonare/sonare_c.h>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <vector>

#include "sonare_c_test_helpers.h"

#ifdef SONARE_WITH_MASTERING

TEST_CASE("streaming mastering C API sets prepared realtime parameters",
          "[c_api][mastering][streaming]") {
  const SonareMasteringParam params[] = {
      {"maximizer.truePeakLimiter.enabled", 1.0},
  };
  SonareStreamingMasteringChain* chain = sonare_streaming_mastering_chain_create(params, 1);
  REQUIRE(chain != nullptr);

  REQUIRE(sonare_streaming_mastering_chain_set_parameter(
              chain, "maximizer.truePeakLimiter.ceilingDb", -12.0) == SONARE_ERROR_INVALID_STATE);
  REQUIRE(sonare_streaming_mastering_chain_prepare(chain, 48000, 128, 1) == SONARE_OK);
  REQUIRE(sonare_streaming_mastering_chain_set_parameter(
              chain, "maximizer.truePeakLimiter.ceilingDb", -12.0) == SONARE_OK);

  std::vector<float> block(128, 0.8f);
  REQUIRE(sonare_streaming_mastering_chain_process_mono(chain, block.data(), block.size()) ==
          SONARE_OK);

  REQUIRE(sonare_streaming_mastering_chain_set_parameter(chain,
                                                         "maximizer.truePeakLimiter.lookaheadMs",
                                                         4.0) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_streaming_mastering_chain_set_parameter(
              chain, "maximizer.truePeakLimiter.ceilingDb",
              std::numeric_limits<double>::quiet_NaN()) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_streaming_mastering_chain_set_parameter(chain, "missing.parameter", -1.0) ==
          SONARE_ERROR_INVALID_PARAMETER);

  sonare_streaming_mastering_chain_destroy(chain);
}

TEST_CASE("streaming mastering C API validates setter handles and keys",
          "[c_api][mastering][streaming]") {
  REQUIRE(sonare_streaming_mastering_chain_set_parameter(nullptr,
                                                         "maximizer.truePeakLimiter.ceilingDb",
                                                         -12.0) == SONARE_ERROR_INVALID_PARAMETER);

  SonareStreamingMasteringChain* chain = sonare_streaming_mastering_chain_create(nullptr, 0);
  REQUIRE(chain != nullptr);
  REQUIRE(sonare_streaming_mastering_chain_set_parameter(chain, nullptr, -12.0) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_streaming_mastering_chain_set_parameter(chain, "", -12.0) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_streaming_mastering_chain_set_parameter(
              chain, "maximizer.truePeakLimiter.ceilingDb",
              std::numeric_limits<double>::infinity()) == SONARE_ERROR_INVALID_PARAMETER);
  sonare_streaming_mastering_chain_destroy(chain);
}

#endif  // SONARE_WITH_MASTERING
