#include <string>
#include <vector>

#include "mastering/api/named_processor.h"

namespace sonare::mastering::api {

std::vector<std::string> processor_names() {
  return {"dynamics.brickwallLimiter",
          "dynamics.compressor",
          "dynamics.deesser",
          "dynamics.expander",
          "dynamics.gate",
          "dynamics.limiter",
          "dynamics.parallelComp",
          "dynamics.sidechainRouter",
          "dynamics.duckingProcessor",
          "dynamics.transientShaper",
          "dynamics.upwardCompressor",
          "dynamics.upwardExpander",
          "dynamics.vocalRider",
          "eq.apiStyle",
          "eq.bandPass",
          "eq.cutFilter",
          "eq.dynamic",
          "eq.equalizer",
          "eq.graphic",
          "eq.linearPhase",
          "eq.midSide",
          "eq.minimumPhase",
          "eq.parametric",
          "eq.pultec",
          "eq.shelving",
          "eq.tilt",
          "final.bitDepth",
          "final.dither",
          "final.outputChain",
          "maximizer.adaptiveRelease",
          "maximizer.loudnessOptimize",
          "maximizer.maximizer",
          "maximizer.softKneeMax",
          "maximizer.truePeakLimiter",
          "multiband.compressor",
          "multiband.dynamicEq",
          "multiband.expander",
          "multiband.imager",
          "multiband.limiter",
          "multiband.saturation",
          "repair.declick",
          "repair.declip",
          "repair.decrackle",
          "repair.dehum",
          "repair.denoiseClassical",
          "repair.dereverbClassical",
          "repair.trimSilence",
          "saturation.bitcrusher",
          "saturation.exciter",
          "saturation.hardClipper",
          "saturation.multibandExciter",
          "saturation.softClipper",
          "saturation.tape",
          "saturation.transformer",
          "saturation.tube",
          "saturation.waveshaper",
          "spectral.airBand",
          "spectral.lowEndFocus",
          "spectral.presenceEnhancer",
          "spectral.spectralShaper",
          "stereo.autoPan",
          "stereo.haasEnhancer",
          "stereo.imager",
          "stereo.monoMaker",
          "stereo.phaseAlign",
          "stereo.stereoBalance"};
}

std::vector<std::string> pair_processor_names() {
  return {"match.applyMatchEq", "match.alignReferenceToSource", "match.abSwitch",
          "match.abCrossfade"};
}

std::vector<std::string> pair_analysis_names() {
  return {"match.referenceLoudness", "match.tonalBalance", "match.tonalBalanceLogBands",
          "match.matchEqCurve", "match.estimateReferenceDelaySamples"};
}

std::vector<std::string> stereo_analysis_names() {
  return {"stereo.monoCompatCheck", "stereo.monoCompatCheckLogBands"};
}

}  // namespace sonare::mastering::api
