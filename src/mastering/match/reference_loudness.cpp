#include "mastering/match/reference_loudness.h"

#include "mastering/common/loudness_measure.h"
#include "metering/lufs.h"
#include "util/exception.h"

namespace sonare::mastering::match {

ReferenceLoudness reference_loudness(const Audio& source, const Audio& reference) {
  if (source.empty() || reference.empty()) {
    throw SonareException(ErrorCode::InvalidParameter, "audio must not be empty");
  }
  const float reference_lufs = common::measure_lufs(reference);
  const metering::LufsGainToTarget solved = metering::gain_to_integrated_lufs(
      source.data(), source.size(), 1, source.sample_rate(), reference_lufs);
  return {solved.measured_lufs, reference_lufs, solved.gain_db};
}

}  // namespace sonare::mastering::match
