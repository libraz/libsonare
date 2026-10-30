/// @file realtime_engine_midi_io.cpp
/// @brief Embind realtime-engine facade: live MIDI input, pushes & external MIDI.

#ifdef __EMSCRIPTEN__

#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

#include "midi/live_push_args.h"
#include "realtime_engine_wasm.h"

namespace {

// Throws the live push argument refusal of midi/live_push_args.h as a RangeError
// naming the JS function.
void refuseLivePushArg(const char* what, const char* error) {
  if (error != nullptr) throw WasmRangeError(std::string(what) + ": " + error);
}

// Packs the four scalar fields of a live MIDI command into Command::arg.i
// (encoding documented in rt/command.h).
int64_t packMidiScalar(int low, int second, int channel, int group) {
  const uint64_t packed = static_cast<uint64_t>(low) | (static_cast<uint64_t>(second) << 8) |
                          (static_cast<uint64_t>(channel) << 16) |
                          (static_cast<uint64_t>(group) << 24);
  return static_cast<int64_t>(packed);
}

// Queues @p command, or throws OutOfMemory (transient back-pressure) with @p message.
template <typename Engine>
void pushEngineCommand(Engine& engine, const sonare::rt::Command& command, const char* message) {
  if (!engine.push_command(command)) {
    throw sonare::SonareException(sonare::ErrorCode::OutOfMemory, message);
  }
}

}  // namespace

void RealtimeEngineWasm::setMidiInputSource(const val& destination_id_val) {
#if defined(SONARE_WITH_ARRANGEMENT)
  engine_.set_midi_input_source(&midi_input_source_,
                                checkedUintFromVal(destination_id_val, "destinationId"));
  midi_input_source_enabled_ = true;
#else
  (void)destination_id_val;
  throwMidiEngineUnavailable();
#endif
}

void RealtimeEngineWasm::clearMidiInputSource() {
#if defined(SONARE_WITH_ARRANGEMENT)
  engine_.set_midi_input_source(nullptr, 0);
  midi_input_source_enabled_ = false;
#endif
}

size_t RealtimeEngineWasm::midiInputPendingCount() const {
#if defined(SONARE_WITH_ARRANGEMENT)
  return midi_input_source_.pending_count();
#else
  return 0;
#endif
}

// Route the MIDI of `destination_id` (a track lane) to the external output
// queue instead of the internal instrument rack, so the track plays an
// external device. Clearing it restores internal-synth playback.
void RealtimeEngineWasm::setMidiDestinationExternal(const val& destination_id_val, bool external) {
#if defined(SONARE_WITH_ARRANGEMENT)
  const uint32_t destination_id = checkedUintFromVal(destination_id_val, "destinationId");
  if (!engine_.set_midi_destination_external(destination_id, external)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                  "external MIDI destination table is full");
  }
#else
  (void)destination_id_val;
  (void)external;
  throwMidiEngineUnavailable();
#endif
}

// Enable/disable forwarding MIDI clock + transport (start/continue/stop) to
// the external output queue so external gear stays tempo-synced.
void RealtimeEngineWasm::setExternalMidiClockEnabled(bool enabled) {
#if defined(SONARE_WITH_ARRANGEMENT)
  engine_.set_external_midi_clock_enabled(enabled);
#else
  (void)enabled;
  throwMidiEngineUnavailable();
#endif
}

// Count of external-MIDI events dropped because the output queue was full.
uint32_t RealtimeEngineWasm::externalMidiDroppedCount() const {
#if defined(SONARE_WITH_ARRANGEMENT)
  return engine_.external_midi_dropped_count();
#else
  return 0;
#endif
}

size_t RealtimeEngineWasm::externalMidiPendingCount() const {
#if defined(SONARE_WITH_ARRANGEMENT)
  return engine_.external_midi_pending_count();
#else
  return 0;
#endif
}

// Drain queued external-MIDI events, already lowered to MIDI 1.0 byte
// messages so the host can write them straight to a Web MIDI output port.
// Each returned item is { destinationId, renderFrame, bytes: number[] };
// transport/clock bytes carry destinationId === kTransportDestination
// (0xFFFFFFFF). A single queued channel-voice UMP may lower to more than one
// item (e.g. a MIDI 2.0 program change with bank select). `max_records` caps
// the number of OUTPUT events produced -- the unit shared by every surface.
// To keep that cap lossless we drain one queue record at a time, consuming it
// only while at least 3 output slots (the most one record can lower to) remain
// in the budget; records that do not fit stay queued for the next call. UMP
// types that do not lower to MIDI 1.0 (SysEx/Data, Utility, MIDI-2-only
// controllers) emit no bytes and are skipped.
//
// renderFrame coordinate: channel-voice events use the timeline sample
// position; clock/transport bytes use the monotonic device render frame. They
// coincide during straight playback and diverge across a loop/seek -- see
// RealtimeEngine::drain_external_midi. Reconcile via the telemetry block's
// renderFrame/timelineSample pair when scheduling sample-accurately.
val RealtimeEngineWasm::drainExternalMidi(const val& max_records_val) {
  val out = val::array();
#if defined(SONARE_WITH_ARRANGEMENT)
  const int max_records = checkedIntFromVal(max_records_val, "maxRecords");
  // One queue record lowers to at most this many MIDI-1 messages, so a smaller
  // budget can never consume a record and the drain would report nothing while
  // the queue keeps growing. The bound is read from the shared lowering type so
  // it cannot drift from the lowering rules or from the C ABI's identical guard.
  constexpr int kMaxLoweredMessages =
      static_cast<int>(std::extent<decltype(sonare::host::ExternalMidi1Lowered::messages)>::value);
  if (max_records > 0 && max_records < kMaxLoweredMessages) {
    throw WasmRangeError("drainExternalMidi: maxRecords must be at least " +
                         std::to_string(kMaxLoweredMessages) + " to guarantee forward progress");
  }
  if (max_records <= 0 || engine_.external_midi_pending_count() == 0) return out;
  sonare::host::ExternalMidiRecord record{};
  int out_count = 0;
  while (out_count + kMaxLoweredMessages <= max_records) {
    if (engine_.drain_external_midi(&record, 1) == 0) break;
    // Shared lowering: identical MIDI-1 rules across every host surface. With a
    // full record's worth of slots free, every lowered message fits the budget.
    const sonare::host::ExternalMidi1Lowered lowered =
        sonare::host::lower_external_midi_record(record);
    for (uint8_t m = 0; m < lowered.count; ++m) {
      const sonare::host::ExternalMidi1Message& msg = lowered.messages[m];
      val item = val::object();
      item.set("destinationId", static_cast<double>(record.destination_id));
      item.set("renderFrame", static_cast<double>(record.event.render_frame));
      val arr = val::array();
      for (uint8_t b = 0; b < msg.byte_count; ++b) arr.set(b, msg.bytes[b]);
      item.set("bytes", arr);
      out.set(out_count++, item);
    }
  }
#else
  (void)max_records_val;
#endif
  return out;
}

bool RealtimeEngineWasm::popExternalMidiToScratch() {
#if defined(SONARE_WITH_ARRANGEMENT)
  while (external_midi_lowered_index_ >= external_midi_lowered_scratch_.count) {
    if (engine_.drain_external_midi(&external_midi_record_scratch_, 1) == 0) return false;
    external_midi_lowered_scratch_ =
        sonare::host::lower_external_midi_record(external_midi_record_scratch_);
    external_midi_lowered_index_ = 0;
  }
  return true;
#else
  return false;
#endif
}

uint32_t RealtimeEngineWasm::externalMidiScratchDestinationId() const {
  return external_midi_record_scratch_.destination_id;
}

int64_t RealtimeEngineWasm::externalMidiScratchRenderFrame() const {
  return external_midi_record_scratch_.event.render_frame;
}

uint32_t RealtimeEngineWasm::externalMidiScratchByteWord() const {
  if (external_midi_lowered_index_ >= external_midi_lowered_scratch_.count) return 0;
  const auto& message = external_midi_lowered_scratch_.messages[external_midi_lowered_index_];
  return static_cast<uint32_t>(message.bytes[0]) | (static_cast<uint32_t>(message.bytes[1]) << 8u) |
         (static_cast<uint32_t>(message.bytes[2]) << 16u);
}

uint32_t RealtimeEngineWasm::externalMidiScratchByteCount() const {
  if (external_midi_lowered_index_ >= external_midi_lowered_scratch_.count) return 0;
  return external_midi_lowered_scratch_.messages[external_midi_lowered_index_].byte_count;
}

void RealtimeEngineWasm::consumeExternalMidiScratch() {
  if (external_midi_lowered_index_ < external_midi_lowered_scratch_.count) {
    ++external_midi_lowered_index_;
  }
}

void RealtimeEngineWasm::pushMidiInputNoteOn(const val& group_val, const val& channel_val,
                                             const val& note_val, const val& velocity_val,
                                             int64_t offset_in_block_samples) {
  const int group = checkedIntFromVal(group_val, "group");
  const int channel = checkedIntFromVal(channel_val, "channel");
  const int note = checkedIntFromVal(note_val, "note");
  const int velocity = checkedIntFromVal(velocity_val, "velocity");
  pushMidiInputEvent(group, channel, note, velocity, offset_in_block_samples, true);
}

void RealtimeEngineWasm::pushMidiInputNoteOff(const val& group_val, const val& channel_val,
                                              const val& note_val, const val& velocity_val,
                                              int64_t offset_in_block_samples) {
  const int group = checkedIntFromVal(group_val, "group");
  const int channel = checkedIntFromVal(channel_val, "channel");
  const int note = checkedIntFromVal(note_val, "note");
  const int velocity = checkedIntFromVal(velocity_val, "velocity");
  pushMidiInputEvent(group, channel, note, velocity, offset_in_block_samples, false);
}

void RealtimeEngineWasm::pushMidiInputCc(const val& group_val, const val& channel_val,
                                         const val& controller_val, const val& value_val,
                                         int64_t offset_in_block_samples) {
#if defined(SONARE_WITH_ARRANGEMENT)
  const int group = checkedIntFromVal(group_val, "group");
  const int channel = checkedIntFromVal(channel_val, "channel");
  const int controller = checkedIntFromVal(controller_val, "controller");
  const int value = checkedIntFromVal(value_val, "value");
  refuseLivePushArg("pushMidiInputCc",
                    sonare::midi::live_push_cc_error(group, channel, controller, value));
  pushMidiInputUmpInternal(sonare::midi::make_midi1_control_change(
                               static_cast<uint8_t>(group), static_cast<uint8_t>(channel),
                               static_cast<uint8_t>(controller), static_cast<uint8_t>(value)),
                           offset_in_block_samples, "pushMidiInputCc");
#else
  (void)group_val;
  (void)channel_val;
  (void)controller_val;
  (void)value_val;
  (void)offset_in_block_samples;
  throwMidiEngineUnavailable();
#endif
}

#if defined(SONARE_WITH_ARRANGEMENT)
void RealtimeEngineWasm::pushMidiInputUmpInternal(const sonare::midi::Ump& ump,
                                                  int64_t offset_in_block_samples,
                                                  const char* what) {
  if (!midi_input_source_enabled_) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                  std::string(what) + ": the MIDI input source is not enabled");
  }
  if (!midi_input_source_.push_event(ump, offset_in_block_samples)) {
    throw sonare::SonareException(sonare::ErrorCode::OutOfMemory,
                                  std::string(what) + ": the MIDI input queue is full");
  }
}
#endif

void RealtimeEngineWasm::pushMidiInputPitchBend(const val& group_val, const val& channel_val,
                                                const val& bend_val,
                                                int64_t offset_in_block_samples) {
  const int group = checkedIntFromVal(group_val, "group");
  const int channel = checkedIntFromVal(channel_val, "channel");
  // 14-bit, so the 7-bit check every sibling here uses would refuse most of the
  // range rather than narrow it.
  const int bend14 = checkedIntFromVal(bend_val, "bend14");
#if defined(SONARE_WITH_ARRANGEMENT)
  refuseLivePushArg("pushMidiInputPitchBend",
                    sonare::midi::live_push_pitch_bend_error(group, channel, bend14));
  pushMidiInputUmpInternal(sonare::midi::make_midi1_pitch_bend(static_cast<uint8_t>(group),
                                                               static_cast<uint8_t>(channel),
                                                               static_cast<uint16_t>(bend14)),
                           offset_in_block_samples, "pushMidiInputPitchBend");
#else
  (void)group;
  (void)channel;
  (void)bend14;
  (void)offset_in_block_samples;
  throwMidiEngineUnavailable();
#endif
}

void RealtimeEngineWasm::pushMidiInputChannelPressure(const val& group_val, const val& channel_val,
                                                      const val& pressure_val,
                                                      int64_t offset_in_block_samples) {
  const int group = checkedIntFromVal(group_val, "group");
  const int channel = checkedIntFromVal(channel_val, "channel");
  const int pressure = checkedIntFromVal(pressure_val, "pressure");
#if defined(SONARE_WITH_ARRANGEMENT)
  refuseLivePushArg("pushMidiInputChannelPressure",
                    sonare::midi::live_push_channel_pressure_error(group, channel, pressure));
  pushMidiInputUmpInternal(sonare::midi::make_midi1_channel_pressure(
                               static_cast<uint8_t>(group), static_cast<uint8_t>(channel),
                               static_cast<uint8_t>(pressure)),
                           offset_in_block_samples, "pushMidiInputChannelPressure");
#else
  (void)group;
  (void)channel;
  (void)pressure;
  (void)offset_in_block_samples;
  throwMidiEngineUnavailable();
#endif
}

void RealtimeEngineWasm::pushMidiInputPolyPressure(const val& group_val, const val& channel_val,
                                                   const val& note_val, const val& pressure_val,
                                                   int64_t offset_in_block_samples) {
  const int group = checkedIntFromVal(group_val, "group");
  const int channel = checkedIntFromVal(channel_val, "channel");
  const int note = checkedIntFromVal(note_val, "note");
  const int pressure = checkedIntFromVal(pressure_val, "pressure");
#if defined(SONARE_WITH_ARRANGEMENT)
  refuseLivePushArg("pushMidiInputPolyPressure",
                    sonare::midi::live_push_poly_pressure_error(group, channel, note, pressure));
  pushMidiInputUmpInternal(sonare::midi::make_midi1_poly_pressure(
                               static_cast<uint8_t>(group), static_cast<uint8_t>(channel),
                               static_cast<uint8_t>(note), static_cast<uint8_t>(pressure)),
                           offset_in_block_samples, "pushMidiInputPolyPressure");
#else
  (void)group;
  (void)channel;
  (void)note;
  (void)pressure;
  (void)offset_in_block_samples;
  throwMidiEngineUnavailable();
#endif
}

void RealtimeEngineWasm::pushMidiNoteOn(const val& destination_id_val, const val& group_val,
                                        const val& channel_val, const val& note_val,
                                        const val& velocity_val, const val& render_frame_val) {
  const uint32_t destination_id = checkedUintFromVal(destination_id_val, "destinationId");
  const int group = checkedIntFromVal(group_val, "group");
  const int channel = checkedIntFromVal(channel_val, "channel");
  const int note = checkedIntFromVal(note_val, "note");
  const int velocity = checkedIntFromVal(velocity_val, "velocity");
  pushMidiNote(destination_id, group, channel, note, velocity, renderFrameFromVal(render_frame_val),
               sonare::rt::CommandType::kMidiNoteOnImmediate);
}

void RealtimeEngineWasm::pushMidiNoteOff(const val& destination_id_val, const val& group_val,
                                         const val& channel_val, const val& note_val,
                                         const val& velocity_val, const val& render_frame_val) {
  const uint32_t destination_id = checkedUintFromVal(destination_id_val, "destinationId");
  const int group = checkedIntFromVal(group_val, "group");
  const int channel = checkedIntFromVal(channel_val, "channel");
  const int note = checkedIntFromVal(note_val, "note");
  const int velocity = checkedIntFromVal(velocity_val, "velocity");
  pushMidiNote(destination_id, group, channel, note, velocity, renderFrameFromVal(render_frame_val),
               sonare::rt::CommandType::kMidiNoteOffImmediate);
}

// Queues an immediate (live) MIDI control change to a MIDI destination. Mirrors
// the C ABI sonare_engine_push_midi_cc: the synthesized MIDI 1.0 CC reaches the
// registered host instrument at @p render_frame (-1 = immediate). Values are
// 7-bit; channel 0..15, group 0..15. The scalar fields are packed into arg.i
// using the encoding documented in rt/command.h (kMidiCcImmediate).
void RealtimeEngineWasm::pushMidiCc(const val& destination_id_val, const val& group_val,
                                    const val& channel_val, const val& controller_val,
                                    const val& value_val, const val& render_frame_val) {
  const uint32_t destination_id = checkedUintFromVal(destination_id_val, "destinationId");
  const int group = checkedIntFromVal(group_val, "group");
  const int channel = checkedIntFromVal(channel_val, "channel");
  const int controller = checkedIntFromVal(controller_val, "controller");
  const int value = checkedIntFromVal(value_val, "value");
  refuseLivePushArg("pushMidiCc",
                    sonare::midi::live_push_cc_error(group, channel, controller, value));
  sonare::rt::Command command{};
  command.type = sonare::rt::CommandType::kMidiCcImmediate;
  command.target_id = destination_id;
  command.sample_time = renderFrameFromVal(render_frame_val);
  command.arg.i = packMidiScalar(value, controller, channel, group);
  pushEngineCommand(engine_, command, "failed to queue MIDI CC command");
}

// Reads 1 to 4 UMP words from a JS array or Uint32Array. A word is idiomatically
// spelled `(0x4 << 28) | …` in JS, which is a signed int once bit 31 is set, so
// the whole 32-bit range is legal per element.
namespace {
size_t umpWordsFromVal(const val& words_val, uint32_t (&words)[4], const char* what) {
  const size_t count = wasmArrayLikeLength(words_val, "words");
  if (count < 1 || count > 4) {
    throw WasmRangeError(std::string(what) + ": words must contain 1..4 UMP words");
  }
  for (size_t i = 0; i < count; ++i) {
    words[i] = checkedWordFromVal(words_val[i], "words[]");
  }
  return count;
}
}  // namespace

// Queues one live UMP of 1 to 4 words to a MIDI destination at @p render_frame
// (-1 = immediate). Mirrors the C ABI sonare_engine_push_midi_ump: malformed
// messages and Data types (MT 0x3 / 0x5) are a RangeError, a full slot ring
// or command queue is transient OutOfMemory back-pressure.
//
// A control-change word reaches the CC binding table exactly as it would through
// pushMidiCc or a live input source: the engine resolves every live entry point
// through one kind-aware decoder.
void RealtimeEngineWasm::pushMidiUmp(const val& destination_id_val, const val& words_val,
                                     const val& render_frame_val) {
  const uint32_t destination_id = checkedUintFromVal(destination_id_val, "destinationId");
  uint32_t words[4] = {0, 0, 0, 0};
  const size_t count = umpWordsFromVal(words_val, words, "pushMidiUmp");
  switch (
      engine_.push_midi_ump(destination_id, words, count, renderFrameFromVal(render_frame_val))) {
    case sonare::engine::MidiUmpPushResult::kQueued:
      return;
    case sonare::engine::MidiUmpPushResult::kInvalidMessage:
      throw WasmRangeError(std::string("pushMidiUmp: ") + sonare::midi::kLivePushUmpShapeError);
    case sonare::engine::MidiUmpPushResult::kSlotsFull:
    case sonare::engine::MidiUmpPushResult::kQueueFull:
      break;
  }
  throw sonare::SonareException(sonare::ErrorCode::OutOfMemory, "failed to queue MIDI UMP command");
}

// Pushes one raw UMP of 1 to 4 words to the engine-owned MIDI input source.
// Mirrors the C ABI sonare_engine_push_midi_input_ump.
void RealtimeEngineWasm::pushMidiInputUmp(const val& words_val, int64_t offset_in_block_samples) {
  uint32_t words[4] = {0, 0, 0, 0};
  const size_t count = umpWordsFromVal(words_val, words, "pushMidiInputUmp");
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!sonare::engine::RealtimeEngine::is_pushable_midi_ump(words, count)) {
    throw WasmRangeError(std::string("pushMidiInputUmp: ") + sonare::midi::kLivePushUmpShapeError);
  }
  sonare::midi::Ump ump{};
  for (size_t i = 0; i < count; ++i) ump.words[i] = words[i];
  ump.word_count = static_cast<uint8_t>(count);
  ump.group = sonare::midi::ump_group_from_word0(words[0]);
  pushMidiInputUmpInternal(ump, offset_in_block_samples, "pushMidiInputUmp");
#else
  (void)offset_in_block_samples;
  throwMidiEngineUnavailable();
#endif
}

namespace {
struct ProgramArgs {
  int group;
  int channel;
  int program;
  bool bank_valid;
  int bank_msb;
  int bank_lsb;
};

// Reads the typed program-change arguments shared by the destination and input
// pushes; the range rule runs at the call site, after the build check.
ProgramArgs programArgsFromVal(const val& group_val, const val& channel_val, const val& program_val,
                               const val& bank_valid_val, const val& bank_msb_val,
                               const val& bank_lsb_val) {
  ProgramArgs args{};
  args.group = checkedIntFromVal(group_val, "group");
  args.channel = checkedIntFromVal(channel_val, "channel");
  args.program = checkedIntFromVal(program_val, "program");
  args.bank_valid = optionalBool(bank_valid_val, "bankValid").value_or(false);
  args.bank_msb = bank_msb_val.isUndefined() ? 0 : checkedIntFromVal(bank_msb_val, "bankMsb");
  args.bank_lsb = bank_lsb_val.isUndefined() ? 0 : checkedIntFromVal(bank_lsb_val, "bankLsb");
  return args;
}

#if defined(SONARE_WITH_ARRANGEMENT)
const char* programArgError(const ProgramArgs& args) {
  return sonare::midi::live_push_program_error(args.group, args.channel, args.program,
                                               args.bank_valid, args.bank_msb, args.bank_lsb);
}
#endif
}  // namespace

// Queues one typed live MIDI 2.0 Program Change (with optional bank) to a MIDI
// destination at @p render_frame (-1 = immediate). The words go through the
// same enqueue as pushMidiUmp, so the receiver's own Rx gates apply. Arguments
// are validated here because this path inherits no C-ABI guard.
void RealtimeEngineWasm::pushMidiProgram(const val& destination_id_val, const val& group_val,
                                         const val& channel_val, const val& program_val,
                                         const val& bank_valid_val, const val& bank_msb_val,
                                         const val& bank_lsb_val, const val& render_frame_val) {
  const uint32_t destination_id = checkedUintFromVal(destination_id_val, "destinationId");
  const ProgramArgs args = programArgsFromVal(group_val, channel_val, program_val, bank_valid_val,
                                              bank_msb_val, bank_lsb_val);
  const int64_t render_frame = renderFrameFromVal(render_frame_val);
#if defined(SONARE_WITH_ARRANGEMENT)
  refuseLivePushArg("pushMidiProgram", programArgError(args));
  const sonare::midi::Ump ump = sonare::midi::make_midi2_program_change(
      static_cast<uint8_t>(args.group), static_cast<uint8_t>(args.channel),
      static_cast<uint8_t>(args.program), static_cast<uint8_t>(args.bank_msb),
      static_cast<uint8_t>(args.bank_lsb), args.bank_valid);
  const auto result =
      engine_.push_midi_ump(destination_id, ump.words, ump.word_count, render_frame);
  if (result == sonare::engine::MidiUmpPushResult::kQueued) return;
  throw sonare::SonareException(sonare::ErrorCode::OutOfMemory,
                                "failed to queue MIDI program command");
#else
  (void)args;
  (void)destination_id;
  (void)render_frame;
  throwMidiEngineUnavailable();
#endif
}

// Pushes one typed MIDI 2.0 Program Change to the engine-owned MIDI input
// source. Mirrors the C ABI sonare_engine_push_midi_input_program.
void RealtimeEngineWasm::pushMidiInputProgram(const val& group_val, const val& channel_val,
                                              const val& program_val, const val& bank_valid_val,
                                              const val& bank_msb_val, const val& bank_lsb_val,
                                              int64_t offset_in_block_samples) {
  const ProgramArgs args = programArgsFromVal(group_val, channel_val, program_val, bank_valid_val,
                                              bank_msb_val, bank_lsb_val);
#if defined(SONARE_WITH_ARRANGEMENT)
  refuseLivePushArg("pushMidiInputProgram", programArgError(args));
  pushMidiInputUmpInternal(
      sonare::midi::make_midi2_program_change(
          static_cast<uint8_t>(args.group), static_cast<uint8_t>(args.channel),
          static_cast<uint8_t>(args.program), static_cast<uint8_t>(args.bank_msb),
          static_cast<uint8_t>(args.bank_lsb), args.bank_valid),
      offset_in_block_samples, "pushMidiInputProgram");
#else
  (void)args;
  (void)offset_in_block_samples;
  throwMidiEngineUnavailable();
#endif
}

void RealtimeEngineWasm::queueMidiUmp(uint32_t destination_id, uint32_t word0,
                                      int64_t render_frame) {
  sonare::rt::Command command{};
  command.type = sonare::rt::CommandType::kMidiUmpImmediate;
  command.target_id = destination_id;
  command.sample_time = render_frame;
  command.arg.i = static_cast<int64_t>(word0);
  pushEngineCommand(engine_, command, "failed to queue MIDI UMP command");
}

// The three per-note expression dimensions take the raw-UMP command path rather
// than the packed scalar one the note and CC entry points use, because a bend is
// 14 bits and that encoding has no room for it.
void RealtimeEngineWasm::pushMidiPitchBend(const val& destination_id_val, const val& group_val,
                                           const val& channel_val, const val& bend_val,
                                           const val& render_frame_val) {
  const uint32_t destination_id = checkedUintFromVal(destination_id_val, "destinationId");
  const int group = checkedIntFromVal(group_val, "group");
  const int channel = checkedIntFromVal(channel_val, "channel");
  const int bend14 = checkedIntFromVal(bend_val, "bend14");
#if defined(SONARE_WITH_ARRANGEMENT)
  refuseLivePushArg("pushMidiPitchBend",
                    sonare::midi::live_push_pitch_bend_error(group, channel, bend14));
  queueMidiUmp(
      destination_id,
      sonare::midi::make_midi1_pitch_bend(
          static_cast<uint8_t>(group), static_cast<uint8_t>(channel), static_cast<uint16_t>(bend14))
          .words[0],
      renderFrameFromVal(render_frame_val));
#else
  (void)destination_id;
  (void)group;
  (void)channel;
  (void)bend14;
  (void)render_frame_val;
  throwMidiEngineUnavailable();
#endif
}

void RealtimeEngineWasm::pushMidiChannelPressure(const val& destination_id_val,
                                                 const val& group_val, const val& channel_val,
                                                 const val& pressure_val,
                                                 const val& render_frame_val) {
  const uint32_t destination_id = checkedUintFromVal(destination_id_val, "destinationId");
  const int group = checkedIntFromVal(group_val, "group");
  const int channel = checkedIntFromVal(channel_val, "channel");
  const int pressure = checkedIntFromVal(pressure_val, "pressure");
#if defined(SONARE_WITH_ARRANGEMENT)
  refuseLivePushArg("pushMidiChannelPressure",
                    sonare::midi::live_push_channel_pressure_error(group, channel, pressure));
  queueMidiUmp(destination_id,
               sonare::midi::make_midi1_channel_pressure(static_cast<uint8_t>(group),
                                                         static_cast<uint8_t>(channel),
                                                         static_cast<uint8_t>(pressure))
                   .words[0],
               renderFrameFromVal(render_frame_val));
#else
  (void)destination_id;
  (void)group;
  (void)channel;
  (void)pressure;
  (void)render_frame_val;
  throwMidiEngineUnavailable();
#endif
}

void RealtimeEngineWasm::pushMidiPolyPressure(const val& destination_id_val, const val& group_val,
                                              const val& channel_val, const val& note_val,
                                              const val& pressure_val,
                                              const val& render_frame_val) {
  const uint32_t destination_id = checkedUintFromVal(destination_id_val, "destinationId");
  const int group = checkedIntFromVal(group_val, "group");
  const int channel = checkedIntFromVal(channel_val, "channel");
  const int note = checkedIntFromVal(note_val, "note");
  const int pressure = checkedIntFromVal(pressure_val, "pressure");
#if defined(SONARE_WITH_ARRANGEMENT)
  refuseLivePushArg("pushMidiPolyPressure",
                    sonare::midi::live_push_poly_pressure_error(group, channel, note, pressure));
  queueMidiUmp(destination_id,
               sonare::midi::make_midi1_poly_pressure(
                   static_cast<uint8_t>(group), static_cast<uint8_t>(channel),
                   static_cast<uint8_t>(note), static_cast<uint8_t>(pressure))
                   .words[0],
               renderFrameFromVal(render_frame_val));
#else
  (void)destination_id;
  (void)group;
  (void)channel;
  (void)note;
  (void)pressure;
  (void)render_frame_val;
  throwMidiEngineUnavailable();
#endif
}

// Queues an immediate (live) MIDI SysEx frame to a MIDI destination. @p data is
// the full message including the leading 0xF0 and trailing 0xF7, 1 byte up to
// RealtimeEngine::kMaxSysExPayloadBytes; its bytes are copied out of the
// Uint8Array before the call returns. Reaches the registered host instrument at
// @p render_frame (-1 = immediate). Mirrors the C ABI
// sonare_engine_push_midi_sysex.
void RealtimeEngineWasm::pushMidiSysex(const val& destination_id_val, val data,
                                       const val& render_frame_val) {
  const uint32_t destination_id = checkedUintFromVal(destination_id_val, "destinationId");
  std::vector<uint8_t> bytes = uint8ArrayToVector(data);
  // Distinguish the two rejection classes the C ABI reports (it bypasses the
  // C-ABI translation unit here, so the mapping is reproduced): malformed or
  // oversized requests and payloads the destination cannot prepare are a
  // RangeError, while full payload slots or a full command queue are
  // transient OutOfMemory back-pressure. The ceiling is the engine's own
  // constant, not a copy of its value, so raising it moves this guard with it.
  constexpr size_t kMaxSysExBytes = sonare::engine::RealtimeEngine::kMaxSysExPayloadBytes;
  if (bytes.empty() || bytes.size() > kMaxSysExBytes) {
    throw WasmRangeError("pushMidiSysex: data must contain 1.." + std::to_string(kMaxSysExBytes) +
                         " bytes");
  }
  if (!engine_.push_midi_sysex(destination_id, bytes.data(), bytes.size(),
                               renderFrameFromVal(render_frame_val))) {
    switch (engine_.last_midi_sysex_push_status()) {
      case sonare::engine::MidiSysExPushStatus::kInvalidPayload:
      case sonare::engine::MidiSysExPushStatus::kPreparationFailed:
        throw WasmRangeError("pushMidiSysex: the destination cannot prepare this SysEx");
      default:
        throw sonare::SonareException(sonare::ErrorCode::OutOfMemory,
                                      "failed to queue MIDI SysEx command");
    }
  }
}

// Queues a MIDI panic (all-notes-off) releasing every sounding note at
// @p render_frame (-1 = immediate). Mirrors the C ABI
// sonare_engine_push_midi_panic.
void RealtimeEngineWasm::pushMidiPanic(const val& render_frame_val) {
  sonare::rt::Command command{};
  command.type = sonare::rt::CommandType::kMidiAllNotesOff;
  command.target_id = 0;
  command.sample_time = renderFrameFromVal(render_frame_val);
  pushEngineCommand(engine_, command, "failed to queue MIDI panic command");
}

void RealtimeEngineWasm::pushMidiNote(uint32_t destination_id, int group, int channel, int note,
                                      int velocity, int64_t render_frame,
                                      sonare::rt::CommandType type) {
  refuseLivePushArg(
      type == sonare::rt::CommandType::kMidiNoteOnImmediate ? "pushMidiNoteOn" : "pushMidiNoteOff",
      sonare::midi::live_push_note_error(group, channel, note, velocity));
  sonare::rt::Command command{};
  command.type = type;
  command.target_id = destination_id;
  command.sample_time = render_frame;
  command.arg.i = packMidiScalar(velocity, note, channel, group);
  pushEngineCommand(engine_, command, "failed to queue MIDI note command");
}

void RealtimeEngineWasm::pushMidiInputEvent(int group, int channel, int note, int velocity,
                                            int64_t offset_in_block_samples, bool note_on) {
#if defined(SONARE_WITH_ARRANGEMENT)
  const char* what = note_on ? "pushMidiInputNoteOn" : "pushMidiInputNoteOff";
  refuseLivePushArg(what, sonare::midi::live_push_note_error(group, channel, note, velocity));
  const sonare::midi::Ump ump =
      note_on ? sonare::midi::make_midi1_note_on(
                    static_cast<uint8_t>(group), static_cast<uint8_t>(channel),
                    static_cast<uint8_t>(note), static_cast<uint8_t>(velocity))
              : sonare::midi::make_midi1_note_off(
                    static_cast<uint8_t>(group), static_cast<uint8_t>(channel),
                    static_cast<uint8_t>(note), static_cast<uint8_t>(velocity));
  pushMidiInputUmpInternal(ump, offset_in_block_samples, what);
#else
  (void)group;
  (void)channel;
  (void)note;
  (void)velocity;
  (void)offset_in_block_samples;
  (void)note_on;
  throwMidiEngineUnavailable();
#endif
}

void registerRealtimeEngineMidiIo(class_<RealtimeEngineWasm>& cls) {
  cls.function("setMidiInputSource", &RealtimeEngineWasm::setMidiInputSource)
      .function("clearMidiInputSource", &RealtimeEngineWasm::clearMidiInputSource)
      .function("midiInputPendingCount", &RealtimeEngineWasm::midiInputPendingCount)
      .function("pushMidiInputNoteOn", &RealtimeEngineWasm::pushMidiInputNoteOn)
      .function("pushMidiInputNoteOff", &RealtimeEngineWasm::pushMidiInputNoteOff)
      .function("pushMidiInputCc", &RealtimeEngineWasm::pushMidiInputCc)
      .function("pushMidiInputPitchBend", &RealtimeEngineWasm::pushMidiInputPitchBend)
      .function("pushMidiInputChannelPressure", &RealtimeEngineWasm::pushMidiInputChannelPressure)
      .function("pushMidiInputPolyPressure", &RealtimeEngineWasm::pushMidiInputPolyPressure)
      .function("pushMidiNoteOn", &RealtimeEngineWasm::pushMidiNoteOn)
      .function("pushMidiNoteOff", &RealtimeEngineWasm::pushMidiNoteOff)
      .function("pushMidiCc", &RealtimeEngineWasm::pushMidiCc)
      .function("pushMidiPitchBend", &RealtimeEngineWasm::pushMidiPitchBend)
      .function("pushMidiChannelPressure", &RealtimeEngineWasm::pushMidiChannelPressure)
      .function("pushMidiPolyPressure", &RealtimeEngineWasm::pushMidiPolyPressure)
      .function("pushMidiUmp", &RealtimeEngineWasm::pushMidiUmp)
      .function("pushMidiInputUmp", &RealtimeEngineWasm::pushMidiInputUmp)
      .function("pushMidiProgram", &RealtimeEngineWasm::pushMidiProgram)
      .function("pushMidiInputProgram", &RealtimeEngineWasm::pushMidiInputProgram)
      .function("pushMidiSysex", &RealtimeEngineWasm::pushMidiSysex)
      .function("pushMidiPanic", &RealtimeEngineWasm::pushMidiPanic)
      .function("setMidiDestinationExternal", &RealtimeEngineWasm::setMidiDestinationExternal)
      .function("setExternalMidiClockEnabled", &RealtimeEngineWasm::setExternalMidiClockEnabled)
      .function("drainExternalMidi", &RealtimeEngineWasm::drainExternalMidi)
      .function("popExternalMidiToScratch", &RealtimeEngineWasm::popExternalMidiToScratch)
      .function("externalMidiScratchDestinationId",
                &RealtimeEngineWasm::externalMidiScratchDestinationId)
      .function("externalMidiScratchRenderFrame",
                &RealtimeEngineWasm::externalMidiScratchRenderFrame)
      .function("externalMidiScratchByteWord", &RealtimeEngineWasm::externalMidiScratchByteWord)
      .function("externalMidiScratchByteCount", &RealtimeEngineWasm::externalMidiScratchByteCount)
      .function("consumeExternalMidiScratch", &RealtimeEngineWasm::consumeExternalMidiScratch)
      .function("externalMidiDroppedCount", &RealtimeEngineWasm::externalMidiDroppedCount)
      .function("externalMidiPendingCount", &RealtimeEngineWasm::externalMidiPendingCount);
}

#endif  // __EMSCRIPTEN__
