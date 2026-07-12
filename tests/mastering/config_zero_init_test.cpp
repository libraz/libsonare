/// @file config_zero_init_test.cpp
/// @brief Regression test for the SonareMasteringConfig zero-init contract.
///
/// The public SonareMasteringConfig struct is documented as zero-init friendly:
/// a field left at 0 must fall back to the library default. Prior to the fix,
/// to_cpp_config copied true_peak_oversample verbatim, so a zero-initialized
/// config (oversample == 0) reached the maximizer validator, which rejects any
/// oversample not in {1, 2, 4, 8, 16} and returned SONARE_ERROR_INVALID_PARAMETER
/// on the raw C-ABI path while the higher-level bindings (which set 4 explicitly)
/// succeeded. The fix guards the copy so 0 falls through to the C++ default.

#include <sonare/sonare_c.h>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "support/audio_fixtures.h"
#include "util/constants.h"

#ifdef SONARE_WITH_MASTERING
namespace {}  // namespace

// NOTE: sonare_mastering_process runs the full maximizer (LUFS measurement plus
// an oversampling true-peak limiter). It is kept short (0.25 s mono) so this
// stays a default-tier test rather than a slow one.
TEST_CASE("zero-init mastering config uses default true_peak_oversample", "[c_api][mastering]") {
  const int sample_rate = 22050;
  auto samples = sonare::test::generate_sine_samples(
      440.0f, sample_rate, static_cast<int>(static_cast<float>(sample_rate) * 0.25f), 0.2f);

  // A doc-compliant zero-init caller that only sets the two real-valued fields;
  // true_peak_oversample (and release_ms / apply_gain_at_input_rate) stay 0.
  SonareMasteringConfig config{};
  config.target_lufs = -18.0f;
  config.ceiling_db = -1.0f;
  REQUIRE(config.true_peak_oversample == 0);

  SonareMasteringResult result{};
  const SonareError err =
      sonare_mastering_process(samples.data(), samples.size(), sample_rate, &config, &result);

  // Before the fix this returned SONARE_ERROR_INVALID_PARAMETER because the
  // 0 oversample was passed straight to the validator.
  REQUIRE(err == SONARE_OK);
  REQUIRE(result.samples != nullptr);
  REQUIRE(result.length == samples.size());
  REQUIRE(std::isfinite(result.output_lufs));

  sonare_free_mastering_result(&result);
}
#endif  // SONARE_WITH_MASTERING
