#include "c_api/project_internal.h"

#if defined(SONARE_WITH_ARRANGEMENT)

#include "c_api/part_rig_json.h"
#include "midi/part_rig.h"
#include "util/json_budget.h"

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

std::string inserts_to_json(const std::vector<midi::PartRigStage>& stages) {
  json::Array inserts;
  for (const midi::PartRigStage& stage : stages) {
    json::Object o;
    o["processor"] = stage.processor;
    o["params"] = stage.params_json;
    inserts.push_back(std::move(o));
  }
  return json::dump(json::Value(std::move(inserts)));
}

}  // namespace

#endif  // SONARE_WITH_ARRANGEMENT

SonareError sonare_project_set_part_rig(SonareProject* project, uint32_t destination_id,
                                        uint8_t part, int mode, const char* inserts_json) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || mode < SONARE_PART_RIG_BANK || mode > SONARE_PART_RIG_CHAIN) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  midi::PartRig rig;
  rig.mode = static_cast<midi::PartRigMode>(mode);
  if ((rig.mode == midi::PartRigMode::kChain) != (inserts_json != nullptr)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  if (inserts_json != nullptr) {
    const SonareError parsed = sonare_c_detail::parse_part_rig_inserts(inserts_json, &rig.stages);
    if (parsed != SONARE_OK) return parsed;
  }
  if (!midi::validate_part_rig(part, rig)) return SONARE_ERROR_INVALID_PARAMETER;
#if defined(SONARE_WITH_MASTERING)
  if (!sonare_c_detail::validate_part_rig_chain(rig.stages)) return SONARE_ERROR_INVALID_PARAMETER;
#else
  if (rig.mode == midi::PartRigMode::kChain) return SONARE_ERROR_NOT_SUPPORTED;
#endif
  auto command = std::make_unique<arr::SetPartRig>(destination_id, part, std::move(rig));
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, destination_id, part, mode, inserts_json);
#endif
}

SonareError sonare_project_get_part_rig(const SonareProject* project, uint32_t destination_id,
                                        uint8_t part, int* out_mode, char** out_inserts_json,
                                        int* out_present) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_inserts_json) *out_inserts_json = nullptr;
  if (out_present) *out_present = 0;
  if (!project || !out_mode || !out_present) return SONARE_ERROR_INVALID_PARAMETER;
  if (part >= 16 && part != SONARE_PART_RIG_ALL_PARTS) return SONARE_ERROR_INVALID_PARAMETER;
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
  if (out_inserts_json) *out_inserts_json = {};
  if (out_present) *out_present = {};
  SONARE_C_STUB_NOT_SUPPORTED(project, destination_id, part, out_mode, out_inserts_json,
                              out_present);
#endif
}

SonareError sonare_project_clear_part_rig(SonareProject* project, uint32_t destination_id,
                                          uint8_t part) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project) return SONARE_ERROR_INVALID_PARAMETER;
  if (part >= 16 && part != SONARE_PART_RIG_ALL_PARTS) return SONARE_ERROR_INVALID_PARAMETER;
  if (project->history.project().find_part_rig(destination_id, part) == nullptr) {
    return SONARE_OK;
  }
  SONARE_C_TRY
  auto command = std::make_unique<arr::ClearPartRig>(destination_id, part);
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, destination_id, part);
#endif
}
