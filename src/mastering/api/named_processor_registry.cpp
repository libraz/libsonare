#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "mastering/api/insert_factory.h"
#include "mastering/api/named_processor.h"
#include "rt/processor_base.h"

namespace sonare::mastering::api {

std::vector<std::string> processor_names() {
  std::vector<std::string> names = {
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
  };
  // Creative streaming effects are not configured offline: apply_named_processor
  // dispatches every "effects." id by building the realtime insert and running
  // it through the latency-compensating runner. The insert factory is therefore
  // the only registry of which effects ship in this build configuration, and
  // deriving the section from it keeps the two from diverging (and keeps the
  // BUILD_FX / acoustic-simulation guards in exactly one place).
  for (const std::string& name : insert_factory_names()) {
    if (name.rfind("effects.", 0) == 0) names.push_back(name);
  }
  return names;
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
  // for any unlisted/legacy id. LowEndFocus is intentionally Multichannel: its
  // width stage couples only planes 0/1 while low-end enhancement is per-plane.
  // Mirrors the per-process() channel-handling audit (the 6 stereo-image
  // processors, eq.midSide, multiband.imager, and every reverb/modulation/delay
  // effect).
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
      // Wah / auto-wah run one bandpass per plane but allocate only a stereo
      // pair; rotary and the pitch shifter cap their inner loop at two planes.
      // All four therefore process planes 0/1 and pass any surround plane through
      // dry, so classify them stereo-pair-only rather than multichannel.
      "effects.modulation.wah",
      "effects.modulation.autoWah",
      "effects.modulation.rotary",
      "effects.modulation.pitchShifter",
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

namespace {

// Representative configuration for probing realtime-insert timing. Config-
// dependent processors (linear-phase EQ FFT length, lookahead limiters/
// maximizers, delays/reverbs) derive their reported latency/tail at prepare()
// time from the block size, sample rate, and parameters, so catalog values
// reflect this default configuration and are representative rather than exact
// for a differently configured insert; the host treats them as fallback
// estimates. Both values come from one prepared instance so they cannot drift.
constexpr double kCatalogProbeSampleRate = 48000.0;
constexpr int kCatalogProbeBlockSize = 512;

struct InsertTiming {
  int latency_samples = 0;
  int tail_samples = 0;
};

// Timing an insertable processor reports for its default configuration, or
// zeros for an id with no realtime insert or that fails to build/prepare.
InsertTiming insert_timing(const std::string& id) {
  try {
    std::unique_ptr<sonare::rt::ProcessorBase> processor = make_insert(id, "{}");
    if (processor == nullptr) return {};
    processor->prepare(kCatalogProbeSampleRate, kCatalogProbeBlockSize);
    return {std::max(0, processor->latency_samples()), std::max(0, processor->tail_samples())};
  } catch (...) {
    return {};
  }
}

// A deliberately coarse host-facing indication of the per-sample algorithmic
// work for realtime inserts under the default `{}` configuration. This is a
// qualitative policy tier, not a machine-specific numeric benchmark: it lets a
// host avoid treating a bounded, multi-tap Velvet reverb as equivalent to a
// lightweight zero-latency insert when assembling a live strip.
const char* realtime_cost(const std::string& id) noexcept {
  // Oversampled triode stages. "saturation.ampSim" runs the same 4x oversampled
  // Dempwolf triode as "saturation.tube" (it owns a Tube instance and processes
  // it unconditionally) and adds a tone stack plus a cab EQ on top, so it can
  // never be cheaper than the stage it embeds.
  if (id == "saturation.tube" || id == "saturation.ampSim") return "high";
  if (id == "effects.reverb.velvet") return "high";
  // Default-on 4x polyphase oversampling. "spectral.airBand" resamples its
  // harmonic band on every block, like the true-peak limiter's detector; the
  // exciter and the presence enhancer own the same oversampler but leave it off
  // in the default configuration, which is what this tier describes.
  if (id == "maximizer.truePeakLimiter" || id == "spectral.airBand") return "moderate";
  // Partitioned FFT convolution ("effects.reverb.room" derives from
  // ConvolutionReverb and inherits its process(); "eq.linearPhase" convolves an
  // FFT-designed linear-phase FIR) and delay-network reverbs, whose tanks of
  // delay lines and allpasses are an order of magnitude above a biquad chain.
  if (id == "effects.reverb.fdn" || id == "effects.reverb.convolution" ||
      id == "effects.reverb.room" || id == "effects.reverb.dattorro" ||
      id == "effects.reverb.plate" || id == "effects.acoustic.roomMorph" ||
      id == "eq.linearPhase") {
    return "moderate";
  }
  return "low";
}

// Stable UI group for the first segment of a named processor id. `match.*`
// processors are reference-analysis/application tools, so their public group
// spells that role rather than leaking the implementation namespace.
const char* catalog_category(const std::string& id) {
  if (id.rfind("match.", 0) == 0) return "reference";
  if (id.rfind("eq.", 0) == 0) return "eq";
  if (id.rfind("dynamics.", 0) == 0) return "dynamics";
  if (id.rfind("multiband.", 0) == 0) return "multiband";
  if (id.rfind("stereo.", 0) == 0) return "stereo";
  if (id.rfind("saturation.", 0) == 0) return "saturation";
  if (id.rfind("repair.", 0) == 0) return "repair";
  if (id.rfind("maximizer.", 0) == 0) return "maximizer";
  if (id.rfind("effects.", 0) == 0) return "effects";
  if (id.rfind("spectral.", 0) == 0) return "spectral";
  if (id.rfind("final.", 0) == 0) return "final";
  return "other";
}

}  // namespace

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
    const InsertTiming timing = realtime_insertable ? insert_timing(id) : InsertTiming{};
    out += "{\"id\":\"";
    out += id;
    out += "\",\"kind\":\"";
    out += kind;
    out += "\",\"realtimeInsertable\":";
    out += realtime_insertable ? "true" : "false";
    out += ",\"stereoOnly\":";
    out += stereo_set.count(id) != 0 ? "true" : "false";
    out += ",\"latencySamples\":";
    out += std::to_string(timing.latency_samples);
    out += ",\"tailSamples\":";
    out += std::to_string(timing.tail_samples);
    out += ",\"realtimeCost\":";
    if (realtime_insertable) {
      out += '"';
      out += realtime_cost(id);
      out += '"';
    } else {
      out += "null";
    }
    out += ",\"channelPolicy\":\"";
    out += channel_policy_to_string(channel_policy(id));
    out += '"';
    out += ",\"category\":\"";
    out += catalog_category(id);
    out += "\",\"params\":";
    out += realtime_insertable ? insert_param_info_json(id) : "[]";
    out += '}';
  }
  out += ']';
  return out;
}

}  // namespace sonare::mastering::api
