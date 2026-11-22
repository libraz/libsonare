#pragma once

/// @file event_time_refinement.h
/// @brief Places an analysis event at the steepest short-time energy rise inside its evidence
///        window.
/// @details An onset-envelope frame only says that an event lies somewhere under its analysis
///          window. The reported time of a high-level beat or onset is defined as the point of
///          steepest rise of a fine energy envelope (5 ms window, 1.25 ms hop, both converted
///          from milliseconds at the audio's rate) inside that window: the start of the fine
///          frame whose level most exceeds the loudest frame ending before it within the
///          preceding half window. It is a placement definition, not a model of perceived onset.
///
///          Levels are in dB, clamped to a floor 50 dB under the loudest frame in reach (never
///          below -100 dBFS), so a rise out of silence and a rise out of a sustained bed are read
///          the same way, and a bed that beats or tremolos does not rise above its own recent
///          peaks. A window whose largest rise is under 6 dB holds no evidence, and its event
///          keeps the time it came with.

#include <optional>
#include <vector>

#include "core/audio.h"

namespace sonare {

/// @brief Half an analysis window in seconds: @p win_length samples, or @p n_fft when it is 0.
inline float evidence_half_window_s(int win_length, int n_fft, int sample_rate) {
  const int window = win_length > 0 ? win_length : n_fft;
  return 0.5f * static_cast<float>(window) / static_cast<float>(sample_rate);
}

/// @brief Fine energy envelope of one signal, queried per event.
class EventTimeRefiner {
 public:
  /// @brief Builds the fine envelope of @p audio at its own rate.
  explicit EventTimeRefiner(const Audio& audio);

  /// @brief Refines ascending event times.
  /// @param frame_times Centre times of the evidence frames in seconds, ascending.
  /// @param half_window_s Half the analysis window that produced the events, in seconds.
  /// @return One entry per event: the refined time, or no value where the window holds no rise.
  /// @details Event i is searched in [t_i - half_window_s, t_i + half_window_s] clipped to the
  ///          midpoints between t_i and its neighbours, so refined times stay in order and never
  ///          cross a neighbour.
  std::vector<std::optional<float>> refine(const std::vector<float>& frame_times,
                                           float half_window_s) const;

 private:
  std::vector<float> level_db_;  ///< Fine-frame level in dB, frame m centred at m * hop
  int sample_rate_ = 1;
  int hop_ = 1;          ///< Fine hop in samples
  int half_window_ = 0;  ///< Half the fine window in samples
  int rise_frames_ = 1;  ///< Fine frames between the two levels a rise compares
};

}  // namespace sonare
