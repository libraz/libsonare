#include "metering/clipping.h"

#include <algorithm>
#include <cmath>

#include "util/constants.h"
#include "util/db.h"
#include "util/exception.h"
#include "util/parameter_checks.h"

namespace sonare::metering {

float clipping_threshold_from_db(float threshold_db) {
  if (std::isnan(threshold_db)) return kDefaultClippingThreshold;
  util::check::in_interval(threshold_db, constants::kFloorDb, true, 0.0, true, "threshold_db");
  return db_to_linear(threshold_db);
}

ClippingResult detect_clipping(const Audio& audio, float threshold, size_t min_region_samples) {
  SONARE_CHECK(threshold >= 0.0f && threshold <= 1.0f, ErrorCode::InvalidParameter);

  ClippingResult result;
  if (audio.empty()) return result;

  const float* data = audio.data();
  size_t i = 0;
  while (i < audio.size()) {
    // Negated inner test, so the two are exact complements: a NaN fails both, and
    // only the inner loop advances i -- a bare `< threshold` here never terminates.
    if (!(std::abs(data[i]) >= threshold)) {
      ++i;
      continue;
    }

    const size_t start = i;
    float region_peak = 0.0f;
    while (i < audio.size() && std::abs(data[i]) >= threshold) {
      region_peak = std::max(region_peak, std::abs(data[i]));
      ++i;
    }

    const size_t length = i - start;
    result.clipped_samples += length;
    result.max_clipped_peak = std::max(result.max_clipped_peak, region_peak);
    if (length >= min_region_samples) {
      result.regions.push_back({start, i, length, region_peak});
    }
  }

  // clipping_ratio is the fraction of ALL clipped samples (including short runs
  // below min_region_samples), whereas `regions` lists only runs at/above that
  // length. The two counts intentionally differ: the ratio measures total clip
  // exposure, the regions are the significant runs worth surfacing.
  result.clipping_ratio =
      static_cast<float>(result.clipped_samples) / static_cast<float>(audio.size());
  return result;
}

}  // namespace sonare::metering
