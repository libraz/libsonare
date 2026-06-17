#pragma once

/// @file coreaudio_device.h
/// @brief CoreAudio (AUHAL) implementation of the sonare::host::AudioDevice
///        seam. This is a concrete out-of-core backend: it owns a real output
///        device and pumps an AudioDeviceCallback on the CoreAudio render
///        thread. macOS only; built behind BUILD_COREAUDIO.
///
/// This public header intentionally includes NO CoreAudio / AudioToolbox
/// headers — the SDK types live entirely behind a pimpl in the .mm so a host
/// (e.g. an app shell) can include and link this without dragging the SDK into
/// its own translation units (invariant 6: the seam stays SDK-free even though
/// the implementation is not).

#include <memory>

#include "host/audio_device.h"

namespace sonare::host::backends {

/// AUHAL-backed output device. Construct, then open()/start() from the control
/// thread; the bound AudioDeviceCallback::render runs on the CoreAudio audio
/// thread. close()/stop() return only once the render thread is quiesced.
///
/// Latency and xrun figures are queried from the live device after open(), so
/// input_latency_samples()/output_latency_samples()/xrun_count() report the
/// driver's actual numbers rather than the requested AudioStreamConfig.
class CoreAudioDevice final : public AudioDevice {
 public:
  CoreAudioDevice();
  ~CoreAudioDevice() override;

  CoreAudioDevice(const CoreAudioDevice&) = delete;
  CoreAudioDevice& operator=(const CoreAudioDevice&) = delete;

  bool open(const AudioStreamConfig& config, AudioDeviceCallback* callback) override;
  bool start() override;
  void stop() noexcept override;
  void close() noexcept override;
  bool is_running() const noexcept override;

  int input_latency_samples() const noexcept override;
  int output_latency_samples() const noexcept override;
  uint32_t xrun_count() const noexcept override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace sonare::host::backends
