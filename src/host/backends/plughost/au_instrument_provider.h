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
