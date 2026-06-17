#include <set>
#include <string>
#include <vector>

#include "mastering/api/insert_factory.h"
#include "mastering/api/named_processor.h"

namespace sonare::mastering::api {

std::vector<std::string> processor_names() {
  return {
      "dynamics.brickwallLimiter",
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
      "saturation.ampSim",
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
      "stereo.stereoBalance",
#ifdef SONARE_HAVE_FX
      // Creative streaming effects reachable from the one-shot apply path
      // (built as inserts and run through the latency-compensating runner).
      // "effects.reverb.plate" is an alias for "effects.reverb.dattorro".
      "effects.reverb.plate",
      "effects.reverb.dattorro",
      "effects.reverb.fdn",
      "effects.reverb.velvet",
      "effects.reverb.convolution",
      "effects.modulation.chorus",
      "effects.modulation.flanger",
      "effects.modulation.phaser",
      "effects.delay.stereo",
#endif
  };
}

std::vector<std::string> stereo_processor_names() {
  // Processors that are only meaningful (or only implemented) on a true stereo
  // signal: the `stereo.*` width/balance tools, `eq.midSide` (operates on the
  // mid/side decomposition), and the `multiband.*` processors whose
  // implementations dispatch through the stereo path. These names appear in
  // processor_names() (they run fine via apply_named_processor_stereo) but they
  // have no mono implementation, so the mono apply_named_processor() rejects
  // them with an opaque INVALID_PARAMETER. Callers can consult this list to give
  // a clear "stereo-only processor" diagnostic instead.
  return {"eq.midSide",           "multiband.compressor", "multiband.dynamicEq",
          "multiband.expander",   "multiband.imager",     "multiband.limiter",
          "multiband.saturation", "stereo.autoPan",       "stereo.haasEnhancer",
          "stereo.imager",        "stereo.monoMaker",     "stereo.phaseAlign",
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

ChannelPolicy channel_policy(const std::string& id) {
  // Inherently-stereo processors: they operate on planes 0/1 and pass any
  // surround planes through dry. Everything else processes all planes correctly
  // in a single full-buffer call (Multichannel), which is also the safe default
  // for any unlisted/legacy id. Mirrors the per-process() channel-handling audit
  // (the 6 stereo-image processors, eq.midSide, multiband.imager, and every
  // reverb/modulation/delay effect).
  static const std::set<std::string> kStereoPairOnly = {
      "stereo.imager",
      "stereo.monoMaker",
      "stereo.stereoBalance",
      "stereo.haasEnhancer",
      "stereo.phaseAlign",
      "stereo.autoPan",
      "eq.midSide",
      "multiband.imager",
      "effects.reverb.plate",
      "effects.reverb.dattorro",
      "effects.reverb.fdn",
      "effects.reverb.velvet",
      "effects.reverb.convolution",
      "effects.reverb.room",
      "effects.acoustic.roomMorph",
      "effects.modulation.chorus",
      "effects.modulation.ensemble",
      "effects.modulation.flanger",
      "effects.modulation.phaser",
      "effects.delay.stereo",
  };
  return kStereoPairOnly.count(id) != 0 ? ChannelPolicy::StereoPairOnly
                                        : ChannelPolicy::Multichannel;
}

const char* channel_policy_to_string(ChannelPolicy policy) noexcept {
  switch (policy) {
    case ChannelPolicy::Multichannel:
      return "multichannel";
    case ChannelPolicy::StereoPairOnly:
      return "stereoPairOnly";
    case ChannelPolicy::PerChannel:
      return "perChannel";
    case ChannelPolicy::Passthrough:
      return "passthrough";
  }
  return "multichannel";
}

std::string processor_catalog_json() {
  const std::set<std::string> insert_set = [] {
    const auto names = insert_factory_names();
    return std::set<std::string>(names.begin(), names.end());
  }();
  const std::set<std::string> pair_set = [] {
    const auto names = pair_processor_names();
    return std::set<std::string>(names.begin(), names.end());
  }();
  const std::set<std::string> stereo_set = [] {
    const auto names = stereo_processor_names();
    return std::set<std::string>(names.begin(), names.end());
  }();

  // Sorted union of every id the host might surface, so realtime-only ids (e.g.
  // effects.reverb.room) and pair ids absent from processor_names() are covered.
  std::set<std::string> ids;
  for (const auto& name : processor_names()) ids.insert(name);
  for (const auto& name : insert_set) ids.insert(name);
  for (const auto& name : pair_set) ids.insert(name);

  std::string out = "[";
  bool first = true;
  for (const std::string& id : ids) {
    if (!first) out += ',';
    first = false;
    const bool realtime_insertable = insert_set.count(id) != 0;
    const bool is_pair = pair_set.count(id) != 0;
    const char* kind = is_pair ? "pair" : (realtime_insertable ? "realtime" : "offline");
    out += "{\"id\":\"";
    out += id;
    out += "\",\"kind\":\"";
    out += kind;
    out += "\",\"realtimeInsertable\":";
    out += realtime_insertable ? "true" : "false";
    out += ",\"stereoOnly\":";
    out += stereo_set.count(id) != 0 ? "true" : "false";
    out += ",\"channelPolicy\":\"";
    out += channel_policy_to_string(channel_policy(id));
    out += '"';
    out += '}';
  }
  out += ']';
  return out;
}

}  // namespace sonare::mastering::api
