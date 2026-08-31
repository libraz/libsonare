#pragma once

/// @file sonare_c_project_instruments.h
/// @brief Instrument-driven bounce surfaces (callback host shim, built-in synth,
///        NativeSynth, SoundFont) for the headless arrangement C ABI. Included
///        via @ref sonare_c_project.h.

#include <stddef.h>
#include <stdint.h>

#include "sonare_c_project_core.h"
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
  /// Reported release / effect tail in samples for auto-length bounces. 0 = no tail.
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
  uint32_t destination_id;
  SonareInstrumentCallbacks callbacks;
} SonareInstrumentBinding;

#ifdef __cplusplus
static_assert(offsetof(SonareInstrumentBinding, destination_id) == 0,
              "InstrumentBinding.destination_id offset");
static_assert(offsetof(SonareInstrumentBinding, callbacks) ==
                  ((sizeof(uint32_t) + alignof(SonareInstrumentCallbacks) - 1u) &
                   ~(alignof(SonareInstrumentCallbacks) - 1u)),
              "InstrumentBinding.callbacks offset");
#endif

/// @brief Like @ref sonare_project_bounce, but registers each callback
///        instrument on the engine before rendering, so MIDI tracks routed to
///        those destinations render the host instrument's audio instead of
///        silence. @p instruments points to @p instrument_count bindings (may be
///        NULL / 0 for a silent MIDI bounce identical to sonare_project_bounce).
///        Deterministic for a fixed project + options + instrument behavior.
/// @note Callback instruments expose destination-level audio only. If tracks
///       sharing one callback destination feed different channel strips, this
///       function returns @ref SONARE_ERROR_NOT_SUPPORTED; use a zero-latency,
///       source-aware built-in / NativeSynth / SF2 binding, or one destination
///       per strip.
SonareError sonare_project_bounce_with_instruments(SonareProject* project,
                                                   const SonareProjectBounceOptions* options,
                                                   const SonareInstrumentBinding* instruments,
                                                   size_t instrument_count, float** out_interleaved,
                                                   size_t* out_len);

// ============================================================================
// Built-in instrument (minimal polyphonic oscillator synth)
// ============================================================================

/// @brief Oscillator waveform for the built-in synth (see
///        @ref SonareBuiltinSynthConfig). Out-of-range values fall back to sine.
typedef enum SONARE_ENUM_BASE {
  SONARE_SYNTH_WAVEFORM_SINE = 0,
  SONARE_SYNTH_WAVEFORM_SAW = 1,
  SONARE_SYNTH_WAVEFORM_SQUARE = 2,
  SONARE_SYNTH_WAVEFORM_TRIANGLE = 3,
} SonareSynthWaveform;

/// @brief Patch for the built-in minimal synth. Zero-initialize then override:
///        a zero-init config is sanitized into a usable sine patch (every field
///        is clamped to an audible range), so callers may fill only what they
///        need. This is a deliberately plain electronic sound source so MIDI
///        arrangements bounce to audio instead of silence; a richer instrument
///        bank is planned separately.
/// Every numeric field uses "0 (or non-positive) => default", so a zero-init
/// config is the default sine patch and callers override only what they need.
typedef struct {
  int waveform;     /* SonareSynthWaveform; 0 = sine. NOTE: a distinct enum from
                       SonareSynthOscWaveform (sonare_c_types_analysis.h), whose 0
                       means "keep base preset", not sine. Do not mix the two. */
  float gain;       /* master output gain (linear); 0 => 0.2 */
  float attack_ms;  /* ADSR attack in ms; 0 => 5 */
  float decay_ms;   /* ADSR decay in ms; 0 => 60 */
  float sustain;    /* ADSR sustain level [0,1]; 0 => 0.7 */
  float release_ms; /* ADSR release in ms; 0 => 120 */
  int polyphony;    /* max simultaneous voices; 0 => 16, clamped to [1, 64] */
} SonareBuiltinSynthConfig;

/// @brief Binds a built-in synth patch to a MIDI destination id (the value set
///        by @ref sonare_project_set_track_midi_destination). The default
///        destination is 0.
typedef struct {
  uint32_t destination_id;
  SonareBuiltinSynthConfig config;
} SonareBuiltinInstrumentBinding;

/// @brief Like @ref sonare_project_bounce, but renders MIDI tracks routed to the
///        given destinations through the built-in oscillator synth, so a
///        MIDI-only arrangement bounces to audible output without the caller
///        supplying its own instrument callbacks. @p instruments points to
///        @p instrument_count bindings (may be NULL / 0 for a silent bounce).
///        When @p options->total_frames <= 0 the render length is auto-derived
///        from the arrangement (musical end + the synth's release tail).
///        A positive total_frames is used as-is and does not auto-extend for
///        mixer FX / instrument tails.
///        Deterministic for a fixed project + options + patch.
SonareError sonare_project_bounce_with_builtin_instruments(
    SonareProject* project, const SonareProjectBounceOptions* options,
    const SonareBuiltinInstrumentBinding* instruments, size_t instrument_count,
    float** out_interleaved, size_t* out_len);

// ============================================================================
// NativeSynth instrument (patch-driven synthesizer; see SonareSynthPatch)
// ============================================================================

/// @brief Returns the NativeSynth preset catalog names separated by '\n'
///        ("sine", "saw-lead", "e-piano", "drum-kit", ...). Use these to
///        discover valid @ref SonareSynthPatch preset names instead of
///        hardcoding magic strings.
/// @details Pointer is owned by libsonare and remains valid for the program
///          lifetime; the caller must NOT free it (mirrors
///          @ref sonare_mastering_insert_names).
const char* sonare_synth_preset_names(void);

typedef enum SONARE_ENUM_BASE {
  SONARE_SYNTH_ENUM_ENGINE_MODE = 0,
  SONARE_SYNTH_ENUM_OSC_WAVEFORM = 1,
  SONARE_SYNTH_ENUM_FILTER_MODEL = 2,
  SONARE_SYNTH_ENUM_FILTER_OUTPUT = 3,
  SONARE_SYNTH_ENUM_BODY_TYPE = 4,
  SONARE_SYNTH_ENUM_MOD_SOURCE = 5,
  SONARE_SYNTH_ENUM_MOD_DESTINATION = 6,
  /// Built-in oscillator synth names, including the ``sawtooth`` alias.
  SONARE_SYNTH_ENUM_BUILTIN_WAVEFORM = 7
} SonareSynthEnumKind;

/// @brief Returns the canonical names for a @ref SonareSynthPatch enum
///        separated by '\n'. Unknown @p kind returns an empty string.
/// @details Pointer is owned by libsonare and remains valid for the program
///          lifetime; the caller must NOT free it.
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
///        exact preset. Unknown names return SONARE_ERROR_INVALID_PARAMETER.
SonareError sonare_synth_preset_patch(const char* name, SonareSynthPatch* out);

/// @brief Binds a NativeSynth patch to a MIDI destination id (the value set by
///        @ref sonare_project_set_track_midi_destination; default 0).
typedef struct {
  uint32_t destination_id;
  SonareSynthPatch patch;
  /// When non-zero, select melodic voices from incoming GM bank/program
  /// messages and route MIDI channel 10 through the GM drum-kit map. The patch
  /// remains the fallback for any unsupported mapping. Zero preserves the
  /// explicit single-patch behavior.
  uint8_t use_gm_programs;
} SonareSynthInstrumentBinding;

/// @brief Like @ref sonare_project_bounce, but renders MIDI tracks routed to
///        the given destinations through the patch-driven NativeSynth (the
///        full synthesizer: subtractive / FM / Karplus-Strong / modal /
///        additive / percussion / extended-waveguide piano engines plus the
///        realism layer). Each binding resolves its @ref SonareSynthPatch via
///        the preset catalog + field overrides; an invalid struct_version or
///        unknown preset name fails with SONARE_ERROR_INVALID_PARAMETER.
///        When @p options->total_frames <= 0 the render length is auto-derived
///        from the arrangement (musical end + the patch's release tail).
///        A positive total_frames is used as-is and does not auto-extend for
///        mixer FX / instrument tails.
///        Deterministic for a fixed project + options + patch.
SonareError sonare_project_bounce_with_synth_instruments(
    SonareProject* project, const SonareProjectBounceOptions* options,
    const SonareSynthInstrumentBinding* instruments, size_t instrument_count,
    float** out_interleaved, size_t* out_len);

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
  SONARE_SOURCE_BACKEND_SYNTH = 0,
  SONARE_SOURCE_BACKEND_SF2 = 1,
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
  uint8_t channel; /* MIDI channel 0-15 */
  uint8_t program; /* program number 0-127 */
  uint16_t bank;   /* effective SF2 bank (128 = drums) */
  int backend;     /* SonareSourceBackend */
  char preset_name[64];
} SonareSf2ProgramStatus;

/// @brief Enumerates every (channel, bank, program) combination the compiled
///        arrangement plays a note through, in first-use order, and reports
///        whether each resolves in the loaded SoundFont (GS variation/drum
///        fallbacks included) or would fall back to the built-in synth.
///        Bank select (CC0) and program-change events are tracked per
///        (destination, channel) in event order; channel 10 and GM2 CC0=120
///        rhythm parts resolve drums via bank 128. @p out may be NULL when
///        @p max_entries is 0 to query the
///        count: @p out_count always receives the TOTAL entry count and at most
///        @p max_entries entries are written.
SonareError sonare_project_soundfont_manifest(SonareProject* project, SonareSf2ProgramStatus* out,
                                              size_t max_entries, size_t* out_count);

/// @brief Versioned SF2 player patch for @ref
///        sonare_project_bounce_with_sf2_instruments. Zero-initialize then
///        override: every field uses "0 => default" (struct_version 0 is
///        treated as version 1; version 2 adds model-first selection; version 3
///        adds clearing the bank's default rig).
typedef struct {
  int struct_version;                    /* 0 or 1 => version 1; 3 => current version */
  float gain;                            /* master output gain (linear); 0 => 0.5 */
  int polyphony;                         /* max simultaneous voices; 0 => 48, clamped to [1, 64] */
  int prefer_model_for_modeled_families; /* v2: non-zero selects dedicated melodic models;
                                            drums remain SF2-first */
  int clear_bank_rig;                    /* v3: non-zero renders the instrument alone, without the
                                            amplifier the bank binds after an electric guitar's
                                            voice; 0 keeps it, so a file that asks for nothing
                                            still sounds complete */
} SonareSf2InstrumentConfig;

/// @brief Binds an SF2 player patch to a MIDI destination id (the value set by
///        @ref sonare_project_set_track_midi_destination; default 0).
typedef struct {
  uint32_t destination_id;
  SonareSf2InstrumentConfig config;
} SonareSf2InstrumentBinding;

/// @brief Like @ref sonare_project_bounce, but renders MIDI tracks routed to
///        the given destinations through a GS-compatible SoundFont player fed
///        by the project's loaded SoundFont (@ref
///        sonare_project_load_soundfont). Each bound player is multitimbral
///        (16 MIDI channels; channel 10 and GM2 CC0=120 rhythm parts as drums
///        via bank 128; GS NRPN part
///        edits and GS/GM SysEx resets honored); programs the SoundFont does
///        not cover — including bouncing with no SoundFont loaded at all —
///        play through the built-in synthesizer GM fallback bank (the
///        data-free floor. @ref sonare_project_soundfont_manifest reports
///        default SF2-first coverage; model-first bindings deliberately override
///        that report for their dedicated melodic families. Deterministic for a fixed project +
///        options + SoundFont + patch.
SonareError sonare_project_bounce_with_sf2_instruments(
    SonareProject* project, const SonareProjectBounceOptions* options,
    const SonareSf2InstrumentBinding* instruments, size_t instrument_count, float** out_interleaved,
    size_t* out_len);

#ifdef __cplusplus
}
#endif
