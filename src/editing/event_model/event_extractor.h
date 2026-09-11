#pragma once

/// @file event_extractor.h
/// @brief Builds percussive events from audio alone.

#include <vector>

#include "analysis/onset_analyzer.h"
#include "core/audio.h"
#include "editing/event_model/percussive_event.h"
#include "effects/hpss.h"

namespace sonare::editing::event_model {

/// @brief The separation an event's signal is lifted out with.
/// @details Extraction measures events against it and rendering has to repeat
///          it, so it is one struct both sides take rather than a framing each
///          of them restates.
///
///          The framing has to satisfy constant overlap-add, because the
///          separation is an inverse STFT: @c n_fft even and at least 2, and
///          @c hop_length positive and no more than half of it.
struct PercussiveSeparationConfig {
  HpssConfig hpss{};
  int n_fft = 2048;
  int hop_length = 512;
};

/// @brief Onset defaults this model needs, which differ from the detector's own
///        only in that backtracking is on.
/// @details Peak-picking lands a frame or more after a transient starts, so
///          without backtracking a span opens inside its own hit and closes
///          inside the next one. It then measures its successor's peak, muting
///          it leaves its own attack behind, and the square cut at its opening
///          edge falls mid-attack. Backtracking puts that edge in front of the
///          transient, which is what the rest of this model assumes.
///
///          It travels at most @c backtrack_range frames, 10 by default, which
///          is 5120 samples at the default framing. That bound is what a caller
///          reasoning backwards from a known hit position has to allow for.
inline OnsetDetectConfig percussive_onset_defaults() {
  OnsetDetectConfig config;
  config.backtrack = true;
  return config;
}

struct PercussiveEventExtractorConfig {
  PercussiveSeparationConfig separation{};

  /// Peak-picking rules. @c n_fft and @c hop_length are overwritten from
  /// @ref separation before detection runs, so the detector and the separation
  /// cannot end up on different framings.
  ///
  /// @c delta is in the units of @ref PercussiveEvent::strength -- the onset
  /// envelope's own scale, which is not normalized. Pick one off the strengths a
  /// default extraction returns rather than assuming the scale is around 1.
  OnsetDetectConfig onset = percussive_onset_defaults();

  /// Caps a span that no onset follows. It binds only at the end of a phrase
  /// and at the end of the track; anywhere else the next onset closes the span
  /// first. A span never runs past the end of the audio whatever this says, so
  /// an extracted set is always renderable over the audio it came from.
  float max_event_ms = 500.0f;

  /// Drops an event whose @c percussive_ratio falls below this. 0 keeps every
  /// detected onset, and is the default because any threshold above it also
  /// drops real hits that happen to sit over a loud sustain -- the ratio is a
  /// property of the span, so those read low too. It is useful on material that
  /// is mostly drums and wrong on a dense mix.
  float min_percussive_ratio = 0.0f;
};

/// @brief Extracts percussive events from @p audio.
/// @details Onsets are detected on the percussive component rather than on the
///          source, so a harmonic attack is attenuated before the detector sees
///          it instead of being filtered out afterwards, and they are
///          backtracked so that a span opens in front of its transient rather
///          than inside it. Each onset opens a span
///          that the next one closes, and each event then carries its strength,
///          the percussive peak over the span, and the share of the span's
///          energy the separation called percussive. Every returned event has an
///          identity edit.
/// @throws SonareException(InvalidParameter) on empty audio, a framing that
///         breaks constant overlap-add, a non-finite or non-positive
///         @c max_event_ms, or a @c min_percussive_ratio outside [0, 1].
std::vector<PercussiveEvent> extract_percussive_events(
    const Audio& audio, const PercussiveEventExtractorConfig& config = {});

}  // namespace sonare::editing::event_model
