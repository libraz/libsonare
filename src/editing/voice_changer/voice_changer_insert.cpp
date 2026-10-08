#include "editing/voice_changer/voice_changer_insert.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

#include "util/exception.h"

namespace sonare::editing::voice_changer {
namespace {

constexpr int kMaxChannels = 2;

// The reverb's combs decay 60 dB per time_ms, but the first echo arrives after up to 0.47 of it
// and is the strongest, so falling 60 dB below the loudest echo takes up to 1.47 decay times.
constexpr double kReverbTailPerDecay = 1.5;

// X(key, member): the automatable fields, in descriptor id order. The keys are the flat
// parameter names the preset JSON and the C POD already use.
#define SONARE_VOICE_CHANGER_INSERT_PARAMS(X)            \
  X("inputGainDb", input_gain_db)                        \
  X("outputGainDb", output_gain_db)                      \
  X("wetMix", wet_mix)                                   \
  X("retuneSemitones", retune.semitones)                 \
  X("retuneMix", retune.mix)                             \
  X("formantFactor", formant.factor)                     \
  X("formantAmount", formant.amount)                     \
  X("formantBody", formant.body)                         \
  X("formantBrightness", formant.brightness)             \
  X("formantNasal", formant.nasal)                       \
  X("eqHighpassHz", eq.highpass_hz)                      \
  X("eqBodyDb", eq.body_db)                              \
  X("eqPresenceDb", eq.presence_db)                      \
  X("eqAirDb", eq.air_db)                                \
  X("gateThresholdDb", gate.threshold_db)                \
  X("gateAttackMs", gate.attack_ms)                      \
  X("gateReleaseMs", gate.release_ms)                    \
  X("gateRangeDb", gate.range_db)                        \
  X("compressorThresholdDb", compressor.threshold_db)    \
  X("compressorRatio", compressor.ratio)                 \
  X("compressorAttackMs", compressor.attack_ms)          \
  X("compressorReleaseMs", compressor.release_ms)        \
  X("compressorMakeupGainDb", compressor.makeup_gain_db) \
  X("deesserFrequencyHz", deesser.frequency_hz)          \
  X("deesserThresholdDb", deesser.threshold_db)          \
  X("deesserRatio", deesser.ratio)                       \
  X("deesserRangeDb", deesser.range_db)                  \
  X("reverbMix", reverb.mix)                             \
  X("reverbTimeMs", reverb.time_ms)                      \
  X("reverbDamping", reverb.damping)                     \
  X("limiterCeilingDb", limiter.ceiling_db)              \
  X("limiterReleaseMs", limiter.release_ms)              \
  X("limiterIspCeilingDbtp", limiter.isp_ceiling_dbtp)

#define SONARE_COUNT_PARAM(key, member) +1
constexpr unsigned int kParamCount = 0 SONARE_VOICE_CHANGER_INSERT_PARAMS(SONARE_COUNT_PARAM);
#undef SONARE_COUNT_PARAM

[[noreturn]] void refuse(const char* key) {
  throw SonareException(ErrorCode::InvalidParameter,
                        std::string("voice.changer: ") + key + " is out of range");
}

// The class's own normalization is the one definition of the accepted ranges; a field it would
// change is out of range.
const RealtimeVoiceChangerConfig& validated(const RealtimeVoiceChangerConfig& config) {
  const RealtimeVoiceChangerConfig accepted = normalize_realtime_voice_changer_config(config);
#define SONARE_CHECK_PARAM(key, member) \
  if (!std::isfinite(config.member) || config.member != accepted.member) refuse(key);
  SONARE_VOICE_CHANGER_INSERT_PARAMS(SONARE_CHECK_PARAM)
#undef SONARE_CHECK_PARAM
  if (config.retune.grain_size != accepted.retune.grain_size) refuse("retuneGrainSize");
  if (config.formant_mode != accepted.formant_mode) refuse("formantMode");
  std::string reach;
  if (!formant_warp_is_reachable(accepted, &reach)) {
    throw SonareException(ErrorCode::InvalidParameter, "voice.changer: " + reach);
  }
  return config;
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

int VoiceChangerInsert::tail_samples() const noexcept {
  if (!(config_.wet_mix > 0.0f) || !(config_.reverb.mix > 0.0f)) return 0;
  const double samples = std::ceil(static_cast<double>(config_.reverb.time_ms) * 0.001 *
                                   kReverbTailPerDecay * sample_rate_);
  return samples >= static_cast<double>(std::numeric_limits<int>::max())
             ? std::numeric_limits<int>::max()
             : static_cast<int>(samples);
}

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
#define SONARE_APPLY_PARAM(key, member) \
  if (id++ == param_id) {               \
    config_.member = value;             \
    found = true;                       \
  }
  SONARE_VOICE_CHANGER_INSERT_PARAMS(SONARE_APPLY_PARAM)
#undef SONARE_APPLY_PARAM
  if (!found) return false;
  config_ = normalize_realtime_voice_changer_config(config_);
  changer_.set_config(config_);
  return true;
}

}  // namespace sonare::editing::voice_changer
