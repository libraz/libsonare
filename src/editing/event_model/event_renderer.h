#pragma once

/// @file event_renderer.h
/// @brief Renders edited percussive events back over their source audio.
///
/// Non-destructive: the source passes through everywhere, and each non-identity
/// event's own percussive signal is lifted out of its span and written back
/// where the edit puts it. A set whose edits are all identity reproduces the
/// input bit for bit and runs no separation at all.

#include <vector>

#include "core/audio.h"
#include "editing/event_model/event_extractor.h"
#include "editing/event_model/percussive_event.h"

namespace sonare::editing::event_model {

struct PercussiveEventRenderConfig {
  /// Hand back the separation the extraction used. A different one lifts a
  /// different signal out of the span than the one the events were measured
  /// against.
  PercussiveSeparationConfig separation{};

  /// Fade-out at the tail of each lifted span. There is no matching fade-in:
  /// a span opens in front of its transient, where the percussive component is
  /// near-silent, so cutting square there costs nothing and keeps a muted hit's
  /// attack from surviving inside a fade. That is why extraction backtracks --
  /// see @ref percussive_onset_defaults.
  float fade_ms = 5.0f;
};

/// @brief Renders @p events over @p audio.
/// @details The output has the input's length and sample rate. Per event the
///          lifted signal is the percussive component over [onset, offset)
///          under the tail fade; it is subtracted where it sits and, unless the
///          event is muted, added back at the shifted position scaled by the
///          gain. Only that signal moves, so muting a hit leaves the harmonic
///          content under it sounding and moving one does not drag its
///          neighbours' sustain along.
///
///          A shift that pushes the signal past either end is truncated there.
///          Overlap is checked on the source spans only -- where
///          time_offset_samples lands an event is not, so two moved events may
///          be written over each other.
///
///          Validation covers every event, identity or not: an unrenderable set
///          is unrenderable whether or not this call would touch it.
/// @throws SonareException(InvalidParameter) on empty audio, an event whose span
///         is empty, reversed or outside the audio, overlapping source spans, a
///         non-finite gain, a framing that breaks constant overlap-add, or a
///         non-finite or non-positive @c fade_ms. The framing is checked even
///         when every edit is the identity and no separation runs, so an
///         unusable config is an error on every set rather than on some of them.
Audio render_percussive_events(const Audio& audio, const std::vector<PercussiveEvent>& events,
                               const PercussiveEventRenderConfig& config = {});

}  // namespace sonare::editing::event_model
