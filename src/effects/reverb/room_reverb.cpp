#include "effects/reverb/room_reverb.h"

#include <cmath>
#include <vector>

#include "acoustic/rir_synthesizer.h"
#include "acoustic/room_model.h"
#include "core/diagnostic.h"
#include "util/exception.h"

namespace sonare::effects::reverb {

namespace {

/// The synthesis configuration this engine derives from its own config. Built in
/// one place so the constructor validates exactly the values prepare() submits.
sonare::acoustic::RirSynthConfig rir_config_from(const RoomReverbConfig& config) {
  sonare::acoustic::RirSynthConfig rc;
  rc.ism_order = config.ism_order;
  rc.seed = config.seed;
  rc.max_seconds = config.max_seconds;
  rc.air_absorption_enabled = config.air_absorption_enabled;
  rc.air = config.air;
  return rc;
}

}  // namespace

RoomReverb::RoomReverb(RoomReverbConfig config) : config_(config) {
  const sonare::acoustic::ShoeboxRoom room =
      sonare::acoustic::uniform_shoebox(config_.dims, config_.absorption);
  const std::vector<Diagnostic> geometry = sonare::acoustic::validate_shoebox(
      room, sonare::acoustic::SourceListener{config_.source, config_.listener});
  SONARE_CHECK_MSG(!has_error(geometry), ErrorCode::InvalidParameter,
                   "room reverb target geometry, placement, or material is invalid");
  // Every input synthesize_rir would reject is rejected here instead: an Error
  // from it yields an empty RIR, which the convolution path treats as dry
  // passthrough, so accepting these values would produce a silently inert insert.
  const std::vector<Diagnostic> synthesis =
      sonare::acoustic::validate_rir_synth_config(rir_config_from(config_));
  SONARE_CHECK_MSG(!has_error(synthesis), ErrorCode::InvalidParameter,
                   "room reverb image-source order, RIR length cap, or air absorption is invalid");

  // Honour the configured mix at construction (sibling reverbs do the same).
  set_parameter(0, config_.dry_wet);
}

void RoomReverb::prepare(double sample_rate, int max_block_size) {
  using namespace sonare::acoustic;

  const ShoeboxRoom room = uniform_shoebox(config_.dims, config_.absorption);
  const RirSynthConfig rc = rir_config_from(config_);

  const int sr = sample_rate > 0.0 ? static_cast<int>(std::lround(sample_rate)) : 48000;
  const RirSynthResult res =
      synthesize_rir(room, SourceListener{config_.source, config_.listener}, sr, rc);

  // Geometry, order, length cap and air absorption were all validated at
  // construction; the host sample rate was not, and it is the one synthesis
  // input this engine first sees here. Synthesis reports a refusal as an Error
  // diagnostic plus an empty RIR, which the convolution path would render as an
  // inaudible insert, so the diagnostics are read rather than dropped and the
  // refusal is raised with the reason the synthesizer gave.
  SONARE_CHECK_MSG(!has_error(res.diagnostics), ErrorCode::InvalidParameter,
                   "room reverb RIR synthesis failed: " + first_error_text(res.diagnostics));

  // Establish partition size and per-channel buffers, then load the synthesized
  // IR (rebuilds the convolvers). The RIR carries its physical 1/(4*pi*d)
  // attenuation, so it is normalized to the same unit energy as the base class's
  // default IR: dryWet must mean the same mix depth on this engine as on the
  // sibling reverbs.
  suppress_default_ir_synthesis();
  ConvolutionReverb::prepare(sample_rate, max_block_size);
  load_ir_unit_energy(res.rir.data(), static_cast<int>(res.rir.size()));
}

}  // namespace sonare::effects::reverb
