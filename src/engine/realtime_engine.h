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
///   @c set_capture_*, @c reset_capture,
///   @c marker_by_index/id, @c seek_marker, @c set_loop_from_markers,
///   @c set_mixing_enabled, @c set_monitoring_enabled,
///   @c set_param_smoothing_ms, @c set_graph_latency_samples_q8,
///   @c transport, @c automation accessors, all @c *_count noexcept getters.
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
///   @c bind_mixing_strip, @c add_monitor_strip, @c remove_monitor_strip,
///   @c swap_graph, @c bind_graph_parameter. These must be called from the
///   thread that owns engine lifecycle; do not call from the audio callback.
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
#endif
  transport::TransportState transport_state_control() const noexcept;
  void set_tempo(double bpm);
  void set_tempo_segments(std::vector<transport::TempoSegment> segments);
  void set_time_signature(int numerator, int denominator);
  void set_time_signature_segments(std::vector<transport::TimeSignatureSegment> segments);
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
  bool bind_midi_cc(uint8_t controller, uint8_t channel, uint32_t param_id, float min_value,
                    float max_value) noexcept {
    midi::CcBinding binding{};
    binding.cc_number = controller;
    binding.channel = channel;
    binding.param_id = param_id;
    binding.min_value = min_value;
    binding.max_value = max_value;
    return midi_cc_map_.bind(binding);
  }
  void clear_midi_cc_bindings() noexcept { midi_cc_map_.clear(); }
  size_t midi_cc_binding_count() const noexcept { return midi_cc_map_.binding_count(); }
  void set_midi_output_sink(host::MidiOutputSink* sink) noexcept {
    midi_dispatch_sink_.output.store(sink, std::memory_order_release);
  }
  void set_midi_sync_sink(MidiSyncSink* sink) noexcept {
    midi_sync_sink_.store(sink, std::memory_order_release);
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
  automation::AutomationEngine& automation() noexcept { return automation_; }
  const automation::AutomationEngine& automation() const noexcept { return automation_; }

  // Mixing channel-strip insert stage. bind_mixing_strip binds a control-thread
  // ChannelStrip whose process_at runs per sub-block when mixing is enabled.
  // A successful bind re-prepares the strip, which allocates on the control
  // thread, so this is intentionally NOT noexcept (a throwing allocation
  // propagates rather than terminating the process).
#if defined(SONARE_WITH_MIXING)
  bool bind_mixing_strip(mixing::ChannelStrip* strip);
  void set_mixing_enabled(bool enabled) noexcept { mixing_enabled_ = enabled; }
  bool mixing_enabled() const noexcept { return mixing_enabled_; }
  MixingRuntime& mixing() noexcept { return mixing_runtime_; }

  // Solo/mute + PFL/AFL monitoring stage applied to a registered set of strips.
  bool add_monitor_strip(mixing::ChannelStrip* strip) noexcept;
  bool remove_monitor_strip(mixing::ChannelStrip* strip) noexcept {
    return monitor_runtime_.remove_strip(strip);
  }
  void set_monitoring_enabled(bool enabled) noexcept { monitoring_enabled_ = enabled; }
  bool monitoring_enabled() const noexcept { return monitoring_enabled_; }
  MonitorRuntime& monitor() noexcept { return monitor_runtime_; }
#endif

  // Default ramp time for engine-level kSetParamSmoothed commands, in ms.
  void set_param_smoothing_ms(float smoothing_ms) noexcept;
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
                  const char* output_node_id, int num_channels);
  bool has_graph() const noexcept { return graph_runtime_.active_graph() != nullptr; }
  size_t graph_node_count() const noexcept;
  size_t graph_connection_count() const noexcept;
  bool bind_graph_parameter(uint32_t param_id, const char* node_id) noexcept;
#endif

  const transport::Transport& transport() const noexcept { return transport_; }
  int max_block_size() const noexcept { return max_block_size_; }

 private:
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
  struct MidiDispatchSink final : midi::MidiEventSink {
    InstrumentRack* rack = nullptr;
    std::atomic<host::MidiOutputSink*> output{nullptr};
    void on_event(uint32_t destination_id, const midi::MidiEvent& event) noexcept override {
      if (rack != nullptr) rack->on_event(destination_id, event);
      host::MidiOutputSink* sink = output.load(std::memory_order_acquire);
      if (sink != nullptr) sink->send(event);
    }
  };

  midi::MidiSequencer midi_sequencer_{};
  midi::ClockGenerator midi_clock_{};
  std::atomic<MidiSyncSink*> midi_sync_sink_{nullptr};
  std::atomic<host::MidiInputSource*> midi_input_source_{nullptr};
  std::atomic<uint32_t> midi_input_destination_id_{0};
  uint32_t live_midi_input_destination_id_ = 0;
  midi::CcMap midi_cc_map_{};
  static constexpr size_t kMaxLiveMidiInputEvents = 256;
  std::array<midi::MidiEvent, kMaxLiveMidiInputEvents> live_midi_input_events_{};
  size_t live_midi_input_count_ = 0;
  // Per-destination host-instrument rack (default empty / opt-in). It is the
  // sequencer's dispatch sink (so routed MIDI reaches the instrument bound to
  // each clip's destination) and the engine sums every bound instrument's audio
  // at the clip/source stage. Owned by the engine; each slot borrows a
  // caller-owned instrument pointer.
  InstrumentRack instrument_rack_{};
  MidiDispatchSink midi_dispatch_sink_{};
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
#endif
  automation::AutomationEngine automation_{};
#if defined(SONARE_WITH_MIXING)
  MixingRuntime mixing_runtime_{};
  MonitorRuntime monitor_runtime_{};
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

  bool mixing_enabled_ = false;
  bool monitoring_enabled_ = false;
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
};

}  // namespace sonare::engine
