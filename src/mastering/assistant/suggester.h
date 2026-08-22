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
  float speech_mono_amount = 1.0f;
  /// @brief Whether the caller named @ref target_lufs / @ref ceiling_db at all.
  /// @details A delivery target only fills in what the caller left alone, and
  ///          "left alone" cannot be decided by comparing against the default:
  ///          a host UI that always sends its slider position sends -14 when
  ///          the slider sits at -14, and the platform target then silently won
  ///          that one position. Callers that fill this struct field by field
  ///          (the flat param parsers on every surface) set these; a direct C++
  ///          caller that leaves them false keeps the historical
  ///          compare-to-default behaviour, so setting a non-default value
  ///          still suppresses the override without the flag.
  bool target_lufs_explicit = false;
  bool ceiling_db_explicit = false;
};

struct AssistantResult {
  api::MasteringChainConfig config{};
  AudioProfile profile{};
  std::vector<std::string> explanation;
  std::vector<GenreCandidate> genre_candidates;
};

AssistantResult suggest_chain(const float* samples, std::size_t length, int sample_rate,
                              const AssistantConfig& config = {});

/// @brief Multi-channel counterpart preserving BS.1770 channel summing.
/// @details Profiles through @ref analyze_audio_profile_interleaved, so the
///          loudness the suggestion is built on is the channel-summed program
///          rather than a `0.5 * (L + R)` downmix. The suggested chain's
///          loudness stage is driven by that measurement, so a downmixed
///          profile asks for roughly 6 dB more gain than the material needs.
/// @param samples Pointer to `frames * channels` interleaved samples.
/// @param frames Number of sample frames.
/// @param channels Channel count; must be positive.
/// @param sample_rate Sample rate in Hz; must be positive.
AssistantResult suggest_chain_interleaved(const float* samples, std::size_t frames, int channels,
                                          int sample_rate, const AssistantConfig& config = {});
AssistantResult suggest_chain(const Audio& audio, const AssistantConfig& config = {});
AssistantResult suggest_chain(const AudioProfile& profile, const AssistantConfig& config = {});
std::string assistant_result_to_json(const AssistantResult& result);

}  // namespace sonare::mastering::assistant
