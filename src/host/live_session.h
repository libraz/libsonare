#pragma once

/// @file live_session.h
/// @brief Binds live audio devices and an optional MIDI input to a RealtimeEngine.
///
/// This class owns no DSP and decides nothing about sound: it resolves the
/// devices a performance needs, hands the audio backend's clock mapper to the
/// MIDI input so incoming timestamps land on render frames, points the engine
/// at that input, and pumps the engine from the device callback that owns the
/// audio clock. A session may use output, input, or both directions. The
/// index-based Config keeps its original output-only behavior.
///
/// macOS only, behind BUILD_COREAUDIO and BUILD_COREMIDI. Like the backends it
/// sits on, this is an out-of-core leaf: no language binding links it and it is
/// absent from the distributed library, so a host reaches it by building the
/// backends in and linking this target directly.
///
/// Threading follows the seam contract: open()/close() are control-thread and
/// may allocate; render() runs on the device's audio thread and does not.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "engine/realtime_engine.h"
#include "host/audio_device.h"
#include "host/backends/coreaudio/coreaudio_device.h"
#include "host/backends/coremidi/coremidi_io.h"
#include "host/live_input_ring.h"
#include "host/midi_io.h"
#include "util/constants.h"

namespace sonare::engine {
class RealtimeEngine;
}  // namespace sonare::engine

namespace sonare::host {

/// What a device enumeration returns for one entry. Fixed storage so listing
/// devices costs no allocation past the call itself; a longer name is truncated
/// at a complete UTF-8 code point rather than refused, because a display string
/// is not an identifier.
struct LiveDeviceInfo {
  char name[128] = {};
};

/// Why an open() did not produce a running session. Deliberately not the C
/// ABI's error enum: nothing here crosses the C boundary, and naming that enum
/// would drag the public headers into a host-only translation unit.
enum class LiveOpenResult : uint8_t {
  kOk,
  /// A Config field is outside its domain, or the engine pointer is null.
  kInvalidArgument,
  /// The named MIDI source could not be opened.
  kMidiInputUnavailable,
  /// A selected MIDI source is currently registered without an owned lifetime.
  kMidiInputSourceNotOwned,
  /// The named audio device could not be opened at the requested format.
  kAudioOutputUnavailable,
  /// The selected audio output opened but would not start streaming.
  kAudioOutputStartFailed,
  /// Starting MIDI capture failed while reserving its bounded event queue.
  kOutOfMemory,
  /// The engine refused the format the device negotiated, e.g. a sample rate other
  /// than the one an installed project timeline is locked to.
  kEngineRefusedFormat,
  /// Another live driver already owns the engine.
  kEngineAlreadyDriven,
  /// The selected audio input is absent or unavailable.
  kAudioInputUnavailable,
  /// The selected devices could not reach a shared native sample rate.
  kAudioFormatRefused,
};

class LiveSession final : public engine::LiveDriver {
 public:
  LiveSession();
  ~LiveSession();

  LiveSession(const LiveSession&) = delete;
  LiveSession& operator=(const LiveSession&) = delete;

  /// CONTROL thread: how many audio output devices and MIDI input sources the
  /// system currently exposes. An index is only meaningful against the count
  /// read beside it — devices come and go, and nothing here holds a snapshot.
  static size_t audio_output_count() noexcept;
  static size_t midi_input_count() noexcept;

  /// CONTROL thread: fills @p out for the device at @p index. False on a bad
  /// index or a device that publishes no name, leaving @p out untouched.
  static bool audio_output_info(size_t index, LiveDeviceInfo* out) noexcept;
  static bool midi_input_info(size_t index, LiveDeviceInfo* out) noexcept;

  /// CONTROL thread: replaces @p out with every audio output, in index order,
  /// from one enumeration. A device that publishes no name has an empty name.
  /// Prefer this to count-then-info when listing: each of those calls
  /// enumerates the system again. The list is a snapshot; re-read it on a
  /// hotplug notification or before presenting a choice.
  static void audio_outputs(std::vector<LiveDeviceInfo>* out);

  /// Both device fields are indices into the lists the four accessors above
  /// describe, not opaque handles: the MIDI seam is index-addressed already,
  /// and one convention beats two. `destination_id` is the engine MIDI
  /// destination the input drives.
  struct Config {
    size_t audio_output_index = 0;
    size_t midi_input_index = 0;
    double sample_rate = sonare::constants::kDefaultDawSampleRate;
    int block_size = 128;
    uint32_t destination_id = 0;
    /// Opens the system default output instead of `audio_output_index`. The
    /// default is what a host wants until a user picks otherwise, and it is not
    /// expressible as an index: CoreAudio names it separately from its device
    /// list, and the entry it points at moves.
    bool use_default_audio_output = true;
    /// Opens no MIDI input, leaving the engine's input source unbound. A
    /// playback-only host is a real configuration, and requiring a source would
    /// tie every session to hardware the machine need not have.
    bool use_midi_input = true;
  };

  enum class DeviceSelection : uint8_t { kNone, kDefault, kByUid };

  struct DeviceSelector {
    DeviceSelection selection = DeviceSelection::kNone;
    std::string uid;
  };

  /// UID-based selection used by hosts that need independent input and output
  /// devices. A zero rate uses the selected device's current nominal rate.
  struct SelectorConfig {
    DeviceSelector audio_output{};
    DeviceSelector audio_input{};
    DeviceSelector midi_input{};
    double sample_rate = 0.0;
    int block_size = 128;
    uint32_t destination_id = 0;
  };

  /// Replaces one selected CoreAudio device with an SDK-free seam. The metadata
  /// still participates in UID/default resolution and rate matching.
  struct AudioDeviceOverride {
    backends::CoreAudioDevice::DeviceInfo info{};
    std::unique_ptr<AudioDevice> device;
  };

  struct Status {
    bool running = false;
    bool device_lost = false;
    bool rate_changed = false;
    int sample_rate = 0;
    int output_block_size = 0;
    int input_block_size = 0;
    double output_latency_ms = 0.0;
    double input_latency_ms = 0.0;
    uint64_t xrun_count = 0;
    /// Input frames dropped at the producer or discarded above high water.
    uint64_t input_overrun_frames = 0;
    /// Aggregate of low-water repeats and actual input starvation after priming.
    uint64_t input_underrun_frames = 0;
    /// Frames missing after the first successful prime, including silence while reprime waits.
    uint64_t input_starvation_frames = 0;
    /// Frames intentionally repeated to compensate for low-water clock drift.
    uint64_t input_drift_repeat_frames = 0;
    uint64_t dropped_midi_events = 0;
  };

  /// CONTROL thread: resolves both devices, wires them to @p engine and starts
  /// streaming. The engine is prepared for the format the device negotiated,
  /// which is not necessarily the one requested. On any result other than kOk
  /// no device is left open, but @p engine is not restored. Once the device has
  /// negotiated a format the engine is prepared for it -- resetting its transport
  /// and dropping queued commands -- even when a later step fails, and
  /// kEngineRefusedFormat means that prepare itself threw.
  LiveOpenResult open(engine::RealtimeEngine* engine, const Config& config);

  /// CONTROL thread: resolve stable selectors, open independent input/output
  /// devices, prepare the engine once, and then start the active units. A MIDI
  /// selector requires an output; NONE leaves an existing engine source alone.
  LiveOpenResult open(engine::RealtimeEngine* engine, const SelectorConfig& config);
  LiveOpenResult open(engine::RealtimeEngine* engine, const SelectorConfig& config,
                      AudioDeviceOverride input_override, AudioDeviceOverride output_override);
  /// Uses an already-open source in place of the selected MIDI device.
  LiveOpenResult open(engine::RealtimeEngine* engine, const SelectorConfig& config,
                      AudioDeviceOverride input_override, AudioDeviceOverride output_override,
                      std::unique_ptr<MidiInputSource> midi_override);

  /// CONTROL thread: stops output before input and closes both devices. Restores
  /// an owned prior MIDI input binding only while this session's source remains
  /// active; an intervening replacement is preserved. Safe before open().
  void close() noexcept;
  void stop() noexcept override { close(); }

  bool is_running() const noexcept;

  Status status() const noexcept;

#if defined(SONARE_WITH_ARRANGEMENT)
  /// CONTROL thread: drains MIDI events captured by the engine since the last
  /// call. Calls from multiple readers are serialized.
  std::vector<engine::CapturedMidiEvent> captured_midi();
#endif

  /// The format the device actually settled on, which a driver may round up or
  /// substitute for the one requested. 0 before a successful open().
  int actual_block_size() const noexcept;
  double actual_sample_rate() const noexcept;

  /// The output latency the driver reports, in milliseconds at the negotiated
  /// rate. This is the device's claim rather than a measured round trip. 0 means
  /// the driver reported nothing or the session has no output.
  double output_latency_ms() const noexcept;

  /// Dropouts the device has reported since it opened, not since it last
  /// started — a stop/start cycle inside one session does not reset it.
  uint32_t xrun_count() const noexcept;

  /// Render callbacks the device has made since it opened, counted whether or
  /// not the callback had anything to fill. A session that reports zero xruns
  /// over zero callbacks has measured nothing, which is why this is readable
  /// rather than inferred from elapsed time.
  uint64_t render_callback_count() const noexcept;

 private:
  /// The device's view of the session. A separate object rather than a base of
  /// LiveSession, because the seam's close() and the session's close() mean
  /// different things -- one ends a stream, the other ends a session -- and one
  /// class cannot hold both names.
  class Pump final : public AudioDeviceCallback {
   public:
    explicit Pump(LiveSession* owner) noexcept : owner_(owner) {}
    bool open(const AudioStreamConfig& config) override;
    void render(const AudioBufferView& buffers) noexcept override;
    void close() noexcept override;

   private:
    LiveSession* owner_;
  };

  struct ResolvedAudioDevice {
    backends::CoreAudioDevice::DeviceInfo info{};
    std::unique_ptr<AudioDevice> device;
  };

  LiveOpenResult open_devices(engine::RealtimeEngine* engine, const SelectorConfig& config,
                              ResolvedAudioDevice input, ResolvedAudioDevice output,
                              std::unique_ptr<MidiInputSource> midi_override);
#if defined(SONARE_WITH_ARRANGEMENT)
  void bind_midi_input() noexcept;
#endif
  bool prepare_from_callback(const AudioStreamConfig& config);
  void close_devices() noexcept;
  void publish_mapper_anchor(const AudioBufferView& buffers) noexcept;

  Pump pump_{this};
  engine::RealtimeEngine* engine_ = nullptr;
  uint32_t destination_id_ = 0;
  std::unique_ptr<AudioDevice> audio_;
  std::unique_ptr<AudioDevice> input_audio_;
  std::unique_ptr<AudioDevice> output_audio_;
#if defined(SONARE_WITH_ARRANGEMENT)
  std::shared_ptr<MidiInputSource> midi_;
  engine::RealtimeEngine::MidiInputBinding previous_midi_{};
#else
  std::unique_ptr<MidiInputSource> midi_;
#endif
  AudioStreamConfig negotiated_{};
  AudioStreamConfig input_negotiated_{};
  AudioStreamConfig output_negotiated_{};
  MidiHostTimeMapper midi_time_mapper_{};
  LiveInputRing input_ring_{};
  std::vector<float> input_scratch_left_;
  std::vector<float> input_scratch_right_;
  std::array<float*, 2> input_scratch_{};
  std::mutex captured_midi_mutex_;
  std::atomic<uint64_t> render_calls_{0};
  bool input_opened_ = false;
  bool output_opened_ = false;
  bool audio_device_open_ = false;
  bool input_device_open_ = false;
  bool output_device_open_ = false;
  bool input_requested_ = false;
  bool output_requested_ = false;
  bool engine_prepared_ = false;
  bool driver_attached_ = false;
  bool midi_selected_ = false;
#if defined(SONARE_WITH_ARRANGEMENT)
  bool midi_bound_ = false;
#endif
  bool midi_capture_started_ = false;
  bool input_uses_ring_ = false;
  bool clock_locked_ = false;
  bool open_ = false;
  /// Set by Pump::open() when the engine, not the device, refused the format.
  bool engine_refused_format_ = false;
  bool device_format_refused_ = false;
  bool require_exact_rate_ = false;
};

}  // namespace sonare::host
