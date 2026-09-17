#include "core/stereo_pair.h"

#include "util/exception.h"

namespace sonare {

void require_stereo_pair(const Audio& left, const Audio& right) {
  if (left.empty() || right.empty()) {
    throw SonareException(ErrorCode::InvalidParameter, "audio must not be empty");
  }
  if (left.size() != right.size()) {
    throw SonareException(ErrorCode::InvalidParameter, "stereo channels must have the same length");
  }
  if (left.sample_rate() != right.sample_rate()) {
    throw SonareException(ErrorCode::InvalidParameter, "stereo channels must share a sample rate");
  }
}

}  // namespace sonare
