#pragma once

/// @file sonare_c_project_timeline.h
/// @brief Compiled project timelines and their installation into a caller-owned
///        realtime engine. Included via @ref sonare_c.h.
///
/// A @ref SonareProjectTimeline is the immutable playback snapshot that
/// @ref sonare_project_compile builds and discards: audio clips, MIDI clips,
/// automation, track lanes, tempo and time-signature segments, markers and the
/// mixer scene's per-track strips. It owns everything it references and never
/// reaches back into the project, so later project edits do not change it.
///
/// Feature gating: the symbols are always exported. Without
/// @c SONARE_WITH_ARRANGEMENT every function returns
/// @c SONARE_ERROR_NOT_SUPPORTED with its outputs zeroed.

#include "sonare_c_project_core.h"
#include "sonare_c_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Opaque compiled-timeline handle. Create with
///        @ref sonare_project_compile_timeline, destroy with
///        @ref sonare_project_timeline_destroy.
typedef struct SonareProjectTimeline SonareProjectTimeline;

/// @brief Compiles the project and keeps the resulting timeline.
/// @details Runs the same compile and fills the same diagnostics as
///          @ref sonare_project_compile. When compilation produced a timeline
///          (@c out_result->has_timeline non-zero) @p *out_timeline receives a
///          new handle; otherwise it is NULL and the call still returns
///          @c SONARE_OK, with the error diagnostics in @p out_result. Both
///          outputs are zeroed on entry. CONTROL thread.
/// @param project Project to compile.
/// @param out_result Receives the diagnostics; free with
///        @ref sonare_project_free_compile_result.
/// @param out_timeline Receives the new timeline handle, or NULL.
/// @return @c SONARE_ERROR_INVALID_PARAMETER for a NULL argument.
SonareError sonare_project_compile_timeline(SonareProject* project,
                                            SonareProjectCompileResult* out_result,
                                            SonareProjectTimeline** out_timeline);

/// @brief Destroys a timeline handle. NULL is safe.
/// @details An engine the timeline was applied to keeps its own reference, so
///          the handle may be destroyed right after
///          @ref sonare_engine_apply_project_timeline.
void sonare_project_timeline_destroy(SonareProjectTimeline* timeline);

/// @brief Installs a compiled timeline into a stopped realtime engine, all or
///        nothing.
/// @details CONTROL thread, never concurrent with @ref sonare_engine_process,
///          and only while the transport is stopped: a playing transport is
///          refused with @c SONARE_ERROR_INVALID_STATE and the engine is left
///          unchanged.
///
///          The timeline owns these engine domains and replaces them in full:
///          tempo and time-signature segments, markers, track lanes, track
///          automation lanes, audio clips, MIDI clips, and the per-track
///          strips of the project's mixer scene (each track routed to a scene
///          strip gets that strip; a track lane without one has its strip
///          released). A value set on one of these domains through a
///          low-level setter is overwritten by the next apply. Instruments,
///          buses, the master strip, metronome, loop and capture are not
///          timeline-owned and are never touched.
///
///          Instruments are not bound by this call. Bind them with the
///          existing destination setters (for example
///          @ref sonare_engine_set_builtin_instrument) using each MIDI track's
///          destination id from @ref sonare_project_set_track_midi_destination.
///
///          The engine keeps its own reference to the timeline until the next
///          successful apply or @ref sonare_engine_destroy, so the handle may
///          be destroyed immediately afterwards; marker names stay valid.
///          A later @ref sonare_engine_set_automation_lane keeps the applied
///          lanes and replaces only a lane with the same target id.
///          The engine's prepared sample rate must equal the project rate used
///          to compile the timeline. Open the device and choose the project
///          rate before compiling. After applying, a prepare at another rate
///          returns INVALID_STATE until both audio and MIDI clip schedules are
///          cleared with their setters; then prepare and compile at the new rate.
///
///          Every validation error is reported before the first change and
///          leaves the engine unchanged. A failure after the first change --
///          @c SONARE_ERROR_OUT_OF_MEMORY, or an engine refusing a strip it
///          had already validated, reported as @c SONARE_ERROR_INVALID_STATE
///          -- leaves every timeline-owned domain empty (no clips, lanes,
///          markers or automation; 120 BPM in 4/4). It never leaves a mix of
///          the previous and the new timeline.
/// @param engine Prepared engine.
/// @param timeline Timeline from @ref sonare_project_compile_timeline.
/// @return @c SONARE_ERROR_INVALID_PARAMETER for a NULL argument or a timeline
///         the engine cannot hold (for example more track lanes than it
///         supports, a sample-rate mismatch, or a scene strip it cannot build);
///         @c SONARE_ERROR_INVALID_STATE for an unprepared engine or while the
///         transport is playing;
///         @c SONARE_ERROR_NOT_SUPPORTED when one scene strip is routed from
///         several tracks, because live playback runs one strip per track.
SonareError sonare_engine_apply_project_timeline(SonareRealtimeEngine* engine,
                                                 const SonareProjectTimeline* timeline);

#ifdef __cplusplus
}
#endif
