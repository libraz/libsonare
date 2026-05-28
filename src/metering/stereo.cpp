#include "metering/stereo.h"

#include "util/constants.h"
#include "util/exception.h"
#include "util/stereo_metrics.h"

namespace sonare::metering {

using sonare::constants::kInvSqrt2;

float correlation(const float* left, const float* right, size_t length) {
  return util::stereo_correlation(left, right, length);
}

float stereo_width(const float* left, const float* right, size_t length) {
  return util::stereo_width(left, right, length);
}

std::vector<VectorscopePoint> vectorscope(const float* left, const float* right, size_t length) {
  SONARE_CHECK(length > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(left != nullptr && right != nullptr, ErrorCode::InvalidParameter);

  std::vector<VectorscopePoint> points(length);
  for (size_t i = 0; i < length; ++i) {
    points[i].mid = (left[i] + right[i]) * kInvSqrt2;
    points[i].side = (left[i] - right[i]) * kInvSqrt2;
  }
  return points;
}

}  // namespace sonare::metering
