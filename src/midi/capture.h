#pragma once

/// @file capture.h
/// @brief Drain an RT input queue of incoming MidiEvents into a PPQ-timed
///        MidiClip on the control thread.
///
/// Layering: depends on rt/spsc_queue, midi/midi_event, midi/midi_clip and
/// transport/tempo_map. It does NOT depend on engine/ or arrangement/.
///
/// Threading / RT contract
/// -----------------------
///  - AUDIO thread (PRODUCER): the live MIDI-in handler pushes incoming
///    MidiEvents (absolute render frame + UMP) into the SpscQueue. push() is
///    wait-free and alloc-0. A full queue drops the event (push returns false);
///    the dropped count is surfaced via dropped_count() telemetry.
///  - CONTROL thread (CONSUMER): drain() pops every queued event, converts its
///    render frame to clip-relative PPQ via the TempoMap, optionally quantizes
///    it, and appends a MidiClipEvent to the target MidiClip. drain() MAY
///    allocate (it grows the MidiClip's event vector) and is NOT RT-safe.
///
/// Determinism: quantization is pure integer/PPQ math (no clock / random); the
/// same queued events with the same config always yield the same MidiClip.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "midi/midi_clip.h"
#include "midi/midi_event.h"
#include "rt/spsc_queue.h"
#include "transport/tempo_map.h"

namespace sonare::midi {

/// Optional quantize-on-capture settings.
struct CaptureQuantize {
  static constexpr size_t kMaxGrooveSteps = 16;
  /// When true, each captured event's PPQ is snapped to the nearest grid line.
  bool enabled = false;
  /// Grid step in quarter notes (e.g. 0.25 = sixteenth-note grid). Ignored when
  /// `enabled` is false or <= 0.
  double grid_ppq = 0.25;
  /// Strength 0..1: 0 leaves the event untouched, 1 snaps fully to the grid.
  /// Values in between interpolate toward the grid line.
  double strength = 1.0;
  /// Swing amount 0..1. 0 is straight; values > 0 delay every odd grid line by
  /// up to half `grid_ppq` while even lines remain fixed.
  double swing = 0.0;
  /// Optional repeating groove-template offsets, expressed as fractions of
  /// `grid_ppq` per grid line. 0 disables the template.
  size_t groove_steps = 0;
  std::array<double, kMaxGrooveSteps> groove_offsets{};
};

/// Configuration for a capture drain pass.
struct CaptureConfig {
  /// Musical position (PPQ) of the captured clip's start. Captured events are
  /// stored relative to this so the clip can be placed at an arrangement slot.
  double clip_start_ppq = 0.0;
  CaptureQuantize quantize{};
};

/// Drains an SPSC input queue of live MidiEvents into a MidiClip. Holds the
/// queue plus a drop-telemetry counter for the producer side.
class MidiCapture {
 public:
  /// Allocate the input queue (control thread, non-RT). `capacity_pow2` must be
  /// a non-zero power of two (forwarded to SpscQueue::reserve).
  void prepare(const transport::TempoMap* tempo_map, size_t capacity_pow2);

  /// AUDIO thread (producer): enqueue one incoming event. Wait-free, alloc-0.
  /// Returns false (and bumps dropped_count()) if the queue is full.
  bool push(const MidiEvent& event) noexcept;

  /// CONTROL thread (consumer): pop every queued event, convert render frame ->
  /// clip-relative PPQ (optionally quantized), and append to `clip`. The clip is
  /// sorted stably afterwards. Returns the number of events drained. MAY
  /// allocate. Returns 0 (no-op) if `clip` or the tempo map is null.
  size_t drain(const CaptureConfig& config, MidiClip* clip);

  /// Number of events the producer failed to enqueue because the queue was full.
  uint32_t dropped_count() const noexcept { return dropped_count_.load(std::memory_order_relaxed); }
  void reset_telemetry() noexcept { dropped_count_.store(0, std::memory_order_relaxed); }

  size_t queue_capacity() const noexcept { return queue_.capacity(); }

 private:
  rt::SpscQueue<MidiEvent> queue_;
  const transport::TempoMap* tempo_map_ = nullptr;
  std::atomic<uint32_t> dropped_count_{0};
};

/// Quantize a PPQ position to a grid. `grid_ppq` is the step in quarter notes,
/// `strength` 0..1 interpolates toward the snapped line. Pure, deterministic.
double quantize_ppq(double ppq, double grid_ppq, double strength, double swing = 0.0) noexcept;
double quantize_ppq(double ppq, const CaptureQuantize& quantize) noexcept;

}  // namespace sonare::midi
