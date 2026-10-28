#include "editing/voice_changer/voice_changer_insert.h"

#include <algorithm>
#include <cmath>
#include <string>
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
  X("retuneSemitones", retune.semitones)                         \
  X("retuneMix", retune.mix)                                     \
  X("formantFactor", formant.factor)                             \
  X("formantAmount", formant.amount)                             \
  X("formantBody", formant.body)                                 \
  X("formantBrightness", formant.brightness)                     \
  X("formantNasal", formant.nasal)                               \
  X("eqHighpassHz", eq.highpass_hz)                              \
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
  X("reverbVariationVariation", reverb_variation.variation)      \
  X("reverbVariationDelayMs", reverb_variation.delay_ms)         \
  X("reverbVariationFeedback", reverb_variation.feedback)        \
  X("reverbVariationTempoBpm", reverb_variation.tempo_bpm)       \
  X("reverbVariationDivision", reverb_variation.division)        \
  X("midiRelease", midi.release)

#define SONARE_COUNT_PARAM(key, member) +1
constexpr unsigned int kParamCount = 0 SONARE_VOICE_CHANGER_INSERT_PARAMS(SONARE_COUNT_PARAM);
#undef SONARE_COUNT_PARAM

// The insert carries no carrier input, so an external vocoder carrier has nothing to read.
bool insert_accepts(const RealtimeVoiceChangerConfig& config, std::string* error) {
  if (!validate_realtime_voice_changer_config(config, nullptr, error)) return false;
  if (config.vocoder.carrier_source == VocoderCarrierSource::External) {
    if (error) *error = "vocoderCarrierSource 'external' needs a carrier input the insert lacks";
    return false;
  }
  return true;
}

const RealtimeVoiceChangerConfig& validated(const RealtimeVoiceChangerConfig& config) {
  std::string error;
  if (!insert_accepts(config, &error)) {
    throw SonareException(ErrorCode::InvalidParameter, "voice.changer: " + error);
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
  sample_rate_ = sample_rate;
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
  const std::uint32_t discards = changer_.non_finite_discard_count();
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
#define SONARE_DESCRIBE_PARAM(key, member) descriptors.push_back({key, id++});
  SONARE_VOICE_CHANGER_INSERT_PARAMS(SONARE_DESCRIBE_PARAM)
#undef SONARE_DESCRIBE_PARAM
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
  if (!found || !insert_accepts(candidate, nullptr)) return false;
  config_ = candidate;
  changer_.set_config(config_);
  return true;
}

}  // namespace sonare::editing::voice_changer
