#pragma once

#include <limits>

#include "util/db.h"
#include "util/parameter_checks.h"

namespace sonare::mastering::eq {

inline constexpr double kEqFloatMax = static_cast<double>(std::numeric_limits<float>::max());
inline constexpr auto kEqGainScaleDomain = sonare::util::check::real_domain(0.0, 2.0);
inline constexpr auto kEqOutputPanDomain = sonare::util::check::real_domain(-1.0, 1.0);
inline constexpr auto kEqDbGainDomain =
    sonare::util::check::real_domain(-kEqFloatMax, static_cast<double>(sonare::kMaxFiniteGainDb));
inline constexpr auto kEqRatioDomain = sonare::util::check::real_domain(1.0, kEqFloatMax);
inline constexpr auto kEqNonnegativeFloatDomain =
    sonare::util::check::real_domain(0.0, kEqFloatMax);
inline constexpr auto kEqPositiveFloatDomain =
    sonare::util::check::real_domain(0.0, false, kEqFloatMax, true);
inline constexpr auto kEqSidechainFrequencyDomain =
    sonare::util::check::with_sentinel(kEqPositiveFloatDomain, -1.0);

inline sonare::util::check::NumericDomain eq_band_frequency_domain(double sample_rate) {
  return sonare::util::check::real_domain(0.0, false, sample_rate * 0.5, false);
}

}  // namespace sonare::mastering::eq
