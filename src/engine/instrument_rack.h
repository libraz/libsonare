#pragma once

/// @file instrument_rack.h
/// @brief Fixed-capacity per-destination host-instrument rack: an RT-safe MIDI
///        sink that demultiplexes dispatched events to the instrument registered
///        for each destination_id, and lets the engine fan-out
///        prepare / transport / render across every hosted instrument.
///
/// Threading / RT contract
/// -----------------------
///  - CONTROL thread (between blocks): set() / clear bind or unbind an
///    instrument for a destination_id; matches the engine's control-thread-only
///    contract for instrument registration. May NOT run concurrently with the
///    audio thread (same contract as RealtimeEngine::set_clips and friends).
///  - AUDIO thread: on_event() routes a dispatched event to its destination's
///    instrument; for_each() fans the render/transport loop across bound
///    instruments. Both are allocation-free, lock-free and I/O-free.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "midi/instrument.h"
#include "midi/midi_event.h"
#include "midi/sequencer.h"

namespace sonare::engine {

/// Demultiplexing MIDI sink + per-destination instrument table. Each active slot
/// binds a destination_id to a borrowed midi::MidiInstrument; the engine owns
/// the rack and feeds it to the sequencer as the dispatch sink so multitrack
/// MIDI reaches the instrument bound to each clip's destination.
class InstrumentRack final : public midi::MidiEventSink {
 public:
  /// Maximum simultaneously-hosted instruments (one per routed destination).
  static constexpr size_t kMaxInstruments = 16;

  /// CONTROL thread: bind `instrument` to `destination_id`, replacing any prior
  /// binding for that id. Passing nullptr clears the binding. Returns false only
  /// when a NEW binding cannot be added because the table is full; replacing or
  /// clearing an existing binding always succeeds.
  bool set(uint32_t destination_id, midi::MidiInstrument* instrument) noexcept {
    for (auto& slot : slots_) {
      if (slot.active && slot.destination_id == destination_id) {
        if (instrument == nullptr) {
          slot = Slot{};
        } else {
          slot.instrument = instrument;
        }
        return true;
      }
    }
    if (instrument == nullptr) return true;  // clearing an absent binding: no-op
    for (auto& slot : slots_) {
      if (!slot.active) {
        slot = Slot{true, destination_id, instrument};
        return true;
      }
    }
    return false;  // table full
  }

  /// Instrument bound to `destination_id`, or nullptr if none.
  midi::MidiInstrument* get(uint32_t destination_id) const noexcept {
    for (const auto& slot : slots_) {
      if (slot.active && slot.destination_id == destination_id) return slot.instrument;
    }
    return nullptr;
  }

  /// AUDIO thread: route one dispatched event to its destination's instrument.
  /// Events for an unbound destination are discarded (no audible output), so an
  /// empty rack behaves exactly like a null sink.
  void on_event(uint32_t destination_id, const midi::MidiEvent& event) noexcept override {
    for (auto& slot : slots_) {
      if (slot.active && slot.destination_id == destination_id) {
        slot.instrument->on_event(destination_id, event);
        return;
      }
    }
  }

  /// True when no instrument is bound (engine renders no instrument audio).
  bool empty() const noexcept {
    for (const auto& slot : slots_) {
      if (slot.active) return false;
    }
    return true;
  }

  /// Number of bound instruments.
  size_t size() const noexcept {
    size_t count = 0;
    for (const auto& slot : slots_) {
      if (slot.active) ++count;
    }
    return count;
  }

  /// Highest latency (samples) reported across bound instruments — the figure
  /// the arrangement compiler folds into the CompiledTimeline PDC summary.
  int max_latency_samples() const noexcept {
    int max = 0;
    for (const auto& slot : slots_) {
      if (slot.active) max = std::max(max, slot.instrument->latency_samples());
    }
    return max;
  }

  /// Highest reported latency in Q8.8 samples (preserves sub-sample latency for
  /// fractional PDC). Integer-latency instruments yield latency_samples() << 8.
  int max_latency_samples_q8() const noexcept {
    int max = 0;
    for (const auto& slot : slots_) {
      if (slot.active) max = std::max(max, slot.instrument->latency_samples_q8());
    }
    return max;
  }

  /// Invoke `fn(destination_id, instrument)` for every bound instrument. RT-safe
  /// when `fn` is (the engine's render/transport fan-out is allocation-free).
  template <typename Fn>
  void for_each(Fn&& fn) const {
    for (const auto& slot : slots_) {
      if (slot.active) fn(slot.destination_id, slot.instrument);
    }
  }

 private:
  struct Slot {
    bool active = false;
    uint32_t destination_id = 0;
    midi::MidiInstrument* instrument = nullptr;
  };
  std::array<Slot, kMaxInstruments> slots_{};
};

}  // namespace sonare::engine
