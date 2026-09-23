#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sonare_c_sample_bank.h"
#include "sonare_c_types.h"
// Realtime tempo / time-signature ramps reuse the shared segment descriptors
// SonareProjectTempoSegment / SonareProjectTimeSignatureSegment.
#include "sonare_c_project_core.h"
#include "sonare_c_project_midi.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @section engine_threading Thread safety (RealtimeEngine)
/// @details Every handle has one AUDIO thread (the render callback) and one
/// CONTROL thread; more than one control thread needs external serialization.
///
/// AUDIO-thread functions (no allocation, no lock, no throw):
/// - `sonare_engine_process`, `sonare_engine_process_with_monitor`. They do not
///   touch the thread-local `sonare_last_error_message()` /
///   `sonare_last_warning_message()` channels, because first-touch TLS setup is
///   not realtime-safe — validate and read diagnostics on the control thread.
///
/// CONTROL-thread, realtime-safe hand-off (lock-free, safe alongside
/// `sonare_engine_process` on the same handle, adopted at the next block):
/// - Transport / tempo / loop: `sonare_engine_play`, `sonare_engine_stop`,
///   `sonare_engine_seek_sample`, `sonare_engine_seek_ppq`,
///   `sonare_engine_set_tempo`, `sonare_engine_set_time_signature`,
///   `sonare_engine_set_loop`, `sonare_engine_seek_marker`,
///   `sonare_engine_set_loop_from_markers`.
/// - Live parameter / MIDI: `sonare_engine_set_parameter`,
///   `sonare_engine_set_parameter_smoothed`, `sonare_engine_push_midi_cc`,
///   `sonare_engine_push_midi_panic`.
/// - Capture control: `sonare_engine_set_capture_buffer`,
///   `sonare_engine_arm_capture`, `sonare_engine_set_capture_punch`,
///   `sonare_engine_reset_capture`. The buffer given to `set_capture_buffer`
///   must outlive capture and must not be freed while armed, and
///   `reset_capture` clears the frame counter, so issue it while not capturing.
///
/// CONTROL-thread, NON realtime-safe (they allocate — issue them between renders
/// or while stopped, never concurrently with `sonare_engine_process`):
/// - Lifecycle: `sonare_engine_create`, `sonare_engine_destroy`,
///   `sonare_engine_prepare`.
/// - Topology / registration: `sonare_engine_set_graph`,
///   `sonare_engine_set_clips`, `sonare_engine_add_parameter`,
///   `sonare_engine_set_automation_lane`, `sonare_engine_set_markers`,
///   `sonare_engine_set_metronome`, `sonare_engine_set_track_lanes`,
///   `sonare_engine_set_track_strip_json`,
///   `sonare_engine_set_master_strip_json`.
/// - Live MIDI configuration: `sonare_engine_set_midi_fx`,
///   `sonare_engine_clear_midi_fx`, `sonare_engine_bind_midi_cc`,
///   `sonare_engine_clear_midi_cc_bindings`. These four publish immutable state
///   adopted at the next block, so they MAY run while `process` is active;
///   callbacks and note flushes stay on the audio thread.
/// - Offline render: `sonare_engine_render_offline`,
///   `sonare_engine_bounce_offline`, `sonare_engine_freeze_offline`. They own
///   the audio role internally, so never call them from a render callback.
///
/// CONTROL-thread read-back (concurrent-safe, consistent, may lag by one block):
/// - `sonare_engine_get_transport_state`, `sonare_engine_capture_status`,
///   `sonare_engine_parameter_*`, `sonare_engine_*_count`,
///   `sonare_engine_marker*`, `sonare_engine_metronome`,
///   `sonare_engine_drain_telemetry`, `sonare_engine_drain_meter_telemetry`.
///   The drain functions are single-consumer: drive them from one thread only.
SonareError sonare_engine_create(SonareRealtimeEngine** out);
void sonare_engine_destroy(SonareRealtimeEngine* engine);

/// @brief Largest @p command_capacity the prepare entry points accept.
#define SONARE_ENGINE_MAX_COMMAND_CAPACITY 65536u
/// @brief Largest @p telemetry_capacity the prepare entry points accept.
/// @details Considerably lower than the command bound because the engine fans
///   this number out internally: the meter tap reserves this many records for
///   every metered lane (each track and bus lane plus master and monitor), so
///   the memory prepare() reserves grows by nearly two orders of magnitude
///   beyond the number supplied here. Sizing this for a host's drain interval
///   rather than for headroom is the intent; the default both other bindings
///   use is 1024.
#define SONARE_ENGINE_MAX_TELEMETRY_CAPACITY 16384u

/// @brief Prepares an engine with 64-channel planar scratch.
/// @details @p command_capacity must not exceed @ref
///          SONARE_ENGINE_MAX_COMMAND_CAPACITY and @p telemetry_capacity must
///          not exceed @ref SONARE_ENGINE_MAX_TELEMETRY_CAPACITY; a larger
///          value returns SONARE_ERROR_INVALID_PARAMETER and leaves the engine
///          untouched. Both are rounded up to a power of two internally, and 0
///          selects the internal minimum rather than disabling the queue.
SonareError sonare_engine_prepare(SonareRealtimeEngine* engine, double sample_rate,
                                  int max_block_size, size_t command_capacity,
                                  size_t telemetry_capacity);
/// @brief Prepares an engine while bounding internal channel-planar scratch.
/// @details Unlike @ref sonare_engine_prepare, this entry point reserves only
///          @p max_channels (1..64) for capture, instrument, PDC, and monitor
///          scratch. The host must not pass more channels to process/render
///          until it prepares again with a larger bound. Use the legacy prepare
///          function when the maximum channel count is not known. The two
///          capacities carry the same bounds as @ref sonare_engine_prepare.
SonareError sonare_engine_prepare_with_channels(SonareRealtimeEngine* engine, double sample_rate,
                                                int max_block_size, size_t command_capacity,
                                                size_t telemetry_capacity, int max_channels);
SonareError sonare_engine_play(SonareRealtimeEngine* engine, int64_t render_frame);
SonareError sonare_engine_stop(SonareRealtimeEngine* engine, int64_t render_frame);
SonareError sonare_engine_seek_sample(SonareRealtimeEngine* engine, int64_t timeline_sample,
                                      int64_t render_frame);
SonareError sonare_engine_seek_ppq(SonareRealtimeEngine* engine, double ppq, int64_t render_frame);
/// @brief Snaps every in-flight parameter ramp (engine-level smoothed params,
///   mixer lane fader/pan/gate, bus gains) to its target value.
/// @details For offline rendering: call after a priming process() block (which
///   drains queued commands and applies automation at the seek position) so
///   the first audible block renders at settled values instead of ramping in
///   from defaults. Not safe concurrently with a running audio thread.
SonareError sonare_engine_settle_parameters(SonareRealtimeEngine* engine);
/// @brief Applies commands queued on an offline/control-only engine immediately.
/// @details For hosts that never call @ref sonare_engine_process (e.g. a
///   control-only mirror driving transport/automation state without rendering
///   audio): the realtime command ring is bounded, so queued commands must be
///   drained explicitly instead of relying on the next process() call to do it.
/// @warning Not safe concurrently with a running @ref sonare_engine_process.
SonareError sonare_engine_flush_control_commands(SonareRealtimeEngine* engine);
/// @brief Sets a finite tempo in the range (0, 100000] BPM.
/// @return @ref SONARE_ERROR_INVALID_PARAMETER for a non-finite, non-positive,
///         or greater-than-100000 value.
SonareError sonare_engine_set_tempo(SonareRealtimeEngine* engine, double bpm);
SonareError sonare_engine_set_time_signature(SonareRealtimeEngine* engine, int numerator,
                                             int denominator);
/// @brief Installs a tempo map from @p segment_count ramp segments (control
///   thread only). Each segment needs a finite non-negative @c start_ppq and a
///   @c bpm in (0, 100000]; a non-zero @c end_bpm ramps to a value in the same
///   range. Passing zero segments clears the map back to the single-tempo
///   value. @c start_sample and @c end_ppq of the descriptor are ignored
///   (derived internally).
SonareError sonare_engine_set_tempo_segments(SonareRealtimeEngine* engine,
                                             const SonareProjectTempoSegment* segments,
                                             size_t segment_count);
/// @brief Installs a time-signature map from @p segment_count segments (control
///   thread only). Each segment needs a finite non-negative @c start_ppq and a
///   positive @c numerator / @c denominator.
SonareError sonare_engine_set_time_signature_segments(
    SonareRealtimeEngine* engine, const SonareProjectTimeSignatureSegment* segments,
    size_t segment_count);
SonareError sonare_engine_sample_at_ppq(SonareRealtimeEngine* engine, double ppq,
                                        int64_t* out_sample);
SonareError sonare_engine_set_loop(SonareRealtimeEngine* engine, double start_ppq, double end_ppq,
                                   int enabled);
/// @brief Registers a parameter's metadata for automation UIs.
/// @details Control-thread only (allocates the name/unit string copies). Returns
///   @c SONARE_ERROR_INVALID_PARAMETER if @p info is NULL, the value range is
///   inverted, @c default_curve is outside [0, 3], @c id falls in the engine's
///   internally reserved parameter-id namespace (the engine-param and
///   insert-param automation ids), or a parameter with the same id is already
///   registered (duplicate ids are rejected, not replaced — clear and
///   re-register to change metadata). On rejection no backing strings are
///   retained, so repeated re-registration does not leak.
SonareError sonare_engine_add_parameter(SonareRealtimeEngine* engine,
                                        const SonareParameterInfo* info);
/// @brief Removes all registered parameters and releases their backing strings.
/// @details Control-thread only. Use before re-registering a parameter id to
///   change its metadata (add() rejects duplicate ids). Not realtime-safe; do
///   not call concurrently with @ref sonare_engine_process.
SonareError sonare_engine_clear_parameters(SonareRealtimeEngine* engine);
SonareError sonare_engine_parameter_count(SonareRealtimeEngine* engine, size_t* out_count);
SonareError sonare_engine_parameter_info_by_index(SonareRealtimeEngine* engine, size_t index,
                                                  SonareParameterInfo* out);
SonareError sonare_engine_parameter_info(SonareRealtimeEngine* engine, uint32_t id,
                                         SonareParameterInfo* out);
/// @brief Replaces the automation lane driving @p param_id (control thread).
/// @details An empty lane (@p point_count == 0) leaves the target
///   undriven rather than snapping it to 0 or a default: once the audio
///   thread adopts the change, the target reverts to the last value
///   explicitly sent through @ref sonare_engine_set_parameter /
///   @ref sonare_engine_set_parameter_smoothed, or is left unchanged if no
///   such value was ever sent for @p param_id. This holds regardless of
///   whether the manual value or the lane clear reaches the audio thread
///   first.
SonareError sonare_engine_set_automation_lane(SonareRealtimeEngine* engine, uint32_t param_id,
                                              const SonareAutomationPoint* points,
                                              size_t point_count);
SonareError sonare_engine_automation_lane_count(SonareRealtimeEngine* engine, size_t* out_count);
/// @brief Atomically replaces all realtime-engine markers.
/// @details Marker ids must be positive and unique, and PPQ positions must be
///   finite and non-negative. An empty list clears the marker map. Any invalid
///   entry rejects the whole list and leaves the previous markers byte-for-byte
///   unchanged.
SonareError sonare_engine_set_markers(SonareRealtimeEngine* engine,
                                      const SonareEngineMarker* markers, size_t marker_count);
SonareError sonare_engine_marker_count(SonareRealtimeEngine* engine, size_t* out_count);
SonareError sonare_engine_marker_by_index(SonareRealtimeEngine* engine, size_t index,
                                          SonareEngineMarker* out);
SonareError sonare_engine_marker(SonareRealtimeEngine* engine, uint32_t id,
                                 SonareEngineMarker* out);
SonareError sonare_engine_seek_marker(SonareRealtimeEngine* engine, uint32_t marker_id,
                                      int64_t render_frame);
SonareError sonare_engine_set_loop_from_markers(SonareRealtimeEngine* engine,
                                                uint32_t start_marker_id, uint32_t end_marker_id);
/// @brief Sets the metronome configuration.
/// @details Explicit click lengths are limited to 384000 samples or one second.
///          The engine clamps accepted values further to one second at the
///          prepared sample rate.
/// @return @ref SONARE_ERROR_INVALID_PARAMETER for non-finite/negative gains,
///         negative durations, or durations above the documented limits.
SonareError sonare_engine_set_metronome(SonareRealtimeEngine* engine,
                                        const SonareEngineMetronomeConfig* config);
SonareError sonare_engine_metronome(SonareRealtimeEngine* engine, SonareEngineMetronomeConfig* out);
SonareError sonare_engine_count_in_end_sample(SonareRealtimeEngine* engine, int64_t start_sample,
                                              int bars, int64_t* out_sample);
SonareError sonare_engine_set_clips(SonareRealtimeEngine* engine, const SonareEngineClip* clips,
                                    size_t clip_count);
SonareError sonare_engine_clip_count(SonareRealtimeEngine* engine, size_t* out_count);
SonareError sonare_engine_set_track_lanes(SonareRealtimeEngine* engine,
                                          const SonareEngineTrackLane* lanes, size_t lane_count);
/// @brief Keys one insert of a lane strip from another lane's post-strip audio.
/// @details Sidechain for ducking/sidechainRouter inserts: the source lane's
///   most recent post-strip buffer feeds the insert's key input every block
///   (same-block when the source renders earlier, previous block otherwise).
///   @p source_track_id 0 removes the binding. Control-thread only; must not
///   be called concurrently with @ref sonare_engine_process.
SonareError sonare_engine_set_lane_sidechain(SonareRealtimeEngine* engine, uint32_t track_id,
                                             unsigned int insert_index, uint32_t source_track_id);

/// @brief Configure realtime engine aux buses used by track sends.
/// @details Control-thread only; must not run concurrently with process().
SonareError sonare_engine_set_track_buses(SonareRealtimeEngine* engine,
                                          const SonareEngineBus* buses, size_t bus_count);

/// @brief Configure a bus strip from the first bus in a mixer scene JSON.
/// @details The bus must already exist via sonare_engine_set_track_buses.
SonareError sonare_engine_set_bus_strip_json(SonareRealtimeEngine* engine, uint32_t bus_id,
                                             const char* scene_json);
/// @brief Builds an engine-owned ChannelStrip for @p track_id from a mixer scene JSON.
/// @details The first `strips[0]` entry in @p scene_json is used as the track strip spec.
///          This is a control-thread structural mutation; do not call concurrently with
///          @ref sonare_engine_process.
SonareError sonare_engine_set_track_strip_json(SonareRealtimeEngine* engine, uint32_t track_id,
                                               const char* scene_json);
/// @brief Sets one embedded EQ band on an engine-owned track strip.
/// @details @p band_json uses the same JSON schema as @ref sonare_eq_set_band.
///          Control-thread mutation; do not call concurrently with @ref sonare_engine_process.
SonareError sonare_engine_set_track_strip_eq_band_json(SonareRealtimeEngine* engine,
                                                       uint32_t track_id, int band_index,
                                                       const char* band_json);
/// @brief Toggles bypass for a track strip insert by combined pre/post insert index.
/// @details Control-thread mutation; do not call concurrently with @ref sonare_engine_process.
SonareError sonare_engine_set_track_strip_insert_bypassed(SonareRealtimeEngine* engine,
                                                          uint32_t track_id,
                                                          unsigned int insert_index, int bypassed,
                                                          int reset_on_bypass);
/// @brief Builds an engine-owned master ChannelStrip from a mixer scene JSON.
/// @details The first `strips[0]` entry in @p scene_json is used as the master strip spec.
///          This is a control-thread structural mutation; do not call concurrently with
///          @ref sonare_engine_process.
SonareError sonare_engine_set_master_strip_json(SonareRealtimeEngine* engine,
                                                const char* scene_json);
/// @brief Sets one embedded EQ band on an engine-owned master strip.
/// @details @p band_json uses the same JSON schema as @ref sonare_eq_set_band.
///          Control-thread mutation; do not call concurrently with @ref sonare_engine_process.
SonareError sonare_engine_set_master_strip_eq_band_json(SonareRealtimeEngine* engine,
                                                        int band_index, const char* band_json);
/// @brief Toggles bypass for a master strip insert by combined pre/post insert index.
/// @details Control-thread mutation; do not call concurrently with @ref sonare_engine_process.
SonareError sonare_engine_set_master_strip_insert_bypassed(SonareRealtimeEngine* engine,
                                                           unsigned int insert_index, int bypassed,
                                                           int reset_on_bypass);
/// @brief Toggles bypass for a bus strip insert by insert index.
/// @details @p bus_id must already exist via sonare_engine_set_track_buses and
///   carry a strip configured by sonare_engine_set_bus_strip_json. Control-thread
///   mutation; do not call concurrently with @ref sonare_engine_process. Returns
///   SONARE_ERROR_INVALID_PARAMETER if the bus or insert is unknown.
SonareError sonare_engine_set_bus_strip_insert_bypassed(SonareRealtimeEngine* engine,
                                                        uint32_t bus_id, unsigned int insert_index,
                                                        int bypassed, int reset_on_bypass);
/// @brief Realtime change of one track-strip insert parameter, addressed by the
///        processor's JSON-key parameter name.
/// @details @p param_name is the key of an entry @ref sonare_mastering_insert_param_info
///   returns with a non-null `id` (the entry also gives the param's `rtSafe` flag;
///   an entry with a null `id` is construction-only and is refused here). The name is resolved to
///   the integer param_id on the control thread and applied at the next block head via the realtime
///   command queue, so this is safe to call during playback without rebuilding the strip. @p
///   insert_index addresses the combined pre/post insert sequence. Returns
///   SONARE_ERROR_INVALID_PARAMETER if the track, insert, or name is unknown, or the param is not
///   realtime-safe. Returns SONARE_ERROR_OUT_OF_MEMORY when the command queue is full (temporary
///   back-pressure); track/insert/param indices must each fit in 8 bits.
SonareError sonare_engine_set_track_strip_insert_param_by_name(SonareRealtimeEngine* engine,
                                                               uint32_t track_id,
                                                               unsigned int insert_index,
                                                               const char* param_name, float value);
/// @brief Realtime change of one master-strip insert parameter by JSON-key name.
/// @details Master-strip counterpart of @ref sonare_engine_set_track_strip_insert_param_by_name.
SonareError sonare_engine_set_master_strip_insert_param_by_name(SonareRealtimeEngine* engine,
                                                                unsigned int insert_index,
                                                                const char* param_name,
                                                                float value);
/// @brief Realtime change of one bus-strip insert parameter by JSON-key name.
/// @details Bus-strip counterpart of @ref sonare_engine_set_track_strip_insert_param_by_name.
///   @p bus_id must already exist via sonare_engine_set_track_buses and carry a
///   strip configured by sonare_engine_set_bus_strip_json. Returns
///   SONARE_ERROR_INVALID_PARAMETER if the bus, insert, or name is unknown;
///   SONARE_ERROR_OUT_OF_MEMORY if the command queue is full.
SonareError sonare_engine_set_bus_strip_insert_param_by_name(SonareRealtimeEngine* engine,
                                                             uint32_t bus_id,
                                                             unsigned int insert_index,
                                                             const char* param_name, float value);
/// @brief Resolves a track-lane insert parameter to its reserved automation id.
/// @details The returned id can be driven over time with
///   @ref sonare_engine_set_automation_lane (a PPQ breakpoint lane) or set once
///   with @ref sonare_engine_set_parameter / @ref sonare_engine_set_parameter_smoothed,
///   exactly like a fader/pan id. Control-thread resolution of the JSON-key name
///   to the strip/insert/param triple. Returns SONARE_ERROR_INVALID_PARAMETER if
///   the track, insert, or name is unknown (and leaves @p out_id untouched).
///
///   This trio is how a mastering processor gets time-varying automation: the
///   `eq.*`, `dynamics.*`, `saturation.*`, `spectral.*`, `stereo.*`,
///   `maximizer.*` and `multiband.*` processors are all available as strip
///   inserts, so placing one on a strip and resolving its parameter here drives
///   it at audio-block precision, live and offline alike. The whole-signal
///   stages of the offline mastering chain (`repair.*`, `loudness`, and the
///   match stages) have no insert form and no automation id: they buffer the
///   entire signal by construction and do not run on the realtime path.
SonareError sonare_engine_resolve_track_insert_automation_id(SonareRealtimeEngine* engine,
                                                             uint32_t track_id,
                                                             unsigned int insert_index,
                                                             const char* param_name,
                                                             uint32_t* out_id);
/// @brief Resolves a master-strip insert parameter to its reserved automation id.
/// @details Master-strip counterpart of
///   @ref sonare_engine_resolve_track_insert_automation_id.
SonareError sonare_engine_resolve_master_insert_automation_id(SonareRealtimeEngine* engine,
                                                              unsigned int insert_index,
                                                              const char* param_name,
                                                              uint32_t* out_id);
/// @brief Resolves a bus-strip insert parameter to its reserved automation id.
/// @details Bus-strip counterpart of
///   @ref sonare_engine_resolve_track_insert_automation_id.
SonareError sonare_engine_resolve_bus_insert_automation_id(SonareRealtimeEngine* engine,
                                                           uint32_t bus_id,
                                                           unsigned int insert_index,
                                                           const char* param_name,
                                                           uint32_t* out_id);
/// @brief Realtime change of a track lane strip's pan position.
/// @details Control-thread mutation; glitch-free (atomic). Returns
///   SONARE_ERROR_INVALID_PARAMETER if @p track_id has no bound lane strip or
///   @p pan is not finite. The pan mode is unchanged; use
///   @ref sonare_engine_set_track_strip_pan_mode to switch modes.
SonareError sonare_engine_set_track_strip_pan(SonareRealtimeEngine* engine, uint32_t track_id,
                                              float pan);
/// @brief Realtime change of a track lane strip's pan law.
/// @details @p pan_law uses SonarePanLaw (0=-3 dB, 1=-4.5 dB, 2=-6 dB, 3=linear).
///   Control-thread mutation; glitch-free. Returns SONARE_ERROR_INVALID_PARAMETER
///   if the track has no bound lane strip or @p pan_law is unknown.
SonareError sonare_engine_set_track_strip_pan_law(SonareRealtimeEngine* engine, uint32_t track_id,
                                                  int pan_law);
/// @brief Realtime change of a track lane strip's pan mode.
/// @details @p pan_mode uses SonarePanMode (0=balance, 1=stereo pan, 2=dual pan).
///   Control-thread mutation; glitch-free. Returns SONARE_ERROR_INVALID_PARAMETER
///   if the track has no bound lane strip or @p pan_mode is unknown.
SonareError sonare_engine_set_track_strip_pan_mode(SonareRealtimeEngine* engine, uint32_t track_id,
                                                   int pan_mode);
/// @brief Realtime change of a track lane strip's dual-pan left/right positions.
/// @details Both positions are in [-1, 1]. Takes effect under pan mode dual pan.
///   Control-thread mutation; glitch-free. Returns SONARE_ERROR_INVALID_PARAMETER
///   if the track has no bound lane strip or a position is not finite.
SonareError sonare_engine_set_track_strip_dual_pan(SonareRealtimeEngine* engine, uint32_t track_id,
                                                   float left_pan, float right_pan);
/// @brief Realtime change of a track lane strip's inter-channel alignment delay.
/// @details @p delay_samples is a non-negative whole-sample delay. This adjusts
///   strip latency, so PDC and the reported graph latency are refreshed; treat it
///   as a structural change (do not call concurrently with
///   @ref sonare_engine_process). Returns SONARE_ERROR_INVALID_PARAMETER if the
///   track has no bound lane strip or @p delay_samples is negative.
SonareError sonare_engine_set_track_strip_channel_delay_samples(SonareRealtimeEngine* engine,
                                                                uint32_t track_id,
                                                                int delay_samples);
/// @note Release @p out_provider with @ref sonare_clip_page_provider_destroy; it is a handle, not a
///       sonare_free_* buffer.
SonareError sonare_clip_page_provider_create(int num_channels, int64_t num_samples,
                                             int64_t page_frames,
                                             SonareClipPageProvider** out_provider);
void sonare_clip_page_provider_destroy(SonareClipPageProvider* provider);
SonareError sonare_clip_page_provider_supply(SonareClipPageProvider* provider, int64_t page_index,
                                             const float* const* channels, int num_channels,
                                             int64_t frames);
SonareError sonare_clip_page_provider_clear(SonareClipPageProvider* provider, int64_t page_index);
SonareError sonare_engine_pop_clip_page_request(SonareRealtimeEngine* engine,
                                                SonareClipPageRequest* out_request,
                                                int* out_has_request);
/// @brief Number of clip-page requests dropped because the bounded queue was full.
/// @details Advisory telemetry; monotonic within a prepared session and reset by
///   @ref sonare_engine_prepare. A non-zero count means the host is draining
///   @ref sonare_engine_pop_clip_page_request too slowly and some pages were
///   never asked for, so their reads produced silence.
SonareError sonare_engine_clip_page_request_overflow_count(SonareRealtimeEngine* engine,
                                                           uint32_t* out_count);
/// @brief Number of blocks in which a time-stretched clip fell back to resampling.
/// @details Advisory telemetry; monotonic within a prepared session and reset by
///   @ref sonare_engine_prepare. Only @ref sonare_engine_warp_voice_capacity
///   stretcher voices exist, so a project with more overlapping @c kTimeStretch
///   clips than voices plays some of them pitch-shifted instead -- except at
///   capacity 0, a deliberate "stretch disabled" choice that never counts here.
///   A non-zero count is the only way to detect that degradation.
SonareError sonare_engine_warp_stretch_overflow_count(SonareRealtimeEngine* engine,
                                                      uint32_t* out_count);
/// @brief Largest @p voices @ref sonare_engine_set_warp_voice_capacity accepts.
#define SONARE_ENGINE_MAX_WARP_VOICES 64u
/// @brief Sets the number of concurrent time-stretch voices.
/// @details Voices must be in `[0, 64]` (@ref SONARE_ENGINE_MAX_WARP_VOICES); a
///   larger value returns @ref SONARE_ERROR_INVALID_PARAMETER and leaves the
///   capacity unchanged. Default is 8. Capacity 0 disables time-stretch, so
///   every warped clip plays resampled instead and none of that counts toward
///   @ref sonare_engine_warp_stretch_overflow_count. A change applied while
///   @ref sonare_engine_process is running rebuilds the voice pool immediately;
///   any clip stretching through a voice at that moment restarts its WSOLA
///   state rather than carrying it over. Control thread only.
SonareError sonare_engine_set_warp_voice_capacity(SonareRealtimeEngine* engine, uint32_t voices);
/// @brief Reads the current time-stretch voice capacity.
/// @details Returns the value most recently accepted by
///   @ref sonare_engine_set_warp_voice_capacity, or the default (8) if it was
///   never called. Control thread.
SonareError sonare_engine_warp_voice_capacity(SonareRealtimeEngine* engine, uint32_t* out_voices);
/// @brief Sets the clip-page look-ahead window in timeline frames.
/// @details The clip player reports the pages it is ABOUT TO read that are not
///   resident yet, so a streaming host can service them before the audio thread
///   reaches them. Without look-ahead a page miss is only reported after the
///   read already produced silence, which costs one block of silence at every
///   page boundary the host has not primed — the reason a sliding-window
///   streamer cannot keep a live playhead fed by miss reports alone.
///
///   Look-ahead requests are drained through the same
///   @ref sonare_engine_pop_clip_page_request queue and are queued AFTER the
///   block's genuine misses, so a host that keeps only the newest request per
///   clip tracks the look-ahead frontier.
///
///   @ref sonare_engine_prepare defaults this to half a second at the engine's
///   sample rate. 0 disables the look-ahead. A clip whose pages are all
///   resident produces no requests at all, with or without look-ahead.
///   Safe to call while audio is running.
SonareError sonare_engine_set_clip_page_prefetch_frames(SonareRealtimeEngine* engine,
                                                        int64_t frames);
/// @brief Reads back the clip-page look-ahead window in timeline frames.
SonareError sonare_engine_clip_page_prefetch_frames(SonareRealtimeEngine* engine,
                                                    int64_t* out_frames);
SonareError sonare_engine_set_capture_buffer(SonareRealtimeEngine* engine,
                                             const SonareEngineCaptureBuffer* buffer);
SonareError sonare_engine_arm_capture(SonareRealtimeEngine* engine, int armed);
SonareError sonare_engine_set_capture_punch(SonareRealtimeEngine* engine, int64_t start_sample,
                                            int64_t end_sample, int enabled);
SonareError sonare_engine_set_capture_source(SonareRealtimeEngine* engine,
                                             SonareEngineCaptureSource source);
/** Shift the recording window in timeline samples. A positive value delays
 * capture: output frame 0 corresponds to punch_start + offset_samples. */
SonareError sonare_engine_set_record_offset_samples(SonareRealtimeEngine* engine,
                                                    int64_t offset_samples);
SonareError sonare_engine_set_input_monitor(SonareRealtimeEngine* engine, int enabled, float gain);
SonareError sonare_engine_reset_capture(SonareRealtimeEngine* engine);
SonareError sonare_engine_capture_status(SonareRealtimeEngine* engine,
                                         SonareEngineCaptureStatus* out);
SonareError sonare_engine_set_graph(SonareRealtimeEngine* engine,
                                    const SonareEngineGraphSpec* spec);
SonareError sonare_engine_graph_node_count(SonareRealtimeEngine* engine, size_t* out_count);
SonareError sonare_engine_graph_connection_count(SonareRealtimeEngine* engine, size_t* out_count);
/// @brief Renders one block in place, adding engine audio to @p channels.
/// @details The input contents are preserved and engine sources are mixed with
/// @c +=. Zero every output plane before this call when no upstream audio is
/// intended. Audio-thread only; @p num_frames must not exceed the prepared
/// block size. If @p num_channels exceeds the channel bound supplied to
/// sonare_engine_prepare_with_channels, the engine silences and advances the
/// block, returns @c SONARE_OK, and publishes a telemetry error with
/// @c error == SONARE_ENGINE_TELEMETRY_ERROR_MAX_CHANNELS_EXCEEDED and
/// @c value == @p num_channels. A block-size violation takes precedence when
/// both limits are exceeded and remains
/// @c SONARE_ENGINE_TELEMETRY_ERROR_MAX_BLOCK_EXCEEDED.
SonareError sonare_engine_process(SonareRealtimeEngine* engine, float* const* channels,
                                  int num_channels, int num_frames);
SonareError sonare_engine_process_with_monitor(SonareRealtimeEngine* engine, float* const* channels,
                                               float* const* monitor_out, int num_channels,
                                               int num_frames);
/// @brief Renders @p total_frames offline from the current transport position,
///        ending the timeline: notes still sounding are released and the PDC /
///        alignment delay lines are flushed before returning.
/// @details Equivalent to sonare_engine_render_offline_ex with @c finalize 1.
SonareError sonare_engine_render_offline(SonareRealtimeEngine* engine, float* const* out,
                                         int num_channels, int64_t total_frames, int block_size);
/// @brief sonare_engine_render_offline with explicit control over whether this
///        call ends the timeline.
/// @details @p finalize 0 renders one CHUNK of a longer timeline: a note held
///   across the chunk boundary keeps sounding into the next call and the delay
///   lines carry their history over, so consecutive chunks concatenate to
///   exactly what one continuous render of the same span produces. Non-zero
///   ends the timeline, which is what a one-shot bounce wants and what
///   sonare_engine_render_offline does. A chunked caller passes 0 for every
///   chunk and calls sonare_engine_finish_offline_render once at the end.
///
///   Sample-exact concatenation requires every chunk to use the same
///   @p block_size and a @p total_frames that is a whole number of blocks. Each
///   call restarts the block grid at its own frame 0 and renders a short final
///   block for the remainder, and the clip / automation / MIDI-clip snapshots
///   are frozen once per block, so a chunk that ends mid-block shifts every
///   later block boundary relative to a continuous render. Such a split still
///   renders continuous audio -- no note is cut and no delay line is cleared --
///   but it is not bit-identical to the one-call result.
SonareError sonare_engine_render_offline_ex(SonareRealtimeEngine* engine, float* const* out,
                                            int num_channels, int64_t total_frames, int block_size,
                                            int finalize);
/// @brief Ends an offline render: releases every note the sequencer still holds
///        and flushes the PDC / alignment delay lines.
/// @details Required after a chunked render (sonare_engine_render_offline_ex
///   with @c finalize 0); the finalizing forms run it themselves, so a one-shot
///   bounce never calls it. Idempotent.
///
///   Skipping it leaves the sequencer holding every note still sounding at the
///   last chunk. For an engine-internal instrument that only means the tail is
///   never released. For a destination marked external
///   (sonare_engine_set_midi_destination_external) the note-ons have already
///   left through the external MIDI queue, so the note-offs this call emits are
///   the only ones the receiving device will ever get: without them the notes
///   hang on hardware, outside the engine, where nothing later clears them.
/// @return @c SONARE_ERROR_INVALID_PARAMETER when @p engine is NULL,
///         @c SONARE_ERROR_INVALID_STATE when the engine was never prepared.
SonareError sonare_engine_finish_offline_render(SonareRealtimeEngine* engine);
/// @brief Renders the whole span in one call and returns the interleaved mix.
/// @details Runs the offline pre-roll before the first audible block: queued
///   commands are applied, one throwaway block resolves lane automation at the
///   start position with the transport held stopped (so the playhead does not
///   move), and every smoother is then snapped to its target. A lane sitting at
///   a static -12 dB therefore bounces at -12 dB from sample 0 instead of ramping
///   in over the first block, which is what live playback would do and what a
///   render is not allowed to do. sonare_engine_render_offline does NOT pre-roll,
///   because a chunked render would re-prime on every chunk; a host driving it
///   directly primes once itself (a process() block plus
///   sonare_engine_settle_parameters).
///
///   The whole result is held in memory, so the span is capped by a 1 GiB peak
///   budget over the full-size float buffers a bounce holds at once. The cap is
///   a FRAME count: inversely proportional to @c num_channels, and independent
///   of the sample rate. Stereo refuses past 44,739,242 frames, or past
///   33,554,432 frames when @c dither is non-zero, which adds one more copy;
///   5.1 refuses past a third of each. Read as a DURATION those two stereo
///   counts are 15 min 32 s and 11 min 39 s at 48 kHz, and half that at 96 kHz.
///   When the rates differ both the source and the resampled length are checked,
///   so the longer of the two is what binds. Longer material renders in chunks
///   through sonare_engine_render_offline_ex.
/// @param out Receives a heap-owned interleaved buffer; free with sonare_free_bounce_result.
/// @return @c SONARE_ERROR_INVALID_PARAMETER when the requested span exceeds the
///         cap above, among the other option validations.
SonareError sonare_engine_bounce_offline(SonareRealtimeEngine* engine,
                                         const SonareEngineBounceOptions* options,
                                         SonareEngineBounceResult* out);
/// @brief Fills @p options with documented defaults for sonare_engine_bounce_offline.
/// @details This is the canonical source of bounce-option defaults for all
///   language bindings. Callers should invoke this helper first and then
///   override only the fields they care about, which guarantees the same
///   normalization target (SONARE_DEFAULT_BOUNCE_TARGET_LUFS) across the C,
///   Node, Python and WASM facades.
/// @param options Output struct; must not be NULL.
/// @return @c SONARE_OK on success or @c SONARE_ERROR_INVALID_PARAMETER if
///         @p options is NULL.
SonareError sonare_engine_bounce_options_default(SonareEngineBounceOptions* options);
/// @brief Free the heap-allocated buffer held by a bounce result.
/// @param result Result whose @c interleaved buffer is deleted and nulled.
void sonare_free_bounce_result(SonareEngineBounceResult* result);
/// @brief Renders the whole span in one call and registers it as a clip.
/// @details Runs the same offline pre-roll as sonare_engine_bounce_offline, so
///   the frozen clip captures the lane at its settled values instead of carrying
///   a fade-in the live lane never had.
SonareError sonare_engine_freeze_offline(SonareRealtimeEngine* engine,
                                         const SonareEngineFreezeOptions* options,
                                         SonareEngineFreezeResult* out);
/// @brief Drains process and error telemetry records from the realtime engine.
/// @details The @c error field uses SonareEngineTelemetryError ordinals; core
/// reserves ordinal 19 for the WASM worklet and publishes the channel-bound
/// diagnostic at ordinal 20. The SonareEngineTelemetry POD layout is unchanged.
SonareError sonare_engine_drain_telemetry(SonareRealtimeEngine* engine, SonareEngineTelemetry* out,
                                          size_t max_records, size_t* written);
/// @brief Drains pending meter telemetry records published by the engine.
/// @param out Caller-owned array receiving up to @p max_records entries.
/// @param max_records Capacity of @p out. May be 0, which is a safe no-op:
///   nothing is copied or drained and @p out_count is always set to 0. This
///   does NOT report the number of pending records — draining is destructive
///   (each returned record is removed from the queue), so there is no way to
///   learn the backlog size without consuming it; drain into a buffer sized
///   for the expected batch instead of probing with max_records == 0.
/// @param out_count Receives the number of records written.
SonareError sonare_engine_drain_meter_telemetry(SonareRealtimeEngine* engine,
                                                SonareMeterTelemetryRecord* out, size_t max_records,
                                                size_t* out_count);
/// @brief Drains pending meter telemetry as per-plane (wide) records for
///   surround targets. Same underlying queue as
///   @ref sonare_engine_drain_meter_telemetry — a host picks the drain matching
///   its target's bus layout; do not call both for one target. Each record
///   carries channel_count valid planes in peak_db/rms_db/true_peak_db.
/// @param out Caller-owned array receiving up to @p max_records entries.
/// @param max_records Capacity of @p out. May be 0, which is a safe no-op:
///   nothing is copied or drained and @p out_count is always set to 0. This
///   does NOT report the number of pending records — draining is destructive
///   (each returned record is removed from the queue), so there is no way to
///   learn the backlog size without consuming it; drain into a buffer sized
///   for the expected batch instead of probing with max_records == 0.
/// @param out_count Receives the number of records written.
SonareError sonare_engine_drain_meter_telemetry_wide(SonareRealtimeEngine* engine,
                                                     SonareMeterTelemetryRecordWide* out,
                                                     size_t max_records, size_t* out_count);
/// @brief Enables/configures per-target spectrum + vectorscope telemetry.
/// @param interval_frames Minimum render-frame gap between published snapshots
///   (0 disables capture). @param band_count Requested FFT band resolution
///   (1..SONARE_SCOPE_MAX_BANDS); changing it re-prepares the tap, so call from
///   the control thread while @ref sonare_engine_process is not running.
/// @param out_band_count Optional; receives the band count actually applied.
SonareError sonare_engine_configure_scope_telemetry(SonareRealtimeEngine* engine,
                                                    int interval_frames, unsigned int band_count,
                                                    unsigned int* out_band_count);
/// @brief Drains pending spectrum + vectorscope telemetry records.
/// @param out Caller-owned array receiving up to @p max_records entries.
/// @param max_records Capacity of @p out. May be 0, which is a safe no-op:
///   nothing is copied or drained and @p out_count is always set to 0. This
///   does NOT report the number of pending records — draining is destructive
///   (each returned record is removed from the queue), so there is no way to
///   learn the backlog size without consuming it; drain into a buffer sized
///   for the expected batch instead of probing with max_records == 0.
/// @param out_count Receives the number of records written. Each record carries
///   band_count FFT bands and point_count interleaved left/right goniometer pairs.
SonareError sonare_engine_drain_scope_telemetry(SonareRealtimeEngine* engine,
                                                SonareScopeTelemetryRecord* out, size_t max_records,
                                                size_t* out_count);
/// @brief Pushes a live parameter value to the engine (immediate jump).
/// @param param_id Target parameter id.
/// @param value New value.
/// @param render_frame Render-frame time to apply, or -1 for immediate.
/// @details This value also becomes @p param_id's base value: if an
///   automation lane later starts (and stops) driving @p param_id, the target
///   reverts to this value once that lane empties -- see
///   @ref sonare_engine_set_automation_lane.
SonareError sonare_engine_set_parameter(SonareRealtimeEngine* engine, uint32_t param_id,
                                        float value, int64_t render_frame);
/// @brief Pushes a live parameter value to the engine using a smoothed ramp.
/// @details The ramp's target (not its in-flight position) becomes
///   @p param_id's base value, with the same restore-on-lane-release behavior
///   as @ref sonare_engine_set_parameter.
SonareError sonare_engine_set_parameter_smoothed(SonareRealtimeEngine* engine, uint32_t param_id,
                                                 float value, int64_t render_frame);
/// @brief Sets the default ramp time (ms) for engine-level smoothed parameters.
/// @details Applies to every smoothed parameter change -- fader/pan glides,
///          insert-parameter automation, and MIDI-CC mappings. The default is
///          20 ms; pass 0 for instant (un-ramped) changes. @p smoothing_ms must
///          be finite and >= 0.
SonareError sonare_engine_set_param_smoothing_ms(SonareRealtimeEngine* engine, float smoothing_ms);
SonareError sonare_engine_set_solo_mute(SonareRealtimeEngine* engine, uint32_t lane_index, int solo,
                                        int mute, int64_t render_frame);
/// @brief Schedules a per-track-lane PFL/AFL monitor tap.
/// @details @p lane_index addresses the currently configured track-lane array;
///          the audio thread applies the mode at @p render_frame (or at the
///          next block head when negative). PFL taps after the lane strip and
///          before lane fader/gate/pan; AFL taps after those lane stages. The
///          monitor bus is folded into @ref sonare_engine_process output and is
///          returned separately by @ref sonare_engine_process_with_monitor.
/// @return @c SONARE_ERROR_INVALID_PARAMETER for a NULL engine or unknown mode,
///         @c SONARE_ERROR_NOT_SUPPORTED when mixing is disabled, and
///         @c SONARE_ERROR_OUT_OF_MEMORY when the realtime command queue is full.
SonareError sonare_engine_set_track_monitor_mode(SonareRealtimeEngine* engine, uint32_t lane_index,
                                                 SonareEngineTrackMonitorMode mode,
                                                 int64_t render_frame);

/// @brief One render-frame MIDI event for @ref sonare_engine_set_midi_clips.
/// @details Mirrors midi::MidiEvent with a fixed UMP payload. `render_frame` is
///          an absolute sample position on the engine timeline. `word_count` is
///          the number of active UMP words (1..4); 0 lets the C bridge infer a
///          one-word MIDI 1.0 event when only `word0` is set.
///
///          `group` is REDUNDANT: a UMP already carries its group in `word0`
///          bits 24..27, and that is the copy the engine uses, because it is the
///          form that reaches a device or a file. Packing the group into `word0`
///          (as this header's `data0` packing describes) is sufficient and is
///          the recommended form; a `group` that contradicts `word0` is ignored
///          rather than honoured, so the two can never be read as different
///          groups by different parts of the engine. The field is still
///          range-checked: a value above 15 makes the struct malformed and
///          @ref sonare_engine_set_midi_clips returns
///          @c SONARE_ERROR_INVALID_PARAMETER, exactly as a non-zero `reserved`
///          does.
///
///          Two message types have no group at all and are read as group 0
///          whatever `word0` bits 24..27 hold: Utility (type nibble `0x0`),
///          where that nibble is Reserved, and UMP Stream (`0xF`), where those
///          bits are the `form` field and the top of the `status` field. Both
///          address the endpoint rather than a group, so packing a group into
///          them has no effect.
typedef struct {
  int64_t render_frame;
  uint32_t word0;
  uint32_t word1;
  uint32_t word2;
  uint32_t word3;
  uint8_t word_count;
  uint8_t group;
  uint16_t reserved;
  uint32_t sysex_handle;
} SonareEngineMidiEvent;

/// @brief One compiled realtime MIDI clip schedule.
/// @details Direct bindings expose the same RT-facing shape as
///          midi::MidiClipSchedule: PPQ has already been compiled to absolute
///          sample frames in @ref SonareEngineMidiEvent.render_frame.
typedef struct {
  uint32_t id;
  uint32_t track_id;
  int64_t start_sample;
  double start_ppq;
  int64_t length_samples;
  int loop;
  int64_t loop_length_samples;
  uint32_t destination_id;
  const SonareEngineMidiEvent* events;
  size_t event_count;
} SonareEngineMidiClipSchedule;

/// @brief Replaces the engine's realtime MIDI clip schedule snapshot.
SonareError sonare_engine_set_midi_clips(SonareRealtimeEngine* engine,
                                         const SonareEngineMidiClipSchedule* clips,
                                         size_t clip_count);

/// @brief Built-in realtime synth patch for @ref sonare_engine_set_builtin_instrument.
/// @details Same zero-init contract as project bounce built-in instruments:
///          non-positive fields use the default sine patch values.
typedef struct {
  int waveform;     /* SonareSynthWaveform: 0=sine, 1=saw, 2=square, 3=triangle.
                       Anything else fails with SONARE_ERROR_INVALID_PARAMETER. */
  float gain;       /* 0 => 0.2 */
  float attack_ms;  /* 0 => 5 */
  float decay_ms;   /* 0 => 60 */
  float sustain;    /* 0 => 0.7 */
  float release_ms; /* 0 => 120 */
  int polyphony;    /* 0 => 16, clamped to [1,64] */
  /* Every field above takes 0 as "use the default". A negative or non-finite
     value is rejected with SONARE_ERROR_INVALID_PARAMETER rather than resolving
     to the default, which is what it used to do without saying so. */
} SonareEngineBuiltinSynthConfig;

/// @brief Binds/replaces a built-in synth on a realtime MIDI destination.
/// @details Control-thread API. The engine owns the synth instance. Live MIDI
///          note/CC commands and scheduled MIDI clips routed to @p destination_id
///          render through this instrument. This is a control-thread structural
///          mutation; do not call concurrently with @ref sonare_engine_process.
SonareError sonare_engine_set_builtin_instrument(SonareRealtimeEngine* engine,
                                                 uint32_t destination_id,
                                                 const SonareEngineBuiltinSynthConfig* config);

/// @brief Binds/replaces the patch-driven NativeSynth on a realtime MIDI
///        destination (the full synthesizer: every @ref SonareSynthEngineMode
///        engine, enumerable at runtime via @ref sonare_synth_enum_names with
///        SONARE_SYNTH_ENUM_ENGINE_MODE). The
///        patch resolves exactly like the project bounce surface
///        (@ref SonareSynthPatch: preset catalog base + field overrides); an
///        invalid struct_version or unknown preset name fails with
///        SONARE_ERROR_INVALID_PARAMETER. Control-thread API; the engine owns
///        the synth. Live MIDI input and scheduled MIDI clips routed to
///        @p destination_id render through it. This is a control-thread
///        structural mutation; do not call concurrently with
///        @ref sonare_engine_process.
SonareError sonare_engine_set_synth_instrument(SonareRealtimeEngine* engine,
                                               uint32_t destination_id,
                                               const SonareSynthPatch* patch);

/// @brief Like @ref sonare_engine_set_synth_instrument, and additionally binds
///        the sample bank a SONARE_SYNTH_ENGINE_SAMPLE patch reads.
/// @details Same patch resolution and the same control-thread contract; the
///          only difference is @p bank, which the engine takes a share of, so
///          the caller may destroy its own handle afterwards. Pass NULL for a
///          patch that does not voice the sample engine — it then behaves
///          exactly like @ref sonare_engine_set_synth_instrument. A sample
///          patch bound WITHOUT a bank is accepted and renders silence, the
///          same way one naming a keymap set the bank lacks does.
SonareError sonare_engine_set_synth_instrument_with_bank(SonareRealtimeEngine* engine,
                                                         uint32_t destination_id,
                                                         const SonareSynthPatch* patch,
                                                         SonareSampleBank* bank);

/// @brief Resolves a hosted instrument's continuous parameter to its reserved
///        automation id.
/// @details Instrument counterpart of
///   @ref sonare_engine_resolve_track_insert_automation_id: the returned id is
///   driven over time with @ref sonare_engine_set_automation_lane (a PPQ
///   breakpoint lane) or set once with @ref sonare_engine_set_parameter /
///   @ref sonare_engine_set_parameter_smoothed, and reaches the instrument at
///   audio-block precision, live and offline alike.
///
///   @p param_name is the instrument's JSON-key parameter name. For the
///   NativeSynth (@ref sonare_engine_set_synth_instrument) these are the
///   continuous @ref SonareSynthPatch fields spelled exactly as the bindings
///   spell them: `gain`, `busDrive`, `cutoffHz`, `resonanceQ`, `drive`,
///   `keyTrack`, `envToCutoffCents`, `velToCutoffCents`, `ampAttackMs`,
///   `ampDecayMs`, `ampSustain`, `ampReleaseMs`, `filterAttackMs`,
///   `filterDecayMs`, `filterSustain`, `filterReleaseMs`, `lfoRateHz`,
///   `lfoToPitchCents`, `lfo2RateHz`, `glideMs`, `bodyMix`, `stereoSpread`,
///   `detuneCents`, `driftCents`, `pitchOffsetCents`, `hpCutoffHz`,
///   `sampleHoldHz`, `bitDepth`.
///
///   Structural patch fields (preset, engine mode, waveform, filter model,
///   unison, polyphony, body type, mod routings) are NOT automatable and fail
///   with SONARE_ERROR_INVALID_PARAMETER: they resize voice pools or swap DSP
///   topology, which is not audio-thread safe. Change them by rebinding the
///   instrument with a new patch.
///
///   Two timing classes among the automatable ones. `gain`, `busDrive`,
///   `cutoffHz`, `resonanceQ`, `envToCutoffCents`, `lfoToPitchCents` and
///   `pitchOffsetCents` reach voices that are ALREADY SOUNDING from the next
///   block. The rest are cached into per-voice state at note-on, so they take
///   effect from the NEXT NOTE — a lane that moves one of them while a note is
///   held looks inert until the next one speaks, which is the behaviour and not
///   a dropped write.
///
///   Returns SONARE_ERROR_INVALID_PARAMETER when no instrument is bound to
///   @p destination_id, when the bound instrument exposes no automatable
///   parameters (the SF2 player and the built-in synth do not), or when the
///   name is unknown; @p out_id is left untouched. The id survives an
///   unbind/rebind of the same destination and applies nothing while that
///   destination is unbound, so a stale lane is inert rather than dangling.
SonareError sonare_engine_resolve_instrument_automation_id(SonareRealtimeEngine* engine,
                                                           uint32_t destination_id,
                                                           const char* param_name,
                                                           uint32_t* out_id);
/// @brief Loads (parses) SF2 bytes into the realtime engine so SoundFont
///        instruments can be bound to destinations with
///        @ref sonare_engine_set_sf2_instrument. Control-thread API; replaces
///        any previously loaded SoundFont (already-bound SF2 instruments keep
///        the SoundFont they were created with). The bytes are copied/decoded,
///        so @p data may be freed after the call. Resource limits are
///        268,435,456 input bytes, 67,108,864 sample points, 536,870,912 peak
///        input-plus-decoded bytes, and 65,536 records per pdta table. A
///        resource-limit failure returns SONARE_ERROR_INVALID_FORMAT and leaves
///        the previously loaded SoundFont unchanged.
SonareError sonare_engine_load_soundfont(SonareRealtimeEngine* engine, const uint8_t* data,
                                         size_t size);

/// @brief Versioned SF2 player patch for @ref sonare_engine_set_sf2_instrument.
/// @details Same zero-init contract as the project-bounce SF2 instruments:
///          every field uses "0 => default" (struct_version 0 => version 1).
///          Version 2 adds @c prefer_model_for_modeled_families; version 3 adds
///          @c clear_bank_rig.
typedef struct {
  int struct_version; /* 0 or 1 => version 1; 3 => current version */
  float gain;         /* master output gain (linear); 0 => 0.5. A negative or non-finite gain is
                         rejected with SONARE_ERROR_INVALID_PARAMETER, not promoted to the default */
  int polyphony;      /* max simultaneous voices; 0 => 48, clamped to [1, 64]. A negative count is
                         rejected with SONARE_ERROR_INVALID_PARAMETER rather than promoted to 48 */
  int prefer_model_for_modeled_families; /* v2: non-zero selects the dedicated model for
                                            covered melodic GM programs; drums stay SF2-first */
  int clear_bank_rig;                    /* v3: non-zero renders the instrument alone, without the
                                            amplifier the bank binds after an electric guitar's
                                            voice; 0 keeps it, so a file that asks for nothing
                                            still sounds complete */
} SonareEngineSf2InstrumentConfig;

/// @brief Binds/replaces a GS-compatible SoundFont player on a realtime MIDI
///        destination, fed by the engine's loaded SoundFont
///        (@ref sonare_engine_load_soundfont). Without a loaded SoundFont —
///        or for programs the SoundFont does not cover — notes play through
///        the built-in synthesizer GM fallback bank (the data-free floor).
///        Control-thread API; the engine owns the player. Live MIDI input
///        (`sonare_engine_push_midi_input_*` / `sonare_engine_push_midi_note_*`)
///        and scheduled MIDI clips routed to @p destination_id render through
///        the player (16 MIDI channels; channel 10 and GM2 CC0=120 rhythm
///        parts as drums; GS NRPN part edits,
///        GS/GM SysEx resets). This is a control-thread structural mutation;
///        do not call concurrently with @ref sonare_engine_process.
SonareError sonare_engine_set_sf2_instrument(SonareRealtimeEngine* engine, uint32_t destination_id,
                                             const SonareEngineSf2InstrumentConfig* config);

/// @brief Clears any realtime instrument bound to @p destination_id.
/// @details This is a control-thread structural mutation; do not call
///          concurrently with @ref sonare_engine_process.
SonareError sonare_engine_clear_midi_instrument(SonareRealtimeEngine* engine,
                                                uint32_t destination_id);
SonareError sonare_engine_midi_instrument_count(SonareRealtimeEngine* engine, size_t* out_count);

/// @brief Controller-profile input ordinals. Mirrors midi::ControllerInput.
/// @details Aftertouch, velocity and bend sit in the same enumeration as a
///          controller number because a device assigns them to the slot a CC
///          would take.
typedef enum SONARE_ENUM_BASE {
  SONARE_CONTROLLER_INPUT_CONTROL_CHANGE = 0,
  SONARE_CONTROLLER_INPUT_CHANNEL_PRESSURE = 1,
  SONARE_CONTROLLER_INPUT_POLY_PRESSURE = 2,
  SONARE_CONTROLLER_INPUT_PITCH_BEND = 3,
  SONARE_CONTROLLER_INPUT_VELOCITY = 4
} SonareControllerInput;

/// @brief Controller-profile axis ordinals. Mirrors midi::ControllerAxis.
/// @details What a gesture MEANS, so an engine is reached by the meaning rather
///          than by the controller number that carried it. The first four are
///          the engine's own exciter; the last three are channel-level state.
typedef enum SONARE_ENUM_BASE {
  SONARE_CONTROLLER_AXIS_NONE = 0,
  SONARE_CONTROLLER_AXIS_EXCITATION = 1,
  SONARE_CONTROLLER_AXIS_POSITION = 2,
  SONARE_CONTROLLER_AXIS_BRIGHTNESS = 3,
  SONARE_CONTROLLER_AXIS_MORPH = 4,
  SONARE_CONTROLLER_AXIS_LOUDNESS = 5,
  SONARE_CONTROLLER_AXIS_PITCH_CENTS = 6,
  SONARE_CONTROLLER_AXIS_VIBRATO_DEPTH = 7
} SonareControllerAxis;

/// @brief Values in @ref SonareControllerInput and @ref SonareControllerAxis.
#define SONARE_CONTROLLER_INPUT_COUNT 5
#define SONARE_CONTROLLER_AXIS_COUNT 8
/// @brief Bindings one profile holds. Binding past it is refused rather than
///        grown, so the audio thread scans a fixed table.
#define SONARE_MAX_CONTROLLER_BINDINGS 32

/// @brief One device gesture bound to one expression axis.
/// @details Binding the same input twice with different axes is how a single
///          gesture reaches two of them, which is what a breath controller
///          driving both excitation and loudness needs.
typedef struct {
  /// One of @ref SonareControllerInput.
  uint8_t input;
  /// CC number 0-127 for SONARE_CONTROLLER_INPUT_CONTROL_CHANGE. Every other
  /// input is identified by its message status alone and ignores this.
  uint8_t index;
  /// One of @ref SonareControllerAxis. SONARE_CONTROLLER_AXIS_NONE is refused:
  /// a binding that means nothing is a caller mistake, not an empty slot.
  uint8_t axis;
  uint8_t reserved;
  /// Axis value at zero deflection, in the axis's own unit — normalized [0,1]
  /// for the excitation axes and loudness, cents for pitch and vibrato depth.
  float lo;
  /// Axis value at full deflection. lo > hi inverts the gesture.
  float hi;
  /// Exponent applied to the normalized input before the range maps it. 1 is
  /// linear and is the default a caller should keep: a wind controller has
  /// already applied the curve its player chose, and a second one on this side
  /// bends a gesture that was already shaped.
  float curve;
} SonareControllerBinding;

#ifdef __cplusplus
static_assert(offsetof(SonareControllerBinding, input) == 0, "ControllerBinding.input offset");
static_assert(offsetof(SonareControllerBinding, index) == 1, "ControllerBinding.index offset");
static_assert(offsetof(SonareControllerBinding, axis) == 2, "ControllerBinding.axis offset");
static_assert(offsetof(SonareControllerBinding, lo) == 4, "ControllerBinding.lo offset");
static_assert(offsetof(SonareControllerBinding, hi) == 8, "ControllerBinding.hi offset");
static_assert(offsetof(SonareControllerBinding, curve) == 12, "ControllerBinding.curve offset");
static_assert(sizeof(SonareControllerBinding) == 16, "SonareControllerBinding layout drift");
#endif

/// @brief Returns the controller-profile preset names separated by '\n'.
/// @details Pointer is owned by libsonare and remains valid for the program
///          lifetime; the caller must NOT free it. Never NULL: a build without
///          arrangement support returns the empty string.
const char* sonare_controller_profile_names(void);

/// @brief Replaces the instrument's controller profile with a named preset.
/// @details Control-thread API. An unknown name is refused rather than resolved
///          to a default, because a default that silently replaced the device's
///          spelling would still play — just not the gestures that were sent.
///          Replacing the profile drops every channel's accumulated axis value:
///          the new bindings say nothing about what the old ones had reached.
///          Returns SONARE_ERROR_NOT_SUPPORTED for a destination whose
///          instrument has nowhere to put a profile.
SonareError sonare_engine_set_controller_profile(SonareRealtimeEngine* engine,
                                                 uint32_t destination_id, const char* preset_name);

/// @brief Adds one binding on top of the instrument's current profile.
/// @details Control-thread API. Refused, adding nothing, when the table is
///          full, when the axis is SONARE_CONTROLLER_AXIS_NONE, or when a
///          poly-pressure binding names an axis that is not one of the four
///          excitation axes: loudness, pitch and vibrato depth are channel-level
///          state here, and applying a per-note value to them channel-wide would
///          be indistinguishable to the caller from a binding that took.
SonareError sonare_engine_bind_controller(SonareRealtimeEngine* engine, uint32_t destination_id,
                                          const SonareControllerBinding* binding);

/// @brief Drops every binding of the instrument's controller profile.
/// @details Control-thread API. The instrument keeps a profile; it resolves
///          nothing until something is bound again.
SonareError sonare_engine_clear_controller_bindings(SonareRealtimeEngine* engine,
                                                    uint32_t destination_id);

/// @brief Bindings the instrument's controller profile currently holds.
SonareError sonare_engine_controller_binding_count(SonareRealtimeEngine* engine,
                                                   uint32_t destination_id, size_t* out_count);

/// @brief Whether note-on velocity is expression for this instrument.
/// @details No fixed default is possible: a wind controller ships sending
///          breath-derived velocity on one model and a constant on the next, so
///          each preset states it and a host building its own profile sets it.
///          When zero the synth takes every note at full scale and the bound
///          axes carry the dynamics alone.
SonareError sonare_engine_set_controller_velocity_meaningful(SonareRealtimeEngine* engine,
                                                             uint32_t destination_id,
                                                             int meaningful);

/// @brief Reads back @ref sonare_engine_set_controller_velocity_meaningful.
SonareError sonare_engine_controller_velocity_meaningful(SonareRealtimeEngine* engine,
                                                         uint32_t destination_id,
                                                         int* out_meaningful);

/// @brief The three dimensions MPE carries per note. Mirrors midi::MpeDimension.
typedef enum SONARE_ENUM_BASE {
  SONARE_MPE_DIMENSION_BEND = 0,
  SONARE_MPE_DIMENSION_PRESSURE = 1,
  /// Carried on CC#74.
  SONARE_MPE_DIMENSION_TIMBRE = 2
} SonareMpeDimension;

/// @brief Values in @ref SonareMpeDimension.
#define SONARE_MPE_DIMENSION_COUNT 3

/// @brief Which note a channel-addressed value belongs to when several are
///        sounding on one channel. Mirrors midi::NoteTracking.
/// @details MPE poses the question and declines to answer it — "When there is
///          more than one concurrent Active Note on a Member Channel,
///          implementation of how controllers affect the notes is up to the
///          Device" — so this is a choice a host makes rather than a rule it
///          follows. A released note is never selected, whatever the rule and
///          however long a pedal keeps it sounding.
typedef enum SONARE_ENUM_BASE {
  /// The most recently started. The default.
  SONARE_NOTE_TRACKING_LAST = 0,
  SONARE_NOTE_TRACKING_LOWEST = 1,
  SONARE_NOTE_TRACKING_HIGHEST = 2,
  /// Every sounding note, which is the rule under which the ambiguity does not
  /// arise.
  SONARE_NOTE_TRACKING_ALL = 3
} SonareNoteTracking;

/// @brief Values in @ref SonareNoteTracking.
#define SONARE_NOTE_TRACKING_COUNT 4

/// @brief Sets the note-attribution rule for one per-note dimension.
/// @details Control-thread API. Set per dimension because the useful answers
///          differ: pressure following the newest note while bend reaches every
///          one is a real configuration, not a mistake. It is read only inside
///          an MPE zone — outside one a channel-addressed value is channel-wide
///          by definition — and only while more than one note is sounding on
///          the channel, which an MPE sender avoids by giving each note its own
///          member channel. Returns SONARE_ERROR_NOT_SUPPORTED for a
///          destination whose instrument has nowhere to put a profile.
SonareError sonare_engine_set_controller_note_tracking(SonareRealtimeEngine* engine,
                                                       uint32_t destination_id, int dimension,
                                                       int tracking);

/// @brief Reads back @ref sonare_engine_set_controller_note_tracking.
SonareError sonare_engine_controller_note_tracking(SonareRealtimeEngine* engine,
                                                   uint32_t destination_id, int dimension,
                                                   int* out_tracking);

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

/// @brief Sets how one channel of a destination's instrument treats a note-on
///        while another note on that channel is still held.
/// @details Control-thread API. Returns SONARE_ERROR_NOT_SUPPORTED for a
///          destination whose instrument has no articulation of its own, rather
///          than succeeding quietly: a discarded mode is indistinguishable from
///          one that took until two notes overlap.
///
///          SONARE_ARTICULATION_MONO_LEGATO is a request, not a guarantee. An
///          engine whose exciter is spent at the onset — anything struck or
///          plucked — and a target pitch below what the engine's delay line can
///          hold both fall back to an ordinary note, which
///          @ref sonare_engine_legato_fallback_count counts.
SonareError sonare_engine_set_articulation(SonareRealtimeEngine* engine, uint32_t destination_id,
                                           uint8_t channel, int articulation);

/// @brief Reads back @ref sonare_engine_set_articulation.
SonareError sonare_engine_articulation(SonareRealtimeEngine* engine, uint32_t destination_id,
                                       uint8_t channel, int* out_articulation);

/// @brief How many times a legato continuation was asked for and refused, so
///        the note started a voice of its own instead.
/// @details Counted rather than inferred: a refusal sounds like an ordinary
///          note, so nothing in the audio separates "this engine declines
///          legato" from "the mode was never set". Saturates at UINT32_MAX
///          rather than wrapping, so a large value stays readable as "at least
///          this many".
///
///          Refuses on the same terms as the two calls above rather than
///          answering zero: an instrument that never had an articulation has
///          refused nothing, and a host reading that zero would read it as
///          "every slur took" — the reading this counter exists to prevent.
SonareError sonare_engine_legato_fallback_count(SonareRealtimeEngine* engine,
                                                uint32_t destination_id, uint32_t* out_count);

/// @brief Binds a live MIDI CC to an engine automation parameter.
/// @details Control-thread API. After binding, @ref sonare_engine_push_midi_cc
///          still routes the MIDI event to the destination instrument, and also
///          maps the 7-bit CC value into [min_value, max_value] for @p param_id.
SonareError sonare_engine_bind_midi_cc(SonareRealtimeEngine* engine, uint8_t channel,
                                       uint8_t controller, uint32_t param_id, float min_value,
                                       float max_value);
/// @brief Binds a full 7-bit/14-bit CC, RPN, or NRPN descriptor to the live engine.
/// @details Uses the same descriptor and validation as project MIDI learn/conversion.
/// The scalar @ref sonare_engine_bind_midi_cc remains a 7-bit compatibility shim.
SonareError sonare_engine_bind_midi_cc_binding(SonareRealtimeEngine* engine,
                                               const SonareMidiCcBinding* binding);
/// @brief Clears all live MIDI CC to parameter bindings.
SonareError sonare_engine_clear_midi_cc_bindings(SonareRealtimeEngine* engine);
/// @brief Returns the number of live MIDI CC bindings.
SonareError sonare_engine_midi_cc_binding_count(SonareRealtimeEngine* engine, size_t* out_count);
/// @brief Installs/replaces a live non-destructive MIDI-FX insert for one destination.
/// @details Control-thread API. The JSON accepts the same fields as
///          @ref sonare_project_bake_midi_fx, but scheduled/live MIDI events are
///          transformed at dispatch time and clip contents are not modified.
SonareError sonare_engine_set_midi_fx(SonareRealtimeEngine* engine, uint32_t destination_id,
                                      const char* config_json);
/// @brief Clears the live MIDI-FX insert on one destination.
SonareError sonare_engine_clear_midi_fx(SonareRealtimeEngine* engine, uint32_t destination_id);
/// @brief Enables the engine-owned live MIDI input source for a destination.
/// @details Hosts can push timestamped input events with
///          `sonare_engine_push_midi_input_*`; the engine drains them at block
///          start through the same `set_midi_input_source` path used by native
///          C++ hosts. `destination_id` selects the realtime MIDI destination.
SonareError sonare_engine_set_midi_input_source(SonareRealtimeEngine* engine,
                                                uint32_t destination_id);
/// @brief Clears the engine-owned live MIDI input source.
SonareError sonare_engine_clear_midi_input_source(SonareRealtimeEngine* engine);
/// @brief Number of queued events in the engine-owned live MIDI input source.
SonareError sonare_engine_midi_input_pending_count(SonareRealtimeEngine* engine, size_t* out_count);
SonareError sonare_engine_push_midi_input_note_on(SonareRealtimeEngine* engine, uint8_t group,
                                                  uint8_t channel, uint8_t note, uint8_t velocity,
                                                  int64_t port_time_samples);
SonareError sonare_engine_push_midi_input_note_off(SonareRealtimeEngine* engine, uint8_t group,
                                                   uint8_t channel, uint8_t note, uint8_t velocity,
                                                   int64_t port_time_samples);
SonareError sonare_engine_push_midi_input_cc(SonareRealtimeEngine* engine, uint8_t group,
                                             uint8_t channel, uint8_t controller, uint8_t value,
                                             int64_t port_time_samples);
/// @brief Pushes a live MIDI pitch bend to the engine-owned MIDI input source.
/// @param bend14 Unsigned 14-bit bend, centre 8192 (0..16383).
SonareError sonare_engine_push_midi_input_pitch_bend(SonareRealtimeEngine* engine, uint8_t group,
                                                     uint8_t channel, uint16_t bend14,
                                                     int64_t port_time_samples);
/// @brief Pushes a live MIDI channel pressure to the engine-owned MIDI input source.
/// @param pressure 7-bit channel pressure (0..127).
SonareError sonare_engine_push_midi_input_channel_pressure(SonareRealtimeEngine* engine,
                                                           uint8_t group, uint8_t channel,
                                                           uint8_t pressure,
                                                           int64_t port_time_samples);
/// @brief Pushes a live MIDI polyphonic key pressure to the engine-owned MIDI input source.
/// @param note Key the pressure belongs to (0..127).
/// @param pressure 7-bit key pressure (0..127).
SonareError sonare_engine_push_midi_input_poly_pressure(SonareRealtimeEngine* engine, uint8_t group,
                                                        uint8_t channel, uint8_t note,
                                                        uint8_t pressure,
                                                        int64_t port_time_samples);
/// @brief Queues an immediate live MIDI note-on to a MIDI destination.
SonareError sonare_engine_push_midi_note_on(SonareRealtimeEngine* engine, uint32_t destination_id,
                                            uint8_t group, uint8_t channel, uint8_t note,
                                            uint8_t velocity, int64_t render_frame);
/// @brief Queues an immediate live MIDI note-off to a MIDI destination.
SonareError sonare_engine_push_midi_note_off(SonareRealtimeEngine* engine, uint32_t destination_id,
                                             uint8_t group, uint8_t channel, uint8_t note,
                                             uint8_t velocity, int64_t render_frame);
/// @brief Queues an immediate (live) MIDI control change to a MIDI destination.
/// @details Routed through the engine's queueable scalar MIDI command path; the
///          synthesized MIDI 1.0 CC reaches the registered host instrument at
///          @p render_frame. Values are 7-bit; channel 0..15, group 0..15.
/// @param destination_id MIDI destination id (clip/instrument destination).
/// @param group UMP group (0..15).
/// @param channel MIDI channel (0..15).
/// @param controller Controller number (0..127).
/// @param value 7-bit controller value (0..127).
/// @param render_frame Render-frame time to apply, or -1 for immediate.
SonareError sonare_engine_push_midi_cc(SonareRealtimeEngine* engine, uint32_t destination_id,
                                       uint8_t group, uint8_t channel, uint8_t controller,
                                       uint8_t value, int64_t render_frame);
/// @brief Queues an immediate (live) MIDI pitch bend to a MIDI destination.
/// @details The three per-note expression dimensions travel as a single-word
///          MIDI 1.0 UMP on the engine's queueable command path, which is what
///          carries them unchanged at their own width: a bend is 14-bit and no
///          7-bit scalar command can spell it.
/// @param destination_id MIDI destination id (clip/instrument destination).
/// @param group UMP group (0..15).
/// @param channel MIDI channel (0..15).
/// @param bend14 Unsigned 14-bit bend, centre 8192 (0..16383).
/// @param render_frame Render-frame time to apply, or -1 for immediate.
SonareError sonare_engine_push_midi_pitch_bend(SonareRealtimeEngine* engine,
                                               uint32_t destination_id, uint8_t group,
                                               uint8_t channel, uint16_t bend14,
                                               int64_t render_frame);
/// @brief Queues an immediate (live) MIDI channel pressure to a MIDI destination.
/// @param pressure 7-bit channel pressure (0..127).
/// @param render_frame Render-frame time to apply, or -1 for immediate.
SonareError sonare_engine_push_midi_channel_pressure(SonareRealtimeEngine* engine,
                                                     uint32_t destination_id, uint8_t group,
                                                     uint8_t channel, uint8_t pressure,
                                                     int64_t render_frame);
/// @brief Queues an immediate (live) MIDI polyphonic key pressure to a MIDI destination.
/// @param note Key the pressure belongs to (0..127).
/// @param pressure 7-bit key pressure (0..127).
/// @param render_frame Render-frame time to apply, or -1 for immediate.
SonareError sonare_engine_push_midi_poly_pressure(SonareRealtimeEngine* engine,
                                                  uint32_t destination_id, uint8_t group,
                                                  uint8_t channel, uint8_t note, uint8_t pressure,
                                                  int64_t render_frame);
/// @brief Queues a MIDI panic (all-notes-off) releasing every sounding note.
/// @param render_frame Render-frame time to apply, or -1 for immediate.
SonareError sonare_engine_push_midi_panic(SonareRealtimeEngine* engine, int64_t render_frame);
/// @brief Queues an immediate (live) MIDI SysEx message to a MIDI destination.
/// @details The bytes are copied into a bounded, allocation-free engine store and
///          a scalar-only command referencing the store slot is enqueued (no
///          pointer crosses the realtime queue, keeping it WASM
///          SharedArrayBuffer-safe). The audio thread dispatches the SysEx to the
///          destination instrument at @p render_frame, where a SoundFont/GS
///          instrument feeds it to the GS layer (GS Reset / GM System On /
///          insertion-effect config). @p data is the full SysEx frame including
///          the leading 0xF0 and trailing 0xF7.
/// @param destination_id MIDI destination id (clip/instrument destination).
/// @param data SysEx bytes (0xF0..0xF7 frame). Must be non-NULL.
/// @param size Byte count; must be 1..512.
/// @param render_frame Render-frame time to apply, or -1 for immediate.
SonareError sonare_engine_push_midi_sysex(SonareRealtimeEngine* engine, uint32_t destination_id,
                                          const uint8_t* data, size_t size, int64_t render_frame);
/// @brief Marks a MIDI destination for external routing (or clears it).
/// @details A destination marked external bypasses the internal instrument rack:
///   its sequenced events are buffered in the engine's external-MIDI output queue
///   for the host to drain with @ref sonare_engine_drain_external_midi and deliver
///   to an external device. Control-thread only. @p external != 0 marks, 0 clears.
///   At most 16 destinations can be external at once; marking a 17th distinct
///   destination returns @ref SONARE_ERROR_INVALID_PARAMETER instead of silently
///   routing it to the internal rack.
SonareError sonare_engine_set_midi_destination_external(SonareRealtimeEngine* engine,
                                                        uint32_t destination_id, int external);
/// @brief Enables forwarding MIDI clock/transport bytes to the external queue.
/// @details When enabled, MIDI clock (0xF8) and transport (start/continue/stop)
///   bytes are enqueued tagged with destination 0xFFFFFFFF so external gear can be
///   tempo-synced. Control-thread only; off by default.
SonareError sonare_engine_set_external_midi_clock_enabled(SonareRealtimeEngine* engine,
                                                          int enabled);
/// @brief Number of external-MIDI events dropped because the queue was full.
/// @details Advisory telemetry; monotonic within a prepared session.
SonareError sonare_engine_external_midi_dropped_count(SonareRealtimeEngine* engine,
                                                      uint32_t* out_count);
/// @brief Drains queued external-MIDI events, lowered to MIDI 1.0 byte messages.
/// @details Each output slot is a @ref SonareExternalMidiEvent. A single queued
///   channel-voice event may lower to more than one output event (e.g. a MIDI 2.0
///   program change with bank select), so the engine only consumes a queue record
///   when its lowered messages all fit in the remaining output capacity; the rest
///   stay queued for the next call. UMP types that do not lower to MIDI 1.0
///   (SysEx/Data, Utility, MIDI-2-only controllers) are skipped. Writes the number
///   of output events to @p out_count. @p max_events must be at least 3 (the most
///   one record can lower to); call repeatedly until @p out_count is 0 to fully
///   drain. Host/control-thread only.
SonareError sonare_engine_drain_external_midi(SonareRealtimeEngine* engine,
                                              SonareExternalMidiEvent* out, size_t max_events,
                                              size_t* out_count);
/// @brief Reads the current engine transport state (playing/position/ppq/tempo).
SonareError sonare_engine_get_transport_state(SonareRealtimeEngine* engine,
                                              SonareTransportState* out);

#ifdef __cplusplus
}
#endif
