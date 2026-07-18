#pragma once

/// @file realtime_engine.h
/// @brief Pass-through realtime engine skeleton.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "automation/automation_engine.h"
#include "engine/boundary_splitter.h"
#include "engine/capture.h"
#include "engine/clip_player.h"
#include "engine/metronome.h"
#include "engine/telemetry.h"
#include "rt/command.h"
#include "rt/param_smoother.h"
#include "rt/rt_publisher.h"
#include "rt/seqlock_cell.h"
#include "rt/spsc_queue.h"
#include "transport/marker.h"
#include "transport/tempo_map.h"
#include "transport/transport.h"

#if defined(SONARE_WITH_ARRANGEMENT)
#include "engine/channel_delay.h"
#include "engine/instrument_rack.h"
#include "host/midi_io.h"
#include "midi/cc_map.h"
#include "midi/clock_sync.h"
#include "midi/instrument.h"
#include "midi/midi_clip.h"
#include "midi/sequencer.h"
#endif
#if defined(SONARE_WITH_GRAPH)
#include "engine/graph_runtime.h"
#endif
#if defined(SONARE_WITH_MIXING)
#include "engine/meter_telemetry.h"
#include "engine/mixing_runtime.h"
#include "engine/monitor_runtime.h"
#include "engine/scope_telemetry.h"
#include "engine/track_mixer.h"
#endif

namespace sonare::engine {

enum class CaptureSource {
  kOutput = 0,
  kInput = 1,
};

/// @brief Realtime audio engine.
///
/// @par Thread-safety contract
/// RealtimeEngine has two callers: a single **audio thread** that drives
/// @c process / @c process_with_monitor / @c render_offline, and a single
/// **control thread** (host UI/scripting) that issues parameter changes and
/// configuration mutations. The two threads must never enter the same
/// non-noexcept method concurrently.
/// - **Audio-thread-safe (RT-safe, noexcept, allocation-free after prepare):**
///   @c process, @c process_with_monitor, @c push_command (lock-free SPSC
///   producer; control-thread is the sole writer in normal flow but the
///   underlying queue is wait-free), @c pop_telemetry,
///   @c pop_clip_page_request, @c pop_meter_telemetry, @c set_loop (publishes
///   the loop region through a seqlock so the audio thread reads a torn-free
///   {start, end, enabled} snapshot),
///   @c marker_by_index/id, @c seek_marker, @c set_loop_from_markers,
///   @c set_mixing_enabled, @c set_monitoring_enabled (atomic flags),
///   @c set_param_smoothing_ms, @c set_graph_latency_samples_q8,
///   @c transport, @c automation accessors, all @c *_count noexcept getters.
/// - **Control-thread seqlock publishers (single writer):** @c set_capture_*
///   and @c reset_capture publish the arm/punch capture state through a seqlock
///   (see @ref sonare::engine::CaptureSink). They are noexcept and
///   allocation-free and may run concurrently with @c process (the audio thread
///   is the torn-free seqlock reader), but they are the sole writer and must be
///   issued from the control thread only — never from the audio thread.
/// - **Control-thread-preferred (lock-free but NOT torn-read-safe):**
///   @c set_metronome_config replaces the metronome config with a plain
///   non-atomic struct copy that the audio thread reads field-by-field. It is
///   noexcept and allocation-free, but a concurrent audio-thread read may
///   observe a partially-updated config for one block; call it from the
///   control thread between blocks, or route changes through @c push_command.
/// - **Control-thread-only (NOT RT-safe; may allocate or take time):**
///   @c prepare, @c render_offline (offline use), @c set_tempo,
///   @c set_tempo_segments, @c set_time_signature,
///   @c set_time_signature_segments, @c set_markers, @c set_clips,
///   @c bind_mixing_strip, @c set_master_strip, @c set_track_strip,
///   @c set_bus_strip, @c bind_track_strip, @c add_monitor_strip,
///   @c remove_monitor_strip, @c swap_graph, @c bind_graph_parameter. These must
///   be called from the thread that owns engine lifecycle, between blocks; do
///   NOT call them from the audio callback AND do NOT call them concurrently with
///   @c process / @c process_with_monitor / @c render_offline. The strip binders
///   (@c set_master_strip, @c set_track_strip, @c set_bus_strip) in particular
///   rebind a raw ChannelStrip pointer the audio thread dereferences and destroy
///   the previously bound strip immediately (no deferred reclaim), so a
///   concurrent render would dereference freed memory. Unlike the graph / clip /
///   automation swaps (which publish through an @c rt::RtPublisher and reclaim
///   the old snapshot off the audio thread), strip binding is not on a deferred
///   reclaim path and relies on this not-concurrent-with-process contract.
/// Cross-thread state changes that must reach the audio thread (e.g. tempo,
/// parameter automation) flow through @c push_command and the SPSC command
/// queue, drained inside @c process at sub-block boundaries.
class RealtimeEngine : private ClipPageRequestSink {
 public:
  static constexpr size_t kMaxCommandsPerBlock = 64;
  static constexpr size_t kMaxPendingCommands = 64;

#if defined(SONARE_WITH_ARRANGEMENT)
  class MidiSyncSink {
   public:
    virtual ~MidiSyncSink() = default;
    virtual void on_midi_sync_byte(int64_t render_frame, uint8_t byte) noexcept = 0;
  };
#endif

  void prepare(double sample_rate, int max_block_size, size_t command_capacity = 1024,
               size_t telemetry_capacity = 1024);
  double sample_rate() const noexcept { return sample_rate_; }

  void process(float* const* io, int num_channels, int num_frames) noexcept;
  void process_with_monitor(float* const* io, float* const* monitor_out, int num_channels,
                            int num_frames) noexcept;
  /// @brief Renders @p total_frames of output offline from the CURRENT transport
  /// position. The transport is rolled for the duration of the render (and the
  /// prior play/stop state restored), so clips and sequenced MIDI render even
  /// when called while stopped. NOT idempotent: it advances the transport like
  /// @c process, so a second call renders the NEXT span, not a re-render of the
  /// same one. To bounce the same span again, seek the transport back to 0 (push
  /// a kTransportSeekSample command with arg 0) before re-calling.
  void render_offline(float* const* out, int num_channels, int64_t total_frames, int block_size);

  bool push_command(const rt::Command& command) noexcept;
  bool pop_telemetry(Telemetry& out) noexcept;
  bool pop_clip_page_request(ClipPageRequest& out) noexcept { return clip_page_requests_.pop(out); }
#if defined(SONARE_WITH_MIXING)
  bool pop_meter_telemetry(MeterTelemetryRecord& out) noexcept { return meter_tap_.pop(out); }
  bool pop_scope_telemetry(ScopeTelemetryRecord& out) noexcept { return scope_tap_.pop(out); }
  // Enables per-target spectrum + vectorscope capture. @p interval_frames is the
  // minimum render-frame gap between published snapshots per block (0 disables
  // capture). @p band_count (1..ScopeTelemetryRecord::kMaxBands) is the FFT band
  // resolution; changing it re-prepares the tap, so call from the control thread
  // while process() is not running. Returns the band count actually applied.
  uint32_t configure_scope_telemetry(int interval_frames, uint32_t band_count) noexcept;
  uint32_t scope_band_count() const noexcept { return scope_tap_.band_count(); }
#endif
  transport::TransportState transport_state_control() const noexcept;
  void set_tempo(double bpm);
  void set_tempo_segments(std::vector<transport::TempoSegment> segments);
  void set_time_signature(int numerator, int denominator);
  void set_time_signature_segments(std::vector<transport::TimeSignatureSegment> segments);
  int64_t sample_at_ppq(double ppq) const noexcept;
  double bpm_at_sample(int64_t sample) const noexcept {
    const transport::TempoMap* map = tempo_map_snapshot_.control_current().get();
    return (map ? map : &tempo_map_)->bpm_at_sample(sample);
  }
  transport::TimeSignature time_signature_at_ppq(double ppq) const noexcept {
    const transport::TempoMap* map = tempo_map_snapshot_.control_current().get();
    return (map ? map : &tempo_map_)->time_signature_at_ppq(ppq);
  }
  void set_loop(double start_ppq, double end_ppq, bool enabled) noexcept;
  void set_markers(std::vector<transport::Marker> markers);
  size_t marker_count() const noexcept { return markers_.marker_count(); }
  bool marker_by_index(size_t index, transport::Marker* out) const noexcept;
  bool marker_by_id(uint32_t id, transport::Marker* out) const noexcept;
  bool seek_marker(uint32_t marker_id) noexcept;
  bool set_loop_from_markers(uint32_t start_marker_id, uint32_t end_marker_id) noexcept;
  void set_metronome_config(MetronomeConfig config) noexcept;
  const MetronomeConfig& metronome_config() const noexcept { return metronome_.config(); }
  int64_t count_in_end_sample(int64_t start_sample, int bars) const noexcept;
  void set_clips(std::vector<ClipSchedule> clips);
  size_t clip_count() const noexcept { return clip_player_.clip_count(); }
#if defined(SONARE_WITH_ARRANGEMENT)
  // Control-thread direct-setter: publishes a compiled MIDI clip set through the
  // sequencer's RtPublisher (NOT an rt::Command, no ABI bump). The audio thread
  // adopts it at block start and fires sample-accurate UMP events. Available
  // only when the arrangement subsystem (and thus the MidiSequencer member) is
  // compiled in.
  void set_midi_clips(std::vector<midi::MidiClipSchedule> clips);
  size_t midi_clip_count() const noexcept { return midi_sequencer_.clip_count(); }
  bool set_midi_fx(uint32_t destination_id, const midi::MidiFxChain& chain) noexcept;
  void clear_midi_fx(uint32_t destination_id) noexcept;
  void set_midi_input_source(host::MidiInputSource* source, uint32_t destination_id = 0) noexcept {
    midi_input_destination_id_.store(destination_id, std::memory_order_relaxed);
    midi_input_source_.store(source, std::memory_order_release);
  }
  // Control-thread: enqueue a live MIDI SysEx message for `destination_id`. The
  // bytes are copied into a bounded, allocation-free payload store slot and a
  // kMidiSysExImmediate command carrying only a scalar slot reference is pushed,
  // so no pointer crosses the SharedArrayBuffer-shared command queue. The audio
  // thread resolves the slot, dispatches a MidiEvent viewing the slot bytes
  // (consumed synchronously by the destination instrument inside apply_command),
  // and never allocates. `data`/`size` are the full SysEx frame (leading 0xF0 /
  // trailing 0xF7 included, as the GS layer expects). Returns false when the
  // arguments are invalid, the payload exceeds kMaxSysExPayloadBytes, or the
  // command queue is full. render_frame < 0 fires at the block head.
  bool push_midi_sysex(uint32_t destination_id, const uint8_t* data, size_t size,
                       int64_t render_frame) noexcept;
  bool bind_midi_cc(uint8_t controller, uint8_t channel, uint32_t param_id, float min_value,
                    float max_value) noexcept;
  void clear_midi_cc_bindings() noexcept;
  size_t midi_cc_binding_count() const noexcept;
  void set_midi_output_sink(host::MidiOutputSink* sink) noexcept {
    midi_dispatch_sink_.output.store(sink, std::memory_order_release);
  }
  void set_midi_sync_sink(MidiSyncSink* sink) noexcept {
    midi_sync_sink_.store(sink, std::memory_order_release);
  }
  // Control-thread: route the MIDI of `destination_id` to the external output
  // queue INSTEAD of the internal instrument rack, so the track plays an
  // external device rather than a built-in synth. Clearing it (external=false)
  // restores internal-rack routing. At most InstrumentRack::kMaxInstruments
  // destinations may be external at once; excess requests are ignored. Routing
  // a destination external does not by itself produce audio, so no internal
  // synth voices are stolen for it (the rack never receives its events).
  // Returns false when enabling a new external destination would exceed the
  // kMaxExternalDestinations slot table; true otherwise (idempotent enable/disable).
  bool set_midi_destination_external(uint32_t destination_id, bool external) noexcept;
  // Control/host thread: drain up to `capacity` queued external-MIDI records
  // (channel-voice events tagged with their destination, plus transport/clock
  // bytes tagged host::kTransportDestination when external clock is enabled).
  // RT-safe on the producer side; this consumer side removes drained records.
  //
  // render_frame coordinate: every record -- sequenced channel-voice events,
  // live-input injection, and clock/transport bytes -- carries the monotonic
  // DEVICE render frame. Sequenced events are stamped in timeline samples
  // internally and translated to the device frame as they enter the queue (the
  // dispatch sink's per-sub-block offset), so the drained order stays monotonic
  // across a loop wrap or seek (where the timeline jumps but the device clock
  // keeps rising). A host can schedule directly against the device clock without
  // reconciling coordinates.
  size_t drain_external_midi(host::ExternalMidiRecord* out, size_t capacity) noexcept;
  // Control-thread: enable/disable forwarding MIDI clock (0xF8) and transport
  // (start/continue/stop) bytes to the external output queue so external gear
  // can be tempo-synced to the transport. Off by default.
  void set_external_midi_clock_enabled(bool enabled) noexcept;
  // Number of external-MIDI events dropped because the output queue was full
  // (advisory telemetry; cleared by reset).
  uint32_t external_midi_dropped_count() const noexcept {
    return external_midi_queue_.dropped_count();
  }
  midi::MidiSequencer& midi_sequencer() noexcept { return midi_sequencer_; }
  const midi::MidiSequencer& midi_sequencer() const noexcept { return midi_sequencer_; }

  // Control-thread: register (or clear, with nullptr) the host instrument node
  // whose audio is summed at the CLIP/source-merge stage of process_subblock —
  // the same source layer as the clip player, BEFORE the mixing strip stage —
  // so instrument output flows through channel strips + monitor/graph normally
  // and its PDC/latency matches clips. Default nullptr: when absent the engine
  // behaves EXACTLY as before (opt-in; no audio-path side effects).
  //
  // The instrument IS-A midi::MidiEventSink, so registering it also makes it the
  // MidiSequencer's sink: dispatched MIDI events reach the instrument at
  // sample-accurate render frames during the block, and the instrument renders
  // them into its prepared scratch buffer. Clearing it restores a null sink.
  //
  // RT contract: the engine never allocates for the instrument on the audio
  // thread; the per-block scratch buffer is sized in prepare(). The instrument's
  // own prepare()/process()/on_event() must honor the rt::ProcessorBase contract.
  void set_midi_instrument(midi::MidiInstrument* instrument) noexcept;
  // Per-destination registration: bind (or clear, with nullptr) the host
  // instrument that renders MIDI routed to `destination_id` (the compiler stamps
  // each MidiClipSchedule with its track's Track.midi_destination_id). The
  // single-argument overload above binds the default destination 0, preserving
  // the prior single-instrument behavior. Returns false only when a new binding
  // cannot be added because the rack is full (kMaxInstruments). Control-thread
  // only; swapping/clearing first releases notes sounding on that destination so
  // the outgoing instrument does not hang. May prepare() the instrument
  // (allocates) when the engine is already prepared.
  bool set_midi_instrument(uint32_t destination_id, midi::MidiInstrument* instrument) noexcept;
  midi::MidiInstrument* midi_instrument() const noexcept { return instrument_rack_.get(0); }
  midi::MidiInstrument* midi_instrument(uint32_t destination_id) const noexcept {
    return instrument_rack_.get(destination_id);
  }
  size_t midi_instrument_count() const noexcept { return instrument_rack_.size(); }
  // Highest instrument latency (samples) across all registered instruments (0
  // when none). Fed into the arrangement compiler's CompiledTimeline PDC /
  // latency summary.
  int midi_instrument_latency_samples() const noexcept {
    return instrument_rack_.max_latency_samples();
  }
#endif
  void set_capture_segment(CaptureSegment segment) noexcept;
  void set_capture_armed(bool armed) noexcept;
  void set_capture_punch(int64_t start_sample, int64_t end_sample, bool enabled) noexcept;
  void set_capture_source(CaptureSource source) noexcept {
    capture_source_.store(source, std::memory_order_release);
  }
  CaptureSource capture_source() const noexcept {
    return capture_source_.load(std::memory_order_acquire);
  }
  void set_record_offset_samples(int64_t offset_samples) noexcept {
    record_offset_samples_.store(offset_samples, std::memory_order_release);
  }
  int64_t record_offset_samples() const noexcept {
    return record_offset_samples_.load(std::memory_order_acquire);
  }
  void set_input_monitor(bool enabled, float gain) noexcept {
    input_monitor_.store(InputMonitorState{enabled, gain});
  }
  bool input_monitor_enabled() const noexcept { return input_monitor_.load().enabled; }
  float input_monitor_gain() const noexcept { return input_monitor_.load().gain; }
  void reset_capture() noexcept;
  int64_t captured_frames() const noexcept { return capture_sink_.captured_frames(); }
  uint32_t capture_overflow_count() const noexcept { return capture_sink_.overflow_count(); }
  bool capture_armed() const noexcept { return capture_sink_.armed(); }
  bool capture_punch_enabled() const noexcept { return capture_sink_.punch_enabled(); }
#if defined(SONARE_WITH_MIXING)
  /// @brief Total insert-parameter automation slot-table overflows since prepare().
  /// Sums the master-strip table and the per-lane/bus tables. The same delta is
  /// surfaced on the telemetry channel as kInsertAutomationOverflow so a host can
  /// observe dropped automation targets without polling.
  uint32_t insert_automation_overflow_count() const noexcept {
    return master_insert_automation_overflow_count_ +
           track_mixer_runtime_.insert_automation_overflow_count();
  }
#endif
  static bool parameter_target_reserved(uint32_t target_id) noexcept;
  automation::AutomationEngine& automation() noexcept { return automation_; }
  const automation::AutomationEngine& automation() const noexcept { return automation_; }

  // Mixing channel-strip insert stage. bind_mixing_strip binds a control-thread
  // ChannelStrip whose process_at runs per sub-block when mixing is enabled.
  // A successful bind re-prepares the strip, which allocates on the control
  // thread, so this is intentionally NOT noexcept (a throwing allocation
  // propagates rather than terminating the process).
#if defined(SONARE_WITH_MIXING)
  bool bind_mixing_strip(mixing::ChannelStrip* strip);
  void set_mixing_enabled(bool enabled) noexcept;
  bool mixing_enabled() const noexcept { return mixing_enabled_.load(std::memory_order_relaxed); }
  MixingRuntime& mixing() noexcept { return mixing_runtime_; }
  bool set_master_strip(const mixing::api::Strip& strip);
  bool set_track_lanes(std::vector<TrackLaneConfig> lanes);
  bool set_track_buses(std::vector<TrackBusConfig> buses);
  /// Keys one insert of a lane strip from another lane's post-strip audio
  /// (see TrackMixerRuntime::set_lane_sidechain). source_track_id 0 clears.
  bool set_lane_sidechain(uint32_t track_id, unsigned int insert_index,
                          uint32_t source_track_id) noexcept {
    return track_mixer_runtime_.set_lane_sidechain(track_id, insert_index, source_track_id);
  }
  bool bind_track_strip(uint32_t track_id, mixing::ChannelStrip* strip);
  bool set_track_strip(uint32_t track_id, const mixing::api::Strip& strip);
  bool set_bus_strip(uint32_t bus_id, const mixing::api::Bus& bus);
  bool set_track_insert_bypassed(uint32_t track_id, unsigned int insert_index, bool bypassed,
                                 bool reset_on_bypass = false) noexcept;
  bool set_master_insert_bypassed(unsigned int insert_index, bool bypassed,
                                  bool reset_on_bypass = false) noexcept;
  bool set_bus_insert_bypassed(uint32_t bus_id, unsigned int insert_index, bool bypassed,
                               bool reset_on_bypass = false) noexcept;
  // Realtime change of one channel-strip insert parameter, addressed by the
  // processor's JSON-key parameter name (the same key used in scene JSON). The
  // name is resolved to the integer param_id on the control thread, then applied
  // at the next block head via the command queue. Returns false if the track,
  // insert, or key is unknown, the param is not realtime-safe, or the queue is
  // full. lane/insert/param indices must each fit in 8 bits.
  bool set_track_insert_param(uint32_t track_id, unsigned int insert_index, const std::string& key,
                              float value) noexcept;
  bool set_master_insert_param(unsigned int insert_index, const std::string& key,
                               float value) noexcept;
  // Realtime change of one BUS insert parameter, addressed by JSON-key name. The
  // name is resolved to the integer param_id on the control thread, then applied
  // at the next block head. Returns false if the bus, insert, or key is unknown,
  // the param is not realtime-safe, or the queue is full. insert/param must fit
  // in 8 bits.
  bool set_bus_insert_param(uint32_t bus_id, unsigned int insert_index, const std::string& key,
                            float value) noexcept;
  // Resolves a track-lane / master / bus insert parameter (JSON-key name) to the
  // reserved insert-automation id used by setAutomationLane / setParameter. The
  // returned id encodes (strip selector, insert index, processor param id) in the
  // reserved insert namespace (see insert_automation_id.h). Returns -1 when the
  // strip, insert, or key is unknown. Control-thread; touches no audio state.
  int64_t resolve_track_insert_automation_id(uint32_t track_id, unsigned int insert_index,
                                             const std::string& key) noexcept;
  int64_t resolve_master_insert_automation_id(unsigned int insert_index,
                                              const std::string& key) noexcept;
  int64_t resolve_bus_insert_automation_id(uint32_t bus_id, unsigned int insert_index,
                                           const std::string& key) noexcept;
  bool set_track_eq_band(uint32_t track_id, size_t band_index,
                         const mastering::eq::EqBand& band) noexcept;
  bool set_master_eq_band(size_t band_index, const mastering::eq::EqBand& band) noexcept;
  // Granular realtime panner/channel-delay updates for a track lane strip.
  // Control-thread only. pan/pan-law/pan-mode/dual-pan are glitch-free atomic
  // writes resolved read-only on the control thread, so they are safe during
  // playback; channel delay adjusts strip latency and refreshes PDC + reported
  // graph latency (structural -- not concurrent with process()). Each returns
  // false if the track has no bound lane strip.
  bool set_track_pan(uint32_t track_id, float pan) noexcept;
  bool set_track_pan_law(uint32_t track_id, mixing::PanLaw law) noexcept;
  bool set_track_pan_mode(uint32_t track_id, mixing::PanMode mode) noexcept;
  bool set_track_dual_pan(uint32_t track_id, float left_pan, float right_pan) noexcept;
  bool set_track_channel_delay_samples(uint32_t track_id, int delay_samples) noexcept;
  TrackMixerRuntime& track_mixer() noexcept { return track_mixer_runtime_; }

  // Solo/mute + PFL/AFL monitoring stage applied to a registered set of strips.
  bool add_monitor_strip(mixing::ChannelStrip* strip) noexcept;
  bool remove_monitor_strip(mixing::ChannelStrip* strip) noexcept {
    return monitor_runtime_.remove_strip(strip);
  }
  void set_monitoring_enabled(bool enabled) noexcept {
    monitoring_enabled_.store(enabled, std::memory_order_relaxed);
  }
  bool monitoring_enabled() const noexcept {
    return monitoring_enabled_.load(std::memory_order_relaxed);
  }
  MonitorRuntime& monitor() noexcept { return monitor_runtime_; }
#endif

  // Default ramp time for engine-level kSetParamSmoothed commands, in ms.
  void set_param_smoothing_ms(float smoothing_ms) noexcept;
  /// Snaps every in-flight parameter ramp to its target: engine-level smoothed
  /// parameters are pushed at their final value and retired, and the track
  /// mixer's lane fader/pan/gate and bus gain smoothers jump to their targets.
  /// Offline renders call this after a priming process() block (which drains
  /// queued commands and applies automation at the seek position) so the first
  /// audible block renders at the settled values instead of ramping in from
  /// defaults. Not safe concurrently with a running audio thread.
  void settle_parameters() noexcept;
  float param_smoothing_ms() const noexcept {
    return param_smoothing_ms_.load(std::memory_order_relaxed);
  }
  void set_graph_latency_samples_q8(int latency_q8) noexcept;
  int graph_latency_samples_q8() const noexcept { return graph_latency_samples_q8_; }
  int64_t audible_timeline_sample(int64_t timeline_sample) const noexcept;
#if defined(SONARE_WITH_GRAPH)
  // Control-thread graph hot-swap. Allocates a new binding internally, so this
  // is intentionally NOT noexcept (a throwing allocation propagates).
  bool swap_graph(std::unique_ptr<graph::Graph> graph, const char* input_node_id,
                  const char* output_node_id, int num_channels,
                  std::vector<GraphRuntime::ParameterBinding> parameter_bindings = {});
  bool has_graph() const noexcept { return graph_runtime_.active_graph() != nullptr; }
  size_t graph_node_count() const noexcept;
  size_t graph_connection_count() const noexcept;
  bool bind_graph_parameter(uint32_t param_id, const char* node_id) noexcept;
#endif

  const transport::Transport& transport() const noexcept { return transport_; }
  int max_block_size() const noexcept { return max_block_size_; }

 private:
  static rt::ProcessorBase* resolve_graph_parameter_thunk(void* context,
                                                          uint32_t param_id) noexcept;
  void drain_commands(int64_t block_render_frame, int num_frames) noexcept;
  // Store a command in the pending bank. When @p prefer_current is true (the
  // command is due in the current block) and the bank is full, evict the
  // furthest-future pending command to make room, so current-block commands are
  // never starved by a backlog of far-future ones.
  void store_pending(const rt::Command& command, bool prefer_current) noexcept;
  void apply_due_commands(int64_t boundary_render_frame) noexcept;
  void apply_command(const rt::Command& command) noexcept;
  void process_impl(float* const* io, float* const* monitor_out, int num_channels, int num_frames,
                    bool fold_monitor_to_main) noexcept;
  void process_subblock(float* const* io, float* const* monitor_out, int num_channels, int offset,
                        int num_frames, bool fold_monitor_to_main) noexcept;
  void silence(float* const* io, int num_channels, int num_frames) noexcept;
  void start_smoothed_param(uint32_t target_id, float value) noexcept;
  void tick_smoothed_params(int num_steps) noexcept;
  bool any_smoothed_param_active() const noexcept;
#if defined(SONARE_WITH_MIXING)
  bool route_engine_parameter(uint32_t target_id, float value) noexcept;
  // AutomationEngine::EngineParamRouter trampoline: forwards reserved-namespace
  // automation lane values to route_engine_parameter on @p context.
  static bool route_engine_parameter_thunk(void* context, uint32_t param_id, float value) noexcept;
  // Sets the smoothed target of a master-strip insert parameter from a reserved
  // automation lane. The master insert chain lives outside TrackMixerRuntime, so
  // its automated params get a parallel slot table here, advanced once per
  // sub-block by tick_smoothed_params (same cadence as the lane/bus slots).
  bool route_master_insert_param_smoothed(unsigned int insert_index, unsigned int param_id,
                                          float value) noexcept;
  void advance_master_insert_automations(int num_steps) noexcept;
  void settle_master_insert_automations() noexcept;
  void clear_master_insert_automations() noexcept;
#endif
  void update_reported_graph_latency() noexcept;
  void enqueue_telemetry(Telemetry telemetry) noexcept;
  void enqueue_error(TelemetryErrorCode code, int64_t render_frame, int64_t timeline_sample,
                     uint32_t value) noexcept;
  void on_clip_page_miss(const ClipPageRequest& request) noexcept override;
  void compact_pending() noexcept;
#if defined(SONARE_WITH_ARRANGEMENT)
  // CONTROL thread: refresh the PDC delays from the current instrument rack and
  // report the resulting graph latency. Called from prepare() and whenever an
  // instrument binding changes.
  void recompute_pdc() noexcept;
  // AUDIO thread: flush the PDC delay lines on a transport discontinuity so no
  // stale clip/instrument audio rings out across a stop/seek/loop.
  void flush_pdc_delays() noexcept;
  void emit_midi_transport_command(uint8_t status, int64_t render_frame) noexcept;
  void emit_midi_clock_block(int64_t timeline_start_sample, int64_t render_start_frame,
                             int num_frames) noexcept;
  void dispatch_live_midi_input(int64_t render_start_frame, int num_frames) noexcept;
#endif
  void publish_tempo_map_snapshot();
  void adopt_tempo_map_snapshot() noexcept;

  struct InputMonitorState {
    bool enabled = true;
    float gain = 1.0f;
  };

  transport::TempoMap tempo_map_{};
  rt::RtPublisher<transport::TempoMap> tempo_map_snapshot_{};
  const transport::TempoMap* active_tempo_map_ = &tempo_map_;
  std::vector<transport::TempoSegment> control_tempo_segments_{};
  std::vector<transport::TimeSignatureSegment> control_time_signatures_{};
  transport::Transport transport_{};
  transport::MarkerMap markers_{};
  ClipPlayer clip_player_{};
#if defined(SONARE_WITH_ARRANGEMENT)
  // Capacity of the destination-tagged external MIDI output queue (events +
  // clock/transport bytes buffered between audio blocks until the host drains).
  static constexpr size_t kExternalMidiQueueCapacity = 1024;
  using ExternalMidiQueue = host::FixedExternalMidiOutputQueue<kExternalMidiQueueCapacity>;

  struct MidiDispatchSink final : midi::MidiEventSink {
    static constexpr size_t kMaxExternalDestinations = InstrumentRack::kMaxInstruments;
    InstrumentRack* rack = nullptr;
    std::atomic<host::MidiOutputSink*> output{nullptr};
    // Destination-tagged external output queue (set once in prepare(); nullptr
    // until then). Events whose destination is marked external are routed here
    // INSTEAD of the instrument rack, so the track drives an external device
    // rather than a built-in synth.
    ExternalMidiQueue* external = nullptr;
    // Published set of destinations routed externally. Each slot is 0 (empty) or
    // ((1<<32) | destination_id); the high marker bit keeps destination 0
    // representable. Linear-scanned on the audio thread (<= 16 slots); the
    // control thread publishes via set_external().
    std::array<std::atomic<uint64_t>, kMaxExternalDestinations> external_destinations{};
    // AUDIO thread only: added to a sequenced external event's render_frame to
    // convert it from the TIMELINE sample position (which wraps backward on a
    // loop / jumps on a seek) to the monotonic DEVICE render frame, so every
    // record in the external output queue shares the device coordinate and stays
    // in dispatch order across loop boundaries. process() sets this to
    // (render_frame - sample_position) around the sequencer's process_block and
    // restores 0 for the device-framed all-notes-off / command paths.
    int64_t timeline_to_device_offset = 0;

    static constexpr uint64_t encode(uint32_t destination_id) noexcept {
      return (uint64_t{1} << 32) | destination_id;
    }
    bool is_external(uint32_t destination_id) const noexcept {
      const uint64_t want = encode(destination_id);
      for (const auto& slot : external_destinations) {
        if (slot.load(std::memory_order_acquire) == want) return true;
      }
      return false;
    }
    // CONTROL thread: mark/unmark a destination as externally routed. Returns
    // false only when enabling a new destination and all kMaxExternalDestinations
    // slots are already taken (so the caller can surface the overflow instead of
    // silently routing the track to the internal rack); enabling an
    // already-external destination and any disable are idempotent and return true.
    bool set_external(uint32_t destination_id, bool on) noexcept {
      const uint64_t want = encode(destination_id);
      if (on) {
        for (auto& slot : external_destinations) {
          if (slot.load(std::memory_order_acquire) == want) return true;
        }
        for (auto& slot : external_destinations) {
          uint64_t empty = 0;
          if (slot.compare_exchange_strong(empty, want, std::memory_order_acq_rel)) return true;
        }
        return false;
      }
      for (auto& slot : external_destinations) {
        if (slot.load(std::memory_order_acquire) == want) {
          slot.store(0, std::memory_order_release);
          return true;
        }
      }
      return true;
    }

    void on_event(uint32_t destination_id, const midi::MidiEvent& event) noexcept override {
      if (is_external(destination_id)) {
        // An external destination drives its own device queue only -- it is
        // routed there INSTEAD of the rack and is not also mirrored to the
        // merged output sink, otherwise a host using both would emit the event
        // twice to the device path. Stamp the queued record with the DEVICE
        // render frame (timeline render frame + the per-sub-block offset) so the
        // queue stays monotonic across loop wraps; the rack copy below is left in
        // its native timeline frame.
        if (external != nullptr) {
          midi::MidiEvent device_event = event;
          device_event.render_frame += timeline_to_device_offset;
          external->send(destination_id, device_event);
        }
        return;
      }
      if (rack != nullptr) rack->on_event(destination_id, event);
      host::MidiOutputSink* sink = output.load(std::memory_order_acquire);
      if (sink != nullptr) sink->send(event);
    }
  };

  // Engine-internal MIDI sync sink: encodes each clock/transport byte as a
  // single-word UMP System message (MT 0x1) and enqueues it into the external
  // output queue tagged host::kTransportDestination. Registered only while
  // external clock is enabled.
  struct ExternalClockSyncSink final : MidiSyncSink {
    ExternalMidiQueue* queue = nullptr;
    void on_midi_sync_byte(int64_t render_frame, uint8_t byte) noexcept override {
      if (queue == nullptr) return;
      midi::Ump ump{};
      ump.words[0] = (uint32_t{0x1u} << 28) | (static_cast<uint32_t>(byte) << 16);
      ump.word_count = 1;
      midi::MidiEvent event{};
      event.render_frame = render_frame;
      event.ump = ump;
      queue->send(host::kTransportDestination, event);
    }
  };

  midi::MidiSequencer midi_sequencer_{};
  midi::ClockGenerator midi_clock_{};
  std::atomic<MidiSyncSink*> midi_sync_sink_{nullptr};
  std::atomic<host::MidiInputSource*> midi_input_source_{nullptr};
  std::atomic<uint32_t> midi_input_destination_id_{0};
  uint32_t live_midi_input_destination_id_ = 0;
  mutable rt::RtPublisher<midi::CcMap> midi_cc_maps_{};
  static constexpr size_t kMaxLiveMidiInputEvents = 256;
  std::array<midi::MidiEvent, kMaxLiveMidiInputEvents> live_midi_input_events_{};
  size_t live_midi_input_count_ = 0;
  // Bounded SysEx payload store for live (queued) SysEx commands. The control
  // thread (push_midi_sysex) copies bytes into a round-robin slot and enqueues a
  // kMidiSysExImmediate command carrying the slot index + generation; the audio
  // thread reads the slot, dispatches a MidiEvent from a torn-free local copy of
  // the slot bytes, and never allocates. Each slot is published as a seqlock: the
  // per-slot atomic generation is an even/odd sequence where an ODD value marks a
  // write in progress and an EVEN value a completed payload. The writer marks the
  // slot odd, writes the bytes, then release-stores the even (done) generation;
  // the audio thread brackets its payload copy with two acquire loads of the
  // generation and accepts only a stable even value matching the command's
  // generation. This drops a slot the control thread recycled or is actively
  // rewriting mid-read (a burst deeper than kSysExPayloadSlots before the audio
  // thread drains it), so a torn payload is never fed to an instrument. SysEx is
  // a sparse control-rate message, so the ring depth is ample in practice.
  static constexpr size_t kMaxSysExPayloadBytes = 512;
  static constexpr size_t kSysExPayloadSlots = 64;
  struct SysExPayloadSlot {
    // The generation seqlock (release/acquire) supplies all cross-thread
    // ordering, but a guard does not make a concurrent plain-POD access race-free
    // in the C++ memory model. So the payload itself lives in relaxed atomic
    // words, mirroring rt::SeqlockCell's packing; the generation still gates
    // torn/recycled slots. size and bytes are packed together: word 0 is the byte
    // count, the rest are the payload bytes.
    static constexpr size_t kPayloadWords =
        (kMaxSysExPayloadBytes + sizeof(uint32_t) - 1) / sizeof(uint32_t);
    std::atomic<uint32_t> size{0};
    std::array<std::atomic<uint32_t>, kPayloadWords> byte_words{};
    std::atomic<uint32_t> generation{0};

    /// Control-thread writer: pack @p n bytes into the relaxed word store.
    /// Callers guarantee n <= kMaxSysExPayloadBytes.
    void store_payload(const uint8_t* data, uint32_t n) noexcept {
      std::array<uint32_t, kPayloadWords> packed{};
      std::memcpy(packed.data(), data, n);
      const size_t words = (n + sizeof(uint32_t) - 1) / sizeof(uint32_t);
      for (size_t i = 0; i < words; ++i) {
        byte_words[i].store(packed[i], std::memory_order_relaxed);
      }
      size.store(n, std::memory_order_relaxed);
    }

    /// Audio-thread reader: copy the payload into @p out and return its clamped
    /// size. Called inside the generation bracket; a torn size is clamped so the
    /// word loop never overruns before the generation mismatch drops the slot.
    uint32_t load_payload(std::array<uint8_t, kMaxSysExPayloadBytes>& out) const noexcept {
      uint32_t n = size.load(std::memory_order_relaxed);
      if (n > kMaxSysExPayloadBytes) n = kMaxSysExPayloadBytes;
      std::array<uint32_t, kPayloadWords> packed{};
      const size_t words = (n + sizeof(uint32_t) - 1) / sizeof(uint32_t);
      for (size_t i = 0; i < words; ++i) {
        packed[i] = byte_words[i].load(std::memory_order_relaxed);
      }
      std::memcpy(out.data(), packed.data(), n);
      return n;
    }
  };
  std::array<SysExPayloadSlot, kSysExPayloadSlots> sysex_payload_slots_{};
  uint32_t sysex_payload_cursor_ = 0;  // control-thread only
  // Per-destination host-instrument rack (default empty / opt-in). It is the
  // sequencer's dispatch sink (so routed MIDI reaches the instrument bound to
  // each clip's destination) and the engine sums every bound instrument's audio
  // at the clip/source stage. Owned by the engine; each slot borrows a
  // caller-owned instrument pointer.
  InstrumentRack instrument_rack_{};
  MidiDispatchSink midi_dispatch_sink_{};
  // Destination-tagged external MIDI output queue (audio thread produces via the
  // dispatch sink / clock sink; the host drains with drain_external_midi()).
  ExternalMidiQueue external_midi_queue_{};
  ExternalClockSyncSink external_clock_sync_sink_{};
  // Per-block instrument render scratch, allocated in prepare() (channel-planar:
  // kMaxAudioChannels rows of max_block_size_). The audio thread only points
  // into it, never allocates.
  std::vector<float> midi_instrument_storage_{};
  std::array<float*, 64> midi_instrument_channels_{};
#endif
  CaptureSink capture_sink_{};
  std::atomic<CaptureSource> capture_source_{CaptureSource::kOutput};
  std::atomic<int64_t> record_offset_samples_{0};
  rt::SeqlockCell<InputMonitorState> input_monitor_{InputMonitorState{}};
  Metronome metronome_{};
#if defined(SONARE_WITH_MIXING)
  MeterTelemetryTap meter_tap_{};
  ScopeTelemetryTap scope_tap_{};
  int scope_interval_frames_ = 0;
  uint32_t scope_band_count_ = 48;
#endif
  automation::AutomationEngine automation_{};
#if defined(SONARE_WITH_MIXING)
  MixingRuntime mixing_runtime_{};
  std::unique_ptr<mixing::ChannelStrip> owned_master_strip_{};
  MonitorRuntime monitor_runtime_{};
  TrackMixerRuntime track_mixer_runtime_{};
  // Automated master-strip insert parameters. The master insert chain is not part
  // of TrackMixerRuntime, so its smoothers live here and are advanced once per
  // sub-block by tick_smoothed_params, mirroring the lane/bus slot table.
  static constexpr size_t kMaxMasterInsertAutomations = 16;
  struct MasterInsertAutoSlot {
    bool active = false;
    unsigned int insert_index = 0;
    unsigned int param_id = 0;
    rt::ParamSmoother smoother{};
  };
  std::array<MasterInsertAutoSlot, kMaxMasterInsertAutomations> master_insert_auto_slots_{};
  uint32_t master_insert_automation_overflow_count_ = 0;
#endif
  rt::SpscQueue<rt::Command> commands_{};
  rt::SpscQueue<Telemetry> telemetry_{};
  rt::SpscQueue<ClipPageRequest> clip_page_requests_{};
  BoundarySplitter boundary_splitter_{};
  std::array<rt::Command, kMaxPendingCommands> pending_{};
  std::array<bool, kMaxPendingCommands> pending_active_{};
#if defined(SONARE_WITH_GRAPH)
  GraphRuntime graph_runtime_{};
#endif

  // Engine-level parameter smoothing for kSetParamSmoothed. Each active slot
  // ramps a bound parameter toward its target over param_smoothing_ms_ and is
  // ticked once per control period. Fixed-size: no audio-thread allocation.
  static constexpr size_t kMaxSmoothedParams = 64;
  static constexpr int kControlPeriod = 64;
  struct SmoothedParam {
    uint32_t target_id = 0;
    bool active = false;
    rt::ParamSmoother smoother{};
  };
  std::array<SmoothedParam, kMaxSmoothedParams> smoothed_params_{};

  // Pre-allocated channel pointer scratch reused by render_offline so the
  // per-block loop performs no heap allocation.
  std::vector<float*> render_block_channels_{};
  static constexpr size_t kMaxAudioChannels = 64;
  std::vector<float> input_capture_storage_{};
  std::array<float*, kMaxAudioChannels> input_capture_channels_{};
#if defined(SONARE_WITH_ARRANGEMENT)
  // Plugin-delay compensation (PDC). A hosted instrument reports an internal
  // latency: its audio for a note dispatched at frame F emerges latency_samples
  // later. To keep clip audio and every instrument mutually phase-aligned, the
  // engine delays the clip bus by pdc_total_samples_ (the maximum instrument
  // latency) and each instrument by (pdc_total_samples_ - its own latency), so
  // all sources reach the source-merge point coincident at +pdc_total_samples_.
  // Recomputed on the control thread whenever an instrument binding changes;
  // a value of 0 (no latency-bearing instrument) leaves the render path
  // bit-identical to the non-PDC path. The clip bus renders into clip_scratch_
  // first so it can be delayed before summing. Tracked in Q8.8 samples so
  // sub-sample instrument latency is compensated (fractional PDC).
  int pdc_total_q8_ = 0;
  ChannelDelay<kMaxAudioChannels> clip_pdc_delay_{};
  std::array<ChannelDelay<kMaxAudioChannels>, InstrumentRack::kMaxInstruments>
      instrument_pdc_delays_{};
  std::array<uint32_t, InstrumentRack::kMaxInstruments> instrument_pdc_dest_{};
  size_t pdc_instrument_count_ = 0;
  std::vector<float> clip_scratch_storage_{};
  std::array<float*, kMaxAudioChannels> clip_scratch_channels_{};
#endif
#if defined(SONARE_WITH_MIXING)
  std::vector<float> monitor_bus_storage_{};
  std::array<float*, kMaxAudioChannels> monitor_bus_channels_{};

  // Toggled from the control thread (@c set_mixing_enabled /
  // @c set_monitoring_enabled) and read on the audio thread in process(). Atomic
  // with relaxed ordering: only the flag itself crosses the boundary, so a
  // single aligned load/store per access is sufficient (no companion state to
  // publish).
  std::atomic<bool> mixing_enabled_{false};
  std::atomic<bool> monitoring_enabled_{false};
#endif
  std::atomic<float> param_smoothing_ms_{20.0f};
  float applied_param_smoothing_ms_ = 20.0f;  // audio thread only
  double sample_rate_ = 48000.0;
  uint32_t telemetry_overflow_count_ = 0;
  bool clip_page_underrun_reported_this_block_ = false;
  int graph_latency_samples_q8_ = 0;
  int max_block_size_ = 0;

  // Command-queue overflow accounting. push_command (control thread) is the
  // sole writer of command_overflow_count_; pop_telemetry (consumer thread)
  // tracks how many it has reported and synthesizes a kCommandQueueOverflow
  // record for any unreported delta. This keeps the control thread off the
  // audio-thread-owned telemetry_ SPSC queue (no producer-side race) while
  // still surfacing dropped commands without requiring a process() call.
  std::atomic<uint32_t> command_overflow_count_{0};
  uint32_t command_overflow_reported_ = 0;
  uint32_t automation_bind_overflow_reported_ = 0;
  uint32_t automation_stale_lane_reported_ = 0;
  uint32_t insert_automation_overflow_reported_ = 0;
};

}  // namespace sonare::engine
