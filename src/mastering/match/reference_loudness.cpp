#include "mastering/match/reference_loudness.h"

#include <cmath>

#include "mastering/common/loudness_measure.h"
#include "util/exception.h"

namespace sonare::mastering::match {

ReferenceLoudness reference_loudness(const Audio& source, const Audio& reference) {
  if (source.empty() || reference.empty()) {
    throw SonareException(ErrorCode::InvalidParameter, "audio must not be empty");
  }
  const float source_lufs = common::measure_lufs(source);
  const float reference_lufs = common::measure_lufs(reference);
  const float gain = std::isfinite(source_lufs) && std::isfinite(reference_lufs)
                         ? reference_lufs - source_lufs
                         : 0.0f;
  return {source_lufs, reference_lufs, gain};
}

}  // namespace sonare::mastering::match
