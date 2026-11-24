#include "c_api/project_internal.h"

#if defined(SONARE_WITH_ARRANGEMENT)

#include "c_api/part_rig_json.h"
#if defined(SONARE_WITH_MASTERING)
#include "mastering/api/insert_factory.h"
#endif
#include "midi/part_rig.h"
#include "util/json_budget.h"
#include "util/parameter_checks.h"

namespace midi = sonare::midi;

static_assert(static_cast<int>(midi::PartRigMode::kBank) == SONARE_PART_RIG_BANK,
              "SonarePartRigMode bank ordinal drift");
static_assert(static_cast<int>(midi::PartRigMode::kNone) == SONARE_PART_RIG_NONE,
              "SonarePartRigMode none ordinal drift");
static_assert(static_cast<int>(midi::PartRigMode::kChain) == SONARE_PART_RIG_CHAIN,
              "SonarePartRigMode chain ordinal drift");
static_assert(midi::kPartRigAllParts == SONARE_PART_RIG_ALL_PARTS,
              "SONARE_PART_RIG_ALL_PARTS drift");

namespace {

/// Refuses a part that is neither a GS part slot nor the destination default.
void check_part(uint8_t part) {
  if (part >= 16 && part != SONARE_PART_RIG_ALL_PARTS) {
    sonare::util::check::refuse("part", "must be an integer in [0, 15] or 255");
  }
}

std::string inserts_to_json(const std::vector<midi::PartRigStage>& stages) {
  json::Array inserts;
  for (const midi::PartRigStage& stage : stages) {
    json::Object o;
    o["processor"] = stage.processor;
    o["params"] = json::parse(stage.params_json);
    inserts.emplace_back(std::move(o));
  }
  return json::dump(json::Value(std::move(inserts)));
}

}  // namespace

#endif  // SONARE_WITH_ARRANGEMENT

SonareError sonare_project_set_part_rig(SonareProject* project, uint32_t destination_id,
                                        uint8_t part, int mode, const char* inserts_json) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  midi::PartRig rig;
  SONARE_C_REQUIRE_NONNULL(project);
  if (const SonareError err = sonare_c_detail::run_validation([&] {
        check_part(part);
        sonare::util::check::integer_in_range(mode, SONARE_PART_RIG_BANK, SONARE_PART_RIG_CHAIN,
                                              "mode");
        rig.mode = static_cast<midi::PartRigMode>(mode);
        const bool chain = rig.mode == midi::PartRigMode::kChain;
        if (chain && inserts_json == nullptr) {
          sonare::util::check::refuse_relation({"inserts_json", "mode"},
                                               sonare::util::refusal::kPresentWhenChainRelation);
        }
        if (!chain && inserts_json != nullptr) {
          sonare::util::check::refuse_relation({"inserts_json", "mode"},
                                               sonare::util::refusal::kAbsentUnlessChainRelation);
        }
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_TRY
  if (inserts_json != nullptr) rig.stages = sonare_c_detail::read_part_rig_inserts(inserts_json);
  // The part was admitted above, so what is left to refuse is the content of inserts_json.
  if (!midi::validate_part_rig(part, rig)) sonare_c_detail::refuse_part_rig_stage_count();
#if defined(SONARE_WITH_MASTERING)
  sonare::mastering::api::validate_part_rig_chain(rig.stages);
#else
  if (rig.mode == midi::PartRigMode::kChain) {
    return sonare_c_detail::refuse_value_not_compiled_in(sonare_c_detail::Feature::kMastering,
                                                         "mode");
  }
#endif
  auto command = std::make_unique<arr::SetPartRig>(destination_id, part, std::move(rig));
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 destination_id, part, mode, inserts_json);
#endif
}

SonareError sonare_project_get_part_rig(const SonareProject* project, uint32_t destination_id,
                                        uint8_t part, int* out_mode, char** out_inserts_json,
                                        int* out_present) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_mode) *out_mode = SONARE_PART_RIG_BANK;
  if (out_inserts_json) *out_inserts_json = nullptr;
  if (out_present) *out_present = 0;
  SONARE_C_REQUIRE_NONNULL(project);
  if (const SonareError err = sonare_c_detail::run_validation([&] { check_part(part); });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out_mode);
  SONARE_C_REQUIRE_NONNULL(out_present);
  SONARE_C_TRY
  const arr::ProjectPartRig* entry = project->history.project().find_part_rig(destination_id, part);
  if (entry == nullptr) return SONARE_OK;
  if (out_inserts_json && entry->rig.mode == midi::PartRigMode::kChain) {
    *out_inserts_json = copy_string(inserts_to_json(entry->rig.stages));
  }
  *out_mode = static_cast<int>(entry->rig.mode);
  *out_present = 1;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  if (out_mode) *out_mode = {};
  if (out_inserts_json) *out_inserts_json = {};
  if (out_present) *out_present = {};
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 destination_id, part, out_mode, out_inserts_json,
                                                 out_present);
#endif
}

SonareError sonare_project_clear_part_rig(SonareProject* project, uint32_t destination_id,
                                          uint8_t part) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(project);
  if (const SonareError err = sonare_c_detail::run_validation([&] { check_part(part); });
      err != SONARE_OK) {
    return err;
  }
  if (project->history.project().find_part_rig(destination_id, part) == nullptr) {
    return SONARE_OK;
  }
  SONARE_C_TRY
  auto command = std::make_unique<arr::ClearPartRig>(destination_id, part);
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 destination_id, part);
#endif
}
