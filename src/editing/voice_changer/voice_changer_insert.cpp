#include "editing/voice_changer/voice_changer_insert.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <string>
#include <string_view>
#include <type_traits>

#include "util/exception.h"

namespace sonare::editing::voice_changer {
namespace {

constexpr int kMaxChannels = 2;

// X(key, member): the automatable fields, in descriptor id order. The keys are the flat
// parameter names the preset JSON and the C POD already use. An integer, enum or switch field
// takes the automation value as its wire value.
#define SONARE_VOICE_CHANGER_INSERT_PARAMS(X)                    \
  X("inputGainDb", input_gain_db)                                \
  X("outputGainDb", output_gain_db)                              \
  X("wetMix", wet_mix)                                           \
  X("retunePitchSemitones", retune.semitones)                    \
  X("retuneMix", retune.mix)                                     \
  X("formantFactor", formant.factor)                             \
  X("formantAmount", formant.amount)                             \
  X("formantBody", formant.body)                                 \
  X("formantBrightness", formant.brightness)                     \
  X("formantNasal", formant.nasal)                               \
  X("eqHighPassHz", eq.highpass_hz)                              \
  X("eqBodyDb", eq.body_db)                                      \
  X("eqPresenceDb", eq.presence_db)                              \
  X("eqAirDb", eq.air_db)                                        \
  X("gateThresholdDb", gate.threshold_db)                        \
  X("gateAttackMs", gate.attack_ms)                              \
  X("gateReleaseMs", gate.release_ms)                            \
  X("gateRangeDb", gate.range_db)                                \
  X("compressorThresholdDb", compressor.threshold_db)            \
  X("compressorRatio", compressor.ratio)                         \
  X("compressorAttackMs", compressor.attack_ms)                  \
  X("compressorReleaseMs", compressor.release_ms)                \
  X("compressorMakeupGainDb", compressor.makeup_gain_db)         \
  X("deesserFrequencyHz", deesser.frequency_hz)                  \
  X("deesserThresholdDb", deesser.threshold_db)                  \
  X("deesserRatio", deesser.ratio)                               \
  X("deesserRangeDb", deesser.range_db)                          \
  X("reverbMix", reverb.mix)                                     \
  X("reverbTimeMs", reverb.time_ms)                              \
  X("reverbDamping", reverb.damping)                             \
  X("limiterCeilingDb", limiter.ceiling_db)                      \
  X("limiterReleaseMs", limiter.release_ms)                      \
  X("limiterIspCeilingDbtp", limiter.isp_ceiling_dbtp)           \
  X("pitchCorrectionMode", pitch_correction.mode)                \
  X("pitchCorrectionAmount", pitch_correction.amount)            \
  X("pitchCorrectionSpeedMs", pitch_correction.speed_ms)         \
  X("pitchCorrectionKeyRoot", pitch_correction.key_root)         \
  X("pitchCorrectionScale", pitch_correction.scale)              \
  X("pitchCorrectionRobotOctave", pitch_correction.robot_octave) \
  X("harmonyVoices", harmony.voices)                             \
  X("harmonyStep1", harmony.steps[0])                            \
  X("harmonyStep2", harmony.steps[1])                            \
  X("harmonyStep3", harmony.steps[2])                            \
  X("harmonyLevel", harmony.level)                               \
  X("harmonyLeadLevel", harmony.lead_level)                      \
  X("harmonyDetuneCents", harmony.detune_cents)                  \
  X("harmonyDelayMs", harmony.delay_ms)                          \
  X("vocoderEnabled", vocoder.enabled)                           \
  X("vocoderAttackMs", vocoder.attack_ms)                        \
  X("vocoderReleaseMs", vocoder.release_ms)                      \
  X("vocoderSibilance", vocoder.sibilance)                       \
  X("vocoderCarrierWave", vocoder.carrier_wave)                  \
  X("vocoderEnsembleDetuneCents", vocoder.ensemble_detune_cents) \
  X("vocoderLevel", vocoder.level)                               \
  X("characterType", character.type)                             \
  X("characterAmount", character.amount)                         \
  X("characterTone", character.tone)                             \
  X("reverbVariation", reverb_variation.variation)               \
  X("reverbVariationDelayMs", reverb_variation.delay_ms)         \
  X("reverbVariationFeedback", reverb_variation.feedback)        \
  X("reverbVariationTempoBpm", reverb_variation.tempo_bpm)       \
  X("reverbVariationDivision", reverb_variation.division)        \
  X("midiRelease", midi.release)

#define SONARE_PARAM_NAME(key, member) key,
constexpr const char* kParamNames[] = {SONARE_VOICE_CHANGER_INSERT_PARAMS(SONARE_PARAM_NAME)};
#undef SONARE_PARAM_NAME
constexpr unsigned int kParamCount = std::size(kParamNames);

// The insert carries no carrier input, so an external vocoder carrier has nothing to read.
bool insert_accepts(const RealtimeVoiceChangerConfig& config) {
  return validate_realtime_voice_changer_config(config, nullptr, nullptr,
                                                ConfigFieldSpelling::FlatPod) &&
         config.vocoder.carrier_source != VocoderCarrierSource::External;
}

// The insert's keys are the flat POD's, except the harmony steps, which it takes as three scalars.
std::string insert_key(const std::string& pod_key) {
  constexpr std::string_view kSteps = "harmonySteps[";
  if (pod_key.size() == kSteps.size() + 2 && pod_key.compare(0, kSteps.size(), kSteps) == 0) {
    return "harmonyStep" + std::to_string(pod_key[kSteps.size()] - '0' + 1);
  }
  return pod_key;
}

const RealtimeVoiceChangerConfig& validated(const RealtimeVoiceChangerConfig& config) {
  std::string error;
  if (!validate_realtime_voice_changer_config(config, nullptr, &error,
                                              ConfigFieldSpelling::FlatPod)) {
    // The validator writes "<flat POD key> <predicate>".
    const std::string::size_type space = error.find(' ');
    throw SonareParameterRefusal::predicate(
        ErrorCode::InvalidParameter, insert_key(error.substr(0, space)), error.substr(space + 1))
        .as_caller_spelled();
  }
  if (config.vocoder.carrier_source == VocoderCarrierSource::External) {
    throw SonareParameterRefusal::predicate(
        ErrorCode::NotSupported, "vocoderCarrierSource",
        "external is not supported for the voice changer insert: it has no carrier input")
        .as_caller_spelled();
  }
  return config;
}

bool assign_param(float& field, float value) noexcept {
  field = value;
  return true;
}

bool assign_param(bool& field, float value) noexcept {
  if (!std::isfinite(value)) return false;
  field = value != 0.0f;
  return true;
}

// Whole numbers only: a fraction names no count, step or enumerator.
template <typename Field,
          std::enable_if_t<std::is_integral_v<Field> || std::is_enum_v<Field>, int> = 0>
bool assign_param(Field& field, float value) noexcept {
  constexpr float kLargestExact = 16777216.0f;
  if (!std::isfinite(value) || std::trunc(value) != value || std::fabs(value) > kLargestExact) {
    return false;
  }
  field = static_cast<Field>(static_cast<int>(value));
  return true;
}

}  // namespace

VoiceChangerInsert::VoiceChangerInsert(RealtimeVoiceChangerConfig config)
    : changer_(validated(config)), config_(config) {}

void VoiceChangerInsert::prepare(double sample_rate, int max_block_size) {
  prepare(sample_rate, max_block_size, kMaxChannels);
}

void VoiceChangerInsert::prepare(double sample_rate, int max_block_size, int max_channels) {
  if (max_channels > kMaxChannels) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "voice.changer processes at most 2 channels");
  }
  const int channels = std::max(1, max_channels);
  max_block_size_ = std::max(1, max_block_size);
  changer_.prepare(sample_rate, max_block_size_, channels);
  prepared_channels_ = channels;
  reported_discards_ = 0;
}

void VoiceChangerInsert::process(float* const* channels, int num_channels, int num_samples) {
  if (!validate_process_buffers(channels, num_channels, num_samples)) return;
  ensure_prepared(prepared_channels_ > 0, "VoiceChangerInsert");
  if (num_channels > prepared_channels_) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "voice.changer received more channels than it was prepared for");
  }
  float* planes[kMaxChannels] = {};
  for (int offset = 0; offset < num_samples; offset += max_block_size_) {
    const int length = std::min(max_block_size_, num_samples - offset);
    for (int ch = 0; ch < num_channels; ++ch) planes[ch] = channels[ch] + offset;
    changer_.process_block(planes, num_channels, length);
  }
  // One discard per process() call, whatever number of inner blocks it took.
  const std::size_t discards = changer_.non_finite_discard_count();
  if (discards != reported_discards_) {
    reported_discards_ = discards;
    note_non_finite_discard();
  }
}

void VoiceChangerInsert::reset() { changer_.reset(); }

int VoiceChangerInsert::latency_samples() const noexcept { return changer_.latency_samples(); }

int VoiceChangerInsert::tail_samples() const noexcept { return changer_.tail_budget().samples(); }

float VoiceChangerInsert::last_gain_reduction_db() const {
  return changer_.last_gain_reduction_db();
}

bool VoiceChangerInsert::parameter_is_realtime_safe(unsigned int param_id) const noexcept {
  return param_id < kParamCount;
}

std::vector<rt::ParamDescriptor> VoiceChangerInsert::parameter_descriptors() const {
  std::vector<rt::ParamDescriptor> descriptors;
  unsigned int id = 0;
  for (const char* key : kParamNames) {
    descriptors.push_back({key, id++});
  }
  return descriptors;
}

bool VoiceChangerInsert::set_parameter_impl(unsigned int param_id, float value) {
  unsigned int id = 0;
  bool found = false;
  RealtimeVoiceChangerConfig candidate = config_;
#define SONARE_APPLY_PARAM(key, member)            \
  if (id++ == param_id) {                          \
    found = assign_param(candidate.member, value); \
  }
  SONARE_VOICE_CHANGER_INSERT_PARAMS(SONARE_APPLY_PARAM)
#undef SONARE_APPLY_PARAM
  if (!found || !insert_accepts(candidate)) return false;
  config_ = candidate;
  changer_.set_config(config_);
  return true;
}

}  // namespace sonare::editing::voice_changer
