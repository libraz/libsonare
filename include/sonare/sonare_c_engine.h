#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sonare_c_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @section engine_threading Thread safety (RealtimeEngine)
/// @details The engine surface (`sonare_engine_*`) is built around two roles:
/// the AUDIO thread (the render callback) and one CONTROL thread (the host / UI
/// thread that drives the transport and edits state). Unless noted otherwise a
/// given engine handle has exactly one of each; do not call from more than one
/// control thread without external serialization.
///
/// AUDIO-thread functions (realtime-safe: no allocation, no lock, no throw —
/// call only from the render callback):
/// - `sonare_engine_process`, `sonare_engine_process_with_monitor`.
///
/// CONTROL-thread, realtime-safe hand-off (lock-free, non-allocating; safe to
/// call concurrently with `sonare_engine_process` on the same handle, adopted
/// at the next block boundary). These either enqueue a command on the engine's
/// lock-free command queue or publish through a lock-free snapshot:
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
///   `sonare_engine_reset_capture`. (These publish the capture state through a
///   lock-free snapshot adopted by the audio thread; the backing capture buffer
///   passed to `set_capture_buffer` must outlive capture and must not be freed
///   while the engine is armed.) Note `reset_capture` clears the captured-frame
///   counter and so should be issued while not actively capturing.
///
/// CONTROL-thread, NON realtime-safe (allocate / build internal structures —
/// call only from the control thread, and NOT concurrently with
/// `sonare_engine_process` unless your engine build documents otherwise; these
/// are intended to be issued between renders or while stopped):
/// - Lifecycle: `sonare_engine_create`, `sonare_engine_destroy`,
///   `sonare_engine_prepare`.
/// - Topology / registration: `sonare_engine_set_graph`,
///   `sonare_engine_set_clips`, `sonare_engine_add_parameter`,
///   `sonare_engine_set_automation_lane`, `sonare_engine_set_markers`,
///   `sonare_engine_set_metronome`, `sonare_engine_set_track_lanes`,
///   `sonare_engine_set_track_strip_json`,
///   `sonare_engine_set_master_strip_json`.
/// - Offline render: `sonare_engine_render_offline`,
///   `sonare_engine_bounce_offline`, `sonare_engine_freeze_offline` (these own
///   the audio role internally; do not also call them from a render callback).
///
/// CONTROL-thread read-back (safe to call concurrently with the audio thread;
/// returns a consistent snapshot, may lag the audio thread by up to one block):
/// - `sonare_engine_get_transport_state`, `sonare_engine_capture_status`,
///   `sonare_engine_parameter_*`, `sonare_engine_*_count`,
///   `sonare_engine_marker*`, `sonare_engine_metronome`,
///   `sonare_engine_drain_telemetry`, `sonare_engine_drain_meter_telemetry`.
///   The drain functions are single-consumer: drive them from one thread only.
SonareError sonare_engine_create(SonareRealtimeEngine** out);
void sonare_engine_destroy(SonareRealtimeEngine* engine);
SonareError sonare_engine_prepare(SonareRealtimeEngine* engine, double sample_rate,
                                  int max_block_size, size_t command_capacity,
                                  size_t telemetry_capacity);
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
SonareError sonare_engine_set_tempo(SonareRealtimeEngine* engine, double bpm);
SonareError sonare_engine_set_time_signature(SonareRealtimeEngine* engine, int numerator,
                                             int denominator);
SonareError sonare_engine_sample_at_ppq(SonareRealtimeEngine* engine, double ppq,
                                        int64_t* out_sample);
SonareError sonare_engine_set_loop(SonareRealtimeEngine* engine, double start_ppq, double end_ppq,
                                   int enabled);
/// @brief Registers a parameter's metadata for automation UIs.
/// @details Control-thread only (allocates the name/unit string copies). Returns
///   @c SONARE_ERROR_INVALID_PARAMETER if @p info is NULL, the value range is
///   inverted, or a parameter with the same id is already registered (duplicate
///   ids are rejected, not replaced — clear and re-register to change metadata).
///   On rejection no backing strings are retained, so repeated re-registration
///   does not leak.
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
SonareError sonare_engine_set_automation_lane(SonareRealtimeEngine* engine, uint32_t param_id,
                                              const SonareAutomationPoint* points,
                                              size_t point_count);
SonareError sonare_engine_automation_lane_count(SonareRealtimeEngine* engine, size_t* out_count);
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
///   @p source_track_id 0 removes the binding. Control-thread only.
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
/// @details @p param_name is a key returned by @ref sonare_mastering_insert_param_info
///   (which also gives each param's `rtSafe` flag). The name is resolved to the
///   integer param_id on the control thread and applied at the next block head
///   via the realtime command queue, so this is safe to call during playback
///   without rebuilding the strip. @p insert_index addresses the combined pre/post
///   insert sequence. Returns SONARE_ERROR_INVALID_PARAMETER if the track, insert,
///   or name is unknown, the param is not realtime-safe, or the command queue is
///   full; track/insert/param indices must each fit in 8 bits.
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
///   SONARE_ERROR_INVALID_PARAMETER if the bus, insert, or name is unknown.
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
SonareError sonare_engine_set_capture_buffer(SonareRealtimeEngine* engine,
                                             const SonareEngineCaptureBuffer* buffer);
SonareError sonare_engine_arm_capture(SonareRealtimeEngine* engine, int armed);
SonareError sonare_engine_set_capture_punch(SonareRealtimeEngine* engine, int64_t start_sample,
                                            int64_t end_sample, int enabled);
SonareError sonare_engine_set_capture_source(SonareRealtimeEngine* engine,
                                             SonareEngineCaptureSource source);
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
SonareError sonare_engine_process(SonareRealtimeEngine* engine, float* const* channels,
                                  int num_channels, int num_frames);
SonareError sonare_engine_process_with_monitor(SonareRealtimeEngine* engine, float* const* channels,
                                               float* const* monitor_out, int num_channels,
                                               int num_frames);
SonareError sonare_engine_render_offline(SonareRealtimeEngine* engine, float* const* out,
                                         int num_channels, int64_t total_frames, int block_size);
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
SonareError sonare_engine_freeze_offline(SonareRealtimeEngine* engine,
                                         const SonareEngineFreezeOptions* options,
                                         SonareEngineFreezeResult* out);
SonareError sonare_engine_drain_telemetry(SonareRealtimeEngine* engine, SonareEngineTelemetry* out,
                                          size_t max_records, size_t* written);
/// @brief Drains pending meter telemetry records published by the engine.
/// @param out Caller-owned array receiving up to @p max_records entries.
/// @param max_records Capacity of @p out. May be 0 to query without copying.
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
/// @param max_records Capacity of @p out. May be 0 to query without copying.
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
/// @param max_records Capacity of @p out. May be 0 to query without copying.
/// @param out_count Receives the number of records written. Each record carries
///   band_count FFT bands and point_count interleaved left/right goniometer pairs.
SonareError sonare_engine_drain_scope_telemetry(SonareRealtimeEngine* engine,
                                                SonareScopeTelemetryRecord* out, size_t max_records,
                                                size_t* out_count);
/// @brief Pushes a live parameter value to the engine (immediate jump).
/// @param param_id Target parameter id.
/// @param value New value.
/// @param render_frame Render-frame time to apply, or -1 for immediate.
SonareError sonare_engine_set_parameter(SonareRealtimeEngine* engine, uint32_t param_id,
                                        float value, int64_t render_frame);
/// @brief Pushes a live parameter value to the engine using a smoothed ramp.
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

/// @brief One render-frame MIDI event for @ref sonare_engine_set_midi_clips.
/// @details Mirrors midi::MidiEvent with a fixed UMP payload. `render_frame` is
///          an absolute sample position on the engine timeline. `word_count` is
///          the number of active UMP words (1..4); 0 lets the C bridge infer a
///          one-word MIDI 1.0 event when only `word0` is set.
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
  int waveform;     /* 0=sine, 1=saw, 2=square, 3=triangle */
  float gain;       /* 0 => 0.2 */
  float attack_ms;  /* 0 => 5 */
  float decay_ms;   /* 0 => 60 */
  float sustain;    /* 0 => 0.7 */
  float release_ms; /* 0 => 120 */
  int polyphony;    /* 0 => 16, clamped to [1,64] */
} SonareEngineBuiltinSynthConfig;

/// @brief Binds/replaces a built-in synth on a realtime MIDI destination.
/// @details Control-thread API. The engine owns the synth instance. Live MIDI
///          note/CC commands and scheduled MIDI clips routed to @p destination_id
///          render through this instrument.
SonareError sonare_engine_set_builtin_instrument(SonareRealtimeEngine* engine,
                                                 uint32_t destination_id,
                                                 const SonareEngineBuiltinSynthConfig* config);

/// @brief Binds/replaces the patch-driven NativeSynth on a realtime MIDI
///        destination (the full synthesizer: subtractive / FM / Karplus-Strong
///        / modal / additive / percussion / waveguide-piano engines). The
///        patch resolves exactly like the project bounce surface
///        (@ref SonareSynthPatch: preset catalog base + field overrides); an
///        invalid struct_version or unknown preset name fails with
///        SONARE_ERROR_INVALID_PARAMETER. Control-thread API; the engine owns
///        the synth. Live MIDI input and scheduled MIDI clips routed to
///        @p destination_id render through it.
SonareError sonare_engine_set_synth_instrument(SonareRealtimeEngine* engine,
                                               uint32_t destination_id,
                                               const SonareSynthPatch* patch);
/// @brief Loads (parses) SF2 bytes into the realtime engine so SoundFont
///        instruments can be bound to destinations with
///        @ref sonare_engine_set_sf2_instrument. Control-thread API; replaces
///        any previously loaded SoundFont (already-bound SF2 instruments keep
///        the SoundFont they were created with). The bytes are copied/decoded,
///        so @p data may be freed after the call.
SonareError sonare_engine_load_soundfont(SonareRealtimeEngine* engine, const uint8_t* data,
                                         size_t size);

/// @brief Versioned SF2 player patch for @ref sonare_engine_set_sf2_instrument.
/// @details Same zero-init contract as the project-bounce SF2 instruments:
///          every field uses "0 => default" (struct_version 0 => version 1).
typedef struct {
  int struct_version; /* 0 or 1 => version 1 */
  float gain;         /* master output gain (linear); 0 => 0.5 */
  int polyphony;      /* max simultaneous voices; 0 => 48, clamped to [1, 64] */
} SonareEngineSf2InstrumentConfig;

/// @brief Binds/replaces a GS-compatible SoundFont player on a realtime MIDI
///        destination, fed by the engine's loaded SoundFont
///        (@ref sonare_engine_load_soundfont). Without a loaded SoundFont —
///        or for programs the SoundFont does not cover — notes play through
///        the built-in synthesizer GM fallback bank (the data-free floor).
///        Control-thread API; the engine owns the player. Live MIDI input
///        (`sonare_engine_push_midi_input_*` / `sonare_engine_push_midi_note_*`)
///        and scheduled MIDI clips routed to @p destination_id render through
///        the player (16 MIDI channels, channel 10 drums, GS NRPN part edits,
///        GS/GM SysEx resets).
SonareError sonare_engine_set_sf2_instrument(SonareRealtimeEngine* engine, uint32_t destination_id,
                                             const SonareEngineSf2InstrumentConfig* config);

/// @brief Clears any realtime instrument bound to @p destination_id.
SonareError sonare_engine_clear_midi_instrument(SonareRealtimeEngine* engine,
                                                uint32_t destination_id);
SonareError sonare_engine_midi_instrument_count(SonareRealtimeEngine* engine, size_t* out_count);
/// @brief Binds a live MIDI CC to an engine automation parameter.
/// @details Control-thread API. After binding, @ref sonare_engine_push_midi_cc
///          still routes the MIDI event to the destination instrument, and also
///          maps the 7-bit CC value into [min_value, max_value] for @p param_id.
SonareError sonare_engine_bind_midi_cc(SonareRealtimeEngine* engine, uint8_t channel,
                                       uint8_t controller, uint32_t param_id, float min_value,
                                       float max_value);
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
