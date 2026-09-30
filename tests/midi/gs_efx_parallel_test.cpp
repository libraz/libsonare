/// @file gs_efx_parallel_test.cpp
/// @brief The GS parallel-2 types run their two halves side by side, or in
///        series where the classic model says so, measured by zeroing one half's
///        level byte and listening to what is left.
///
/// The modern halves' level bytes reach the chain through binding rows. Until the
/// generated tables carry the parallel types, those rows are cloned here from the
/// generated output-level row (the same conversion the row files name) and handed
/// over the GsEfxRowView seam; the unit is then built and run by the player's own
/// sf2_build_efx_unit / sf2_run_efx_unit.

#include <catch2/catch_test_macros.hpp>

#if defined(SONARE_MIDI_WITH_FX) && defined(SONARE_WITH_MASTERING)

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ios>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mastering/api/insert_factory.h"
#include "midi/synth/gs_address_table.h"
#include "midi/synth/gs_classic/model_registry.h"
#include "midi/synth/gs_efx_bindings.h"
#include "midi/synth/gs_layer.h"
#include "midi/synth/sf2_player.h"

namespace {

namespace s = sonare::midi::synth;
namespace gc = sonare::midi::synth::gs_classic;

constexpr double kRate = 48000.0;
constexpr int kBlock = 256;
constexpr int kBlocks = 96;  // ~0.5 s: past the classic resamplers' delay and the effects' onsets
/// EFX PARAMETER 17 / 19 (40 03 13 / 40 03 15): the two halves' output levels.
constexpr uint8_t kLevelA = 16;
constexpr uint8_t kLevelB = 18;
/// A half's contribution is the output with its level at full minus the output
/// with it at the floor. Side by side, that difference does not depend on the
/// other half's level (superposition), to this fraction of the unedited output.
constexpr double kSuperposition = 1e-4;
/// Each half must contribute at least this fraction of the unedited output (-60 dB).
constexpr double kContributes = 1e-3;
/// "Silenced" for the classic in-series case: 60 dB under the unedited output.
constexpr double kSilent = 1e-3;

s::GsEfx efx_holding(uint16_t type) {
  s::GsEfx efx;
  efx.type = type;
  efx.type_msb = static_cast<uint8_t>(type >> 8);
  const auto* defaults = s::gs_efx_type_defaults(type);
  if (defaults != nullptr) efx.params = defaults->params;
  efx.assigned = true;
  return efx;
}

bool is_parallel(const std::vector<s::GsEfxStage>& chain) {
  for (const s::GsEfxStage& stage : chain) {
    if (stage.branch == s::kGsEfxBranchHalfA || stage.branch == s::kGsEfxBranchHalfB) return true;
  }
  return false;
}

/// Every type the skeleton realises as two halves.
std::vector<uint16_t> modern_parallel_types() {
  std::vector<uint16_t> out;
  for (uint16_t lsb = 0; lsb < 0x80; ++lsb) {
    const auto type = static_cast<uint16_t>(0x1100 | lsb);
    if (is_parallel(s::gs_efx_insert_chain(efx_holding(type)))) out.push_back(type);
  }
  return out;
}

/// The generated rows plus, per parallel type, the two half-level rows the row
/// files give it (utility.gain ordinal 0 and 1), cloned from the generated
/// output-level row so the conversion is the one the tables already carry.
std::vector<s::GsEfxBindingRow> rows_with_half_levels(const std::vector<uint16_t>& types) {
  std::vector<s::GsEfxBindingRow> rows(s::kGsEfxBindingRows.begin(), s::kGsEfxBindingRows.end());
  const s::GsEfxBindingRow* level = nullptr;
  for (const s::GsEfxBindingRow& row : s::kGsEfxBindingRows) {
    if (s::kGsEfxRowStages[row.stage] == "utility.gain" && s::kGsEfxRowKeys[row.key] == "levelDb") {
      level = &row;
      break;
    }
  }
  REQUIRE(level != nullptr);
  for (const uint16_t type : types) {
    for (const auto& [slot, ordinal] : {std::pair<uint8_t, uint8_t>{kLevelA, 0}, {kLevelB, 1}}) {
      s::GsEfxBindingRow row = *level;
      row.type = type;
      row.slot = slot;
      row.ordinal = ordinal;
      rows.push_back(row);
    }
  }
  return rows;
}

/// The second half of @p unit's response to a steady stereo tone, L and R interleaved.
std::vector<double> run_wave(const s::Sf2EfxUnitRt& unit) {
  constexpr double kTwoPi = 6.28318530717958647692;
  std::vector<float> l(kBlock);
  std::vector<float> r(kBlock);
  std::vector<double> out;
  for (int b = 0; b < kBlocks; ++b) {
    for (int i = 0; i < kBlock; ++i) {
      const double t = static_cast<double>(b * kBlock + i) / kRate;
      l[static_cast<size_t>(i)] = 0.25f * static_cast<float>(std::sin(kTwoPi * 220.0 * t));
      r[static_cast<size_t>(i)] = 0.25f * static_cast<float>(std::sin(kTwoPi * 330.0 * t));
    }
    s::sf2_run_efx_unit(unit, l.data(), r.data(), kBlock);
    if (b < kBlocks / 2) continue;
    for (int i = 0; i < kBlock; ++i) {
      out.push_back(l[static_cast<size_t>(i)]);
      out.push_back(r[static_cast<size_t>(i)]);
    }
  }
  return out;
}

double rms(const std::vector<double>& x) {
  double acc = 0.0;
  for (const double v : x) acc += v * v;
  return std::sqrt(acc / static_cast<double>(x.size()));
}

/// x - y.
std::vector<double> minus(const std::vector<double>& x, const std::vector<double>& y) {
  std::vector<double> out(x.size());
  for (size_t i = 0; i < x.size(); ++i) out[i] = x[i] - y[i];
  return out;
}

/// The four renders a parallel type is judged by: unedited, A floored, B floored, both.
struct Quad {
  std::vector<double> full, a_floored, b_floored, both;
};

/// Checks the side-by-side claim on @p q: each half contributes, and its
/// contribution is the same with the other half at full or at its floor.
void check_side_by_side(const Quad& q) {
  const double full = rms(q.full);
  const std::vector<double> a_with_b = minus(q.full, q.a_floored);
  const std::vector<double> a_without_b = minus(q.b_floored, q.both);
  const std::vector<double> b_with_a = minus(q.full, q.b_floored);
  const std::vector<double> b_without_a = minus(q.a_floored, q.both);
  INFO("rms full " << full << " A " << rms(a_with_b) << " B " << rms(b_with_a) << " A-drift "
                   << rms(minus(a_with_b, a_without_b)) << " B-drift "
                   << rms(minus(b_with_a, b_without_a)));
  CHECK(full > 1e-3);
  CHECK(rms(a_with_b) > kContributes * full);
  CHECK(rms(b_with_a) > kContributes * full);
  CHECK(rms(minus(a_with_b, a_without_b)) < kSuperposition * full);
  CHECK(rms(minus(b_with_a, b_without_a)) < kSuperposition * full);
}

const auto kFactory = [](std::string_view name, std::string_view json) {
  return sonare::mastering::api::make_insert(std::string(name), std::string(json));
};

}  // namespace

TEST_CASE("modern parallel-2: one half's level at zero leaves the other", "[gs-efx-parallel]") {
  const std::vector<uint16_t> types = modern_parallel_types();
  REQUIRE(types.size() == 9);
  const std::vector<s::GsEfxBindingRow> rows = rows_with_half_levels(types);
  const s::GsEfxRowView view{rows.data(), rows.size(), s::kGsEfxEnables.data(),
                             s::kGsEfxEnables.size()};

  for (const uint16_t type : types) {
    INFO("type " << std::hex << type);
    const auto render = [&](bool floor_a, bool floor_b) {
      s::GsEfx efx = efx_holding(type);
      if (floor_a) efx.params[kLevelA] = 0;
      if (floor_b) efx.params[kLevelB] = 0;
      const s::Sf2EfxUnitRt unit =
          s::sf2_build_efx_unit(efx, s::gs_efx_insert_chain(efx, view),
                                s::GsEfxRealization::kModern, kFactory, kRate, kBlock);
      for (const s::Sf2EfxStageRt& stage : unit.stages) {
        INFO("stage " << stage.name);
        REQUIRE(stage.proc != nullptr);
      }
      return run_wave(unit);
    };
    check_side_by_side(
        {render(false, false), render(true, false), render(false, true), render(true, true)});
  }
}

TEST_CASE("classic parallel-2 follows the model's topology", "[gs-efx-parallel]") {
  const gc::GsClassicModelRegistry& registry = gc::gs_classic_default_registry();
  REQUIRE(registry.valid());
  const gc::GsClassicModelSet& models = registry.models();
  size_t checked = 0;
  for (size_t t = 0; t < models.n_types; ++t) {
    const gc::GsClassicType& model = models.types[t];
    if (model.topology == gc::GsClassicTopology::kNotApplicable) continue;
    ++checked;
    INFO("type " << std::hex << model.type);
    REQUIRE(model.printed_lo[kLevelA] < model.printed_hi[kLevelA]);
    REQUIRE(model.printed_lo[kLevelB] < model.printed_hi[kLevelB]);
    const auto render = [&](bool floor_a, bool floor_b) {
      s::GsEfx efx = efx_holding(model.type);
      if (floor_a) efx.params[kLevelA] = model.printed_lo[kLevelA];
      if (floor_b) efx.params[kLevelB] = model.printed_lo[kLevelB];
      const s::Sf2EfxUnitRt unit =
          s::sf2_build_efx_unit(efx, {}, s::GsEfxRealization::kClassic, nullptr, kRate, kBlock);
      REQUIRE(unit.stages.size() == 1);
      REQUIRE(unit.stages[0].proc != nullptr);
      return run_wave(unit);
    };
    const Quad q{render(false, false), render(true, false), render(false, true),
                 render(true, true)};
    if (model.topology == gc::GsClassicTopology::kSideBySide) {
      check_side_by_side(q);
    } else {
      // In series, either level at its floor takes the whole output with it.
      const double full = rms(q.full);
      INFO("rms full " << full << " A floored " << rms(q.a_floored) << " B floored "
                       << rms(q.b_floored));
      CHECK(full > 1e-3);
      CHECK(rms(q.a_floored) < kSilent * full);
      CHECK(rms(q.b_floored) < kSilent * full);
    }
  }
  REQUIRE(checked == 9);
}

#else

TEST_CASE("GS EFX parallel routing", "[gs-efx-parallel]") {
  SKIP("needs the FX and mastering builds");
}

#endif  // SONARE_MIDI_WITH_FX && SONARE_WITH_MASTERING
