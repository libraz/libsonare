#pragma once

/// @file audio_device.h
/// @brief AudioDevice callback seam: the abstract boundary a host audio
///        backend (PortAudio / CoreAudio / WASAPI / AudioWorklet / ...)
///        implements to PUMP audio through the engine. Header-only.
///
/// Scope and invariants
/// --------------------
///  - This is a pure abstract interface over CORE types only (channel buffers,
///    frame counts, stream/sample time). It includes NO OS or device SDK
///    headers (invariant 6: core stays free of system/device/plugin SDK
///    includes). The real backend lives out-of-tree or behind a build option
///    and is EXCLUDED from the default distribution.
///  - The engine does NOT depend on this seam. It is the boundary a HOST uses
///    to drive rendering: the host owns the device, opens it, and on each
///    hardware callback hands the engine input/output channel buffers plus a
///    frame count. The engine itself is pumped by the host's
///    RealtimeEngine::process(...) call from inside AudioDeviceCallback::render.
///  - Header-only: abstract interfaces + small POD structs, no .cpp, no lib.
///
/// Threading / RT contract
/// -----------------------
/// open()/close() run on the CONTROL thread and MAY block / allocate.
/// render() runs on the AUDIO (device) thread and MUST be allocation-free,
/// lock-free and I/O-free, exactly like rt::ProcessorBase::process.

#include <cstdint>

namespace sonare::host {

/// Negotiated stream format the host opens the device with. Plain value data;
/// no device handle. `sample_rate` is the agreed device sample rate (Hz);
/// `max_block_size` is the largest frame count a single render() call may
/// request (the host sizes its scratch and calls engine.prepare() with it).
struct AudioStreamConfig {
  double sample_rate = 48000.0;
  int max_block_size = 512;
  int num_input_channels = 0;
  int num_output_channels = 2;
};

/// Per-callback timing handed to the render callback. `stream_time_seconds` is
/// the device's monotonic stream clock at the first frame of this buffer;
/// `sample_time` is the absolute frame index of the first frame since the
/// stream started. Both are advisory (for sync / capture timestamping); a
/// backend that cannot provide them leaves them at 0.
struct AudioCallbackTime {
  double stream_time_seconds = 0.0;
  int64_t sample_time = 0;
};

/// One render request: deinterleaved input/output channel buffers plus the
/// frame count for this callback. `inputs`/`outputs` are borrowed pointers
/// owned by the backend and valid only for the duration of the render() call;
/// the callee must not retain them. `num_frames` is <= the negotiated
/// max_block_size. When the device has no inputs, `inputs` is nullptr and
/// `num_input_channels` is 0.
struct AudioBufferView {
  const float* const* inputs = nullptr;
  float* const* outputs = nullptr;
  int num_input_channels = 0;
  int num_output_channels = 0;
  int num_frames = 0;
  AudioCallbackTime time{};
};

/// The seam a host audio backend drives. A concrete backend owns the device and
/// calls render() on its audio thread; render() is where the host pumps the
/// engine (engine.process(buffers.outputs, ...)). This interface is what the
/// out-of-tree backend implements; the engine never sees it.
class AudioDeviceCallback {
 public:
  virtual ~AudioDeviceCallback() = default;

  /// CONTROL thread: called once when the stream opens with the negotiated
  /// format, before any render() call. The callee sizes scratch / prepares the
  /// engine here (allocation allowed). Returns false to abort opening.
  virtual bool open(const AudioStreamConfig& config) = 0;

  /// AUDIO thread: render one device buffer. MUST be allocation-free,
  /// lock-free and I/O-free. The callee fills `buffers.outputs` for
  /// `buffers.num_frames` frames, optionally reading `buffers.inputs`.
  virtual void render(const AudioBufferView& buffers) noexcept = 0;

  /// CONTROL thread: called once when the stream closes; no render() follows.
  /// Releasing scratch / engine state here is allowed (allocation/free).
  virtual void close() noexcept = 0;
};

/// The backend side: an abstract handle a host uses to OPEN/START/STOP a device
/// without naming any SDK type. A concrete out-of-tree implementation wraps the
/// real device API; core only sees this interface and the POD config. Optional
/// — a host may pump the engine directly — but provided so the device boundary
/// is fully expressible in core terms.
class AudioDevice {
 public:
  virtual ~AudioDevice() = default;

  /// CONTROL thread: open the device with `config` and bind `callback` as the
  /// render sink. May block / allocate. Returns false on failure.
  virtual bool open(const AudioStreamConfig& config, AudioDeviceCallback* callback) = 0;

  /// CONTROL thread: begin streaming (callback->render starts firing).
  virtual bool start() = 0;

  /// CONTROL thread: stop streaming (no further render after it returns).
  virtual void stop() noexcept = 0;

  /// CONTROL thread: close the device (calls callback->close()).
  virtual void close() noexcept = 0;

  /// True while the device is actively streaming.
  virtual bool is_running() const noexcept = 0;
};

}  // namespace sonare::host
