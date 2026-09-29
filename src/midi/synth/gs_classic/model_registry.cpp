#include "midi/synth/gs_classic/model_registry.h"

#include "midi/synth/gs_address_table.h"
#include "midi/synth/gs_classic/graph_engine.h"

namespace sonare::midi::synth::gs_classic {

#define GS_CLASSIC_SET_PREFIX gs_classic_models_default
#include "midi/synth/gs_classic_models.inc"
#undef GS_CLASSIC_SET_PREFIX

GsClassicModelRegistry::GsClassicModelRegistry(const GsClassicModelSet& set) : set_(set) {
  luts_.resize(set.n_map_specs);
  for (std::size_t i = 0; i < set.n_map_specs; ++i) {
    if (!gs_classic_expand_map(set.map_specs[i], set.map_keys, set.map_values, luts_[i])) {
      valid_ = false;
    }
  }
  set_.luts = luts_.data();
}

const GsClassicType* GsClassicModelRegistry::find(uint16_t type) const noexcept {
  const uint16_t alias = gs_efx_alias_type(type);
  for (std::size_t i = 0; i < set_.n_types; ++i) {
    if (set_.types[i].type == type || set_.types[i].type == alias) return &set_.types[i];
  }
  return nullptr;
}

const GsClassicModelRegistry& gs_classic_default_registry() {
  static const GsClassicModelRegistry registry(gs_classic_models_default());
  return registry;
}

}  // namespace sonare::midi::synth::gs_classic
