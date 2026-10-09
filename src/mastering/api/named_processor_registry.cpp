#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "mastering/api/insert_factory.h"
#include "mastering/api/named_processor.h"
#include "mastering/saturation/amp_presets.h"
#include "rt/processor_base.h"
#include "util/exception.h"
#include "util/json.h"

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
      "saturation.distortion",
      "saturation.exciter",
      "saturation.hardClipper",
      "saturation.multibandExciter",
      "saturation.overdrive",
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
      "stereo.binaural",
      "stereo.haasEnhancer",
      "stereo.imager",
      "stereo.monoMaker",
      "stereo.phaseAlign",
      "stereo.stereoBalance",
      "utility.gain",
  };
  // Creative streaming effects and the voice changer are not configured offline:
  // apply_named_processor dispatches every "effects." and "voice." id by building
  // the realtime insert and running it through the latency-compensating runner. The insert factory
  // is therefore the only registry of which effects ship in this build configuration, and deriving
  // the section from it keeps the two from diverging (and keeps the BUILD_FX / acoustic-simulation
  // guards in exactly one place).
  for (const std::string& name : insert_factory_names()) {
    if (name.rfind("effects.", 0) == 0 || name.rfind("voice.", 0) == 0) names.push_back(name);
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
  // Mirrors the per-process() channel-handling audit (the 7 stereo-image
  // processors, eq.midSide, multiband.imager, and every reverb/modulation/delay
  // effect).
  static const std::set<std::string> kStereoPairOnly = {
      "stereo.imager",
      "stereo.monoMaker",
      "stereo.stereoBalance",
      "stereo.haasEnhancer",
      "stereo.phaseAlign",
      "stereo.autoPan",
      "stereo.binaural",
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
      // GS EFX stages expose the same two-leg audio graph as the MIDI path;
      // on a surround strip they must receive the front stereo pair only.
      "effects.gsEfx",
      // The vowel filter allocates a stereo pair of banks, like wah.
      "effects.filter.vowel",
      "effects.delay.stereo",
      // The voice changer prepares one or two channels.
      "voice.changer",
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

InsertTiming insert_timing(const std::string& name, const std::string& json_params,
                           double sample_rate) {
  std::vector<std::string> unknown;
  std::unique_ptr<sonare::rt::ProcessorBase> processor = make_insert(name, json_params, &unknown);
  if (processor == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter, "unknown insert processor: " + name);
  }
  if (!unknown.empty()) {
    std::string keys;
    for (const std::string& key : unknown) {
      if (!keys.empty()) keys += ", ";
      keys += key;
    }
    throw SonareException(ErrorCode::InvalidParameter,
                          name + " does not read parameter(s): " + keys);
  }
  processor->prepare(sample_rate, kInsertProbeBlockSize);
  return {std::max(0, processor->latency_samples()), std::max(0, processor->tail_samples())};
}

namespace {

// Timing an insertable processor reports for its default configuration at the
// probe rate, or zeros for an id with no realtime insert or that fails to
// build/prepare. A configured instance can differ; insert_timing() answers it.
InsertTiming default_insert_timing(const std::string& id) {
  try {
    return insert_timing(id, "{}", kInsertProbeSampleRate);
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
  // Oversampled triode stages. "saturation.ampSim" runs a 4x oversampled
  // Dempwolf triode whichever head its topology selects — the embedded
  // "saturation.tube" instance under kVoiced, its own multi-stage cascade under
  // kCircuit, which is if anything the dearer of the two — and adds a tone stack
  // plus a cab EQ on top, so it can never be cheaper than a bare triode stage.
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
  // Four direct-form HRIR FIRs per sample, plus the canceller's two in speakers mode.
  if (id == "stereo.binaural") return "moderate";
  // Grain resampling per channel, a reverb tank and a 4x oversampled inter-sample-peak limiter.
  if (id == "voice.changer") return "moderate";
  // An FFT frame and a spectral mask per hop, the linear-phase EQ's class of work.
  if (id == "repair.denoiseClassical" || id == "repair.dereverbClassical") return "moderate";
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
  if (id.rfind("utility.", 0) == 0) return "utility";
  if (id.rfind("voice.", 0) == 0) return "voice";
  return "other";
}

// Whether the stage has a causal configuration: its output at a sample depends only on the
// input up to it plus the latency it reports. Declick and declip need the far side of a defect
// to reconstruct it, and a trim chooses its range from the whole signal; the other repair
// stages have a causal setting, and every insert is causal by construction.
bool catalog_causal(const std::string& id, bool realtime_insertable) {
  if (id == "repair.declick" || id == "repair.declip" || id == "repair.trimSilence") return false;
  if (id.rfind("repair.", 0) == 0) return true;
  return realtime_insertable;
}

std::string build_processor_catalog_json() {
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
    const InsertTiming timing = realtime_insertable ? default_insert_timing(id) : InsertTiming{};
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
    out += "\",\"causal\":";
    out += catalog_causal(id, realtime_insertable) ? "true" : "false";
    out += ",\"params\":";
    out += realtime_insertable ? insert_param_info_json(id) : repair_param_info_json(id);
    out += ",\"slots\":";
    out += realtime_insertable ? insert_slot_info_json(id) : "[]";
    out += '}';
  }
  out += ']';
  return out;
}

}  // namespace

std::string processor_catalog_json() {
  // A pure function of the build that costs tens of thousands of probe
  // constructions, so it is measured once per process and shared by every thread.
  static const std::string catalog = build_processor_catalog_json();
  return catalog;
}

const std::vector<std::string>& processor_catalog_schema_paths() {
  static const std::vector<std::string> paths = {
      "[].id",
      "[].kind",
      "[].realtimeInsertable",
      "[].stereoOnly",
      "[].latencySamples",
      "[].tailSamples",
      "[].realtimeCost",
      "[].channelPolicy",
      "[].category",
      "[].causal",
      "[].params",
      "[].params[].name",
      "[].params[].id",
      "[].params[].rtSafe",
      "[].params[].type",
      "[].params[].min",
      "[].params[].max",
      "[].params[].minExclusive",
      "[].params[].maxExclusive",
      "[].params[].maxRelativeTo",
      "[].params[].default",
      "[].params[].unit",
      "[].params[].uiMin",
      "[].params[].uiMax",
      "[].params[].scale",
      "[].params[].choices",
      "[].params[].choices[].name",
      "[].params[].choices[].value",
      "[].params[].slot",
      "[].params[].dependsOn",
      "[].params[].dependsOn[].key",
      "[].params[].dependsOn[].relation",
      "[].params[].dependsOn[].factor",
      "[].slots",
      "[].slots[].name",
      "[].slots[].parent",
      "[].slots[].activation",
      "[].slots[].minCrossoverCutoffs",
  };
  return paths;
}

std::string amp_preset_catalog_json() {
  namespace json = sonare::util::json;
  namespace sat = sonare::mastering::saturation;
  const std::vector<std::string> names = sat::amp_preset_names();
  json::Array catalog;
  for (size_t index = 0; index < names.size(); ++index) {
    const sat::AmpSimConfig config = sat::amp_preset_config(static_cast<sat::AmpPreset>(index));
    json::Object params;
    params["topology"] = static_cast<int>(config.topology);
    params["inputDb"] = static_cast<double>(config.input_db);
    params["drive"] = static_cast<double>(config.drive);
    params["bassDb"] = static_cast<double>(config.bass_db);
    params["midDb"] = static_cast<double>(config.mid_db);
    params["trebleDb"] = static_cast<double>(config.treble_db);
    params["presenceDb"] = static_cast<double>(config.presence_db);
    params["levelDb"] = static_cast<double>(config.level_db);
    params["cab"] = config.cab;
    params["ampModel"] = static_cast<int>(config.amp_model);
    params["cabModel"] = static_cast<int>(config.cab_model);
    params["micModel"] = static_cast<int>(config.mic_model);
    params["power"] = static_cast<double>(config.power);
    params["sag"] = static_cast<double>(config.sag);
    params["transformer"] = static_cast<double>(config.transformer);
    params["nfb"] = static_cast<double>(config.nfb);
    params["micAxis"] = static_cast<double>(config.mic_axis);
    params["micDistanceCm"] = static_cast<double>(config.mic_distance_cm);
    params["micBlend"] = static_cast<double>(config.mic_blend);
    params["micBModel"] = static_cast<int>(config.mic_b_model);
    params["micBAxis"] = static_cast<double>(config.mic_b_axis);
    params["micBDistanceCm"] = static_cast<double>(config.mic_b_distance_cm);
    params["micBInvert"] = config.mic_b_invert;
    params["cone"] = static_cast<double>(config.cone);
    params["doppler"] = static_cast<double>(config.doppler);
    params["preampStages"] = static_cast<int>(config.preamp_stages);
    params["biasShift"] = static_cast<double>(config.bias_shift);
    params["crossover"] = static_cast<double>(config.crossover);
    params["powerTube"] = static_cast<int>(config.power_tube);

    json::Object entry;
    entry["index"] = static_cast<int>(index);
    entry["name"] = names[index];
    entry["params"] = std::move(params);
    catalog.push_back(std::move(entry));
  }
  return json::dump(catalog);
}

}  // namespace sonare::mastering::api
