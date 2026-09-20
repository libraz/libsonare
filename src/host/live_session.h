#pragma once

/// @file live_session.h
/// @brief Binds a live audio output and a live MIDI input to a RealtimeEngine.
///
/// This class owns no DSP and decides nothing about sound: it resolves the
/// devices a performance needs, hands the audio backend's clock mapper to the
/// MIDI input so incoming timestamps land on render frames, points the engine
/// at that input, and pumps engine.process() from the device's render callback.
/// Everything it does is available piecemeal from the backends; what it adds is
/// that the bindings cannot be done in the wrong order. The audio output is
/// required and the MIDI input is not, so a playback-only host is one Config
/// field rather than a second class.
///
/// macOS only, behind BUILD_COREAUDIO and BUILD_COREMIDI. Like the backends it
/// sits on, this is an out-of-core leaf: no language binding links it and it is
/// absent from the distributed library, so a host reaches it by building the
/// backends in and linking this target directly.
///
/// Threading follows the seam contract: open()/close() are control-thread and
/// may allocate; render() runs on the device's audio thread and does not.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "host/audio_device.h"
#include "host/backends/coreaudio/coreaudio_device.h"
#include "host/backends/coremidi/coremidi_io.h"

namespace sonare::engine {
class RealtimeEngine;
}  // namespace sonare::engine

namespace sonare::host {

/// What a device enumeration returns for one entry. Fixed storage so listing
/// devices costs no allocation past the call itself; a longer name is truncated
/// rather than refused, because a display string is not an identifier.
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
  /// The named audio device could not be opened at the requested format.
  kAudioOutputUnavailable,
  /// The device opened but would not start streaming.
  kAudioStartFailed,
};

class LiveSession final {
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

  /// Both device fields are indices into the lists the four accessors above
  /// describe, not opaque handles: the MIDI seam is index-addressed already,
  /// and one convention beats two. `destination_id` is the engine MIDI
  /// destination the input drives.
  struct Config {
    size_t audio_output_index = 0;
    size_t midi_input_index = 0;
    double sample_rate = 48000.0;
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

  /// CONTROL thread: resolves both devices, wires them to @p engine and starts
  /// streaming. The engine is prepared for the format the device negotiated,
  /// which is not necessarily the one requested. On any result other than kOk
  /// nothing is left open and @p engine is untouched.
  LiveOpenResult open(engine::RealtimeEngine* engine, const Config& config);

  /// CONTROL thread: stops streaming, unbinds the engine's MIDI input and
  /// closes both devices. Safe to call on a session that never opened.
  void close() noexcept;

  bool is_running() const noexcept;

  /// The format the device actually settled on, which a driver may round up or
  /// substitute for the one requested. 0 before a successful open().
  int actual_block_size() const noexcept;
  double actual_sample_rate() const noexcept;

  /// The output latency the driver reports, in milliseconds at the negotiated
  /// rate. This is what the device claims, not a measured round trip: only a
  /// loopback from the output back into an input can check the claim, and this
  /// session opens no input. 0 means the driver reported nothing.
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

  Pump pump_{this};
  engine::RealtimeEngine* engine_ = nullptr;
  uint32_t destination_id_ = 0;
  std::unique_ptr<backends::CoreAudioDevice> audio_;
  std::unique_ptr<backends::CoreMidiInput> midi_;
  AudioStreamConfig negotiated_{};
  std::atomic<uint64_t> render_calls_{0};
  bool open_ = false;
};

}  // namespace sonare::host
