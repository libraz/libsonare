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

// The enum below is split into two disjoint groups, marked inline:
//
//  (1) RT-QUEUE VOCABULARY -- enqueueable via push_command() and applied on the
//      audio thread. POD scalars only, in-place and allocation-free. The live
//      MIDI commands stay POD by synthesizing a UMP from packed scalar fields
//      and routing it through the sequencer's host-injection path.
//  (2) DIRECT-SETTER OPERATIONS -- must NOT flow through the queue, because they
//      own data swapped through the RtPublisher pattern on control-thread
//      setters. Clip set replacement, route tables, SMF import, device binding
//      and host-node swap all belong here for that reason.
//
// Pushing a group-(2) value is rejected with
// TelemetryErrorCode::kNonQueueableCommand, not the misleading kUnknownTarget,
// which is reserved for a queueable command naming an unbound target. Both
// groups share one enum so the binding ABI and the SharedArrayBuffer record
// layout stay stable.
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
  // Immediate (live) realtime change of one channel-strip insert parameter.
  // Applied at the block head via the strip's allocation-free
  // ChannelStrip::apply_insert_parameter(); the control thread resolves the
  // JSON-key parameter name to its integer param_id before enqueuing. POD/scalar:
  //   target_id   = (lane_index << 16) | (insert_index << 8) | param_id.
  //   sample_time = render frame to fire at (<0 / past => block head).
  //   arg.f       = parameter value.
  kSetTrackInsertParam,
  // Same as kSetTrackInsertParam but targets the master strip (no lane field):
  //   target_id   = (insert_index << 8) | param_id.
  kSetMasterInsertParam,
  // Immediate (live) MIDI SysEx routed to a destination instrument. SysEx is
  // variable-length, which a fixed POD Command cannot carry inline, so the
  // control thread copies the bytes into the engine's bounded SysEx payload
  // store (see RealtimeEngine::push_midi_sysex) and enqueues this command with a
  // scalar slot reference -- no pointer crosses the queue, so the record stays
  // WASM SharedArrayBuffer-safe. Field encoding:
  //   target_id   = MIDI destination id.
  //   sample_time = render frame to fire at (<0 / past => block head).
  //   arg.i       = packed slot reference: bits[0..31]=slot index,
  //                 bits[32..63]=slot generation (validated on apply to drop a
  //                 slot the control thread already recycled). The slot records
  //                 the payload length. New value; appended at the enum end to
  //                 keep kEngineAbiVersion and the SharedArrayBuffer command
  //                 record layout stable.
  kMidiSysExImmediate,
  // Immediate single-word UMP MIDI 1.0 channel-voice event. This restores
  // program, pitch bend and pressure state on transport seek, which cannot be
  // represented by the note/CC convenience commands. arg.i contains word0.
  // Appended to preserve existing command numeric values.
  kMidiUmpImmediate,
  // Queueable lane monitor-mode transition. target_id is the lane index,
  // sample_time is the requested render frame, and arg.i is the mode ordinal
  // (0=off, 1=PFL, 2=AFL). This is deliberately terminal (ordinal 26) so the
  // H6 command ids remain unchanged.
  kSetTrackMonitorMode,
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
