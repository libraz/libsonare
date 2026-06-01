#include "mastering/final/output_chain.h"

#include "util/exception.h"

namespace sonare::mastering::final {

Audio output_chain(const Audio& audio, const OutputChainConfig& config) {
  if (audio.empty()) throw SonareException(ErrorCode::InvalidParameter, "audio must not be empty");
  Audio processed = dither(audio, {config.dither_type, config.target_bits});
  return bit_depth(processed, {config.target_bits, config.clamp});
}

}  // namespace sonare::mastering::final
