#pragma once

/// @file au_instrument_provider.h
/// @brief Audio Unit implementation of the sonare::host::InstrumentProvider
///        seam. macOS only; built behind BUILD_AU_HOST.
///
/// The provider turns a PluginDescriptor (format "au") into a core instance:
/// a MusicDevice AU becomes a midi::MidiInstrument, an effect AU becomes an
/// rt::ProcessorBase. The SDK objects (AudioUnit / AudioComponent) live behind
/// the .mm — this public header includes NO AudioUnit headers, and the returned
/// instances are pure core types (invariant 6: core never sees the SDK).
///
/// Descriptor convention: `format` is "au" and `id` encodes the component as
/// "<type>:<subtype>:<manufacturer>" with each field a lowercase 8-char hex of
/// the OSType (e.g. "61756d75:..." ). `enumerate` fills descriptors for the
/// MusicDevice / effect AUs installed on the system.

#include <memory>
#include <string>
#include <vector>

#include "host/plugin_host.h"

namespace sonare::host::backends {

/// Optional telemetry an AuInstrumentProvider::create_instrument() result may
/// support, alongside the plain midi::MidiInstrument interface it always
/// returns. midi::MidiInstrument (src/midi/instrument.h) has no telemetry seam
/// of its own and is not the place to add an AU-specific one (invariant 6:
/// core stays SDK-free and generic across all instrument providers, not just
/// this one). A caller that specifically wants to observe this AU-backed
/// instrument's MIDI-event drop counter downcasts:
///   auto instrument = provider.create_instrument(descriptor);
///   if (auto* telemetry =
///           dynamic_cast<const AuInstrumentTelemetry*>(instrument.get())) {
///     telemetry->dropped_count();
///   }
class AuInstrumentTelemetry {
 public:
  virtual ~AuInstrumentTelemetry() = default;

  /// Cumulative count of MIDI events dropped because the block-local event
  /// queue (bounded, no-allocation) was full when on_event() was called.
  virtual uint32_t dropped_count() const noexcept = 0;
};

namespace detail {

/// Deterministic result from the fake-AudioUnit render-path call spy.
struct AuProcessCallSpyResult {
  bool instrument_controls_unchanged = false;
  bool effect_controls_unchanged = false;
  unsigned render_calls = 0;
};

/// Exercises the actual instrument/effect process implementations with a fake
/// AudioUnit call table. No SDK object or installed plugin is required.
AuProcessCallSpyResult run_au_process_call_spy();

/// Result from probing AuEffectProcessor's input render callback with a block
/// smaller than the prepared maximum. No SDK object or installed plugin is
/// required: the fake AudioUnit call table's render() invokes the captured
/// input callback directly, requesting the prepared maximum frame count
/// regardless of the block size the outer process() call used, mirroring a
/// third-party AU's internal input buffering.
struct AuUndersizedBlockProbeResult {
  bool ran = false;
  size_t block_samples = 0;
  size_t probe_frames = 0;
};

AuUndersizedBlockProbeResult run_au_effect_undersized_block_probe();

/// Result from probing the MusicDevice adapter's dropped-event counter. No SDK
/// object or installed plugin is required.
struct AuInstrumentDroppedEventProbeResult {
  // Whether AuInstrumentTelemetry was reachable via dynamic_cast from the
  // plain midi::MidiInstrument* every real caller of create_instrument() gets
  // back — the actual regression this probes for.
  bool telemetry_reachable = false;
  uint32_t dropped_before_overflow = 0;
  uint32_t dropped_after_overflow = 0;
};

AuInstrumentDroppedEventProbeResult run_au_instrument_dropped_event_probe();

/// Result from probing where the MusicDevice adapter places queued events once
/// the host pushes a transport snapshot. Two blocks are rendered: the second one
/// starts at a render frame that does NOT continue the first, as a seek or a loop
/// wrap does, so an adapter deriving the block base from its own accumulated
/// position misplaces the second block's event. No SDK object or installed
/// plugin is required.
struct AuInstrumentTransportPlacementProbeResult {
  bool ran = false;
  // Block-relative frames handed to MusicDeviceMIDIEvent, one per rendered block.
  unsigned first_event_frame = 0;
  unsigned second_event_frame = 0;
  // Sample times stamped on each block's render timestamp.
  double first_sample_time = 0.0;
  double second_sample_time = 0.0;
};

AuInstrumentTransportPlacementProbeResult run_au_instrument_transport_placement_probe();

}  // namespace detail

/// Factory over the system's Audio Units. Control-thread only; instantiation
/// allocates. A single provider can create both instruments and effects.
class AuInstrumentProvider final : public InstrumentProvider {
 public:
  AuInstrumentProvider();
  ~AuInstrumentProvider() override;

  /// CONTROL thread: enumerate installed AUs of `kind` as ready-to-instantiate
  /// PluginDescriptors (format "au"). kInstrument lists MusicDevices; kEffect
  /// lists effect units.
  static std::vector<PluginDescriptor> enumerate(PluginKind kind);

  bool can_create(const PluginDescriptor& descriptor) const noexcept override;
  std::unique_ptr<midi::MidiInstrument> create_instrument(
      const PluginDescriptor& descriptor) override;
  std::unique_ptr<rt::ProcessorBase> create_effect(const PluginDescriptor& descriptor) override;

  size_t parameter_count(const PluginDescriptor& descriptor) const noexcept override;
  bool parameter_descriptor(const PluginDescriptor& descriptor, size_t index,
                            PluginParameterDescriptor* out) const noexcept override;

 private:
  // Single AU instance reused across parameter_count / parameter_descriptor calls
  // for the same descriptor, so enumerating N parameters instantiates the AU once
  // instead of N+1 times. Held as void* to keep the SDK's AudioUnit type out of
  // this public header (invariant 6); disposed in the destructor. Control-thread
  // only, matching the rest of the provider.
  mutable void* param_cache_unit_ = nullptr;
  mutable std::string param_cache_id_;
};

}  // namespace sonare::host::backends
