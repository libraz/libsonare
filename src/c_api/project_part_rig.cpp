#include "c_api/project_internal.h"

#if defined(SONARE_WITH_ARRANGEMENT)

#if defined(SONARE_WITH_MASTERING)
#include "mastering/api/insert_factory.h"
#endif
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

// Parses the inserts array into chain stages. Shape only; names are resolved by
// validate_chain_stages. A document that does not parse or has the wrong shape
// is INVALID_FORMAT, as at every other C-ABI JSON entry point.
SonareError parse_inserts(const char* inserts_json, std::vector<midi::PartRigStage>* stages) {
  json::Value root;
  try {
    root = json::admit_strict(inserts_json);
  } catch (const json::JsonError& ex) {
    set_last_error(ex.what());
    return SONARE_ERROR_INVALID_FORMAT;
  }
  const auto bad_shape = [] {
    set_last_error("inserts_json must be an array of {\"processor\": string, \"params\": string}");
    return SONARE_ERROR_INVALID_FORMAT;
  };
  if (!root.is_array()) return bad_shape();
  for (const json::Value& item : root.as_array()) {
    if (!item.is_object()) return bad_shape();
    const json::Value* processor = item.find("processor");
    if (processor == nullptr || !processor->is_string()) return bad_shape();
    midi::PartRigStage stage;
    stage.processor = processor->as_string();
    if (const json::Value* params = item.find("params")) {
      if (!params->is_string()) return bad_shape();
      stage.params_json = params->as_string();
    } else {
      stage.params_json = "{}";
    }
    stages->push_back(std::move(stage));
  }
  return SONARE_OK;
}

#if defined(SONARE_WITH_MASTERING)
// Every stage must build, accept its params, and stay within the latency bound
// at the reference rate.
bool validate_chain_stages(const std::vector<midi::PartRigStage>& stages) {
  constexpr double kValidationSampleRate = 48000.0;
  constexpr int kValidationBlockSize = 512;
  for (const midi::PartRigStage& stage : stages) {
    try {
      auto insert = sonare::mastering::api::make_insert(stage.processor, stage.params_json);
      if (!insert) return false;
      insert->prepare(kValidationSampleRate, kValidationBlockSize);
      if (insert->latency_samples() > midi::kMaxPartRigLatencySamples) return false;
    } catch (const sonare::SonareException&) {
      return false;
    }
  }
  return true;
}
#endif

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
    const SonareError parsed = parse_inserts(inserts_json, &rig.stages);
    if (parsed != SONARE_OK) return parsed;
  }
  if (!midi::validate_part_rig(part, rig)) return SONARE_ERROR_INVALID_PARAMETER;
#if defined(SONARE_WITH_MASTERING)
  if (!validate_chain_stages(rig.stages)) return SONARE_ERROR_INVALID_PARAMETER;
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
