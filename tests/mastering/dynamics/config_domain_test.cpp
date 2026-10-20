#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>

#include "mastering/dynamics/compressor.h"
#include "mastering/dynamics/deesser.h"
#include "mastering/dynamics/transient_shaper.h"
#include "mastering/dynamics/vocal_rider.h"
#include "util/exception.h"

using namespace sonare;
using namespace sonare::mastering::dynamics;

namespace {

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

template <typename Processor, typename Config>
bool rejects_invalid_parameter(const Config& config) {
  try {
    Processor processor(config);
  } catch (const SonareException& e) {
    return e.code() == ErrorCode::InvalidParameter;
  }
  return false;
}

}  // namespace

TEST_CASE("dynamics config: default configs construct", "[mastering][dynamics][config]") {
  CHECK_NOTHROW(Compressor(CompressorConfig{}));
  CHECK_NOTHROW(DeEsser(DeEsserConfig{}));
  CHECK_NOTHROW(TransientShaper(TransientShaperConfig{}));
  CHECK_NOTHROW(VocalRider(VocalRiderConfig{}));
}

TEST_CASE("dynamics config: non-finite lookahead is rejected at construction",
          "[mastering][dynamics][config]") {
  for (float bad : {kInf, kNaN}) {
    TransientShaperConfig config;
    config.lookahead_ms = bad;
    CHECK(rejects_invalid_parameter<TransientShaper>(config));
  }
}

TEST_CASE("dynamics config: non-finite time constants are rejected",
          "[mastering][dynamics][config]") {
  for (float bad : {kNaN, kInf}) {
    SECTION("Compressor") {
      CompressorConfig attack;
      attack.attack_ms = bad;
      CHECK(rejects_invalid_parameter<Compressor>(attack));
      CompressorConfig release;
      release.release_ms = bad;
      CHECK(rejects_invalid_parameter<Compressor>(release));
    }
    SECTION("DeEsser") {
      DeEsserConfig attack;
      attack.attack_ms = bad;
      CHECK(rejects_invalid_parameter<DeEsser>(attack));
      DeEsserConfig release;
      release.release_ms = bad;
      CHECK(rejects_invalid_parameter<DeEsser>(release));
    }
    SECTION("TransientShaper") {
      TransientShaperConfig fast_attack;
      fast_attack.fast_attack_ms = bad;
      CHECK(rejects_invalid_parameter<TransientShaper>(fast_attack));
      TransientShaperConfig slow_release;
      slow_release.slow_release_ms = bad;
      CHECK(rejects_invalid_parameter<TransientShaper>(slow_release));
    }
    SECTION("VocalRider") {
      VocalRiderConfig attack;
      attack.attack_ms = bad;
      CHECK(rejects_invalid_parameter<VocalRider>(attack));
      VocalRiderConfig release;
      release.release_ms = bad;
      CHECK(rejects_invalid_parameter<VocalRider>(release));
    }
  }
}

TEST_CASE("dynamics config: NaN threshold is rejected", "[mastering][dynamics][config]") {
  CompressorConfig compressor;
  compressor.threshold_db = kNaN;
  CHECK(rejects_invalid_parameter<Compressor>(compressor));

  DeEsserConfig deesser;
  deesser.threshold_db = kNaN;
  CHECK(rejects_invalid_parameter<DeEsser>(deesser));
}
