/// @file realtime_engine_wasm.h
/// @brief Shared declaration of the embind realtime-engine facade.
///
/// The facade is large enough that its method implementations are split across
/// several translation units (realtime_engine.cpp + realtime_engine_*.cpp), one
/// per domain. They all define members of the single RealtimeEngineWasm class
/// declared here, and each contributes its slice of the embind class_<> via a
/// registerRealtimeEngine*() helper that the core TU calls while building the
/// one class_ handle. Splitting the registration this way keeps every domain's
/// JS-facing surface unchanged (same class name, same method names).

#pragma once

#ifdef __EMSCRIPTEN__

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "wasm/bindings/common/common.h"
#if defined(SONARE_WITH_ARRANGEMENT)
#include "midi/synth/sf2_player.h"
#include "midi/ump.h"
#endif

// Paged audio provider backing lazily-streamed clips; the full definition lives
// in realtime_engine_clips.cpp (only that TU constructs/dereferences it). The
// facade holds them via shared_ptr, so a forward declaration suffices here.
class WasmClipPageProvider;

// Canonical AutomationCurve ordinals (Linear=0, Exp=1, Hold=2, SCurve=3) are
// shared with the C ABI and other bindings; conversion is a direct cast.
sonare::automation::CurveType automationCurveFromInt(int curve);
int automationCurveToInt(sonare::automation::CurveType curve);

class RealtimeEngineWasm {
 public:
  // Mirror the C-ABI guard (sonare_engine_prepare): reject a non-positive
  // sample_rate / max_block_size instead of silently falling back to a default,
  // which would leave the engine's sample-rate state internally inconsistent.
  static void validatePrepare(double sample_rate, int max_block_size);

  RealtimeEngineWasm(double sample_rate, const emscripten::val& max_block_size,
                     const emscripten::val& command_capacity,
                     const emscripten::val& telemetry_capacity);
  RealtimeEngineWasm(double sample_rate, const emscripten::val& max_block_size,
                     const emscripten::val& command_capacity,
                     const emscripten::val& telemetry_capacity,
                     const emscripten::val& max_channels);
  void prepare(double sample_rate, const emscripten::val& max_block_size,
               const emscripten::val& command_capacity, const emscripten::val& telemetry_capacity);
  void prepareWithChannels(double sample_rate, const emscripten::val& max_block_size,
                           const emscripten::val& command_capacity,
                           const emscripten::val& telemetry_capacity,
                           const emscripten::val& max_channels);

  // ---- Transport & timing (realtime_engine_transport.cpp) --------------
  void play(const emscripten::val& render_frame_val);
  void stop(const emscripten::val& render_frame_val);
  void settleParameters();
  void flushControlCommands();
  void seekSample(int64_t timeline_sample, const emscripten::val& render_frame_val);
  void seekPpq(double ppq, const emscripten::val& render_frame_val);
  void setTempo(double bpm);
  void setTempoSegments(emscripten::val segments);
  void setTimeSignature(const emscripten::val& numerator_val,
                        const emscripten::val& denominator_val);
  void setTimeSignatureSegments(emscripten::val segments);
  int64_t sampleAtPpq(double ppq);
  void setLoop(double start_ppq, double end_ppq, bool enabled);
  void setMarkers(emscripten::val markers);
  int markerCount() const;
  emscripten::val markerByIndex(const emscripten::val& index_val) const;
  emscripten::val marker(const emscripten::val& id_val) const;
  void seekMarker(const emscripten::val& id_val, const emscripten::val& render_frame_val);
  void setLoopFromMarkers(const emscripten::val& start_marker_id_val,
                          const emscripten::val& end_marker_id_val);
  void setMetronome(emscripten::val config);
  emscripten::val metronome() const;
  int64_t countInEndSample(int64_t start_sample, const emscripten::val& bars_val) const;
  emscripten::val getTransportState() const;

  // ---- Parameters & automation (realtime_engine_params.cpp) ------------
  void addParameter(emscripten::val info);
  int parameterCount() const;
  emscripten::val parameterInfoByIndex(const emscripten::val& index_val) const;
  emscripten::val parameterInfo(double id) const;
  void setAutomationLane(double param_id, emscripten::val points);
  int automationLaneCount() const;
  void setParameter(double param_id, const emscripten::val& value_val,
                    const emscripten::val& render_frame_val);
  void setParameterSmoothed(double param_id, const emscripten::val& value_val,
                            const emscripten::val& render_frame_val);
  void setParamSmoothingMs(const emscripten::val& smoothing_ms_val);
  void setSoloMute(const emscripten::val& lane_index_val, bool solo, bool mute,
                   const emscripten::val& render_frame_val);
  void setTrackMonitorMode(const emscripten::val& lane_index_val, const emscripten::val& mode_val,
                           const emscripten::val& render_frame_val);
  void clearParameters();

  // ---- MIDI instruments, control & events (realtime_engine_midi.cpp) ---
  void setBuiltinInstrument(const emscripten::val& destination_id_val, emscripten::val config);
  void setMidiClips(emscripten::val clips_val);
  void setSynthInstrument(const emscripten::val& destination_id_val, emscripten::val patch);
  double resolveInstrumentAutomationId(const emscripten::val& destination_id_val,
                                       const std::string& param_name);
  void loadSoundFont(emscripten::val data);
  void setSf2Instrument(const emscripten::val& destination_id_val, emscripten::val config);
#if defined(SONARE_WITH_ARRANGEMENT)
  void bindInstrument(uint32_t destination_id,
                      std::unique_ptr<sonare::midi::MidiInstrument> instrument);
#endif
  void clearMidiInstrument(const emscripten::val& destination_id_val);
  size_t midiInstrumentCount() const;
  void bindMidiCc(const emscripten::val& channel_val, const emscripten::val& controller_val,
                  const emscripten::val& param_id_val, const emscripten::val& min_value_val,
                  const emscripten::val& max_value_val);
  void bindMidiCcBinding(emscripten::val binding);
  void clearMidiCcBindings();
  size_t midiCcBindingCount() const;
  void setControllerProfile(const emscripten::val& destination_id_val,
                            const std::string& preset_name);
  void bindController(const emscripten::val& destination_id_val, emscripten::val binding);
  void clearControllerBindings(const emscripten::val& destination_id_val);
  size_t controllerBindingCount(const emscripten::val& destination_id_val) const;
  void setControllerVelocityMeaningful(const emscripten::val& destination_id_val, bool meaningful);
  bool controllerVelocityMeaningful(const emscripten::val& destination_id_val) const;
  void setControllerNoteTracking(const emscripten::val& destination_id_val,
                                 emscripten::val dimension, emscripten::val tracking);
  emscripten::val controllerNoteTracking(const emscripten::val& destination_id_val,
                                         emscripten::val dimension) const;
  void setArticulation(const emscripten::val& destination_id_val,
                       const emscripten::val& channel_val, emscripten::val articulation);
  emscripten::val articulation(const emscripten::val& destination_id_val,
                               const emscripten::val& channel_val) const;
  uint32_t legatoFallbackCount(const emscripten::val& destination_id_val) const;
  void setMidiFx(const emscripten::val& destination_id_val, const std::string& config_json);
  void clearMidiFx(const emscripten::val& destination_id_val);
  void setMidiInputSource(const emscripten::val& destination_id_val);
  void clearMidiInputSource();
  size_t midiInputPendingCount() const;
  void setMidiDestinationExternal(const emscripten::val& destination_id_val, bool external);
  void setExternalMidiClockEnabled(bool enabled);
  uint32_t externalMidiDroppedCount() const;
  size_t externalMidiPendingCount() const;
  emscripten::val drainExternalMidi(const emscripten::val& max_records_val);
  // Scalar scratch drain for the AudioWorklet external-MIDI SAB path. This
  // avoids embind materialising JS arrays/objects in the render callback.
  bool popExternalMidiToScratch();
  uint32_t externalMidiScratchDestinationId() const;
  int64_t externalMidiScratchRenderFrame() const;
  uint32_t externalMidiScratchByteWord() const;
  uint32_t externalMidiScratchByteCount() const;
  void consumeExternalMidiScratch();
  void pushMidiInputNoteOn(const emscripten::val& group_val, const emscripten::val& channel_val,
                           const emscripten::val& note_val, const emscripten::val& velocity_val,
                           int64_t port_time_samples);
  void pushMidiInputNoteOff(const emscripten::val& group_val, const emscripten::val& channel_val,
                            const emscripten::val& note_val, const emscripten::val& velocity_val,
                            int64_t port_time_samples);
  void pushMidiInputCc(const emscripten::val& group_val, const emscripten::val& channel_val,
                       const emscripten::val& controller_val, const emscripten::val& value_val,
                       int64_t port_time_samples);
  void pushMidiNoteOn(const emscripten::val& destination_id_val, const emscripten::val& group_val,
                      const emscripten::val& channel_val, const emscripten::val& note_val,
                      const emscripten::val& velocity_val, const emscripten::val& render_frame_val);
  void pushMidiNoteOff(const emscripten::val& destination_id_val, const emscripten::val& group_val,
                       const emscripten::val& channel_val, const emscripten::val& note_val,
                       const emscripten::val& velocity_val,
                       const emscripten::val& render_frame_val);
  void pushMidiInputPitchBend(const emscripten::val& group_val, const emscripten::val& channel_val,
                              const emscripten::val& bend_val, int64_t port_time_samples);
  void pushMidiInputChannelPressure(const emscripten::val& group_val,
                                    const emscripten::val& channel_val,
                                    const emscripten::val& pressure_val, int64_t port_time_samples);
  void pushMidiInputPolyPressure(const emscripten::val& group_val,
                                 const emscripten::val& channel_val,
                                 const emscripten::val& note_val,
                                 const emscripten::val& pressure_val, int64_t port_time_samples);
  void pushMidiCc(const emscripten::val& destination_id_val, const emscripten::val& group_val,
                  const emscripten::val& channel_val, const emscripten::val& controller_val,
                  const emscripten::val& value_val, const emscripten::val& render_frame_val);
  void pushMidiPitchBend(const emscripten::val& destination_id_val,
                         const emscripten::val& group_val, const emscripten::val& channel_val,
                         const emscripten::val& bend_val, const emscripten::val& render_frame_val);
  void pushMidiChannelPressure(const emscripten::val& destination_id_val,
                               const emscripten::val& group_val, const emscripten::val& channel_val,
                               const emscripten::val& pressure_val,
                               const emscripten::val& render_frame_val);
  void pushMidiPolyPressure(const emscripten::val& destination_id_val,
                            const emscripten::val& group_val, const emscripten::val& channel_val,
                            const emscripten::val& note_val, const emscripten::val& pressure_val,
                            const emscripten::val& render_frame_val);
  void pushMidiUmp(const emscripten::val& destination_id_val, const emscripten::val& word0_val,
                   const emscripten::val& render_frame_val);
  void pushMidiSysex(const emscripten::val& destination_id_val, emscripten::val data,
                     const emscripten::val& render_frame_val);
  void pushMidiPanic(const emscripten::val& render_frame_val);

  // ---- Mixer: tracks, buses, strips (realtime_engine_mixer.cpp) --------
  void setTrackLanes(emscripten::val lanes);
  void setLaneSidechain(const emscripten::val& track_id_val,
                        const emscripten::val& insert_index_val,
                        const emscripten::val& source_track_id_val);
  void setTrackBuses(emscripten::val buses);
  void setBusStripJson(const emscripten::val& bus_id_val, const std::string& scene_json);
  void setTrackStripJson(const emscripten::val& track_id_val, const std::string& scene_json);
  void setTrackStripEqBandJson(const emscripten::val& track_id_val,
                               const emscripten::val& band_index_val, const std::string& band_json);
  void setTrackStripInsertBypassed(const emscripten::val& track_id_val,
                                   const emscripten::val& insert_index_val, bool bypassed,
                                   bool reset_on_bypass);
  void setMasterStripJson(const std::string& scene_json);
  void setMasterStripEqBandJson(const emscripten::val& band_index_val,
                                const std::string& band_json);
  void setMasterStripInsertBypassed(const emscripten::val& insert_index_val, bool bypassed,
                                    bool reset_on_bypass);
  void setTrackStripInsertParamByName(const emscripten::val& track_id_val,
                                      const emscripten::val& insert_index_val,
                                      const std::string& param_name,
                                      const emscripten::val& value_val);
  void setMasterStripInsertParamByName(const emscripten::val& insert_index_val,
                                       const std::string& param_name,
                                       const emscripten::val& value_val);
  void setBusStripInsertParamByName(const emscripten::val& bus_id_val,
                                    const emscripten::val& insert_index_val,
                                    const std::string& param_name,
                                    const emscripten::val& value_val);
  void setBusStripInsertBypassed(const emscripten::val& bus_id_val,
                                 const emscripten::val& insert_index_val, bool bypassed,
                                 bool reset_on_bypass);
  double resolveTrackInsertAutomationId(const emscripten::val& track_id_val,
                                        const emscripten::val& insert_index_val,
                                        const std::string& param_name);
  double resolveMasterInsertAutomationId(const emscripten::val& insert_index_val,
                                         const std::string& param_name);
  double resolveBusInsertAutomationId(const emscripten::val& bus_id_val,
                                      const emscripten::val& insert_index_val,
                                      const std::string& param_name);
  void setTrackStripPan(const emscripten::val& track_id_val, const emscripten::val& pan_val);
  void setTrackStripPanLaw(const emscripten::val& track_id_val, const emscripten::val& pan_law_val);
  void setTrackStripPanMode(const emscripten::val& track_id_val,
                            const emscripten::val& pan_mode_val);
  void setTrackStripDualPan(const emscripten::val& track_id_val,
                            const emscripten::val& left_pan_val,
                            const emscripten::val& right_pan_val);
  void setTrackStripChannelDelaySamples(const emscripten::val& track_id_val,
                                        const emscripten::val& delay_samples_val);

  // ---- Clips & paged providers (realtime_engine_clips.cpp) -------------
  void setClips(emscripten::val clips);
  emscripten::val prebakedClipChannels(const emscripten::val& clip_id_val) const;
  int clipCount() const;
  int createClipPageProvider(const emscripten::val& num_channels_val, int64_t num_samples,
                             int64_t page_frames);
  void supplyClipPage(const emscripten::val& provider_id_val, int64_t page_index,
                      emscripten::val channels);
  void clearClipPage(const emscripten::val& provider_id_val, int64_t page_index);
  void destroyClipPageProvider(const emscripten::val& provider_id_val);
  emscripten::val popClipPageRequest();
  // Allocation-free scalar variant for the AudioWorklet SAB request ring.
  // popClipPageRequest() remains for public/control-plane callers that need an
  // embind object; process() must use this scratch-backed API instead.
  bool popClipPageRequestToScratch();
  uint32_t clipPageRequestScratchClipId() const;
  double clipPageRequestScratchSample() const;
  uint32_t clipPageRequestOverflowCount() const;
  uint32_t warpStretchOverflowCount() const;
  void setWarpVoiceCapacity(const emscripten::val& voices_val);
  uint32_t warpVoiceCapacity() const;
  void setClipPagePrefetchFrames(double frames);
  double clipPagePrefetchFrames() const;

  // ---- Capture / recording (realtime_engine_capture.cpp) ---------------
  void setCaptureBuffer(const emscripten::val& num_channels_val,
                        const emscripten::val& capacity_frames_val);
  void armCapture(bool armed);
  void setCapturePunch(int64_t start_sample, int64_t end_sample, bool enabled);
  void setCaptureSource(emscripten::val source);
  void setRecordOffsetSamples(int64_t offset_samples);
  void setInputMonitor(bool enabled, const emscripten::val& gain_val);
  void resetCapture();
  emscripten::val captureStatus() const;
  emscripten::val capturedAudio() const;

  // ---- Audio processing, graph & offline (realtime_engine_processing.cpp)
  void setGraph(emscripten::val spec);
  int graphNodeCount() const;
  int graphConnectionCount() const;
  emscripten::val process(emscripten::val channels_val);
  void prepareChannels(const emscripten::val& num_channels_val,
                       const emscripten::val& max_frames_val);
  emscripten::val getChannelBuffer(const emscripten::val& channel_val,
                                   const emscripten::val& num_frames_val);
  void processPrepared(const emscripten::val& num_frames_val);
  void prepareMonitorChannels(const emscripten::val& num_channels_val,
                              const emscripten::val& max_frames_val);
  emscripten::val getMonitorChannelBuffer(const emscripten::val& channel_val,
                                          const emscripten::val& num_frames_val);
  void processPreparedWithMonitor(const emscripten::val& num_frames_val);
  emscripten::val processWithMonitor(emscripten::val channels_val);
  emscripten::val renderOffline(emscripten::val channels_val, const emscripten::val& block_size_val,
                                bool finalize);
  void finishOfflineRender();
  emscripten::val bounceOffline(emscripten::val options_val);
  emscripten::val freezeOffline(emscripten::val options_val);

  // ---- Telemetry & metering (realtime_engine_telemetry.cpp) ------------
  emscripten::val drainTelemetry(const emscripten::val& max_records_val);
  // Scalar scratch drain for the AudioWorklet SAB path. Unlike drainTelemetry,
  // this never materializes JS arrays or objects per render quantum.
  bool popTelemetryToScratch();
  uint32_t telemetryScratchType() const;
  uint32_t telemetryScratchError() const;
  int64_t telemetryScratchRenderFrame() const;
  int64_t telemetryScratchTimelineSample() const;
  int64_t telemetryScratchAudibleTimelineSample() const;
  int32_t telemetryScratchGraphLatencySamplesQ8() const;
  uint32_t telemetryScratchValue() const;
  emscripten::val drainMeterTelemetry(const emscripten::val& max_records_val);
  bool popMeterTelemetryToScratch();
  uint32_t meterScratchTargetId() const;
  int64_t meterScratchRenderFrame() const;
  float meterScratchValue(const emscripten::val& field_val) const;
  emscripten::val drainMeterTelemetryWide(const emscripten::val& max_records_val);
  unsigned int configureScopeTelemetry(const emscripten::val& interval_frames_val,
                                       const emscripten::val& band_count_val);
  emscripten::val drainScopeTelemetry(const emscripten::val& max_records_val);
  bool popScopeTelemetryToScratch();
  uint32_t scopeScratchTargetId() const;
  int64_t scopeScratchRenderFrame() const;
  uint32_t scopeScratchBandCount() const;
  float scopeScratchBand(const emscripten::val& index_val) const;
  uint32_t scopeScratchPointCount() const;
  float scopeScratchPointLeft(const emscripten::val& index_val) const;
  float scopeScratchPointRight(const emscripten::val& index_val) const;

 private:
  // Maps a JS-supplied queue depth to the engine's size_t capacity. 0 selects
  // the engine default (1024), matching the Node/Python bindings; a negative
  // depth is refused by the caller before reaching this.
  static size_t capacity(int requested);

  struct ChannelBlock {
    std::vector<std::vector<float>> storage;
    std::vector<float*> pointers;
    int frames = 0;
  };

  static ChannelBlock readChannels(emscripten::val channels_val);
  static emscripten::val channelsToJs(const ChannelBlock& block);
  static std::vector<float> interleave(const std::vector<std::vector<float>>& channels);
  static mastering::final::DitherType ditherTypeFromInt(int value);
  static emscripten::val parameterToVal(const sonare::automation::ParameterInfo& info);
  // A reserved-namespace description carries no id of its own (the id is the
  // query, not a stored field), so it is echoed back from @p id rather than
  // read off @p description -- same split as the C ABI's fill_c_parameter_description.
  static emscripten::val describedParameterToVal(
      uint32_t id, const sonare::automation::ParameterDescription& description);
  static emscripten::val markerToVal(const sonare::transport::Marker& marker);
  void publishParameterMetadata();
  bool registeredParameterRejectsRealtime(uint32_t param_id) const;
  void pushMidiNote(uint32_t destination_id, int group, int channel, int note, int velocity,
                    int64_t render_frame, sonare::rt::CommandType type);
  void pushMidiInputEvent(int group, int channel, int note, int velocity, int64_t port_time_samples,
                          bool note_on);
  /// Queues one single-word MIDI 1.0 UMP to a destination. Takes the raw word
  /// rather than a Ump so it stays compilable with the arrangement feature off,
  /// where the MIDI headers are not included.
  void queueMidiUmp(uint32_t destination_id, uint32_t word0, int64_t render_frame);
#if defined(SONARE_WITH_ARRANGEMENT)
  /// Enqueues one single-word MIDI 1.0 UMP on the live input source. @p what
  /// names the entry point in the refusal, which is the only thing the three
  /// per-note dimensions do not share.
  void pushMidiInputUmp(const sonare::midi::Ump& ump, int64_t port_time_samples, const char* what);
#endif

  sonare::engine::RealtimeEngine engine_{};
  /// Engine-owned instrument per destination (built-in synth or SF2 player).
  std::vector<std::pair<uint32_t, std::unique_ptr<sonare::midi::MidiInstrument>>>
      builtin_instruments_{};
#if defined(SONARE_WITH_ARRANGEMENT)
  /// Loaded SoundFont (loadSoundFont); shared read-only with the SF2 players
  /// bound through setSf2Instrument.
  std::shared_ptr<const sonare::midi::synth::Sf2File> soundfont_;
#endif
  sonare::host::FixedMidiInputSource<512> midi_input_source_{};
  bool midi_input_source_enabled_ = false;
  sonare::automation::ParameterRegistry parameters_{};
  std::vector<sonare::automation::AutomationLane> automation_lanes_;
  std::deque<std::string> parameter_strings_;
  std::deque<std::string> marker_strings_;
  std::vector<std::shared_ptr<WasmClipPageProvider>> clip_page_providers_;
  sonare::engine::ClipPageRequest clip_page_request_scratch_{};
  sonare::engine::Telemetry telemetry_scratch_{};
  sonare::engine::MeterTelemetryRecord meter_telemetry_scratch_{};
  sonare::engine::ScopeTelemetryRecord scope_telemetry_scratch_{};
  sonare::host::ExternalMidiRecord external_midi_record_scratch_{};
  sonare::host::ExternalMidi1Lowered external_midi_lowered_scratch_{};
  uint8_t external_midi_lowered_index_ = 0;
  std::vector<std::vector<std::vector<float>>> clip_storage_;
  std::vector<std::vector<const float*>> clip_ptrs_;
  std::vector<uint32_t> clip_ids_;
  std::vector<uint8_t> clip_tempo_baked_;
  std::vector<std::vector<float>> capture_storage_;
  std::vector<float*> capture_ptrs_;
  // Persistent per-channel scratch for the zero-copy prepared process() path.
  std::vector<std::vector<float>> prepared_storage_;
  std::vector<float*> prepared_ptrs_;
  int prepared_channels_ = 0;
  int prepared_capacity_ = 0;
  // Second plane for the cue (PFL/AFL) bus on the prepared path. Kept separate
  // from prepared_storage_ so a host that never monitors pays nothing.
  std::vector<std::vector<float>> monitor_storage_;
  std::vector<float*> monitor_ptrs_;
  int monitor_channels_ = 0;
  int monitor_capacity_ = 0;
};

// Each domain TU registers its slice of the single RealtimeEngine class_ handle.
// The core TU (realtime_engine.cpp) creates the handle, registers lifecycle, and
// calls these in turn from registerRealtimeEngineBindings().
void registerRealtimeEngineTransport(emscripten::class_<RealtimeEngineWasm>& cls);
void registerRealtimeEngineParams(emscripten::class_<RealtimeEngineWasm>& cls);
void registerRealtimeEngineMidi(emscripten::class_<RealtimeEngineWasm>& cls);
void registerRealtimeEngineMixer(emscripten::class_<RealtimeEngineWasm>& cls);
void registerRealtimeEngineClips(emscripten::class_<RealtimeEngineWasm>& cls);
void registerRealtimeEngineCapture(emscripten::class_<RealtimeEngineWasm>& cls);
void registerRealtimeEngineProcessing(emscripten::class_<RealtimeEngineWasm>& cls);
void registerRealtimeEngineTelemetry(emscripten::class_<RealtimeEngineWasm>& cls);

#endif  // __EMSCRIPTEN__
