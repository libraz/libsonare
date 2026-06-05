#include "effects/reverb/room_reverb.h"

#include <cmath>

#include "acoustic/rir_synthesizer.h"
#include "acoustic/room_model.h"

namespace sonare::effects::reverb {

RoomReverb::RoomReverb(RoomReverbConfig config) : config_(config) {
  // Honour the configured mix at construction (sibling reverbs do the same).
  set_parameter(0, config_.dry_wet);
}

void RoomReverb::prepare(double sample_rate, int max_block_size) {
  using namespace sonare::acoustic;

  const ShoeboxRoom room = uniform_shoebox(config_.dims, config_.absorption);

  RirSynthConfig rc;
  rc.ism_order = config_.ism_order;
  rc.seed = config_.seed;
  rc.max_seconds = config_.max_seconds;

  const int sr = sample_rate > 0.0 ? static_cast<int>(std::lround(sample_rate)) : 48000;
  const RirSynthResult res =
      synthesize_rir(room, SourceListener{config_.source, config_.listener}, sr, rc);

  // Establish partition size and per-channel buffers, then load the synthesized
  // IR (rebuilds the convolvers). An invalid geometry yields an empty IR, which
  // ConvolutionReverb::process() treats as a dry passthrough.
  suppress_default_ir_synthesis();
  ConvolutionReverb::prepare(sample_rate, max_block_size);
  load_ir(res.rir.data(), static_cast<int>(res.rir.size()));
}

}  // namespace sonare::effects::reverb
