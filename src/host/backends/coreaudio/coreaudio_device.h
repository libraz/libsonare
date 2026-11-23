#pragma once

/// @file coreaudio_device.h
/// @brief CoreAudio (AUHAL) implementation of the sonare::host::AudioDevice
///        seam. It owns a real input and/or output device and pumps an
///        AudioDeviceCallback on the CoreAudio I/O thread. macOS only.
///
/// This public header intentionally includes NO CoreAudio / AudioToolbox
/// headers — the SDK types live entirely behind a pimpl in the .mm so a host
/// (e.g. an app shell) can include and link this without dragging the SDK into
/// its own translation units (invariant 6: the seam stays SDK-free even though
/// the implementation is not).

#include <memory>
#include <string>
#include <vector>

#include "host/audio_device.h"
#include "host/midi_io.h"

namespace sonare::host::backends {

/// AUHAL-backed input and/or output device. Construct, then open()/start() from
/// the control thread; AudioDeviceCallback::render runs on the CoreAudio I/O
/// thread. close()/stop() return only once the I/O thread is quiesced.
///
/// Latency and xrun figures are queried from the live device after open(), so
/// input_latency_samples()/output_latency_samples()/xrun_count() report the
/// driver's actual numbers rather than the requested AudioStreamConfig.
class CoreAudioDevice final : public AudioDevice {
 public:
  CoreAudioDevice();
  explicit CoreAudioDevice(uint32_t device_id);
  /// Binds an enumerated ID to its UID so a recycled ID cannot open a new device.
  CoreAudioDevice(uint32_t device_id, std::string uid);
  ~CoreAudioDevice() override;

  CoreAudioDevice(const CoreAudioDevice&) = delete;
  CoreAudioDevice& operator=(const CoreAudioDevice&) = delete;

  bool open(const AudioStreamConfig& config, AudioDeviceCallback* callback) override;

  /// CONTROL thread: binds the device @p device_id names rather than the
  /// direction's system default. 0 selects that default. Allocates while
  /// resolving the device; never called from the audio thread.
  bool open_device(uint32_t device_id, const AudioStreamConfig& config,
                   AudioDeviceCallback* callback);

  /// One system audio device as observed by a single point-in-time snapshot.
  struct DeviceInfo {
    uint32_t id = 0;
    std::string uid;
    std::string name;
    int input_channels = 0;
    int output_channels = 0;
    bool is_default_input = false;
    bool is_default_output = false;
    double nominal_sample_rate = 0.0;
    uint32_t clock_domain = 0;
  };

  /// CONTROL thread: every device with at least one input or output channel.
  /// The snapshot is point-in-time; use the UID to resolve a fresh ID after a
  /// hardware-list change.
  static std::vector<DeviceInfo> devices();

  /// CONTROL thread: request a nominal device rate and wait up to 500 ms for a
  /// serial property listener to observe it. False means the device refused or
  /// did not settle on the requested rate.
  static bool set_nominal_sample_rate(uint32_t device_id, double rate);
  /// UID-checked version for a device selected from a prior enumeration.
  static bool set_nominal_sample_rate(uint32_t device_id, double rate,
                                      const std::string& expected_uid);

  /// Latched until the next open attempt. These readers remain valid after a
  /// failed open has released its CoreAudio resources.
  bool device_lost() const noexcept;
  bool rate_changed() const noexcept;
  bool format_refused() const noexcept;

  /// CONTROL thread: how many devices expose at least one output channel. The
  /// index the two accessors below take is into that filtered list, not into
  /// the system's full device list, so an input-only interface does not shift
  /// the outputs a host displays.
  static size_t output_device_count();

  /// CONTROL thread: the opaque id of output device @p index, which is what
  /// open_device() accepts. 0 on a bad index, which is also the value meaning
  /// "the system default" there -- an unreachable index cannot be distinguished
  /// from a deliberate default, so check the count first.
  static uint32_t output_device_id(size_t index);

  /// CONTROL thread: writes output device @p index's display name into @p out
  /// as NUL-terminated UTF-8, truncated to @p capacity. False on a bad index, a
  /// zero capacity, or a name the device does not publish; @p out is left
  /// untouched in that case.
  static bool output_device_name(size_t index, char* out, size_t capacity);

  /// One output device as a single enumeration saw it. `name` is empty when the
  /// device publishes none.
  struct OutputDevice {
    uint32_t id = 0;
    char name[128] = {};
  };

  /// CONTROL thread: every output device, in the order the index accessors use,
  /// read from one enumeration so ids and names describe the same device set.
  /// The snapshot is a point-in-time view; hotplug is picked up by calling
  /// again, and open_device() still resolves its id against the live system.
  static std::vector<OutputDevice> output_devices();
  bool start() override;
  void stop() noexcept override;
  void close() noexcept override;
  bool is_running() const noexcept override;

  int input_latency_samples() const noexcept override;
  int output_latency_samples() const noexcept override;
  uint32_t xrun_count() const noexcept override;

  /// Shared audio-clock correlation for CoreMIDI input/output. Attach this
  /// mapper to CoreMidiInput/CoreMidiOutput before streaming; the render
  /// callback refreshes it from CoreAudio's sample/host timestamps.
  MidiHostTimeMapper& midi_time_mapper() noexcept;

  /// TEST-ONLY seam: record one xrun, exactly as the render callback does when
  /// it detects a sample-clock discontinuity.
  ///
  /// The counter is otherwise driven only from the CoreAudio render thread on a
  /// real dropout, which no test can provoke on demand, so the invariant that
  /// the count spans a whole open() — surviving any number of start()/stop()
  /// cycles — has nothing deterministic to assert against without this. Not
  /// part of the AudioDevice seam and no production caller uses it.
  void note_xrun_for_test() noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace sonare::host::backends
