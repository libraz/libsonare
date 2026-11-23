#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "mastering/api/insert_factory.h"
#include "mastering/api/named_processor.h"
#include "mastering/api/param_field_tables.h"
#include "mastering/api/param_map.h"
#include "mastering/api/retired_names.h"
#include "mastering/saturation/amp_presets.h"
#include "rt/processor_base.h"
#include "util/exception.h"
#include "util/json.h"
#include "util/json_schema.h"

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
      "eq.bandPass",
      "eq.cutFilter",
      "eq.dynamic",
      "eq.equalizer",
      "eq.graphic",
      "eq.linearPhase",
      "eq.midSide",
      "eq.minimumPhase",
      "eq.parametric",
      "eq.passiveTube",
      "eq.shelving",
      "eq.steppedProportionalQ",
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

namespace {

// make_insert() refusing a key the insert does not read; nullptr for an unknown @p name.
std::unique_ptr<sonare::rt::ProcessorBase> make_insert_reading_every_key(
    const std::string& name, const std::string& json_params) {
  std::vector<std::string> unknown;
  std::unique_ptr<sonare::rt::ProcessorBase> processor = make_insert(name, json_params, &unknown);
  if (processor != nullptr) detail::refuse_unread_params(unknown, insert_param_names(name));
  return processor;
}

[[noreturn]] void refuse_part_rig_stage(const std::string& subject, const std::string& predicate) {
  throw SonareParameterRefusal::predicate(ErrorCode::InvalidParameter, subject, predicate)
      .as_caller_spelled();
}

}  // namespace

InsertTiming insert_timing(const std::string& name, const std::string& json_params,
                           double sample_rate) {
  std::unique_ptr<sonare::rt::ProcessorBase> processor =
      make_insert_reading_every_key(name, json_params);
  if (processor == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "unknown insert processor: " + name + retired_processor_note(name));
  }
  // The default width, so the timing is the one a host gets from an insert it prepares plainly.
  processor->prepare(sample_rate, kInsertProbeBlockSize);
  return {std::max(0, processor->latency_samples()), std::max(0, processor->tail_samples())};
}

void validate_part_rig_chain(const std::vector<midi::PartRigStage>& stages) {
  namespace schema = sonare::util::json::schema;
  for (std::size_t i = 0; i < stages.size(); ++i) {
    const midi::PartRigStage& stage = stages[i];
    const std::string at = schema::index_path("inserts", i);
    std::unique_ptr<sonare::rt::ProcessorBase> insert;
    try {
      insert = make_insert_reading_every_key(stage.processor, stage.params_json);
      if (insert) insert->prepare(kInsertProbeSampleRate, kInsertProbeBlockSize, 1);
    } catch (const SonareParameterRefusal& ex) {
      throw schema::relabel_under(ex, schema::child_path(at, "params"), ErrorCode::InvalidParameter)
          .as_caller_spelled();
    } catch (const SonareException& ex) {
      refuse_part_rig_stage(at, std::string("is refused: ") + ex.what());
    }
    if (!insert) {
      refuse_part_rig_stage(
          schema::child_path(at, "processor"),
          "must name an insert processor" + retired_processor_note(stage.processor));
    }
    if (insert->latency_samples_q8() > (midi::kMaxPartRigLatencySamples << 8)) {
      refuse_part_rig_stage(at, "must report a latency of at most " +
                                    std::to_string(midi::kMaxPartRigLatencySamples) + " samples");
    }
  }
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
      id == "effects.reverb.room" || id == "effects.reverb.plate" ||
      id == "effects.acoustic.roomMorph" || id == "eq.linearPhase") {
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

const std::string& catalog_description(const std::string& id) {
  static const std::map<std::string, std::string> descriptions = {
      {"dynamics.brickwallLimiter", "Keeps sample peaks at or below the configured ceiling."},
      {"dynamics.compressor", "Reduces gain above a threshold with a soft knee and makeup gain."},
      {"dynamics.deesser",
       "Uses a split-band detector to attenuate sibilant high-frequency energy."},
      {"dynamics.expander", "Reduces quieter signals with a downward expansion ratio."},
      {"dynamics.gate", "Attenuates signals below a threshold using an expander curve."},
      {"dynamics.limiter", "Uses lookahead to limit peaks above the configured threshold."},
      {"dynamics.parallelComp",
       "Blends compressed audio with the dry signal using the configured mix."},
      {"dynamics.sidechainRouter",
       "Applies linked gain reduction from the main or an external detector."},
      {"dynamics.duckingProcessor", "Ducks programme audio from a separately supplied key signal."},
      {"dynamics.transientShaper",
       "Adjusts attack and sustain separately from their envelope difference."},
      {"dynamics.upwardCompressor",
       "Raises quieter material below its threshold with bounded gain."},
      {"dynamics.upwardExpander",
       "Emphasizes material above its threshold with an upward expansion curve."},
      {"dynamics.vocalRider", "Moves signal level toward a target with bounded boost and cut."},
      {"eq.tilt",
       "Raises one side of the spectrum while lowering the other around a pivot frequency."},
      {"eq.steppedProportionalQ",
       "Uses stepped frequency and gain controls with narrower bands at larger boosts or cuts."},
      {"eq.parametric", "Applies configurable biquad bands using RBJ or matched-Z coefficients."},
      {"eq.minimumPhase", "Applies a low-latency IIR equalizer with a matched-Z phase response."},
      {"eq.linearPhase", "Applies an FFT-designed FIR equalizer with linear phase."},
      {"eq.dynamic",
       "Changes an equalizer band's gain when its level detector crosses the threshold."},
      {"eq.equalizer",
       "Applies a configurable collection of equalizer sections through one processor."},
      {"eq.passiveTube",
       "Applies a passive-style program equalizer with low and high section controls."},
      {"eq.cutFilter",
       "Attenuates frequencies below a high-pass cutoff or above a low-pass cutoff."},
      {"eq.bandPass", "Passes a selected frequency band and attenuates frequencies on both sides."},
      {"eq.shelving", "Applies a low- or high-shelf filter at a selected corner frequency."},
      {"eq.graphic", "Adjusts gain across 31 fixed frequency bands."},
      {"eq.midSide",
       "Applies independent parametric equalizer settings to the mid and side signals."},
      {"final.bitDepth",
       "Quantizes samples to an integer grid at the configured output bit depth."},
      {"final.dither", "Adds the selected dither before quantizing to the configured bit depth."},
      {"final.outputChain",
       "Either dithers the signal or quantizes it directly to the configured bit depth."},
      {"maximizer.adaptiveRelease",
       "Sets limiter release from the input envelope and configured recovery range."},
      {"maximizer.loudnessOptimize",
       "Adjusts integrated loudness toward a target while limiting true peaks."},
      {"maximizer.maximizer",
       "Raises programme level and limits its peaks to the configured ceiling."},
      {"maximizer.softKneeMax", "Limits peaks above a threshold with a soft-knee gain curve."},
      {"maximizer.truePeakLimiter",
       "Oversamples the signal to limit reconstructed peaks at the dBTP ceiling."},
      {"multiband.compressor",
       "Compresses each crossover band with its own threshold and timing controls."},
      {"multiband.expander", "Expands each crossover band independently below its threshold."},
      {"multiband.limiter", "Limits peaks independently in each crossover band."},
      {"multiband.imager",
       "Adjusts stereo width separately across crossover bands using mid and side levels."},
      {"multiband.saturation",
       "Applies the selected saturation curve independently in each crossover band."},
      {"multiband.dynamicEq",
       "Applies threshold-controlled equalization independently in each crossover band."},
      {"repair.declick",
       "Interpolates short click runs using neighboring samples and an LPC model."},
      {"repair.declip",
       "Reconstructs clipped runs from the surrounding waveform within solver limits."},
      {"repair.decrackle",
       "Suppresses short crackle transients with the selected median or wavelet method."},
      {"repair.dehum", "Attenuates a configured or tracked fundamental and its harmonic series."},
      {"repair.denoiseClassical",
       "Estimates a noise spectrum and reduces it with a frequency-dependent gain mask."},
      {"repair.dereverbClassical",
       "Estimates late reverberation from spectral decay and suppresses its contribution."},
      {"repair.trimSilence",
       "Finds and removes low-level leading and trailing audio using a gated level estimate."},
      {"saturation.bitcrusher",
       "Quantizes sample amplitude and holds samples at a lower rate, with optional filters and "
       "noise."},
      {"saturation.distortion",
       "Runs two hard-clipping stages with coupling and tone filters between the gain controls."},
      {"saturation.exciter",
       "Generates harmonics in a selected band and blends them with the direct signal."},
      {"saturation.hardClipper",
       "Clamps samples at the selected ceiling with a hard clipping curve."},
      {"saturation.multibandExciter",
       "Generates harmonics independently in each configured frequency band."},
      {"saturation.overdrive", "Blends a filtered, soft-clipped path with the direct signal."},
      {"saturation.ampSim",
       "Runs the selected guitar amplifier model with its tone stack and cabinet response."},
      {"saturation.softClipper", "Rounds waveform peaks with a smooth saturation curve."},
      {"saturation.tape", "Models tape saturation with a Jiles-Atherton hysteresis loop."},
      {"saturation.transformer",
       "Models transformer magnetization with configurable drive and asymmetry."},
      {"saturation.tube", "Models a tube stage with configurable drive, bias, and harmonic level."},
      {"saturation.waveshaper",
       "Maps each sample through a selected static nonlinear transfer curve."},
      {"spectral.airBand",
       "Adds a dynamic high-shelf boost and blends in high-frequency harmonics."},
      {"spectral.lowEndFocus",
       "Shapes low-frequency energy and stereo width below a crossover frequency."},
      {"spectral.presenceEnhancer", "Adds driven harmonics inside a configurable presence band."},
      {"spectral.spectralShaper",
       "Changes gain in a selected spectral band according to its envelope."},
      {"stereo.autoPan", "Sweeps the stereo image left and right with a low-frequency oscillator."},
      {"stereo.binaural",
       "Places a folded mono source on the horizontal plane using measured head-related "
       "responses."},
      {"stereo.haasEnhancer", "Widens a stereo signal by delaying one side relative to the other."},
      {"stereo.imager", "Changes stereo width by adjusting the mid-to-side balance."},
      {"stereo.monoMaker",
       "Reduces stereo width below a crossover by bringing low frequencies toward mono."},
      {"stereo.phaseAlign",
       "Delays one stereo channel by the configured whole and fractional sample offset."},
      {"stereo.stereoBalance", "Changes left and right gain with a selectable balance law."},
      {"utility.gain", "Applies a constant gain to every sample."},
      {"match.applyMatchEq",
       "Applies an equalizer curve derived from the reference signal to the source."},
      {"match.alignReferenceToSource",
       "Shifts the reference by its estimated delay relative to the source."},
      {"match.abSwitch", "Selects either the source or reference signal for A/B comparison."},
      {"match.abCrossfade", "Crossfades between equal-length source and reference signals."},
      {"effects.gsEfx", "Applies the selected GS insert effect to a stereo signal."},
      {"effects.reverb.plate",
       "Uses a modulated Dattorro plate tank with input diffusion and a wet-output gate."},
      {"effects.reverb.fdn",
       "Uses a four-line feedback delay network with frequency-dependent decay."},
      {"effects.reverb.velvet",
       "Builds a sparse velvet-noise impulse response with a configurable decay time."},
      {"effects.reverb.convolution",
       "Convolves each channel with a supplied or synthesized impulse response."},
      {"effects.reverb.room",
       "Synthesizes a room impulse response from room geometry and material settings."},
      {"effects.acoustic.roomMorph",
       "Morphs room response as its acoustic geometry and surface materials change."},
      {"effects.filter.vowel",
       "Moves three resonant formant bands to shape the signal into vowel sounds."},
      {"effects.modulation.chorus",
       "Mixes modulated fractional-delay voices with the stereo input."},
      {"effects.modulation.ensemble",
       "Combines three phase-offset delay taps for a string-machine ensemble sound."},
      {"effects.modulation.flanger",
       "Feeds a modulated delay back into the input to create moving comb notches."},
      {"effects.modulation.phaser",
       "Sweeps first-order allpass stages to create moving spectral notches."},
      {"effects.modulation.wah", "Sweeps a resonant filter with a low-frequency oscillator."},
      {"effects.modulation.autoWah", "Sweeps a resonant filter from the input envelope."},
      {"effects.modulation.rotary",
       "Simulates a rotating speaker with dual-rotor Doppler shift and tremolo."},
      {"effects.modulation.ringModulator",
       "Multiplies the input by a sinusoidal carrier to create sidebands."},
      {"effects.modulation.pitchShifter",
       "Shifts pitch in real time with two crossfaded delay taps."},
      {"effects.delay.stereo",
       "Feeds independent stereo delay lines with selectable cross-routing and four output taps."},
      {"voice.changer",
       "Changes vocal pitch and formants using the selected realtime voice configuration."},
  };
  return descriptions.at(id);
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

// A parameter list as the set of declarations it makes, without the automation id and realtime
// safety only an insert form carries.
std::set<std::string> declared_surface(const std::string& info_json) {
  namespace json = sonare::util::json;
  std::set<std::string> surface;
  const json::Value parsed = json::parse(info_json);
  for (json::Value entry : parsed.as_array()) {
    entry.as_object().erase("id");
    entry.as_object().erase("rtSafe");
    surface.insert(json::dump(entry));
  }
  return surface;
}

// The offline declaration of dual-form @p id when it is not the one @p params describes, else
// empty.
std::string separate_offline_params(const std::string& id, const std::string& params) {
  std::string offline = offline_param_info_json(id);
  if (offline == "[]" || declared_surface(offline) == declared_surface(params)) return {};
  return offline;
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
  // The ids apply_named_processor() and apply_named_pair_processor() run on a whole buffer.
  const std::set<std::string> offline_set = [&pair_set] {
    const auto names = processor_names();
    std::set<std::string> set(names.begin(), names.end());
    set.insert(pair_set.begin(), pair_set.end());
    return set;
  }();

  // Sorted union of every id the host might surface, so realtime-only ids (e.g.
  // effects.reverb.room) and pair ids absent from processor_names() are covered.
  std::set<std::string> ids = offline_set;
  ids.insert(insert_set.begin(), insert_set.end());

  std::string out = "[";
  bool first = true;
  for (const std::string& id : ids) {
    if (!first) out += ',';
    first = false;
    const bool realtime_insertable = insert_set.count(id) != 0;
    const bool is_pair = pair_set.count(id) != 0;
    const char* kind = is_pair ? "pair" : (realtime_insertable ? "realtime" : "offline");
    const InsertTiming timing = realtime_insertable ? default_insert_timing(id) : InsertTiming{};
    const bool runs_offline = offline_set.count(id) != 0;
    const std::string params = insert_param_info_json(id);
    const std::string offline_params =
        realtime_insertable && runs_offline ? separate_offline_params(id, params) : std::string();
    out += "{\"id\":\"";
    out += id;
    out += "\",\"description\":";
    out += sonare::util::json::dump(sonare::util::json::Value(catalog_description(id)));
    out += ",\"kind\":\"";
    out += kind;
    out += "\",\"realtimeInsertable\":";
    out += realtime_insertable ? "true" : "false";
    out += ",\"forms\":[";
    if (realtime_insertable) out += "\"realtime\"";
    if (runs_offline) out += realtime_insertable ? ",\"offline\"" : "\"offline\"";
    out += ']';
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
    out += params;
    if (!offline_params.empty()) {
      out += ",\"offlineParams\":";
      out += offline_params;
    }
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
      "[].description",
      "[].kind",
      "[].realtimeInsertable",
      "[].forms",
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
      "[].params[].domainMin",
      "[].params[].domainMinExclusive",
      "[].params[].domainMax",
      "[].params[].domainMaxExclusive",
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
      "[].params[].coupledWith",
      "[].offlineParams",
      "[].offlineParams[].name",
      "[].offlineParams[].id",
      "[].offlineParams[].rtSafe",
      "[].offlineParams[].type",
      "[].offlineParams[].min",
      "[].offlineParams[].max",
      "[].offlineParams[].minExclusive",
      "[].offlineParams[].maxExclusive",
      "[].offlineParams[].maxRelativeTo",
      "[].offlineParams[].default",
      "[].offlineParams[].unit",
      "[].offlineParams[].uiMin",
      "[].offlineParams[].uiMax",
      "[].offlineParams[].scale",
      "[].offlineParams[].choices",
      "[].offlineParams[].choices[].name",
      "[].offlineParams[].choices[].value",
      "[].offlineParams[].slot",
      "[].offlineParams[].dependsOn",
      "[].offlineParams[].dependsOn[].key",
      "[].offlineParams[].dependsOn[].relation",
      "[].offlineParams[].dependsOn[].factor",
      "[].offlineParams[].coupledWith",
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
    detail::put_enum(params, "topology", config.topology);
    params["inputDb"] = static_cast<double>(config.input_db);
    params["drive"] = static_cast<double>(config.drive);
    params["bassDb"] = static_cast<double>(config.bass_db);
    params["midDb"] = static_cast<double>(config.mid_db);
    params["trebleDb"] = static_cast<double>(config.treble_db);
    params["presenceDb"] = static_cast<double>(config.presence_db);
    params["levelDb"] = static_cast<double>(config.level_db);
    params["cab"] = config.cab;
    detail::put_enum(params, "ampModel", config.amp_model);
    detail::put_enum(params, "cabModel", config.cab_model);
    detail::put_enum(params, "micModel", config.mic_model);
    params["power"] = static_cast<double>(config.power);
    params["sag"] = static_cast<double>(config.sag);
    params["transformer"] = static_cast<double>(config.transformer);
    params["nfb"] = static_cast<double>(config.nfb);
    params["micAxis"] = static_cast<double>(config.mic_axis);
    params["micDistanceCm"] = static_cast<double>(config.mic_distance_cm);
    params["micBlend"] = static_cast<double>(config.mic_blend);
    detail::put_enum(params, "micBModel", config.mic_b_model);
    params["micBAxis"] = static_cast<double>(config.mic_b_axis);
    params["micBDistanceCm"] = static_cast<double>(config.mic_b_distance_cm);
    params["micBInvert"] = config.mic_b_invert;
    params["cone"] = static_cast<double>(config.cone);
    params["doppler"] = static_cast<double>(config.doppler);
    params["preampStages"] = static_cast<int>(config.preamp_stages);
    params["biasShift"] = static_cast<double>(config.bias_shift);
    params["crossover"] = static_cast<double>(config.crossover);
    detail::put_enum(params, "powerTube", config.power_tube);

    json::Object entry;
    entry["index"] = static_cast<int>(index);
    entry["name"] = names[index];
    entry["params"] = std::move(params);
    catalog.push_back(std::move(entry));
  }
  return json::dump(catalog);
}

}  // namespace sonare::mastering::api
