#include "mastering/final/output_chain.h"

#include "util/exception.h"

namespace sonare::mastering::final {

Audio output_chain(const Audio& audio, const OutputChainConfig& config, QuantizeReport* report) {
  // A dithered result already sits on the target grid, so only an undithered input still needs
  // the bit-depth stage; each stage substitutes non-finite samples and reports its counts.
  if (config.dither_type == DitherType::None) {
    return bit_depth(audio, {config.target_bits}, report);
  }
  return dither(audio, {config.dither_type, config.target_bits}, report);
}

}  // namespace sonare::mastering::final
