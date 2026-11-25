#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "c_api/daw_config_domains.h"
#include "core/audio.h"
#include "editing/voice_changer/realtime.h"
#include "mastering/api/chain.h"
#include "mastering/eq/eq_band.h"
#include "mastering/eq/equalizer.h"
#include "mastering/eq/spectrum_engine.h"
#include "mastering/match/match_eq.h"
#include "mastering/match/reference_spectrum.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_streaming.h"
#include "sonare_wrap_utils.h"

namespace sonare_node {

namespace {

std::string JsonTextFromJs(const Napi::Value& value) {
  if (value.IsUndefined() || value.IsNull()) return "neutralMonitor";
  if (value.IsString()) return node_narrow_string(value.Env(), value, "config");
  Napi::Env env = value.Env();
  Napi::Object json = env.Global().Get("JSON").As<Napi::Object>();
  Napi::Function stringify = json.Get("stringify").As<Napi::Function>();
  return node_narrow_string(env, stringify.Call(json, {value}), "config");
}

/// @brief The flat POD is recognised by its `retunePitchSemitones` key.
bool IsFlatPodConfig(const Napi::Value& value) {
  return value.IsObject() && !value.As<Napi::Object>().Get("retunePitchSemitones").IsUndefined();
}

/// @brief The one place a refused preset-or-config document is coded.
/// @details A preset id or nested document is InvalidFormat; the flat POD (recognised by its
///   `retunePitchSemitones` key) is a per-call parameter bag and stays InvalidParameter.
sonare::editing::voice_changer::RealtimeVoiceChangerConfig ConfigFromJs(const Napi::Value& value) {
  sonare::editing::voice_changer::RealtimeVoiceChangerConfig config;
  std::string error;
  if (!sonare::editing::voice_changer::realtime_voice_changer_config_from_input(
          JsonTextFromJs(value), &config, &error)) {
    throw sonare::SonareException(IsFlatPodConfig(value) ? sonare::ErrorCode::InvalidParameter
                                                         : sonare::ErrorCode::InvalidFormat,
                                  error);
  }
  return config;
}

/// @brief The spelling a live-config refusal names a field in: the caller's flat POD keys,
///   otherwise the preset document's, which a preset id stands for.
sonare::editing::voice_changer::ConfigFieldSpelling ConfigFieldSpellingOf(
    const Napi::Value& value) {
  using sonare::editing::voice_changer::ConfigFieldSpelling;
  return IsFlatPodConfig(value) ? ConfigFieldSpelling::FlatPod
                                : ConfigFieldSpelling::PresetDocument;
}

}  // namespace

Napi::Object RealtimeVoiceChangerWrap::Init(Napi::Env env, Napi::Object exports) {
  Napi::Function func = DefineClass(
      env, "RealtimeVoiceChanger",
      {
          InstanceMethod<&RealtimeVoiceChangerWrap::Prepare>("prepare", napi_default_method),
          InstanceMethod<&RealtimeVoiceChangerWrap::Reset>("reset", napi_default_method),
          InstanceMethod<&RealtimeVoiceChangerWrap::SetConfig>("setConfig", napi_default_method),
          InstanceMethod<&RealtimeVoiceChangerWrap::Config>("config", napi_default_method),
          InstanceMethod<&RealtimeVoiceChangerWrap::ConfigJson>("configJson", napi_default_method),
          InstanceMethod<&RealtimeVoiceChangerWrap::LatencySamples>("latencySamples",
                                                                    napi_default_method),
          InstanceMethod<&RealtimeVoiceChangerWrap::MaxBlockSize>("maxBlockSize",
                                                                  napi_default_method),
          InstanceMethod<&RealtimeVoiceChangerWrap::NonFiniteDiscardCount>("nonFiniteDiscardCount",
                                                                           napi_default_method),
          InstanceMethod<&RealtimeVoiceChangerWrap::ProcessMono>("processMono",
                                                                 napi_default_method),
          InstanceMethod<&RealtimeVoiceChangerWrap::ProcessMonoInto>("processMonoInto",
                                                                     napi_default_method),
          InstanceMethod<&RealtimeVoiceChangerWrap::ProcessInterleaved>("processInterleaved",
                                                                        napi_default_method),
          InstanceMethod<&RealtimeVoiceChangerWrap::ProcessInterleavedInto>(
              "processInterleavedInto", napi_default_method),
          InstanceMethod<&RealtimeVoiceChangerWrap::ProcessPlanarStereo>("processPlanarStereo",
                                                                         napi_default_method),
          InstanceMethod<&RealtimeVoiceChangerWrap::NoteOn>("noteOn", napi_default_method),
          InstanceMethod<&RealtimeVoiceChangerWrap::NoteOff>("noteOff", napi_default_method),
          InstanceMethod<&RealtimeVoiceChangerWrap::AllNotesOff>("allNotesOff",
                                                                 napi_default_method),
          InstanceMethod<&RealtimeVoiceChangerWrap::Destroy>("destroy", napi_default_method),
      });

  exports.Set("RealtimeVoiceChanger", func);
  return exports;
}

RealtimeVoiceChangerWrap::RealtimeVoiceChangerWrap(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<RealtimeVoiceChangerWrap>(info) {
  Napi::Env env = info.Env();
  try {
    changer_ = std::make_unique<sonare::editing::voice_changer::RealtimeVoiceChanger>();
    changer_->set_config(info.Length() >= 1 ? ConfigFromJs(info[0]) : ConfigFromJs(env.Null()));
  } catch (...) {
    sonare_node::ThrowCurrentException(env);
  }
}

RealtimeVoiceChangerWrap::~RealtimeVoiceChangerWrap() { ReleaseNative(); }

bool RealtimeVoiceChangerWrap::EnsureAlive(Napi::Env env) const {
  if (changer_) return true;
  sonare_node::ThrowHandleClosed(env, "RealtimeVoiceChanger");
  return false;
}

void RealtimeVoiceChangerWrap::ReleaseNative() noexcept {
  changer_.reset();
  prepared_ = false;
  max_block_size_ = 0;
  channels_ = 0;
  std::vector<float>().swap(planar_scratch_);
  std::vector<float*>().swap(planar_ptrs_);
}

Napi::Value RealtimeVoiceChangerWrap::Destroy(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  ReleaseNative();
  return info.Env().Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeVoiceChangerWrap::Prepare(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!EnsureAlive(env)) return env.Undefined();
  if (!info[0].IsNumber()) {
    Napi::TypeError::New(env, "sampleRate must be an integer").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  const double sample_rate = info[0].As<Napi::Number>().DoubleValue();
  int max_block_size{};
  if (!OptionalDomainArg<int>(env, info, 1, "maxBlockSize",
                              sonare::editing::voice_changer::kRealtimeVoiceChangerDefaultMaxBlock,
                              sonare_c_detail::kVoiceChangerBlockSizeDomain, &max_block_size)) {
    return env.Undefined();
  }
  int channels{};
  if (!RequiredDomainArg<int>(env, info, 2, "numChannels",
                              sonare_c_detail::kVoiceChangerChannelsDomain, &channels)) {
    return env.Undefined();
  }
  changer_->prepare(sample_rate, max_block_size, channels);
  prepared_ = true;
  max_block_size_ = max_block_size;
  channels_ = channels;
  // RT-safe planar scratch + pointer table. Allocated once per prepare(); the
  // process methods only deinterleave into this buffer without further
  // allocations.
  planar_scratch_.assign(static_cast<size_t>(channels_) * static_cast<size_t>(max_block_size_),
                         0.0f);
  planar_ptrs_.assign(static_cast<size_t>(channels_), nullptr);
  for (int ch = 0; ch < channels_; ++ch) {
    planar_ptrs_[static_cast<size_t>(ch)] =
        planar_scratch_.data() + static_cast<size_t>(ch) * static_cast<size_t>(max_block_size_);
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeVoiceChangerWrap::Reset(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!EnsureAlive(env)) return env.Undefined();
  SONARE_NODE_TRY
  changer_->reset();
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeVoiceChangerWrap::SetConfig(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!EnsureAlive(env)) return env.Undefined();
  if (info.Length() < 1) {
    Napi::TypeError::New(env, "config must be a preset id or a config object")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  changer_->set_config(ConfigFromJs(info[0]), ConfigFieldSpellingOf(info[0]));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Object RealtimeVoiceChangerConfigObject(
    Napi::Env env, const sonare::editing::voice_changer::RealtimeVoiceChangerConfig& config) {
  namespace vc = sonare::editing::voice_changer;
  Napi::Object out = Napi::Object::New(env);
  const auto number = [&env](auto value) { return Napi::Number::New(env, value); };
  const auto name = [&env](const char* value) { return Napi::String::New(env, value); };
  const auto flag = [&env](bool value) { return Napi::Boolean::New(env, value); };
  out.Set("inputGainDb", number(config.input_gain_db));
  out.Set("outputGainDb", number(config.output_gain_db));
  out.Set("wetMix", number(config.wet_mix));
  out.Set("retunePitchSemitones", number(config.retune.semitones));
  out.Set("retuneMix", number(config.retune.mix));
  // The automatic grain is the absent key.
  if (config.retune.grain_size != 0) {
    out.Set("retuneGrainSizeSamples", number(config.retune.grain_size));
  }
  out.Set("formantFactor", number(config.formant.factor));
  out.Set("formantAmount", number(config.formant.amount));
  out.Set("formantBody", number(config.formant.body));
  out.Set("formantBrightness", number(config.formant.brightness));
  out.Set("formantNasal", number(config.formant.nasal));
  out.Set("formantMode", name(vc::formant_mode_name(config.formant_mode)));
  out.Set("eqHighPassHz", number(config.eq.highpass_hz));
  out.Set("eqBodyDb", number(config.eq.body_db));
  out.Set("eqPresenceDb", number(config.eq.presence_db));
  out.Set("eqAirDb", number(config.eq.air_db));
  out.Set("gateThresholdDb", number(config.gate.threshold_db));
  out.Set("gateAttackMs", number(config.gate.attack_ms));
  out.Set("gateReleaseMs", number(config.gate.release_ms));
  out.Set("gateRangeDb", number(config.gate.range_db));
  out.Set("compressorThresholdDb", number(config.compressor.threshold_db));
  out.Set("compressorRatio", number(config.compressor.ratio));
  out.Set("compressorAttackMs", number(config.compressor.attack_ms));
  out.Set("compressorReleaseMs", number(config.compressor.release_ms));
  out.Set("compressorMakeupGainDb", number(config.compressor.makeup_gain_db));
  out.Set("deesserFrequencyHz", number(config.deesser.frequency_hz));
  out.Set("deesserThresholdDb", number(config.deesser.threshold_db));
  out.Set("deesserRatio", number(config.deesser.ratio));
  out.Set("deesserRangeDb", number(config.deesser.range_db));
  out.Set("reverbMix", number(config.reverb.mix));
  out.Set("reverbTimeMs", number(config.reverb.time_ms));
  out.Set("reverbDamping", number(config.reverb.damping));
  out.Set("reverbSeed", number(config.reverb.seed));
  out.Set("limiterCeilingDb", number(config.limiter.ceiling_db));
  out.Set("limiterReleaseMs", number(config.limiter.release_ms));
  out.Set("limiterEnableIspLimiter", flag(config.limiter.enable_isp_limiter));
  out.Set("limiterIspCeilingDbtp", number(config.limiter.isp_ceiling_dbtp));
  // Enums are the lowerCamelCase spellings the flat-config parser reads.
  static constexpr const char* kEngines[] = {"grain", "psola"};
  static constexpr const char* kCorrectionModes[] = {"off", "auto", "robot"};
  static constexpr const char* kScales[] = {"major", "naturalMinor", "chromatic"};
  static constexpr const char* kCarrierSources[] = {"internal", "external"};
  static constexpr const char* kCarrierWaves[] = {"saw", "pulse", "ensemble"};
  static constexpr const char* kCharacters[] = {"none", "megaphone", "radio", "bbdChorus",
                                                "strobo"};
  static constexpr const char* kVariations[] = {"reverb", "echo", "tempoDelay", "dubEcho"};
  static constexpr const char* kDivisions[] = {"quarter", "eighth", "dottedEighth", "sixteenth"};
  static constexpr const char* kMidiReleases[] = {"hold", "input"};
  // An ordinal past a table is a mirror that has drifted from the core, refused rather than read.
  const auto enum_name = [&env, &name](const char* const* names, std::size_t count, auto value,
                                       const char* noun) {
    const auto ordinal = static_cast<std::size_t>(value);
    if (ordinal >= count) {
      throw sonare_node::MakeSonareError(
          env, SONARE_ERROR_INVALID_FORMAT,
          std::string("unknown ") + noun + " ordinal: " + std::to_string(ordinal));
    }
    return name(names[ordinal]);
  };
  out.Set("voiceEngine",
          enum_name(kEngines, std::size(kEngines), config.voice_engine.engine, "voice engine"));
  out.Set("voiceEngineF0MinHz", number(config.voice_engine.f0_min_hz));
  out.Set("pitchCorrectionMode", enum_name(kCorrectionModes, std::size(kCorrectionModes),
                                           config.pitch_correction.mode, "pitch correction mode"));
  out.Set("pitchCorrectionAmount", number(config.pitch_correction.amount));
  out.Set("pitchCorrectionSpeedMs", number(config.pitch_correction.speed_ms));
  out.Set("pitchCorrectionKeyRoot", number(config.pitch_correction.key_root));
  out.Set("pitchCorrectionScale",
          enum_name(kScales, std::size(kScales), config.pitch_correction.scale,
                    "pitch correction scale"));
  out.Set("pitchCorrectionRobotOctave", number(config.pitch_correction.robot_octave));
  out.Set("harmonyVoices", number(config.harmony.voices));
  Napi::Array harmony_steps = Napi::Array::New(env, vc::kMaxHarmonyVoices);
  for (uint32_t i = 0; i < static_cast<uint32_t>(vc::kMaxHarmonyVoices); ++i) {
    harmony_steps.Set(i, number(config.harmony.steps[i]));
  }
  out.Set("harmonySteps", harmony_steps);
  out.Set("harmonyLevel", number(config.harmony.level));
  out.Set("harmonyLeadLevel", number(config.harmony.lead_level));
  out.Set("harmonyDetuneCents", number(config.harmony.detune_cents));
  out.Set("harmonyDelayMs", number(config.harmony.delay_ms));
  out.Set("vocoderEnabled", flag(config.vocoder.enabled));
  out.Set("vocoderBands", number(config.vocoder.bands));
  out.Set("vocoderAttackMs", number(config.vocoder.attack_ms));
  out.Set("vocoderReleaseMs", number(config.vocoder.release_ms));
  out.Set("vocoderSibilance", number(config.vocoder.sibilance));
  out.Set("vocoderCarrierSource",
          enum_name(kCarrierSources, std::size(kCarrierSources), config.vocoder.carrier_source,
                    "vocoder carrier source"));
  out.Set("vocoderCarrierWave", enum_name(kCarrierWaves, std::size(kCarrierWaves),
                                          config.vocoder.carrier_wave, "vocoder carrier wave"));
  out.Set("vocoderEnsembleDetuneCents", number(config.vocoder.ensemble_detune_cents));
  out.Set("vocoderLevel", number(config.vocoder.level));
  out.Set("characterType",
          enum_name(kCharacters, std::size(kCharacters), config.character.type, "character type"));
  out.Set("characterAmount", number(config.character.amount));
  out.Set("characterTone", number(config.character.tone));
  out.Set("reverbVariation", enum_name(kVariations, std::size(kVariations),
                                       config.reverb_variation.variation, "reverb variation"));
  out.Set("reverbVariationDelayMs", number(config.reverb_variation.delay_ms));
  out.Set("reverbVariationFeedback", number(config.reverb_variation.feedback));
  out.Set("reverbVariationTempoBpm", number(config.reverb_variation.tempo_bpm));
  out.Set("reverbVariationDivision",
          enum_name(kDivisions, std::size(kDivisions), config.reverb_variation.division,
                    "reverb variation division"));
  out.Set("midiRelease",
          enum_name(kMidiReleases, std::size(kMidiReleases), config.midi.release, "midi release"));
  return out;
}

Napi::Value RealtimeVoiceChangerWrap::Config(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!EnsureAlive(env)) return env.Undefined();
  return RealtimeVoiceChangerConfigObject(env, changer_->config());
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeVoiceChangerWrap::ConfigJson(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!EnsureAlive(info.Env())) return info.Env().Undefined();
  return Napi::String::New(
      info.Env(),
      sonare::editing::voice_changer::realtime_voice_changer_config_to_json(changer_->config()));
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeVoiceChangerWrap::LatencySamples(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!EnsureAlive(info.Env())) return info.Env().Undefined();
  return Napi::Number::New(info.Env(), changer_->latency_samples());
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeVoiceChangerWrap::MaxBlockSize(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!EnsureAlive(env)) return env.Undefined();
  return Napi::Number::New(env, changer_->max_block_size());
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeVoiceChangerWrap::NonFiniteDiscardCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!EnsureAlive(info.Env())) return info.Env().Undefined();
  return Napi::Number::New(info.Env(), changer_->non_finite_discard_count());
  SONARE_NODE_CATCH(env)
}

namespace {

constexpr const char* kNotPreparedMessage =
    "RealtimeVoiceChanger must be prepared before processing";
constexpr const char* kNoteRefusedMessage =
    "the note was not accepted: the changer is unprepared, uses the grain engine, or its note "
    "queue is full";
constexpr const char* kCarrierRequiredMessage =
    "the vocoder reads an external carrier: pass a carrier to the process call";

bool IsGiven(const Napi::Value& value) { return !value.IsUndefined() && !value.IsNull(); }

}  // namespace

Napi::Value RealtimeVoiceChangerWrap::ProcessMonoImpl(const Napi::CallbackInfo& info, bool into) {
  Napi::Env env = info.Env();
  if (!EnsureAlive(env)) return env.Undefined();
  if (!prepared_) {
    sonare_node::ThrowSonareErrorMessage(env, SONARE_ERROR_INVALID_STATE, kNotPreparedMessage);
    return env.Undefined();
  }
  // (samples, carrier?) or (samples, output, carrier?)
  const size_t output_index = 1;
  const size_t carrier_index = into ? 2 : 1;
  const bool with_carrier = IsGiven(info[carrier_index]);
  if (!IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "samples must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (with_carrier && !IsFloat32Array(info[carrier_index])) {
    Napi::TypeError::New(env, "carrier must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (into && !IsFloat32Array(info[output_index])) {
    Napi::TypeError::New(env, "output must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  Napi::Float32Array input = info[0].As<Napi::Float32Array>();
  if (with_carrier &&
      info[carrier_index].As<Napi::Float32Array>().ElementLength() != input.ElementLength()) {
    Napi::RangeError::New(env, "carrier must have the same length as samples")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (into &&
      info[output_index].As<Napi::Float32Array>().ElementLength() != input.ElementLength()) {
    Napi::RangeError::New(env, "output must have the same length as samples")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (input.ElementLength() > static_cast<size_t>(max_block_size_)) {
    Napi::RangeError::New(env, "block exceeds maxBlockSize").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Float32Array output = into ? info[output_index].As<Napi::Float32Array>()
                                   : Napi::Float32Array::New(env, input.ElementLength());
  const int frames = static_cast<int>(input.ElementLength());
  const bool ok = with_carrier
                      ? changer_->process_block(
                            sonare_node::ElementData(input),
                            sonare_node::ElementData(info[carrier_index].As<Napi::Float32Array>()),
                            sonare_node::ElementData(output), frames)
                      : changer_->process_block(sonare_node::ElementData(input),
                                                sonare_node::ElementData(output), frames);
  if (!ok) {
    sonare_node::ThrowSonareErrorMessage(env, SONARE_ERROR_INVALID_STATE, kCarrierRequiredMessage);
    return env.Undefined();
  }
  return into ? env.Undefined() : static_cast<Napi::Value>(output);
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeVoiceChangerWrap::ProcessInterleavedImpl(const Napi::CallbackInfo& info,
                                                             bool into) {
  Napi::Env env = info.Env();
  if (!EnsureAlive(env)) return env.Undefined();
  if (!prepared_) {
    sonare_node::ThrowSonareErrorMessage(env, SONARE_ERROR_INVALID_STATE, kNotPreparedMessage);
    return env.Undefined();
  }
  // (samples, channels, carrier?) or (samples, channels, output, carrier?)
  const size_t channels_index = 1;
  const size_t output_index = 2;
  const size_t carrier_index = into ? 3 : 2;
  const bool with_carrier = IsGiven(info[carrier_index]);
  if (!IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "samples must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (with_carrier && !IsFloat32Array(info[carrier_index])) {
    Napi::TypeError::New(env, "carrier must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[channels_index].IsNumber()) {
    Napi::TypeError::New(env, "channels must be an integer").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (into && !IsFloat32Array(info[output_index])) {
    Napi::TypeError::New(env, "output must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  Napi::Float32Array input = info[0].As<Napi::Float32Array>();
  const int channels = sonare_node::node_narrow_int(env, info[channels_index], "channels");
  if (into &&
      info[output_index].As<Napi::Float32Array>().ElementLength() != input.ElementLength()) {
    Napi::RangeError::New(env, "output must have the same length as input")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (channels < 1 || channels > channels_ ||
      input.ElementLength() % static_cast<size_t>(channels) != 0) {
    Napi::RangeError::New(env, "invalid channel count").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const size_t frames = input.ElementLength() / static_cast<size_t>(channels);
  if (frames > static_cast<size_t>(max_block_size_)) {
    Napi::RangeError::New(env, "block exceeds maxBlockSize").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (with_carrier && info[carrier_index].As<Napi::Float32Array>().ElementLength() != frames) {
    Napi::RangeError::New(env, "carrier length must equal the number of frames")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  // RT-safe path: reuse the per-instance planar scratch (allocated in Prepare) and pointer
  // table. Deinterleave -> process_block -> interleave; no per-call scratch allocation.
  const size_t stride = static_cast<size_t>(max_block_size_);
  const float* in_data = sonare_node::ElementData(input);
  for (int ch = 0; ch < channels; ++ch) {
    float* dst = planar_scratch_.data() + static_cast<size_t>(ch) * stride;
    for (size_t i = 0; i < frames; ++i) {
      dst[i] = in_data[i * static_cast<size_t>(channels) + ch];
    }
  }
  const bool ok =
      with_carrier
          ? changer_->process_block(
                planar_ptrs_.data(), channels,
                sonare_node::ElementData(info[carrier_index].As<Napi::Float32Array>()),
                static_cast<int>(frames))
          : changer_->process_block(planar_ptrs_.data(), channels, static_cast<int>(frames));
  if (!ok) {
    sonare_node::ThrowSonareErrorMessage(env, SONARE_ERROR_INVALID_STATE, kCarrierRequiredMessage);
    return env.Undefined();
  }
  Napi::Float32Array output = into ? info[output_index].As<Napi::Float32Array>()
                                   : Napi::Float32Array::New(env, input.ElementLength());
  float* out_data = sonare_node::ElementData(output);
  for (int ch = 0; ch < channels; ++ch) {
    const float* src = planar_scratch_.data() + static_cast<size_t>(ch) * stride;
    for (size_t i = 0; i < frames; ++i) {
      out_data[i * static_cast<size_t>(channels) + ch] = src[i];
    }
  }
  return into ? env.Undefined() : static_cast<Napi::Value>(output);
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeVoiceChangerWrap::ProcessPlanarStereoImpl(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const bool with_carrier = IsGiven(info[2]);
  if (!EnsureAlive(env)) return env.Undefined();
  if (!prepared_) {
    sonare_node::ThrowSonareErrorMessage(env, SONARE_ERROR_INVALID_STATE, kNotPreparedMessage);
    return env.Undefined();
  }
  if (!IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "left must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!IsFloat32Array(info[1])) {
    Napi::TypeError::New(env, "right must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (with_carrier && !IsFloat32Array(info[2])) {
    Napi::TypeError::New(env, "carrier must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (channels_ < 2) {
    sonare_node::ThrowSonareErrorMessage(
        env, SONARE_ERROR_INVALID_STATE,
        "RealtimeVoiceChanger must be prepared with at least 2 channels");
    return env.Undefined();
  }
  SONARE_NODE_TRY
  Napi::Float32Array left = info[0].As<Napi::Float32Array>();
  Napi::Float32Array right = info[1].As<Napi::Float32Array>();
  if (left.ElementLength() != right.ElementLength()) {
    Napi::RangeError::New(env, "left and right must have the same length")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const size_t frames = left.ElementLength();
  if (frames > static_cast<size_t>(max_block_size_)) {
    Napi::RangeError::New(env, "block exceeds maxBlockSize").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (with_carrier && info[2].As<Napi::Float32Array>().ElementLength() != frames) {
    Napi::RangeError::New(env, "carrier must have the same length as left")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  // In-place planar stereo: the planar process_block reads and writes the
  // supplied channel buffers directly, so the caller's L/R arrays are mutated.
  float* channels[2] = {sonare_node::ElementData(left), sonare_node::ElementData(right)};
  const bool ok = with_carrier
                      ? changer_->process_block(
                            channels, 2, sonare_node::ElementData(info[2].As<Napi::Float32Array>()),
                            static_cast<int>(frames))
                      : changer_->process_block(channels, 2, static_cast<int>(frames));
  if (!ok) {
    sonare_node::ThrowSonareErrorMessage(env, SONARE_ERROR_INVALID_STATE, kCarrierRequiredMessage);
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeVoiceChangerWrap::ProcessMono(const Napi::CallbackInfo& info) {
  return ProcessMonoImpl(info, false);
}
Napi::Value RealtimeVoiceChangerWrap::ProcessMonoInto(const Napi::CallbackInfo& info) {
  return ProcessMonoImpl(info, true);
}
Napi::Value RealtimeVoiceChangerWrap::ProcessInterleaved(const Napi::CallbackInfo& info) {
  return ProcessInterleavedImpl(info, false);
}
Napi::Value RealtimeVoiceChangerWrap::ProcessInterleavedInto(const Napi::CallbackInfo& info) {
  return ProcessInterleavedImpl(info, true);
}
Napi::Value RealtimeVoiceChangerWrap::ProcessPlanarStereo(const Napi::CallbackInfo& info) {
  return ProcessPlanarStereoImpl(info);
}

Napi::Value RealtimeVoiceChangerWrap::NoteOn(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!EnsureAlive(env)) return env.Undefined();
  SONARE_NODE_TRY
  int note{};
  if (!RequiredIntArg(env, info, 0, "note", &note)) return env.Undefined();
  float velocity{};
  if (!sonare_node::RequiredDomainFloatValue(env, info[1], "velocity", &velocity)) {
    return env.Undefined();
  }
  using Changer = sonare::editing::voice_changer::RealtimeVoiceChanger;
  if (!Changer::is_valid_note(note)) {
    Napi::RangeError::New(env, "note must be an integer in [0, 127]").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!Changer::is_valid_velocity(velocity)) {
    Napi::RangeError::New(env, "velocity must be in (0, 1]; release a note with noteOff")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!prepared_ || !changer_->note_on(note, velocity)) {
    sonare_node::ThrowSonareErrorMessage(env, SONARE_ERROR_INVALID_STATE, kNoteRefusedMessage);
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeVoiceChangerWrap::NoteOff(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!EnsureAlive(env)) return env.Undefined();
  SONARE_NODE_TRY
  int note{};
  if (!RequiredIntArg(env, info, 0, "note", &note)) return env.Undefined();
  if (!sonare::editing::voice_changer::RealtimeVoiceChanger::is_valid_note(note)) {
    Napi::RangeError::New(env, "note must be an integer in [0, 127]").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!prepared_ || !changer_->note_off(note)) {
    sonare_node::ThrowSonareErrorMessage(env, SONARE_ERROR_INVALID_STATE, kNoteRefusedMessage);
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeVoiceChangerWrap::AllNotesOff(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!EnsureAlive(env)) return env.Undefined();
  SONARE_NODE_TRY
  changer_->all_notes_off();
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeVoiceChangerPresetNames(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  Napi::Array out = Napi::Array::New(env);
  const auto names = sonare::editing::voice_changer::realtime_voice_changer_preset_names();
  for (size_t i = 0; i < names.size(); ++i) {
    out.Set(static_cast<uint32_t>(i), names[i]);
  }
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeVoiceChangerPresetJson(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!info[0].IsString()) {
    Napi::TypeError::New(env, "presetId must be a string").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  const auto preset = sonare::editing::voice_changer::realtime_voice_changer_preset_from_id(
      node_narrow_string(env, info[0], "presetId"));
  return Napi::String::New(
      env, sonare::editing::voice_changer::realtime_voice_changer_preset_json(preset));
  SONARE_NODE_CATCH(env)
}

Napi::Value ValidateRealtimeVoiceChangerPresetJson(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!info[0].IsString()) {
    Napi::TypeError::New(env, "document must be a string").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  std::string normalized;
  std::string error;
  const bool ok = sonare::editing::voice_changer::validate_realtime_voice_changer_preset_json(
      node_narrow_string(env, info[0], "document"), &normalized, &error);
  Napi::Object out = Napi::Object::New(env);
  out.Set("ok", ok);
  if (ok) {
    out.Set("normalizedJson", normalized);
  } else {
    out.Set("error", error);
  }
  return out;
  SONARE_NODE_CATCH(env)
}

}  // namespace sonare_node
