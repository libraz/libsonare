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
#endif

// Paged audio provider backing lazily-streamed clips; the full definition lives
// in realtime_engine_clips.cpp (only that TU constructs/dereferences it). The
// facade holds them via shared_ptr, so a forward declaration suffices here.
class WasmClipPageProvider;

// Canonical AutomationCurve ordinals (Linear=0, Exp=1, Hold=2, SCurve=3) are
// shared with the C ABI and other bindings; conversion is a direct cast.
sonare::automation::CurveType automationCurveFromInt(int curve);
int automationCurveToInt(sonare::automation::CurveType curve);

// Object-property readers used across several facade TUs. The C-ABI TU is not
// linked into WASM, so these mirror its numeric coercions inline.
inline uint32_t uintProperty(emscripten::val object, const char* key, uint32_t default_value) {
  emscripten::val value = objectProperty(object, key);
  return value.isUndefined() ? default_value : value.as<uint32_t>();
}

inline int64_t int64Property(emscripten::val object, const char* key, int64_t default_value) {
  emscripten::val value = objectProperty(object, key);
  return value.isUndefined() ? default_value : static_cast<int64_t>(value.as<double>());
}

inline double doubleProperty(emscripten::val object, const char* key, double default_value) {
  emscripten::val value = objectProperty(object, key);
  return value.isUndefined() ? default_value : value.as<double>();
}

class RealtimeEngineWasm {
 public:
  // Mirror the C-ABI guard (sonare_engine_prepare): reject a non-positive
  // sample_rate / max_block_size instead of silently falling back to a default,
  // which would leave the engine's sample-rate state internally inconsistent.
  static void validatePrepare(double sample_rate, int max_block_size);

  RealtimeEngineWasm(double sample_rate, int max_block_size, int command_capacity,
                     int telemetry_capacity);
  void prepare(double sample_rate, int max_block_size, int command_capacity,
               int telemetry_capacity);

  // ---- Transport & timing (realtime_engine_transport.cpp) --------------
  void play(int64_t render_frame);
  void stop(int64_t render_frame);
  void settleParameters();
  void seekSample(int64_t timeline_sample, int64_t render_frame);
  void seekPpq(double ppq, int64_t render_frame);
  void setTempo(double bpm);
  void setTempoSegments(emscripten::val segments);
  void setTimeSignature(int numerator, int denominator);
  void setTimeSignatureSegments(emscripten::val segments);
  int64_t sampleAtPpq(double ppq);
  void setLoop(double start_ppq, double end_ppq, bool enabled);
  void setMarkers(emscripten::val markers);
  int markerCount() const;
  emscripten::val markerByIndex(int index) const;
  emscripten::val marker(int id) const;
  void seekMarker(int id, int64_t render_frame);
  void setLoopFromMarkers(int start_marker_id, int end_marker_id);
  void setMetronome(emscripten::val config);
  emscripten::val metronome() const;
  int64_t countInEndSample(int64_t start_sample, int bars) const;
  emscripten::val getTransportState() const;

  // ---- Parameters & automation (realtime_engine_params.cpp) ------------
  void addParameter(emscripten::val info);
  int parameterCount() const;
  emscripten::val parameterInfoByIndex(int index) const;
  emscripten::val parameterInfo(double id) const;
  void setAutomationLane(double param_id, emscripten::val points);
  int automationLaneCount() const;
  void setParameter(double param_id, float value, int64_t render_frame);
  void setParameterSmoothed(double param_id, float value, int64_t render_frame);
  void setParamSmoothingMs(float smoothing_ms);
  void setSoloMute(uint32_t lane_index, bool solo, bool mute, int64_t render_frame);
  void clearParameters();

  // ---- MIDI instruments, control & events (realtime_engine_midi.cpp) ---
  void setBuiltinInstrument(uint32_t destination_id, emscripten::val config);
  void setMidiClips(emscripten::val clips_val);
  void setSynthInstrument(uint32_t destination_id, emscripten::val patch);
  void loadSoundFont(emscripten::val data);
  void setSf2Instrument(uint32_t destination_id, emscripten::val config);
#if defined(SONARE_WITH_ARRANGEMENT)
  void bindInstrument(uint32_t destination_id,
                      std::unique_ptr<sonare::midi::MidiInstrument> instrument);
#endif
  void clearMidiInstrument(uint32_t destination_id);
  size_t midiInstrumentCount() const;
  void bindMidiCc(int channel, int controller, uint32_t param_id, float min_value, float max_value);
  void clearMidiCcBindings();
  size_t midiCcBindingCount() const;
  void setMidiFx(uint32_t destination_id, const std::string& config_json);
  void clearMidiFx(uint32_t destination_id);
  void setMidiInputSource(uint32_t destination_id);
  void clearMidiInputSource();
  size_t midiInputPendingCount() const;
  void setMidiDestinationExternal(uint32_t destination_id, bool external);
  void setExternalMidiClockEnabled(bool enabled);
  uint32_t externalMidiDroppedCount() const;
  emscripten::val drainExternalMidi(int max_records);
  void pushMidiInputNoteOn(int group, int channel, int note, int velocity,
                           int64_t port_time_samples);
  void pushMidiInputNoteOff(int group, int channel, int note, int velocity,
                            int64_t port_time_samples);
  void pushMidiInputCc(int group, int channel, int controller, int value,
                       int64_t port_time_samples);
  void pushMidiNoteOn(uint32_t destination_id, int group, int channel, int note, int velocity,
                      int64_t render_frame);
  void pushMidiNoteOff(uint32_t destination_id, int group, int channel, int note, int velocity,
                       int64_t render_frame);
  void pushMidiCc(uint32_t destination_id, int group, int channel, int controller, int value,
                  int64_t render_frame);
  void pushMidiSysex(uint32_t destination_id, emscripten::val data, int64_t render_frame);
  void pushMidiPanic(int64_t render_frame);

  // ---- Mixer: tracks, buses, strips (realtime_engine_mixer.cpp) --------
  void setTrackLanes(emscripten::val lanes);
  void setLaneSidechain(uint32_t track_id, unsigned int insert_index, uint32_t source_track_id);
  void setTrackBuses(emscripten::val buses);
  void setBusStripJson(uint32_t bus_id, const std::string& scene_json);
  void setTrackStripJson(uint32_t track_id, const std::string& scene_json);
  void setTrackStripEqBandJson(uint32_t track_id, int band_index, const std::string& band_json);
  void setTrackStripInsertBypassed(uint32_t track_id, unsigned int insert_index, bool bypassed,
                                   bool reset_on_bypass);
  void setMasterStripJson(const std::string& scene_json);
  void setMasterStripEqBandJson(int band_index, const std::string& band_json);
  void setMasterStripInsertBypassed(unsigned int insert_index, bool bypassed, bool reset_on_bypass);
  void setTrackStripInsertParamByName(uint32_t track_id, unsigned int insert_index,
                                      const std::string& param_name, float value);
  void setMasterStripInsertParamByName(unsigned int insert_index, const std::string& param_name,
                                       float value);
  void setBusStripInsertParamByName(uint32_t bus_id, unsigned int insert_index,
                                    const std::string& param_name, float value);
  void setBusStripInsertBypassed(uint32_t bus_id, unsigned int insert_index, bool bypassed,
                                 bool reset_on_bypass);
  double resolveTrackInsertAutomationId(uint32_t track_id, unsigned int insert_index,
                                        const std::string& param_name);
  double resolveMasterInsertAutomationId(unsigned int insert_index, const std::string& param_name);
  double resolveBusInsertAutomationId(uint32_t bus_id, unsigned int insert_index,
                                      const std::string& param_name);
  void setTrackStripPan(uint32_t track_id, float pan);
  void setTrackStripPanLaw(uint32_t track_id, int pan_law);
  void setTrackStripPanMode(uint32_t track_id, int pan_mode);
  void setTrackStripDualPan(uint32_t track_id, float left_pan, float right_pan);
  void setTrackStripChannelDelaySamples(uint32_t track_id, int delay_samples);

  // ---- Clips & paged providers (realtime_engine_clips.cpp) -------------
  void setClips(emscripten::val clips);
  int clipCount() const;
  int createClipPageProvider(int num_channels, int64_t num_samples, int64_t page_frames);
  void supplyClipPage(int provider_id, int64_t page_index, emscripten::val channels);
  void clearClipPage(int provider_id, int64_t page_index);
  void destroyClipPageProvider(int provider_id);
  emscripten::val popClipPageRequest();

  // ---- Capture / recording (realtime_engine_capture.cpp) ---------------
  void setCaptureBuffer(int num_channels, int capacity_frames);
  void armCapture(bool armed);
  void setCapturePunch(int64_t start_sample, int64_t end_sample, bool enabled);
  void setCaptureSource(std::string source);
  void setRecordOffsetSamples(int64_t offset_samples);
  void setInputMonitor(bool enabled, float gain);
  void resetCapture();
  emscripten::val captureStatus() const;
  emscripten::val capturedAudio() const;

  // ---- Audio processing, graph & offline (realtime_engine_processing.cpp)
  void setGraph(emscripten::val spec);
  int graphNodeCount() const;
  int graphConnectionCount() const;
  emscripten::val process(emscripten::val channels_val);
  void prepareChannels(int num_channels, int max_frames);
  emscripten::val getChannelBuffer(int channel, int num_frames);
  void processPrepared(int num_frames);
  emscripten::val processWithMonitor(emscripten::val channels_val);
  emscripten::val renderOffline(emscripten::val channels_val, int block_size);
  emscripten::val bounceOffline(emscripten::val options_val);
  emscripten::val freezeOffline(emscripten::val options_val);

  // ---- Telemetry & metering (realtime_engine_telemetry.cpp) ------------
  emscripten::val drainTelemetry(int max_records);
  emscripten::val drainMeterTelemetry(int max_records);
  emscripten::val drainMeterTelemetryWide(int max_records);
  unsigned int configureScopeTelemetry(int interval_frames, unsigned int band_count);
  emscripten::val drainScopeTelemetry(int max_records);

 private:
  // Maps a JS-supplied queue depth to the engine's size_t capacity. A value <= 0
  // selects the engine default (1024), matching the Node/Python bindings.
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
  static emscripten::val markerToVal(const sonare::transport::Marker& marker);
  void publishParameterMetadata();
  bool registeredParameterRejectsRealtime(uint32_t param_id) const;
  void pushMidiNote(uint32_t destination_id, int group, int channel, int note, int velocity,
                    int64_t render_frame, sonare::rt::CommandType type);
  void pushMidiInputEvent(int group, int channel, int note, int velocity, int64_t port_time_samples,
                          bool note_on);

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
  std::vector<std::vector<std::vector<float>>> clip_storage_;
  std::vector<std::vector<const float*>> clip_ptrs_;
  std::vector<std::vector<float>> capture_storage_;
  std::vector<float*> capture_ptrs_;
  // Persistent per-channel scratch for the zero-copy prepared process() path.
  std::vector<std::vector<float>> prepared_storage_;
  std::vector<float*> prepared_ptrs_;
  int prepared_channels_ = 0;
  int prepared_capacity_ = 0;
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
