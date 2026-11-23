#pragma once

/// @file sonare_c_project_instruments.h
/// @brief Instrument-driven bounce surfaces (callback host shim, built-in synth,
///        NativeSynth, SoundFont) for the headless arrangement C ABI. Included
///        via @ref sonare_c_project.h.

#include <stddef.h>
#include <stdint.h>

#include "sonare_c_project_core.h"
#include "sonare_c_sample_bank.h"
#include "sonare_c_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// External-instrument host shim (callback-driven)
// ============================================================================

/// @brief Callback table a host supplies to drive an EXTERNAL instrument as the
///        engine's MIDI-driven instrument during a bounce. The bounce engine
///        treats it as a sonare::midi::MidiInstrument: @p prepare runs once on
///        the control thread, then per render block the engine delivers each
///        dispatched UMP event to @p on_event at its sample-accurate render
///        frame and calls @p render to sum the instrument's audio. Invariant 6:
///        only opaque words / buffers cross this seam — NO plugin-SDK type.
///
/// All callbacks receive @p user_data verbatim. @p render is REQUIRED; the
/// others may be NULL. The table (and any state @p user_data points to) must
/// outlive the bounce call. The callbacks run on the bounce's render thread and
/// must not free the project.
typedef struct {
  /// Opaque pointer passed verbatim as the first argument of every callback.
  void* user_data;
  /// CONTROL thread (once, before rendering): negotiated sample rate + max block.
  void (*prepare)(void* user_data, double sample_rate, int max_block_size);
  /// RENDER thread: one dispatched channel-voice UMP event for @p destination_id
  /// at @p render_frame. @p ump_words points to @p word_count (1 or 2) 32-bit
  /// UMP words; the pointer is valid only for the duration of the call.
  void (*on_event)(void* user_data, uint32_t destination_id, const uint32_t* ump_words,
                   int word_count, int64_t render_frame);
  /// RENDER thread: ADD @p num_frames of the instrument's audio into the planar
  /// @p channels (channels[ch][i]); the engine zero-fills the scratch first.
  void (*render)(void* user_data, float* const* channels, int num_channels, int num_frames);
  /// Reported instrument latency in samples (PDC). 0 = no latency.
  int latency_samples;
  /// Reported release / effect tail in samples for auto-length bounces. 0 = no tail;
  /// INT_MAX = unbounded, which makes an auto-length bounce fail.
  int tail_samples;
} SonareInstrumentCallbacks;

#ifdef __cplusplus
static_assert(offsetof(SonareInstrumentCallbacks, user_data) == 0,
              "InstrumentCallbacks.user_data offset");
static_assert(offsetof(SonareInstrumentCallbacks, prepare) == sizeof(void*),
              "InstrumentCallbacks.prepare offset");
static_assert(offsetof(SonareInstrumentCallbacks, latency_samples) == 4u * sizeof(void*),
              "InstrumentCallbacks.latency_samples offset");
static_assert(offsetof(SonareInstrumentCallbacks, tail_samples) == 4u * sizeof(void*) + sizeof(int),
              "InstrumentCallbacks.tail_samples offset");
static_assert(sizeof(SonareInstrumentCallbacks) == 4u * sizeof(void*) + 2u * sizeof(int),
              "SonareInstrumentCallbacks layout drift");
#endif

/// @brief Binds a callback instrument to a MIDI destination id (the value set by
///        @ref sonare_project_set_track_midi_destination and stamped onto the
///        track's compiled clips). The default destination is 0.
typedef struct {
  uint32_t destination_id;              ///< MIDI destination id the instrument serves.
  SonareInstrumentCallbacks callbacks;  ///< Callback table; copied for the bounce call.
} SonareInstrumentBinding;

#ifdef __cplusplus
static_assert(offsetof(SonareInstrumentBinding, destination_id) == 0,
              "InstrumentBinding.destination_id offset");
static_assert(offsetof(SonareInstrumentBinding, callbacks) ==
                  ((sizeof(uint32_t) + alignof(SonareInstrumentCallbacks) - 1u) &
                   ~(alignof(SonareInstrumentCallbacks) - 1u)),
              "InstrumentBinding.callbacks offset");
#endif

// ============================================================================
// Built-in instrument (minimal polyphonic oscillator synth)
// ============================================================================

/// @brief Oscillator waveform for the built-in synth (see
///        @ref SonareBuiltinSynthConfig). A value outside this set is rejected
///        with @c SONARE_ERROR_INVALID_PARAMETER, not substituted.
typedef enum SONARE_ENUM_BASE {
  SONARE_SYNTH_WAVEFORM_SINE = 0,      ///< Sine wave.
  SONARE_SYNTH_WAVEFORM_SAW = 1,       ///< Sawtooth wave.
  SONARE_SYNTH_WAVEFORM_SQUARE = 2,    ///< Square wave.
  SONARE_SYNTH_WAVEFORM_TRIANGLE = 3,  ///< Triangle wave.
} SonareSynthWaveform;
/// Number of @ref SonareSynthWaveform values.
#define SONARE_SYNTH_WAVEFORM_COUNT 4
#ifdef __cplusplus
static_assert(SONARE_SYNTH_WAVEFORM_TRIANGLE + 1 == SONARE_SYNTH_WAVEFORM_COUNT,
              "SonareSynthWaveform count changed");
#endif

/// @brief Patch for the built-in minimal synth. Start from @ref sonare_builtin_synth_config_init
///        and override what you need; every field is then read literally and refused outside its
///        domain with SONARE_ERROR_INVALID_PARAMETER, never clamped or replaced by a default.
///        This is a deliberately plain electronic sound source so MIDI arrangements bounce to
///        audio instead of silence; a richer instrument bank is planned separately.
typedef struct {
  uint32_t struct_size;  ///< sizeof(SonareBuiltinSynthConfig); set by the init function.
  /// A @ref SonareSynthWaveform selecting the oscillator; outside the enum refused. Default
  /// SONARE_SYNTH_WAVEFORM_SINE. A distinct enum from SonareSynthOscWaveform
  /// (sonare_c_types_analysis.h), whose 0 means "keep base preset", not sine. Do not mix the two.
  int waveform;
  float gain;       ///< Master output gain (linear); in (0, 4]. Default 0.2.
  float attack_ms;  ///< ADSR attack in ms; in (0, 20000]. Default 5. Linear ramp, full level at the
                    ///< set time.
  float decay_ms;   ///< ADSR decay in ms; in (0, 20000]. Default 60. Linear ramp: the set time is a
                    ///< full 0 to 1 sweep.
  float sustain;    ///< ADSR sustain level; in (0, 1]. Default 0.7.
  float release_ms;  ///< ADSR release in ms; in (0, 20000]. Default 120. Linear ramp, as decay.
  int polyphony;     ///< Max simultaneous voices; in [1, 64]. Default 16.
} SonareBuiltinSynthConfig;

/// @brief Fills @p config with sizeof and the library defaults: a sine patch with gain 0.2,
///        attack 5 ms, decay 60 ms, sustain 0.7, release 120 ms and 16 voices. A NULL @p config
///        is ignored.
/// @details Allocates nothing and does not transfer ownership; every pointer it writes is NULL.
void sonare_builtin_synth_config_init(SonareBuiltinSynthConfig* config);

/// @brief Binds a built-in synth patch to a MIDI destination id (the value set
///        by @ref sonare_project_set_track_midi_destination). The default
///        destination is 0.
typedef struct {
  uint32_t destination_id;          ///< MIDI destination id the synth serves.
  SonareBuiltinSynthConfig config;  ///< Synth patch for that destination.
} SonareBuiltinInstrumentBinding;

// ============================================================================
// NativeSynth instrument (patch-driven synthesizer; see SonareSynthPatch)
// ============================================================================

/// @brief Returns the NativeSynth preset catalog names separated by '\n'
///        ("sine", "sawLead", "ePiano", "drumKit", ...). Use these to
///        discover valid @ref SonareSynthPatch preset names instead of
///        hardcoding magic strings.
/// @details Pointer is owned by libsonare and remains valid for the program
///          lifetime; the caller must NOT free it. Never NULL: a build without
///          arrangement support returns the empty string.
const char* sonare_synth_preset_names(void);

/// @brief Selects which synth enum @ref sonare_synth_enum_names lists.
typedef enum SONARE_ENUM_BASE {
  SONARE_SYNTH_ENUM_ENGINE_MODE = 0,      ///< Synth engine modes (SonareSynthEngineMode).
  SONARE_SYNTH_ENUM_OSC_WAVEFORM = 1,     ///< Patch oscillator waveforms (SonareSynthOscWaveform).
  SONARE_SYNTH_ENUM_FILTER_MODEL = 2,     ///< Patch filter models.
  SONARE_SYNTH_ENUM_FILTER_OUTPUT = 3,    ///< Patch filter output types.
  SONARE_SYNTH_ENUM_BODY_TYPE = 4,        ///< Patch body resonance types.
  SONARE_SYNTH_ENUM_MOD_SOURCE = 5,       ///< Mod-matrix sources.
  SONARE_SYNTH_ENUM_MOD_DESTINATION = 6,  ///< Mod-matrix destinations.
  /// Built-in oscillator synth names, including the ``sawtooth`` alias.
  SONARE_SYNTH_ENUM_BUILTIN_WAVEFORM = 7,
  /// How a device spells a gesture (@ref SonareControllerInput).
  SONARE_SYNTH_ENUM_CONTROLLER_INPUT = 8,
  /// What a gesture means (@ref SonareControllerAxis).
  SONARE_SYNTH_ENUM_CONTROLLER_AXIS = 9,
  /// What a channel does with an overlapping note-on (@ref SonareArticulation).
  SONARE_SYNTH_ENUM_ARTICULATION = 10,
  /// The per-note dimensions MPE carries (@ref SonareMpeDimension).
  SONARE_SYNTH_ENUM_MPE_DIMENSION = 11,
  /// Which note a channel-addressed value belongs to (@ref SonareNoteTracking).
  SONARE_SYNTH_ENUM_NOTE_TRACKING = 12,
  /// The patch's override of a sample's looping (@ref SonareSampleLoopMode).
  SONARE_SYNTH_ENUM_SAMPLE_LOOP_MODE = 13,
  /// The patch's override of a sample's key tracking (@ref SonareSampleKeyTrack).
  SONARE_SYNTH_ENUM_SAMPLE_KEY_TRACK = 14,
  /// How a voice's start phases and random streams restart (@ref SonareSynthRetrigger).
  SONARE_SYNTH_ENUM_RETRIGGER = 15,
  /// Where a part's rig comes from (@ref SonarePartRigMode).
  SONARE_SYNTH_ENUM_PART_RIG_MODE = 16,
  /// A sample's own looping, as SonareSampleDesc.loop_mode spells it. The names
  /// are not ordinals: "none" is 0, "continuous" 1 and "keyDown" 3.
  SONARE_SYNTH_ENUM_SAMPLE_DESC_LOOP_MODE = 17,
  /// The patch's LFO waveform, vibrato and tremolo LFOs (@ref SonareSynthLfoShape).
  SONARE_SYNTH_ENUM_LFO_SHAPE = 18
} SonareSynthEnumKind;

/// @brief Returns the canonical names for one synth enum separated by '\n'.
///        Unknown @p kind returns an empty string.
/// @details Pointer is owned by libsonare and remains valid for the program
///          lifetime; the caller must NOT free it. Never NULL: an unknown
///          @p kind returns the empty string.
const char* sonare_synth_enum_names(int kind);

/// @brief Converts a built-in oscillator synth waveform name to its C enum value.
/// @details Accepts the canonical names returned by
///          @ref sonare_synth_enum_names with
///          @ref SONARE_SYNTH_ENUM_BUILTIN_WAVEFORM, including the ``sawtooth``
///          alias. Returns -1 for NULL or an unknown name.
int sonare_synth_builtin_waveform_from_name(const char* name);

/// @brief Fills @p out with the named catalog preset: the preset name plus the
///        wrapper-section values (oscillator / filter / envelopes / LFO /
///        realism / bus), so hosts can inspect a preset and tweak fields
///        before binding it. Passing the result back unchanged selects the
///        exact preset. The engine section stays inside the preset, so
///        @c engine_param_count is 0 and @c engine_params is NULL: nothing in
///        @p out is library-allocated, so there is nothing to free (the
///        preset-name and pointer fields must not be freed). Unknown names
///        return SONARE_ERROR_INVALID_PARAMETER.
SonareError sonare_synth_preset_patch(const char* name, SonareSynthPatch* out);

/// @brief Fills @p patch with sizeof and the empty patch: no preset, every numeric field 0 (keep
///        the base) and no presence bit set. NULL is ignored.
/// @details Allocates nothing and does not transfer ownership; every pointer it writes is NULL.
void sonare_synth_patch_init(SonareSynthPatch* patch);

/// @brief Describes the engine-section fields @ref SonareSynthPatch
///        @c engine_params accepts for @p engine_mode (a SonareSynthEngineMode).
/// @details A JSON array in the field names of
///          @ref sonare_mastering_insert_param_info:
///          `[{"name","type","integer","min","max","default","unit","activeWhen"}, ...]`.
///          `name` is the key; `type` is `"number"` or `"boolean"` (a switch,
///          given as 0 or 1); `integer` is present and true only for a field
///          taking whole numbers; `min` / `max` are present only for a field
///          with a closed range, and a value outside it is refused rather than
///          clamped; `default` is the engine's base preset's value; `unit` is
///          always present, from the vocabulary listed there. `activeWhen` is
///          present only for a field that reaches the voice under conditions
///          on other patch fields: an array of alternatives, each an array of
///          `{"key","scope","relation","value"}`, and the field reaches the
///          voice when every condition of at least one alternative holds.
///          `scope` is `"engine"` (a key of this array) or `"patch"` (a
///          @ref SonareSynthPatch field by its binding spelling), `relation` is
///          one of `"eq"`, `"ne"`, `"gt"`, `"ge"`, `"lt"`, `"le"`, and `value`
///          is a number, a boolean (against a switch) or a vocabulary name
///          (against a wrapper enum such as `body`). What a field further
///          needs from a performance (velocity, a controller, overlapping
///          notes), or a value past which it saturates, is not stated. The default
///          and sample modes have no engine section and answer `[]`.
/// @param[out] out_json Receives the document; the caller releases it with
///   @ref sonare_free_string. Set to NULL on failure.
/// @return SONARE_OK; SONARE_ERROR_INVALID_PARAMETER for an out-of-range
///   @p engine_mode; SONARE_ERROR_NOT_SUPPORTED in a build without arrangement
///   support.
SonareError sonare_synth_engine_param_info(int engine_mode, char** out_json);

/// @brief Describes every numeric and enum @ref SonareSynthPatch field and the
///        @c one_shot switch, one descriptor per field; numeric rows take the layout of
///        @ref sonare_synth_engine_param_info.
/// @details `name` is the field's binding spelling (the name
///          @ref sonare_engine_resolve_instrument_automation_id takes where the
///          field is automatable); `min` / `max` are the range the field is
///          accepted in (a value outside it, or a non-finite one, is refused
///          when the patch is applied), and `default` is the init patch's value, which a zero
///          left in the field keeps when no preset is named. An enum field follows the numeric
///          rows with `type` "enum", `values` its tokens in ordinal order and `default` the
///          token of ordinal 0. The preset name and the mod matrix are not described here.
/// @param[out] out_json Receives the document; the caller releases it with
///   @ref sonare_free_string. Set to NULL on failure.
/// @return SONARE_OK, or SONARE_ERROR_NOT_SUPPORTED in a build without
///   arrangement support.
SonareError sonare_synth_patch_param_info(char** out_json);

/// @brief Returns the GS rhythm-set name a rhythm part's @p program selects
///        ("Standard", "Room", "Jazz", ...), or NULL when the module's own
///        tone map defines no set at that program.
/// @details Pointer is owned by libsonare and remains valid for the program
///          lifetime; the caller must NOT free it. The answer is the module's
///          own map, which is the newest one and therefore reaches every set
///          this build voices; a file that selects an older map reaches fewer.
const char* sonare_synth_gs_drum_kit_name(int program);

/// @brief Reports whether the GS rhythm set at @p program is voiced apart from
///        Standard: 1 when at least one drum note differs, 0 when the set
///        renders exactly as Standard, -1 when no set sits at @p program.
/// @details Derived by applying the set to every note's resolved patch and
///          comparing, so the answer follows the voicing rather than a list
///          that has to be kept in step with it. Four sets GS fills with
///          one-shots share the Standard voicing deliberately, so a host that
///          offers every set without this query offers four choices that
///          change nothing. A build without arrangement support returns -1.
int sonare_synth_gs_drum_kit_is_voiced_apart(int program);

/// @brief Reports whether melodic Bank Select @p bank on @p program is voiced
///        apart from the capital tone: 1 when the bank has a patch of its own,
///        0 when it resolves to the capital, -1 when either argument is outside
///        [0, 127] -- both are seven-bit MIDI values, so 128 is out of range
///        rather than the drum bank here.
/// @details Resolving an unvoiced variation to its capital is what GS
///          specifies, so a 0 here is correct behaviour rather than a gap —
///          but only this query separates it from a bank that is voiced, which
///          otherwise takes rendering both and comparing. Accepts the GS
///          Bank Select MSB and the GM2 LSB alike, since both address the same
///          variation. A build without arrangement support returns -1.
int sonare_synth_gs_variation_is_voiced_apart(int bank, int program);

/// @brief Binds a NativeSynth patch to a MIDI destination id (the value set by
///        @ref sonare_project_set_track_midi_destination; default 0).
typedef struct {
  uint32_t destination_id;  ///< MIDI destination id the synth serves.
  SonareSynthPatch patch;   ///< Patch for that destination; resolved through the preset catalog.
  /// When non-zero, select melodic voices from incoming GM bank/program
  /// messages and route MIDI channel 10 through the GM drum-kit map. The patch
  /// remains the fallback for any unsupported mapping. Zero preserves the
  /// explicit single-patch behavior.
  uint8_t use_gm_programs;
  /// Sample bank the SONARE_SYNTH_ENGINE_SAMPLE patch reads, or NULL. Borrowed
  /// for the call: it must outlive the bounce, and adding to it while the
  /// bounce runs is not allowed (see @ref sonare_c_sample_bank.h). A sample
  /// patch bound without a bank renders silence rather than failing, the same
  /// way a patch naming a keymap set the bank lacks does.
  SonareSampleBank* sample_bank;
} SonareSynthInstrumentBinding;

// ============================================================================
// SoundFont (SF2) instrument: host-supplied sample data, GS-compatible player
// ============================================================================

/// @brief Loads (parses) SF2 bytes into the project: presets / instruments /
///        sample headers plus the sample PCM converted to a float pool.
///        Replaces any previously loaded SoundFont. CONTROL thread; the bytes
///        are copied/decoded, so @p data may be freed after the call. On a
///        malformed file the project's SoundFont is left unchanged and
///        SONARE_ERROR_INVALID_FORMAT is returned (sonare_last_error_message
///        carries the parser detail); a file rejected against the resource
///        limits below returns the same code, because that too is a statement
///        about the supplied data. SONARE_ERROR_INVALID_PARAMETER is reserved
///        for a NULL project/@p data or a zero @p size, so a caller can tell a
///        broken file from a broken call — the same split
///        @ref sonare_engine_load_soundfont makes. Resource limits are
///        268,435,456 input bytes, 67,108,864 sample points, 536,870,912 peak
///        input-plus-decoded bytes, and 65,536 records per pdta table.
SonareError sonare_project_load_soundfont(SonareProject* project, const uint8_t* data, size_t size);

/// @brief Releases the project's loaded SoundFont (no-op when none is loaded).
SonareError sonare_project_clear_soundfont(SonareProject* project);

/// @brief Number of presets in the project's loaded SoundFont (0 when none).
SonareError sonare_project_soundfont_preset_count(SonareProject* project, size_t* out_count);

/// @brief Source backend a resolved MIDI program renders through: the loaded
///        SoundFont (kSf2) or the built-in synthesizer fallback (kSynth).
typedef enum SONARE_ENUM_BASE {
  SONARE_SOURCE_BACKEND_SYNTH = 0,  ///< Built-in synthesizer fallback.
  SONARE_SOURCE_BACKEND_SF2 = 1,    ///< The loaded SoundFont.
} SonareSourceBackend;

/// @brief One bounce-manifest entry: a (channel, bank, program) combination the
///        arrangement actually plays, with its SF2-first backend coverage.
///        `bank` is the effective SF2 bank (drum channels report 128, melodic
///        channels the CC0 variation bank). `preset_name` is the resolved SF2
///        preset name (GS fallback included), empty for SONARE_SOURCE_BACKEND_SYNTH.
///        SF2 is reported only when every scanned note-on for the combination
///        matches a playable SF2 key/velocity zone. This reports default
///        SF2-first coverage, not the optional model-first override in a
///        SonareSf2InstrumentConfig supplied to a bounce call.
typedef struct {
  uint8_t channel;       ///< MIDI channel 0-15.
  uint8_t program;       ///< Program number 0-127.
  uint16_t bank;         ///< Effective SF2 bank (128 = drums).
  int backend;           ///< A @ref SonareSourceBackend saying which source renders it.
  char preset_name[64];  ///< NUL-terminated resolved SF2 preset name; empty for the synth backend.
} SonareSf2ProgramStatus;

/// @brief Enumerates every (channel, bank, program) combination the compiled
///        arrangement plays a note through, in first-use order, and reports
///        whether each resolves in the loaded SoundFont (GS variation/drum
///        fallbacks included) or would fall back to the built-in synth.
///        Each destination's events drive the SoundFont player's own part
///        state in event order and every note-on is resolved by that player,
///        so bank select, program change, GS/GM resets, GS part SysEx (tone
///        number, rhythm part, receive channel and switches, key range,
///        velocity sense) and the kit fallback reach the report exactly as
///        they reach a bounce. A note-on the part refuses plays nothing and is
///        not reported. Channel 10 and GM2 CC0=120 rhythm parts resolve drums
///        via bank 128. @p out may be NULL when
///        @p max_entries is 0 to query the
///        count: @p out_count always receives the TOTAL entry count and at most
///        @p max_entries entries are written.
SonareError sonare_project_soundfont_manifest(SonareProject* project, SonareSf2ProgramStatus* out,
                                              size_t max_entries, size_t* out_count);

/// @brief SF2 player patch for @ref SonareProjectBounceOptions::sf2_instruments. Start from
///        @ref sonare_sf2_instrument_config_init and override what you need; every field is then
///        read literally and refused outside its domain with SONARE_ERROR_INVALID_PARAMETER.
typedef struct {
  uint32_t struct_size;  ///< sizeof(SonareSf2InstrumentConfig); set by the init function.
  /// Master output gain (linear); finite, in [0, 4]. Default 0.5; 0 is silence.
  float gain;
  /// Max simultaneous voices; in [1, 64]. Default 48.
  int polyphony;
  /// Non-zero selects dedicated melodic models; drums remain SF2-first; negative refused.
  /// Default 0.
  int prefer_model_for_modeled_families;
  /// Non-zero renders the instrument alone, without the amplifier the bank binds after an
  /// electric guitar's voice; 0 keeps it, so a file that asks for nothing still sounds complete;
  /// negative refused. Default 0.
  int clear_bank_rig;
  /// How the GS insertion effects are realised -- 0 modern (the library's own inserts),
  /// 1 classic (the measured hardware models); any other value is refused. Default 0.
  int gs_efx_realization;
} SonareSf2InstrumentConfig;

/// @brief Fills @p config with sizeof and the library defaults: gain 0.5, 48 voices, models not
///        preferred, the bank's rig kept and the modern GS insertion effects. A NULL @p config is
///        ignored.
/// @details Allocates nothing and does not transfer ownership; every pointer it writes is NULL.
void sonare_sf2_instrument_config_init(SonareSf2InstrumentConfig* config);

/// @brief Binds an SF2 player patch to a MIDI destination id (the value set by
///        @ref sonare_project_set_track_midi_destination; default 0).
typedef struct {
  uint32_t destination_id;           ///< MIDI destination id the player serves.
  SonareSf2InstrumentConfig config;  ///< Player patch for that destination.
} SonareSf2InstrumentBinding;

/// @brief Configuration kind persisted for a project MIDI destination.
typedef enum SONARE_ENUM_BASE {
  SONARE_PROJECT_INSTRUMENT_SYNTH = 0,    ///< NativeSynth patch.
  SONARE_PROJECT_INSTRUMENT_BUILTIN = 1,  ///< Built-in oscillator patch.
  SONARE_PROJECT_INSTRUMENT_SF2 = 2,      ///< SoundFont player configuration.
} SonareProjectInstrumentKind;

/// Resolve this MIDI bank member or program from the clip events.
#define SONARE_PROJECT_INSTRUMENT_FROM_CLIP (-1)

/// @brief Instrument and optional MIDI bank/program for a project destination.
/// @details Initialize with @ref sonare_project_instrument_init. Only the config
/// corresponding to @p kind may be non-null. Settings are copied by the setter.
typedef struct {
  uint32_t struct_size;                     ///< sizeof(SonareProjectInstrument).
  SonareProjectInstrumentKind kind;         ///< Configuration kind.
  const SonareSynthPatch* synth;            ///< Patch for a NativeSynth binding.
  const SonareBuiltinSynthConfig* builtin;  ///< Configuration for a built-in binding.
  const SonareSf2InstrumentConfig* sf2;     ///< Configuration for a SoundFont binding.
  const char* sound_font_uri;               ///< URI for a SoundFont binding, or NULL for default.
  int32_t use_gm_programs;                  ///< Nonzero enables GM programs for NativeSynth.
  int32_t bank_msb;                         ///< MIDI bank MSB in [0, 127], or FROM_CLIP.
  int32_t bank_lsb;                         ///< MIDI bank LSB in [0, 127], or FROM_CLIP.
  int32_t program;                          ///< MIDI program in [0, 127], or FROM_CLIP.
} SonareProjectInstrument;

/// @brief Initializes @p instrument with the default kind and no config or bank/program.
/// @details Initializes caller-provided storage and does not transfer ownership.
void sonare_project_instrument_init(SonareProjectInstrument* instrument);

/// @brief Sets a destination's persistent instrument as one undoable edit.
/// @details A NULL instrument clears the binding. A destination must be referenced by a track.
SonareError sonare_project_set_destination_instrument(SonareProject* project,
                                                      uint32_t destination_id,
                                                      const SonareProjectInstrument* instrument);

/// @brief An instrument and the ID of the destination it serves.
typedef struct {
  uint32_t destination_id;             ///< MIDI destination ID.
  uint32_t reserved;                   ///< Reserved for future use, written as zero.
  SonareProjectInstrument instrument;  ///< Config and nested pointers owned by the list.
} SonareProjectInstrumentBinding;

/// @brief Ordered snapshot of destination instruments.
typedef struct {
  SonareProjectInstrumentBinding* items;  ///< Bindings in ascending destination ID order.
  size_t count;                           ///< Number of bindings in @p items.
} SonareProjectInstrumentList;

/// @brief Copies the project's persistent destination instrument bindings.
/// @details Release @p out with @ref sonare_free_project_instrument_list.
SonareError sonare_project_destination_instruments(const SonareProject* project,
                                                   SonareProjectInstrumentList* out);

/// @brief Releases all bindings and clears the list. NULL is ignored.
void sonare_free_project_instrument_list(SonareProjectInstrumentList* list);

/// @brief A list of owned UTF-8 strings.
typedef struct {
  char** items;  ///< Owned pointers to owned NUL-terminated strings.
  size_t count;  ///< Number of strings in @p items.
} SonareStringList;

/// @brief Copies distinct unresolved SoundFont URIs used by persistent instrument bindings.
/// @details Release @p out with @ref sonare_free_string_list.
SonareError sonare_project_unresolved_sound_font_uris(const SonareProject* project,
                                                      SonareStringList* out);

/// @brief Releases every string and the array, and clears the list. NULL is ignored.
void sonare_free_string_list(SonareStringList* list);

#ifdef __cplusplus
static_assert(offsetof(SonareProjectInstrument, synth) == 2u * sizeof(uint32_t),
              "SonareProjectInstrument.synth offset");
static_assert(sizeof(SonareProjectInstrument) == 6u * sizeof(uint32_t) + 4u * sizeof(void*),
              "SonareProjectInstrument layout drift");
#endif

// ============================================================================
// Bounce (compile + offline render, with hosted instruments)
// ============================================================================

/// The render length derived from the compiled arrangement (its musical end plus the longest
/// hosted-instrument tail: a sounded voice's release followed by the instrument's effect ring-out;
/// an SF2 / GM instrument reserves its system and part effect ring-out even when no note sounds).
/// Outside the length's real domain (a positive frame count), so it never reads as a length.
#define SONARE_PROJECT_BOUNCE_LENGTH_FROM_ARRANGEMENT (-1)

/// The output width of the scene master's layout (2 with no master). Outside the channel count's
/// real domain, so it never reads as a width.
#define SONARE_PROJECT_BOUNCE_CHANNELS_FROM_MASTER (-1)

/// The project's own sample rate. Outside the rate's real domain, so it never reads as a rate.
#define SONARE_PROJECT_SAMPLE_RATE_PROJECT (-1)

/// @brief Articulation ordinals. Mirrors midi::ArticulationMode.
/// @details What a channel does with a note-on while another note on the same
///          channel is still held.
typedef enum SONARE_ENUM_BASE {
  /// Every note-on takes its own voice.
  SONARE_ARTICULATION_POLY = 0,
  /// One note at a time; a new note-on stops the previous note and starts over.
  /// This is what GS MONO MODE and CC126 mean, and it is all they can reach.
  SONARE_ARTICULATION_MONO_RETRIGGER = 1,
  /// One note at a time, carried: a new note-on re-tunes the sounding voice
  /// instead of starting one, so the exciter and the amplitude envelope never
  /// restart. This is a wind player's slur, and it is deliberately unreachable
  /// from a spec-compliant GS file — no standard names it, and reading CC126 as
  /// this one would change what such a file sounds like.
  SONARE_ARTICULATION_MONO_LEGATO = 2
} SonareArticulation;

/// @brief Values in @ref SonareArticulation.
#define SONARE_ARTICULATION_COUNT 3

/// @brief Applies a named controller profile to a hosted MIDI destination.
typedef struct {
  uint32_t destination_id;  ///< ID of a hosted instrument destination.
  uint32_t reserved;        ///< Reserved for future use, set to zero.
  const char* preset_name;  ///< Name returned by @ref sonare_controller_profile_names.
} SonareProjectControllerProfileBinding;

/// @brief Applies a note articulation on a hosted MIDI destination and channel.
typedef struct {
  uint32_t destination_id;          ///< ID of a hosted instrument destination.
  uint32_t channel;                 ///< MIDI channel in [0, 15].
  SonareArticulation articulation;  ///< Mode for overlapping notes on this channel.
  uint32_t reserved;                ///< Reserved for future use, set to zero.
} SonareProjectArticulationBinding;

/// @brief Options for @ref sonare_project_bounce. Start from
///        @ref sonare_project_bounce_options_init; every field is then read literally and
///        checked against the range given beside it. The four instrument lists host the
///        instruments MIDI tracks render through; a pointer may be NULL only with its count 0.
/// @note Pointer members stay the caller's; the call does not transfer ownership.
typedef struct {
  uint32_t struct_size;  ///< sizeof(SonareProjectBounceOptions); set by the init function.
  /// Render length in frames at @p sample_rate; positive. Default
  /// @ref SONARE_PROJECT_BOUNCE_LENGTH_FROM_ARRANGEMENT, which derives it.
  int64_t total_frames;
  /// Render block size in frames; positive. Default 128.
  int block_size;
  /// Output channel count: 1, 2, 6 or 8, at most the width of the scene master's layout
  /// (mono/stereo/no master allow 2). Default @ref SONARE_PROJECT_BOUNCE_CHANNELS_FROM_MASTER,
  /// the width of the scene master's layout (2 with no master). The master is mixed at this
  /// width; 1 folds a 2-channel master to 0.5(L+R).
  int num_channels;
  /// Sample rate in Hz; must equal the project sample rate. Default
  /// @ref SONARE_PROJECT_SAMPLE_RATE_PROJECT, the project's.
  int sample_rate;
  /// Host-instrument latency compensation in samples fed to the compiler; >= 0. Default 0.
  int instrument_latency_samples;
  /// Callback instruments (see @ref SonareInstrumentBinding): MIDI tracks routed to their
  /// destinations render the host instrument's audio instead of silence. Each needs a render
  /// callback. Callback instruments expose destination-level audio only: tracks sharing one
  /// callback destination that feed different channel strips are refused with
  /// SONARE_ERROR_NOT_SUPPORTED; use a zero-latency, source-aware built-in / NativeSynth / SF2
  /// binding, or one destination per strip. Default NULL.
  const SonareInstrumentBinding* instruments;
  size_t instrument_count;  ///< Entries in @p instruments. Default 0.
  /// Built-in oscillator synths, so a MIDI-only arrangement bounces to audible output without
  /// callbacks. Default NULL.
  const SonareBuiltinInstrumentBinding* builtin_instruments;
  size_t builtin_instrument_count;  ///< Entries in @p builtin_instruments. Default 0.
  /// Patch-driven NativeSynths (the full synthesizer: every @ref SonareSynthEngineMode engine
  /// plus the realism layer). Each patch resolves through the preset catalog + field overrides;
  /// a struct_size below sizeof, an unknown preset name or a refused engine param fails with
  /// SONARE_ERROR_INVALID_PARAMETER. Default NULL.
  const SonareSynthInstrumentBinding* synth_instruments;
  size_t synth_instrument_count;  ///< Entries in @p synth_instruments. Default 0.
  /// GS-compatible SoundFont players fed by the project's loaded SoundFont
  /// (@ref sonare_project_load_soundfont). Each player is multitimbral (16 MIDI channels;
  /// channel 10 and GM2 CC0=120 rhythm parts as drums via bank 128; GS NRPN part edits and GS/GM
  /// SysEx resets honored); programs the SoundFont does not cover, including bouncing with no
  /// SoundFont loaded, play through the built-in synthesizer GM fallback bank.
  /// @ref sonare_project_soundfont_manifest reports default SF2-first coverage. Default NULL.
  const SonareSf2InstrumentBinding* sf2_instruments;
  size_t sf2_instrument_count;  ///< Entries in @p sf2_instruments. Default 0.
  /// Optional click, mixed after the master chain when enabled is nonzero.
  /// NULL and a disabled config both render without clicks.
  const SonareEngineMetronomeConfig* metronome;
  const SonareProjectControllerProfileBinding*
      controller_profiles;                                ///< Profiles, or NULL if empty.
  size_t controller_profile_count;                        ///< One profile per destination.
  const SonareProjectArticulationBinding* articulations;  ///< Articulations, or NULL if empty.
  size_t articulation_count;  ///< One articulation per destination and MIDI channel.
  int32_t count_in_bars;      ///< Bars of pre-roll in [0, 16], silent without a click.
  uint32_t reserved;          ///< Reserved for future use, set to zero.
} SonareProjectBounceOptions;

/// @brief Writes every default into @p options and sets its @c struct_size; NULL is ignored.
void sonare_project_bounce_options_init(SonareProjectBounceOptions* options);

#ifdef __cplusplus
static_assert(sizeof(SonareProjectControllerProfileBinding) == 2 * sizeof(uint32_t) + sizeof(void*),
              "SonareProjectControllerProfileBinding layout drift");
static_assert(sizeof(SonareProjectArticulationBinding) == 4 * sizeof(uint32_t),
              "SonareProjectArticulationBinding layout drift");
static_assert(offsetof(SonareProjectBounceOptions, struct_size) == 0,
              "BounceOptions.struct_size offset");
static_assert(offsetof(SonareProjectBounceOptions, total_frames) == sizeof(int64_t),
              "BounceOptions.total_frames offset");
static_assert(offsetof(SonareProjectBounceOptions, instruments) ==
                  2 * sizeof(int64_t) + 4 * sizeof(int),
              "BounceOptions.instruments offset");
static_assert(offsetof(SonareProjectBounceOptions, count_in_bars) ==
                  2 * sizeof(int64_t) + 4 * sizeof(int) + 13 * sizeof(void*),
              "BounceOptions.count_in_bars offset");
// The tail is padded to the int64_t alignment where pointers are 4 bytes (wasm32).
static_assert(sizeof(SonareProjectBounceOptions) ==
                  (offsetof(SonareProjectBounceOptions, count_in_bars) + 2 * sizeof(uint32_t) +
                   alignof(int64_t) - 1) /
                      alignof(int64_t) * alignof(int64_t),
              "SonareProjectBounceOptions layout drift");
#endif

/// @brief Returns the count-in length at the project's initial tempo and meter.
/// @param count_in_bars Number of initial bars, in [0, 16].
/// @param sample_rate The project sample rate, or @ref SONARE_PROJECT_SAMPLE_RATE_PROJECT.
/// @param out_frames Receives the count-in length in frames.
SonareError sonare_project_count_in_frames(const SonareProject* project, int32_t count_in_bars,
                                           int sample_rate, int64_t* out_frames);

/// @brief Compiles + renders the project offline to interleaved float audio.
/// @param options Render options, started from @ref sonare_project_bounce_options_init. May be
///        NULL for those defaults (project sample rate, the scene master's layout width, block
///        128).
/// @param out Receives a heap float array (free with @ref sonare_free_floats) of
///        @p out_frames * @p out_channels interleaved samples.
/// @param out_frames Receives the sample frames rendered (samples per channel).
/// @param out_channels Receives the width the render was mixed at: the requested
///        num_channels, or the resolved default when it was
///        @ref SONARE_PROJECT_BOUNCE_CHANNELS_FROM_MASTER. Written 0 first, so it is
///        defined on every return.
/// @details Deterministic: the same project + options + instrument behavior yields
///          bit-identical output within one build. Each instrument list in @p options is
///          optional. A per-call instrument takes precedence over a persistent instrument for
///          the same destination. Part rigs apply afterwards. MIDI destinations without an
///          instrument render silence. A destination named twice within the per-call lists
///          is refused with SONARE_ERROR_INVALID_PARAMETER.
///          The render window includes the count-in. A positive total_frames sets
///          the length from the start of the count-in; mixer FX /
///          instrument tails are auto-extended only when total_frames is
///          @ref SONARE_PROJECT_BOUNCE_LENGTH_FROM_ARRANGEMENT.
/// @return SONARE_ERROR_INVALID_PARAMETER for a refused option;
///         SONARE_ERROR_INVALID_STATE when the stored project cannot be
///         rendered (its derived length overflows, or the compiled arrangement
///         does not fit the engine or the scene's mixer).
SonareError sonare_project_bounce(SonareProject* project, const SonareProjectBounceOptions* options,
                                  float** out, size_t* out_frames, int* out_channels);

#ifdef __cplusplus
}
#endif
