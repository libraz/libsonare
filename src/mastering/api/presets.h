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

/// @brief Parses a preset string identifier. Throws std::invalid_argument if unknown.
Preset preset_from_string(const std::string& name);

/// @brief Returns the canonical string identifier of a preset.
/// Returns "unknown" for invalid values; never throws.
const char* preset_to_string(Preset preset) noexcept;

/// @brief Returns a MasteringChainConfig pre-populated for the given preset.
/// Callers may inspect or further mutate the returned config.
MasteringChainConfig preset_config(Preset preset);

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
