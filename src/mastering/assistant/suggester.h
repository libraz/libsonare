#pragma once

/// @file suggester.h
/// @brief Rule-based mastering assistant chain suggestion.

#include <cstddef>
#include <string>
#include <vector>

#include "mastering/api/chain.h"
#include "mastering/assistant/audio_profile.h"

namespace sonare::mastering::assistant {

struct AssistantConfig {
  std::string target_platform = "streaming";
  float target_lufs = -14.0f;
  float ceiling_db = -1.0f;
  bool enable_repair = false;
  bool prefer_streaming_safe = true;
};

struct AssistantResult {
  api::MasteringChainConfig config{};
  AudioProfile profile{};
  std::vector<std::string> explanation;
  std::vector<GenreCandidate> genre_candidates;
};

AssistantResult suggest_chain(const float* samples, std::size_t length, int sample_rate,
                              const AssistantConfig& config = {});
AssistantResult suggest_chain(const Audio& audio, const AssistantConfig& config = {});
AssistantResult suggest_chain(const AudioProfile& profile, const AssistantConfig& config = {});

}  // namespace sonare::mastering::assistant
