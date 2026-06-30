#pragma once

/// @file sonare_c_project_midi.h
/// @brief MIDI event PODs, CC-binding descriptors, packing / routing / learn
///        helpers, SMF / clip-file IO, and MIDI-FX bake for the headless
///        arrangement C ABI. Included via @ref sonare_c_project.h.

#include <stddef.h>
#include <stdint.h>

#include "sonare_c_project_core.h"
#include "sonare_c_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief A flat MIDI event POD bridging to sonare::arrangement::MidiClipEvent.
///
/// Layout rationale: this C POD is the stable public channel-voice event shape.
/// @p data0 and @p data1 are the first two UMP words of a channel-voice
/// message: callers building a note may pack
///   data0 = (0x2u << 28) | (status << 16) | (note << 8) | velocity, data1 = 0
/// (matching @ref make_midi1_note_on word[0]). The core stores these words in
/// the project and the compiler / SMF exporter interpret valid UMP channel-voice
/// packets as MIDI events. SysEx payloads imported from SMF are carried by an
/// internal side store and are preserved by project serialization / SMF export;
/// they are not constructible through this flat C event POD. @p ppq is the event
/// position within the clip source timeline (quarter notes). Prefer the
/// sonare_midi_* helpers below over hand-packing words.
typedef struct {
  double ppq;
  uint32_t data0;
  uint32_t data1;
} SonareMidiEventPod;

#ifdef __cplusplus
static_assert(sizeof(SonareMidiEventPod) == sizeof(double) + 2u * sizeof(uint32_t),
              "SonareMidiEventPod layout drift");
static_assert(offsetof(SonareMidiEventPod, ppq) == 0, "MidiEventPod.ppq offset");
static_assert(offsetof(SonareMidiEventPod, data0) == 8, "MidiEventPod.data0 offset");
static_assert(offsetof(SonareMidiEventPod, data1) == 12, "MidiEventPod.data1 offset");
#endif

/// @brief MIDI route config for @ref sonare_midi_route_events.
/// @details `-1` means any group/channel for filters and no remap for
///          `remap_channel`. Otherwise values must be in [0, 15].
typedef struct {
  int filter_group;
  int filter_channel;
  int remap_channel;
  int thru;
} SonareMidiRouteConfig;

#ifdef __cplusplus
static_assert(offsetof(SonareMidiRouteConfig, filter_group) == 0,
              "MidiRouteConfig.filter_group offset");
static_assert(offsetof(SonareMidiRouteConfig, filter_channel) == sizeof(int),
              "MidiRouteConfig.filter_channel offset");
static_assert(offsetof(SonareMidiRouteConfig, remap_channel) == 2u * sizeof(int),
              "MidiRouteConfig.remap_channel offset");
static_assert(offsetof(SonareMidiRouteConfig, thru) == 3u * sizeof(int),
              "MidiRouteConfig.thru offset");
static_assert(sizeof(SonareMidiRouteConfig) == 4u * sizeof(int),
              "SonareMidiRouteConfig layout drift");
#endif

/// @brief MIDI CC binding kind ordinals. Mirrors midi::CcBindingKind.
typedef enum {
  SONARE_MIDI_CC_CONTROL_CHANGE_7 = 0,
  SONARE_MIDI_CC_CONTROL_CHANGE_14 = 1,
  SONARE_MIDI_CC_RPN = 2,
  SONARE_MIDI_CC_NRPN = 3
} SonareMidiCcBindingKind;

/// @brief MIDI CC <-> automation binding descriptor for pure conversion helpers.
typedef struct {
  uint8_t cc_number;
  uint8_t channel;
  uint8_t kind;
  uint8_t cc_lsb_number;
  uint8_t selector_msb;
  uint8_t selector_lsb;
  uint16_t reserved;
  uint32_t param_id;
  float min_value;
  float max_value;
} SonareMidiCcBinding;

#ifdef __cplusplus
static_assert(offsetof(SonareMidiCcBinding, cc_number) == 0, "MidiCcBinding.cc_number offset");
static_assert(offsetof(SonareMidiCcBinding, channel) == 1, "MidiCcBinding.channel offset");
static_assert(offsetof(SonareMidiCcBinding, kind) == 2, "MidiCcBinding.kind offset");
static_assert(offsetof(SonareMidiCcBinding, cc_lsb_number) == 3,
              "MidiCcBinding.cc_lsb_number offset");
static_assert(offsetof(SonareMidiCcBinding, selector_msb) == 4,
              "MidiCcBinding.selector_msb offset");
static_assert(offsetof(SonareMidiCcBinding, selector_lsb) == 5,
              "MidiCcBinding.selector_lsb offset");
static_assert(offsetof(SonareMidiCcBinding, param_id) == 8, "MidiCcBinding.param_id offset");
static_assert(offsetof(SonareMidiCcBinding, min_value) == 12, "MidiCcBinding.min_value offset");
static_assert(offsetof(SonareMidiCcBinding, max_value) == 16, "MidiCcBinding.max_value offset");
static_assert(sizeof(SonareMidiCcBinding) == 20, "SonareMidiCcBinding layout drift");
#endif

/// @brief Result of @ref sonare_project_validate_midi_notes. @p ok is 1 when
///        every note-on in the clip has a matching note-off and vice versa, else
///        0; the unmatched counts are diagnostic.
typedef struct {
  int ok;
  uint32_t unmatched_note_ons;
  uint32_t unmatched_note_offs;
} SonareNotePairValidation;

// ============================================================================
// MIDI
// ============================================================================

/// @brief Replaces a MIDI clip's entire event list in the content store. Pass
///        @p count == 0 to clear. Events are stored opaquely (see
///        @ref SonareMidiEventPod).
SonareError sonare_project_set_midi_events(SonareProject* project, uint32_t clip_id,
                                           const SonareMidiEventPod* events, size_t count);

/// @brief Packs a MIDI 1.0 note-on event POD at @p ppq.
SonareError sonare_midi_note_on(double ppq, uint8_t group, uint8_t channel, uint8_t note,
                                uint8_t velocity, SonareMidiEventPod* out);

/// @brief Packs a MIDI 1.0 note-off event POD at @p ppq.
SonareError sonare_midi_note_off(double ppq, uint8_t group, uint8_t channel, uint8_t note,
                                 uint8_t velocity, SonareMidiEventPod* out);

/// @brief Packs a MIDI 1.0 control-change event POD at @p ppq.
SonareError sonare_midi_cc(double ppq, uint8_t group, uint8_t channel, uint8_t controller,
                           uint8_t value, SonareMidiEventPod* out);

/// @brief Packs a MIDI 1.0 poly-pressure event POD at @p ppq.
SonareError sonare_midi_poly_pressure(double ppq, uint8_t group, uint8_t channel, uint8_t note,
                                      uint8_t pressure, SonareMidiEventPod* out);

/// @brief Packs a MIDI 1.0 program-change event POD at @p ppq.
SonareError sonare_midi_program(double ppq, uint8_t group, uint8_t channel, uint8_t program,
                                SonareMidiEventPod* out);

/// @brief Returns the General MIDI Level 1 instrument name for @p program.
/// @details Returns NULL when @p program is outside [0,127]. The returned
///          pointer is owned by libsonare and valid for the program lifetime.
const char* sonare_midi_gm_instrument_name(int program);
/// @brief Reverse GM instrument lookup. Returns -1 when @p name is NULL or unknown.
int sonare_midi_gm_program_for_name(const char* name);
/// @brief Returns the GM family name for @p family [0,15], or NULL.
const char* sonare_midi_gm_family_name(int family);
/// @brief Returns the first GM program in @p family [0,15], or -1.
int sonare_midi_gm_family_first_program(int family);
/// @brief Returns the GM2 melodic instrument name for bank LSB + program.
const char* sonare_midi_gm2_instrument_name(int bank_lsb, int program);
/// @brief Returns the GM drum name for note [35,81], or NULL.
const char* sonare_midi_gm_drum_name(int note);
/// @brief Reverse GM drum lookup. Returns -1 when @p name is NULL or unknown.
int sonare_midi_gm_drum_note_for_name(const char* name);
/// @brief Returns the GM2 drum set name for bank LSB, or NULL.
const char* sonare_midi_gm2_drum_set_name(int bank_lsb);
/// @brief Returns the GM2 drum name for bank LSB + note, or NULL.
const char* sonare_midi_gm2_drum_name(int bank_lsb, int note);
/// @brief Returns the standard MIDI CC name for controller [0,127], or NULL.
const char* sonare_midi_cc_name(int controller);
/// @brief Reverse standard MIDI CC lookup. Returns -1 when @p name is NULL or unknown.
int sonare_midi_cc_index_for_name(const char* name);
/// @brief Returns a MIDI 2.0 registered per-note controller name, or NULL.
const char* sonare_midi_per_note_controller_name(int index);
/// @brief Lowers a bank/program selection to MIDI 1.0 bank MSB, bank LSB,
///        program-change events at @p ppq.
SonareError sonare_midi_bank_program(double ppq, uint8_t group, uint8_t channel, int bank_msb,
                                     int bank_lsb, int program, SonareMidiEventPod* out_events,
                                     size_t out_capacity, size_t* out_count);

/// @brief Routes MIDI events through the RT MidiRouter filter/remap/thru logic.
/// @details `out_events` receives at most `out_capacity` events. Overflow due to
///          the router fixed capacity or caller capacity is reported through
///          `out_overflowed` / `out_overflow_count` when those pointers are not
///          NULL. `out_count` is always required.
SonareError sonare_midi_route_events(const SonareMidiEventPod* events, size_t count,
                                     const SonareMidiRouteConfig* config,
                                     SonareMidiEventPod* out_events, size_t out_capacity,
                                     size_t* out_count, int* out_overflowed,
                                     uint32_t* out_overflow_count);

/// @brief Runs MIDI learn over an event stream and returns the learned binding.
/// @details Observes events in order using the native CcMap learn logic,
///          including 14-bit CC pair and RPN/NRPN selector assembly. Returns
///          SONARE_ERROR_INVALID_STATE if no binding is learned.
SonareError sonare_midi_cc_learn(const SonareMidiEventPod* events, size_t count, uint32_t param_id,
                                 float min_value, float max_value, uint8_t min_movement,
                                 SonareMidiCcBinding* out_binding);

/// @brief Converts a CC event to one automation breakpoint using a binding table.
SonareError sonare_midi_cc_to_breakpoint(const SonareMidiCcBinding* bindings, size_t binding_count,
                                         const SonareMidiEventPod* event,
                                         SonareAutomationPoint* out_point);

/// @brief Converts an automation parameter value back to a CC UMP event.
SonareError sonare_midi_param_to_cc(const SonareMidiCcBinding* bindings, size_t binding_count,
                                    uint32_t param_id, float unit_value, uint8_t group, double ppq,
                                    SonareMidiEventPod* out_event);

/// @brief Packs a MIDI 1.0 channel-pressure event POD at @p ppq.
SonareError sonare_midi_channel_pressure(double ppq, uint8_t group, uint8_t channel,
                                         uint8_t pressure, SonareMidiEventPod* out);

/// @brief Packs a MIDI 1.0 pitch-bend event POD at @p ppq. @p bend is 0..16383,
///        center = 8192.
SonareError sonare_midi_pitch_bend(double ppq, uint8_t group, uint8_t channel, uint16_t bend,
                                   SonareMidiEventPod* out);

/// @brief Imports an in-memory SMF byte buffer, adding one MIDI track + clip per
///        imported channel-voice track. @p out_first_clip_id (optional) receives
///        the id of the first added clip. Malformed input returns an error
///        without crashing.
SonareError sonare_project_import_smf(SonareProject* project, const uint8_t* bytes, size_t len,
                                      uint32_t* out_first_clip_id);

/// @brief Exports the project's tempo map + MIDI clips to an SMF byte buffer.
/// @param out_bytes Receives a heap byte array (free with @ref sonare_free_bytes).
/// @param out_len   Receives the byte length.
/// @note Valid MIDI 1.0/2.0 channel-voice UMP words stored via
///       @ref sonare_project_set_midi_events are emitted. MIDI 2.0-only events
///       that cannot be represented in SMF/MIDI 1.0 are dropped by the MIDI
///       exporter, matching @ref sonare::midi::export_smf.
SonareError sonare_project_export_smf(const SonareProject* project, uint8_t** out_bytes,
                                      size_t* out_len);

/// @brief Imports an in-memory MIDI 2.0 Clip File ("SMF2CLIP", MIDI Association
///        MIDI Clip File). Unlike SMF, the UMP-based container carries MIDI 2.0
///        channel-voice messages (16-bit velocity, 32-bit CC, per-note /
///        registered controllers, bank-valid Program Change) without loss.
///        Adds one MIDI track + clip. @p out_first_clip_id (optional) receives
///        the added clip id. Malformed input returns an error without crashing.
SonareError sonare_project_import_clip_file(SonareProject* project, const uint8_t* bytes,
                                            size_t len, uint32_t* out_first_clip_id);

/// @brief Exports the project's tempo map + MIDI clips to a MIDI 2.0 Clip File
///        byte buffer (single-clip container). MIDI 2.0-only events are written
///        WITHOUT loss — the reason to prefer this over @ref
///        sonare_project_export_smf when MIDI 2.0 fidelity matters.
/// @param out_bytes Receives a heap byte array (free with @ref sonare_free_bytes).
/// @param out_len   Receives the byte length.
SonareError sonare_project_export_clip_file(const SonareProject* project, uint8_t** out_bytes,
                                            size_t* out_len);

/// @brief Sets a MIDI clip's program / bank by inserting deterministic
///        MIDI-1.0 bank-select CC + program-change events at source PPQ 0.
///        Uses group 0 / channel 0. For explicit routing use
///        @ref sonare_project_set_program_on_channel.
///        @p program must be in [0,127]. @p bank may be -1 (no bank select) or
///        a 14-bit bank number [0,16383] encoded as CC0(MSB), CC32(LSB).
SonareError sonare_project_set_program(SonareProject* project, uint32_t clip_id, int program,
                                       int bank);

/// @brief Sets a MIDI clip's program / bank on a specific UMP group/channel.
///        Existing PPQ-0 bank-select/program events for that same group/channel
///        are replaced; other channels' program events are preserved.
SonareError sonare_project_set_program_on_channel(SonareProject* project, uint32_t clip_id,
                                                  uint8_t group, uint8_t channel, int program,
                                                  int bank);

/// @brief Applies a deterministic MIDI-FX transform to a clip's stored events.
///
/// Supported JSON object fields are optional:
///   - transpose_semitones: integer
///   - velocity_scale / velocity_offset / velocity_gamma: numbers
///   - quantize_ppq: positive PPQ grid, with quantize_strength in [0,1]
///   - chord_intervals: array of up to 8 integer semitone offsets
///   - arpeggiator_intervals: array of up to 16 integer semitone offsets (one
///     per step), with arpeggiator_step_ppq (positive PPQ between steps) and an
///     optional arpeggiator_gate_ppq (positive gate length, capped to the step;
///     defaults to the full step)
///   - humanize_ppq / humanize_velocity / seed: deterministic jitter controls
///
/// This is an offline/control-plane destructive bake over the clip's event list,
/// not a live RT insert chain. Malformed JSON returns
/// SONARE_ERROR_INVALID_FORMAT; invalid values return
/// SONARE_ERROR_INVALID_PARAMETER.
SonareError sonare_project_bake_midi_fx(SonareProject* project, uint32_t clip_id,
                                        const char* config_json);

/// @brief Backward alias for @ref sonare_project_bake_midi_fx.
/// @deprecated Use @ref sonare_project_bake_midi_fx for destructive clip edits,
///             or RealtimeEngine MIDI-FX APIs for live non-destructive inserts.
SonareError sonare_project_set_midi_fx(SonareProject* project, uint32_t clip_id,
                                       const char* config_json);

/// @brief Pre-flight check for hanging / unmatched notes in a MIDI clip.
///
/// Builds a transient view of the clip's stored events and reports whether every
/// note-on is matched by a note-off (FIFO per channel+note). Useful before
/// bouncing to catch a stuck note that would otherwise sustain. Does not mutate
/// the project. Returns SONARE_ERROR_INVALID_PARAMETER if @p project / @p out is
/// null or @p clip_id is not a MIDI clip.
SonareError sonare_project_validate_midi_notes(const SonareProject* project, uint32_t clip_id,
                                               SonareNotePairValidation* out);

#ifdef __cplusplus
}
#endif
