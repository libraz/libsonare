#pragma once

/// @file command.h
/// @brief POD command records for realtime engine control queues.

#include <cstdint>
#include <type_traits>

namespace sonare::rt {

/// Increment when Command, telemetry, or SharedArrayBuffer record layouts
/// change. Bindings compare this value at startup before connecting to the
/// realtime engine.
inline constexpr uint32_t kEngineAbiVersion = 3;

// The CommandType space is split into two disjoint groups:
//
//  (1) RT-QUEUE VOCABULARY -- safe to enqueue via push_command() and applied by
//      apply_command() on the audio thread. These carry only POD scalars and
//      perform in-place, allocation-free updates:
//        kSetParam, kSetParamSmoothed, kTransportPlay, kTransportStop,
//        kTransportSeekSample, kTransportSeekPpq, kSeekMarker,
//        kMidiNoteOnImmediate, kMidiNoteOffImmediate, kMidiCcImmediate,
//        kMidiAllNotesOff (a.k.a. MIDI panic), kSetSoloMute.
//
//      Live scalar MIDI commands stay strictly POD: they synthesize a UMP from
//      packed scalar fields (no pointer, no variable-length payload) and route
//      it through the MidiSequencer's host-injection path. MIDI CLIP set
//      replacement, route tables, SMF import, device binding and host-node swap
//      are NOT queueable -- they own data swapped via direct setters and remain
//      group (2).
//
//  (2) DIRECT-SETTER OPERATIONS -- known command names that must NOT flow
//      through the realtime queue because they own data swapped via the
//      RtPublisher pattern on control-thread setters (set_tempo, set_loop,
//      swap_graph, set_clips, set_capture_*, set_metronome_config,
//      set_markers, ...):
//        kSetTempoMap, kSetLoop, kSwapGraph, kSwapAutomation, kAddClip,
//        kRemoveClip, kArmRecord, kPunch, kSetMetronome, kSetMarker.
//
// If a group-(2) value is pushed through the queue, apply_command() rejects it
// with TelemetryErrorCode::kNonQueueableCommand (NOT the misleading
// kUnknownTarget, which is reserved for queueable commands referencing an
// unbound target). The two groups are kept in a single enum so the binding ABI
// (kEngineAbiVersion) and the SharedArrayBuffer record layout stay stable.
enum class CommandType : uint16_t {
  // -- Group (1): RT-queue vocabulary --
  kSetParam,
  kSetParamSmoothed,
  kTransportPlay,
  kTransportStop,
  kTransportSeekSample,
  kTransportSeekPpq,
  // -- Group (2): direct-setter operations (rejected if queued) --
  kSetTempoMap,
  kSetLoop,
  kSwapGraph,
  kSwapAutomation,
  // -- Group (1) exception kept in its historical enum position: queueable
  // lane solo/mute in the realtime mixer. The numeric value stays unchanged so
  // the engine ABI version and SharedArrayBuffer command layout stay stable.
  kSetSoloMute,
  kAddClip,
  kRemoveClip,
  kArmRecord,
  kPunch,
  kSetMetronome,
  kSetMarker,
  // -- Group (1) continued --
  kSeekMarker,
  // Immediate (live) MIDI note events routed to a destination. Field encoding:
  //   target_id   = MIDI destination id.
  //   sample_time = render frame to fire at.
  //   arg.i       = packed bytes: bits[0..6]=velocity, bits[8..14]=note,
  //                 bits[16..19]=channel(0..15), bits[24..27]=group(0..15).
  kMidiNoteOnImmediate,
  kMidiNoteOffImmediate,
  // Immediate (live) MIDI control change routed to the host instrument via the
  // MidiSequencer's injection path. Strictly POD/scalar -- no pointer, no
  // variable-length payload. Field encoding:
  //   target_id   = MIDI destination id (the clip/instrument destination).
  //   sample_time = render frame to fire at (<0 / past => block head, like the
  //                 other queueable commands).
  //   arg.i       = packed bytes: bits[0..6]=value7, bits[8..14]=controller,
  //                 bits[16..19]=channel(0..15), bits[24..27]=group(0..15).
  kMidiCcImmediate,
  // MIDI panic / all-notes-off: release every sounding note tracked by the
  // sequencer (hang-note safety) at the command's render frame. POD/scalar:
  //   target_id   = ignored (panic is global across the sequencer's table).
  //   sample_time = render frame to fire at.
  kMidiAllNotesOff,
};

union CommandArg {
  float f;
  double d;  // full-precision scalar (e.g. seek PPQ); shares the 64-bit slot
  int64_t i;
  void* ptr;
};

struct Command {
  CommandType type = CommandType::kSetParam;
  uint32_t target_id = 0;
  /// Monotonic render-frame time. -1 means block-head/immediate.
  int64_t sample_time = -1;
  CommandArg arg{};
};

static_assert(std::is_trivially_copyable_v<CommandArg>,
              "CommandArg must stay trivially copyable for lock-free queues");
static_assert(std::is_trivially_copyable_v<Command>,
              "Command must stay trivially copyable for lock-free queues");

}  // namespace sonare::rt
