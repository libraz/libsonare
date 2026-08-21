#pragma once

/// @file presets.h
/// @brief Built-in mastering chain presets and high-level master_audio API.

#include <cstddef>
#include <string>
#include <vector>

#include "mastering/api/chain.h"
#include "mastering/api/named_processor.h"

namespace sonare::mastering::api {

/// @brief Built-in preset identifiers.
///
/// Every preset carries an integrated-loudness target and a true-peak ceiling.
/// The two are not equally binding: the ceiling always holds, while the loudness
/// target is what one normalization pass aims at.
///
/// Reaching a target above the input's loudness costs gain the input's peak
/// headroom may not have. The stage therefore drives its true-peak limiter up to
/// @c loudness.maxLimiterGainReductionDb (12 dB by default) to close the
/// distance, and stops there. Two cases finish below target and set
/// @c loudness_target_limited on the result: material that would need more
/// limiting than that allowance, and material where the limiter's own gain
/// reduction takes back part of the applied gain, which a single pass does not
/// re-measure. Peak-normalized input, whose headroom is ~0 dB, is the common
/// case for both. Read the result's @c output_lufs for what was achieved rather
/// than assuming the preset's target.
enum class Preset {
  Pop,
  EDM,
  Acoustic,
  HipHop,
  AIMusic,
  Speech,
  Streaming,
  YouTube,
  Broadcast,
  Podcast,
  Audiobook,
  Cinema,
  JPop,
  Ambient,
  Lofi,
  Classical,
  DrumAndBass,
  Techno,
  Metal,
  Trap,
  RnB,
  Jazz,
  KPop,
  Trance,
  GameOst,
};

/// @brief Returns string identifiers of all built-in presets, in display order.
std::vector<std::string> preset_names();

/// @brief Parses a preset string identifier.
/// @throws SonareException (ErrorCode::InvalidParameter) if the name is unknown.
Preset preset_from_string(const std::string& name);

/// @brief Returns the canonical string identifier of a preset.
/// Returns "unknown" for invalid values; never throws.
const char* preset_to_string(Preset preset) noexcept;

/// @brief Returns a MasteringChainConfig pre-populated for the given preset.
/// Callers may inspect or further mutate the returned config.
MasteringChainConfig preset_config(Preset preset);

/// @brief Enable the chain's canonical loudness stage without a duplicate
/// standalone limiter.
void enable_loudness(MasteringChainConfig& config, float target_lufs, float ceiling_db);

/// @brief High-level: build preset config, apply optional overrides, run mono chain.
/// @param overrides Optional flat-params (same dot-notation as parse_chain_config_params)
/// applied on top of preset config. Pass nullptr / 0 for preset defaults only.
MonoChainResult master_audio_mono(Preset preset, const float* samples, std::size_t length,
                                  int sample_rate, const Param* overrides = nullptr,
                                  std::size_t override_count = 0);

/// @brief Stereo equivalent of master_audio_mono.
StereoChainResult master_audio_stereo(Preset preset, const float* left, const float* right,
                                      std::size_t length, int sample_rate,
                                      const Param* overrides = nullptr,
                                      std::size_t override_count = 0);

}  // namespace sonare::mastering::api
