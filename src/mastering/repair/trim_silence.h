#pragma once

#include <cstddef>

#include "core/audio.h"

namespace sonare::mastering::repair {

enum class TrimSilenceMode {
  Peak,
  LufsGated,
};

struct TrimSilenceConfig {
  float threshold = 0.001f;
  size_t padding_samples = 0;
  TrimSilenceMode mode = TrimSilenceMode::Peak;
  float gate_lufs = -60.0f;
  float window_ms = 400.0f;
};

/// @brief Largest accepted padding_samples.
/// @details The padded tail is `last + padding_samples`, so a value above this
/// wraps before it is compared against the input length and shortens the kept
/// range instead of extending it. Only a negative count that crossed a language
/// boundary reaches this size.
inline constexpr size_t kMaxTrimPaddingSamples = static_cast<size_t>(-1) / 2;

struct TrimRange {
  size_t first = 0;
  size_t last_exclusive = 0;
};

/// Validates every public TrimSilenceConfig field. Named mono/stereo dispatch
/// and the direct DSP entrypoints share this oracle so enum/range handling
/// cannot drift between surfaces.
void validate_config(const TrimSilenceConfig& config);

/// @brief What a trim pass kept and what it dropped.
struct TrimReport {
  TrimRange range;                  ///< The kept range, padding included.
  size_t removed_head_samples = 0;  ///< Samples dropped before range.first.
  size_t removed_tail_samples = 0;  ///< Samples dropped after range.last_exclusive.
};

TrimRange detect_trim_range(const float* samples, size_t size, int sample_rate,
                            const TrimSilenceConfig& config = {});

/// @brief The one range a stereo pair is cut to.
/// @details Each channel is scanned on its own and the two ranges are unioned,
///   so the pair keeps whatever either channel calls signal. One range still cuts
///   both channels, so the outputs stay the same length -- cutting each channel
///   by its own range would not, but that is a different rule from this one.
///   A downmix is not read: `0.5 * (left + right)` halves material present in one
///   channel, which can drop it under the gate, and cancels an antiphase pair to
///   exactly zero, which reads full-level audio in both channels as silence.
///   Trimming is destructive, so the rule errs toward keeping.
TrimRange detect_trim_range_stereo(const float* left, const float* right, size_t size,
                                   int sample_rate, const TrimSilenceConfig& config = {});

Audio trim_silence(const Audio& audio, const TrimSilenceConfig& config = {});

/// @brief Trims @p audio and reports what the pass kept and dropped.
Audio trim_silence(const Audio& audio, const TrimSilenceConfig& config, TrimReport* report);

/// @brief A trimmed stereo pair and the one range both channels were cut to.
/// @details One report, not one per channel: the range is shared by contract, so
///   returning it twice would read as two decisions that happened to agree.
struct TrimSilenceStereoResult {
  Audio left;
  Audio right;
  TrimReport report;     ///< The union range, applied to both channels.
  TrimRange left_range;  ///< What the left channel alone called signal. The two
                         ///  are what report.range is the union of.
  TrimRange right_range;
};

/// @brief Trims a stereo pair to one shared range.
/// @details Both channels are emptied only when neither carries signal.
TrimSilenceStereoResult trim_silence_stereo(const Audio& left, const Audio& right,
                                            const TrimSilenceConfig& config = {});

}  // namespace sonare::mastering::repair
