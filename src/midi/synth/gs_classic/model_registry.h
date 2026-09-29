#pragma once

/// @file model_registry.h
/// @brief GS classic models looked up by type number, with byte maps expanded once.
///
/// A registry takes one configuration's generated pools and expands every map spec
/// into its 128-entry LUT at construction, so the audio path only indexes. The
/// default configuration (with overlays) is `gs_classic_models_default()`, generated
/// into `gs_classic_models.inc`; the overlay-less one exists only in the tests.

#include <cstdint>
#include <vector>

#include "midi/synth/gs_classic/model_format.h"

namespace sonare::midi::synth::gs_classic {

/// The generated default configuration; its `luts` is null until a registry expands it.
GsClassicModelSet gs_classic_models_default();

class GsClassicModelRegistry {
 public:
  /// Expands every map spec of @p set; `valid()` is false if any spec does not expand.
  explicit GsClassicModelRegistry(const GsClassicModelSet& set);
  GsClassicModelRegistry(const GsClassicModelRegistry&) = delete;
  GsClassicModelRegistry& operator=(const GsClassicModelRegistry&) = delete;

  bool valid() const noexcept { return valid_; }
  /// The configuration with `luts` pointing at this registry's expansions.
  const GsClassicModelSet& models() const noexcept { return set_; }
  /// The model of @p type (MSB << 8 | LSB), or null; `03 00` finds its alias `02 0C`.
  const GsClassicType* find(uint16_t type) const noexcept;

 private:
  GsClassicModelSet set_;
  std::vector<GsClassicLut> luts_;
  bool valid_ = true;
};

/// The process-wide registry of the default configuration, built on first use.
const GsClassicModelRegistry& gs_classic_default_registry();

}  // namespace sonare::midi::synth::gs_classic
