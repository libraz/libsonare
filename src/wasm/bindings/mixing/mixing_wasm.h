/// @file mixing_wasm.h
/// @brief Shared declaration of the embind scene-based mixer facade.
///
/// The facade is large enough that its method implementations are split across
/// several translation units (mixing.cpp + mixing_*.cpp), one per domain. They
/// all define members of the single MixerWasm class declared here, and each
/// contributes its slice of the embind class_<> via a registerMixer*() helper
/// that the core TU calls while building the one class_ handle. Splitting the
/// registration this way keeps every domain's JS-facing surface unchanged (same
/// class name, same method names).

#pragma once

#ifdef __EMSCRIPTEN__

#include <optional>

#include "mixing/meter.h"
#include "wasm/bindings/common/common.h"

// The offline mixing assistant is registered from its own TU. It needs neither
// the routing graph nor the mixer facade, so it sits outside the
// SONARE_WITH_MIXING / SONARE_WITH_GRAPH guard below; when the assistant itself
// is compiled out its entry points report NotSupported rather than disappearing.
void registerMixingAssistantBindings();

#if defined(SONARE_WITH_MIXING) && defined(SONARE_WITH_GRAPH)

// ---------------------------------------------------------------------------
// MixerWasm: persistent scene-based mixer wrapper around the C mixer API
// (sonare_mixer_*). Owns a SonareMixer* built from a scene JSON string, routes
// strips through the compiled routing graph, and sums to a stereo master.
//
// processStereo takes planar inputs: leftChannels[i] / rightChannels[i] are the
// L/R Float32Array for strip i (matching mixStereo's input layout). It returns
// { left, right, sampleRate } with the mixed stereo master. Call delete() (or
// use try/finally) to release the underlying WASM object.
// ---------------------------------------------------------------------------
class MixerWasm {
 public:
  MixerWasm(SonareMixer* mixer, int sample_rate, int block_size);

  ~MixerWasm();

  MixerWasm(const MixerWasm&) = delete;
  MixerWasm& operator=(const MixerWasm&) = delete;

  static MixerWasm* fromSceneJson(std::string json, int sample_rate, int block_size);

  // Non-fatal warnings captured when this mixer was built from scene JSON, one
  // entry per insert handed param keys it does not read; empty when all consumed.
  val sceneWarnings() const;

  static std::string presetJson(std::string name);

  void compile();

  size_t stripCount() const;

  // Schedules sample-accurate insert-parameter automation on the strip at
  // strip_index. insert_index addresses the strip's combined insert sequence
  // [pre-inserts... post-inserts...]. param_id is processor-specific. sample_pos
  // is in absolute samples from the start of processing. curve: 0 = Linear,
  // 1 = Exponential.
  void scheduleInsertAutomation(unsigned int strip_index, unsigned int insert_index,
                                unsigned int param_id, double sample_pos, float value, int curve);

  // Borrowed strip handle by index in [0, stripCount()). Throws if out of range.
  // The handle is owned by the mixer; do not free it.
  SonareStrip* stripAt(unsigned int strip_index);

  // Sets the strip's input trim in dB.
  void setInputTrimDb(unsigned int strip_index, float db);

  // Sets the strip's fader level in dB.
  void setFaderDb(unsigned int strip_index, float db);

  // Sets the strip's pan position. pan_mode is the SONARE_PAN_MODE_* ordinal;
  // pass SONARE_PAN_MODE_KEEP (-1) to keep the strip's current pan mode (e.g. a
  // scene-defined mode) on a plain pan nudge.
  void setPan(unsigned int strip_index, float pan, int pan_mode);

  // Sets the strip's stereo width.
  void setWidth(unsigned int strip_index, float width);

  // Sets the strip's mute state.
  void setMuted(unsigned int strip_index, bool muted);

  // Sets the strip's solo state. Takes effect on the next process without a
  // graph recompile.
  void setSoloed(unsigned int strip_index, bool soloed);

  // Marks a strip as solo-safe so it is never implied-muted by another strip's
  // solo. Takes effect on the next process without a graph recompile.
  void setSoloSafe(unsigned int strip_index, bool solo_safe);

  // Inverts the polarity of the left and/or right channel.
  void setPolarityInvert(unsigned int strip_index, bool invert_left, bool invert_right);

  // Sets the strip's pan law. pan_law: 0 = -3 dB, 1 = -4.5 dB, 2 = -6 dB,
  // 3 = linear (0 dB).
  void setPanLaw(unsigned int strip_index, int pan_law);

  // Sets a per-strip channel delay in samples. This changes the strip's reported
  // latency; recompile to re-run latency compensation.
  void setChannelDelaySamples(unsigned int strip_index, int delay_samples);

  // Sets the strip's live VCA gain offset in dB (not persisted to the scene).
  void setVcaOffsetDb(unsigned int strip_index, float offset_db);

  // Sets independent left/right pan positions (dual-pan mode).
  void setDualPan(unsigned int strip_index, float left_pan, float right_pan);

  // Sets the strip's surround pan from a JS object {azimuth, elevation,
  // divergence, lfe, distance}; absent/non-numeric fields fall back to the
  // centered point-source default.
  void setSurroundPan(unsigned int strip_index, val pan);

  // Adds a post-construction send to the strip. timing mirrors SonareSendTiming:
  // 0 = post-fader, 1 = pre-fader. Returns the new send's index.
  size_t addSend(unsigned int strip_index, std::string id, std::string destination_bus_id,
                 float send_db, int timing);

  // Sets the send level (in dB) for an existing send by index.
  void setSendDb(unsigned int strip_index, size_t send_index, float send_db);

  // Removes the send at send_index (in add order) from the strip. Higher send
  // indices shift down by one after removal; recompile before processing.
  void removeSend(unsigned int strip_index, size_t send_index);

  // Reads a meter snapshot at the given tap point. tap: 0 = pre-fader,
  // 1 = post-fader (see SonareMeterTap). Returns the full snapshot.
  val meterTap(unsigned int strip_index, int tap);

  // Reads the strip's current (post-fader) meter snapshot. Tap-less, mirroring
  // the Node/Python stripMeter contract which calls sonare_strip_meter; the
  // tap-selectable variant is meterTap.
  val stripMeter(unsigned int strip_index);
  val busMeter(std::string bus_id);

  // Schedules sample-accurate fader automation on a strip. sample_pos uses the
  // absolute-sample timeline; curve: 0 = Linear, 1 = Exponential.
  void scheduleFaderAutomation(unsigned int strip_index, double sample_pos, float fader_db,
                               int curve);

  void schedulePanAutomation(unsigned int strip_index, double sample_pos, float pan, int curve);

  void scheduleWidthAutomation(unsigned int strip_index, double sample_pos, float width, int curve);

  // Schedules sample-accurate send-level automation on a strip's send.
  void scheduleSendAutomation(unsigned int strip_index, size_t send_index, double sample_pos,
                              float db, int curve);

  // Reads up to max_points of the strip's most recent goniometer samples.
  // Returns an array of { left, right } points (oldest to newest).
  val readGoniometerLatest(unsigned int strip_index, size_t max_points);

  // Resolves a strip's index from its id. Returns -1 when the id is not found;
  // the TS wrapper maps -1 to null for cross-binding consistency (Node returns
  // number | null).
  int stripById(std::string id);

  // Adds a bus to the mixer topology. role is one of "master", "aux", "submix"
  // (empty defaults to "aux"). Marks the routing graph dirty; call compile (or
  // process) to rebuild.
  void addBus(std::string id, std::string role);

  void removeBus(std::string id);

  size_t busCount() const;

  // Adds a VCA group with the given gain offset. members is an array of strip-id
  // strings (may be empty).
  void addVcaGroup(std::string id, float gain_db, val members);

  void removeVcaGroup(std::string id);

  void setVcaGroupGainDb(std::string id, float gain_db);

  void setVcaGroupMembers(std::string id, val members);

  size_t vcaGroupCount() const;

  std::string toSceneJson() const;

  val processStereo(val left_channels, val right_channels);

  void processStereoInto(val left_channels, val right_channels, val out_left, val out_right);

  val inputLeftView(size_t index);

  val inputRightView(size_t index);

  val outputLeftView();

  val outputRightView();

  void processPreparedStereo(size_t num_samples);

  // Turns the master-output meter on or off. While on, processPreparedStereo
  // meters the stereo master it just produced, so the caller neither copies the
  // output nor re-implements the measurement.
  //
  // The measurement is sonare::mixing::MeterProcessor, the same processor the
  // engine's meter telemetry publishes from: its true peak is an inter-sample
  // peak taken after oversampling (ITU-R BS.1770-4 Annex 2 requires at least
  // 4x), which is a different quantity from the sample peak and the one a
  // ceiling decision depends on. @p true_peak_oversample must be 0 (= 4x) or a
  // power of two in [1, 16]; the core raises anything below 4x to the standard's
  // minimum.
  //
  // Enabling resets the meter, so a reading never mixes audio from before a
  // period when metering was off. Re-enabling at the same factor does not
  // reallocate, which keeps it usable from an audio-thread message handler.
  void configureMeter(bool enabled, int true_peak_oversample);

  // Latest meter reading, describing the most recently metered block. All dB
  // fields are finite and floored at kFloorDb (-120). Throws when the meter has
  // never been enabled.
  val meterSnapshot() const;

  // Reports the longest audible serial processor-tail path to the master
  // (samples). Lazily compiles if the topology is dirty.
  int tailSamples();

  // Reports the compiled mixer graph's latency (samples) for aligning dry/wet
  // material. Lazily compiles if the topology is dirty.
  int latencySamples();

  // Drains delayed/tail audio by processing a zero-input block of num_samples
  // frames. Returns { left, right, sampleRate } mirroring processStereo.
  val drainTailStereo(double num_samples);

  // Converts a C-ABI mix-meter snapshot to the JS meter object (same shape as
  // meterSnapshotToVal / Node's MixMeterToObject). Depends on no instance
  // state, so the free mixing entry points share it rather than carrying their
  // own copy.
  static val mixMeterSnapshotToVal(const SonareMixMeterSnapshot& snapshot);

 private:
  static void checkStripError(SonareError err, const char* what);

  SonareMixer* mixer_ = nullptr;
  int sample_rate_ = 48000;
  int block_size_ = 0;
  // Non-fatal warning captured at scene load (newline-joined; empty if none).
  std::string scene_warning_;
  std::vector<std::vector<float>> left_scratch_;
  std::vector<std::vector<float>> right_scratch_;
  std::vector<const float*> left_ptrs_;
  std::vector<const float*> right_ptrs_;
  std::vector<float> out_scratch_left_;
  std::vector<float> out_scratch_right_;
  // Master-output meter. Absent until configureMeter() first enables it, and
  // kept prepared afterwards so toggling costs no allocation.
  std::optional<sonare::mixing::MeterProcessor> meter_;
  bool meter_active_ = false;
  int meter_oversample_ = 0;
};

MixerWasm* createMixerFromSceneJson(std::string json, int sample_rate, int block_size);

// Each domain TU registers its slice of the single Mixer class_ handle. The core
// TU (mixing.cpp) creates the handle, registers the free functions, and calls
// these in turn from registerMixingBindings().
void registerMixerStripControls(emscripten::class_<MixerWasm>& cls);
void registerMixerAutomationMeters(emscripten::class_<MixerWasm>& cls);
void registerMixerProcessing(emscripten::class_<MixerWasm>& cls);
void registerMixerTopology(emscripten::class_<MixerWasm>& cls);

#endif  // SONARE_WITH_MIXING && SONARE_WITH_GRAPH

#endif  // __EMSCRIPTEN__
