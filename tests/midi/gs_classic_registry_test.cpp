/// @file gs_classic_registry_test.cpp
/// @brief The GS classic model registry: every type in both configurations, the C++ map
/// expander against soundings' `_from_map`, and the Parallel-2 arrangement as generated.
///
/// `gs_classic_map_expansion.inc` holds every map spec of both configurations expanded
/// by the archive's own `_from_map` (the nearest-state rule where a states map names
/// nothing), rounded to float, as `tools/gs/classic_models.py` wrote it.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <iterator>

#include "midi/synth/gs_address_table.h"
#include "midi/synth/gs_classic/graph_engine.h"
#include "midi/synth/gs_classic/model_registry.h"

namespace sonare::midi::synth::gs_classic {
#define GS_CLASSIC_SET_PREFIX gs_classic_models_raw
#include "midi/gs_classic_models_raw.inc"
#undef GS_CLASSIC_SET_PREFIX
}  // namespace sonare::midi::synth::gs_classic

#include "midi/gs_classic_map_expansion.inc"

namespace {

namespace gc = sonare::midi::synth::gs_classic;
using sonare::midi::synth::kGsEfxTypeDefaults;

constexpr uint16_t kThru = 0x0000;
constexpr uint16_t kParallelMsb = 0x11;

const gc::GsClassicModelRegistry& raw_registry() {
  static const gc::GsClassicModelRegistry registry(gc::gs_classic_models_raw());
  return registry;
}

/// Every entry of every LUT against the Python expansion: equal after float rounding,
/// within one float ulp where a points map interpolates in the log.
void check_expansion(const gc::GsClassicModelRegistry& registry, const uint16_t* rows,
                     std::size_t n_rows) {
  const gc::GsClassicModelSet& m = registry.models();
  REQUIRE(registry.valid());
  REQUIRE(m.n_map_specs == n_rows);
  std::size_t mismatches = 0;
  for (std::size_t i = 0; i < m.n_map_specs; ++i) {
    const gc::GsClassicMapSpec& spec = m.map_specs[i];
    const bool log_points = spec.kind == gc::GsClassicMapKind::kPoints && spec.log != 0;
    const float* expected = kGsClassicMapExpansion[rows[i]];
    // A value that cancels to near zero keeps a residue whose size depends on FMA contraction, so
    // it is compared against one float rounding of the LUT's own magnitude.
    float magnitude = 0.0f;
    for (std::size_t b = 0; b < gc::kGsClassicLutSize; ++b) {
      magnitude = std::max(magnitude, std::fabs(expected[b]));
    }
    const float cancellation = FLT_EPSILON * magnitude;
    for (std::size_t b = 0; b < gc::kGsClassicLutSize; ++b) {
      const float got = m.luts[i].v[b];
      const float want = expected[b];
      const bool same = got == want || std::fabs(got - want) <= cancellation ||
                        (log_points && (std::nextafter(want, INFINITY) == got ||
                                        std::nextafter(want, -INFINITY) == got));
      if (!same) {
        ++mismatches;
        UNSCOPED_INFO("spec " << i << " byte " << b << ": " << got << " != " << want);
      }
    }
  }
  CHECK(mismatches == 0);
}

}  // namespace

TEST_CASE("every GS EFX type resolves in both classic configurations", "[gs-classic-registry]") {
  const gc::GsClassicModelRegistry& def = gc::gs_classic_default_registry();
  const gc::GsClassicModelRegistry& raw = raw_registry();
  REQUIRE(def.valid());
  REQUIRE(raw.valid());
  std::size_t types = 0;
  for (const auto& entry : kGsEfxTypeDefaults) {
    if (entry.type == kThru) continue;
    ++types;
    INFO("type " << std::hex << entry.type);
    const gc::GsClassicType* d = def.find(entry.type);
    const gc::GsClassicType* r = raw.find(entry.type);
    REQUIRE(d != nullptr);
    REQUIRE(r != nullptr);
    CHECK(d->node_end > d->node_begin);
    CHECK(r->node_end > r->node_begin);
  }
  CHECK(types == 64);
  CHECK(def.models().n_types == 64);
  CHECK(raw.models().n_types == 64);
  CHECK(def.find(kThru) == nullptr);
  // Rotary Multi answers to both spellings.
  CHECK(def.find(0x0300) == def.find(0x020C));
}

TEST_CASE("the C++ map expander equals _from_map on every spec and byte", "[gs-classic-registry]") {
  SECTION("default configuration") {
    check_expansion(gc::gs_classic_default_registry(), kGsClassicMapExpansionDefault,
                    std::size(kGsClassicMapExpansionDefault));
  }
  SECTION("raw configuration") {
    check_expansion(raw_registry(), kGsClassicMapExpansionRaw,
                    std::size(kGsClassicMapExpansionRaw));
  }
}

TEST_CASE("Parallel-2 types carry an arrangement and no other type does", "[gs-classic-registry]") {
  for (const gc::GsClassicModelRegistry* registry :
       {&gc::gs_classic_default_registry(), &raw_registry()}) {
    const gc::GsClassicModelSet& m = registry->models();
    std::size_t parallel = 0;
    for (std::size_t i = 0; i < m.n_types; ++i) {
      const gc::GsClassicType& t = m.types[i];
      INFO("type " << std::hex << t.type);
      if ((t.type >> 8) == kParallelMsb) {
        ++parallel;
        CHECK((t.topology == gc::GsClassicTopology::kSideBySide ||
               t.topology == gc::GsClassicTopology::kInSeries));
      } else {
        CHECK(t.topology == gc::GsClassicTopology::kNotApplicable);
      }
    }
    CHECK(parallel == 9);
  }
  // Both configurations read the arrangement off the same p0 graphs.
  const gc::GsClassicModelSet& d = gc::gs_classic_default_registry().models();
  for (std::size_t i = 0; i < d.n_types; ++i) {
    const gc::GsClassicType* r = raw_registry().find(d.types[i].type);
    REQUIRE(r != nullptr);
    CHECK(r->topology == d.types[i].topology);
  }
}
