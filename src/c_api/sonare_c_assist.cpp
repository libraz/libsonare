#include <sonare/sonare_c_assist.h>

#include "c_api/project_internal.h"

#if defined(SONARE_WITH_ARRANGEMENT) && defined(SONARE_WITH_ASSIST)
#include "midi/assist/assist_registry.h"
#include "midi/assist/assist_request.h"
#include "midi/assist/composition_assist.h"
#include "midi/assist/modules/chord_tone_generator.h"
#include "midi/assist/modules/diatonic_harmonizer.h"
#include "midi/assist/modules/dissonance_analyzer.h"
#include "midi/assist/modules/harmony_context.h"
#include "midi/assist/modules/placement_judge.h"
#endif

// ============================================================================
// Composition assist
// ============================================================================

#if defined(SONARE_WITH_ARRANGEMENT) && defined(SONARE_WITH_ASSIST)
namespace {

namespace assist = sonare::midi::assist;
namespace mods = sonare::midi::assist::modules;
namespace json = sonare::util::json;

/// The built-in module set, held together so the query slots the generators
/// point at outlive them. Constructed per call: every module is stateless, so
/// there is nothing to carry between runs and nothing to lock.
struct BuiltinModules {
  mods::TimelineHarmonyContext harmony;
  mods::IntervalDissonanceAnalyzer dissonance{&harmony};
  mods::RangeScaleJudge judge{&harmony, &dissonance};
  mods::ChordToneGenerator generator{&harmony, &judge};
  mods::DiatonicHarmonizer harmonizer{};
};

const char* const kGeneratorId = mods::ChordToneGenerator::kModuleId;
const char* const kHarmonizerId = mods::DiatonicHarmonizer::kModuleId;

/// Reads the requested module ids. An unknown id is an error rather than a
/// silent skip: a caller that misspells a module would otherwise get an empty
/// result that looks exactly like "the module ran and found nothing".
bool select_modules(const json::Value& root, bool* out_generator, bool* out_harmonizer,
                    std::string* error) {
  const json::Value* modules = root.find("modules");
  if (modules == nullptr) {
    *out_generator = true;
    *out_harmonizer = true;
    return true;
  }
  if (!modules->is_array()) {
    *error = "modules must be an array of module ids";
    return false;
  }
  *out_generator = false;
  *out_harmonizer = false;
  for (const json::Value& entry : modules->as_array()) {
    if (!entry.is_string()) {
      *error = "modules must contain only strings";
      return false;
    }
    if (entry.as_string() == kGeneratorId) {
      *out_generator = true;
    } else if (entry.as_string() == kHarmonizerId) {
      *out_harmonizer = true;
    } else {
      *error = "unknown module id: " + entry.as_string();
      return false;
    }
  }
  return true;
}

bool read_optional_ppq(const json::Value& scope, const char* key, std::optional<double>* out,
                       std::string* error) {
  const json::Value* value = scope.find(key);
  if (value == nullptr) return true;
  if (!value->is_number()) {
    *error = std::string("scope.") + key + " must be a number";
    return false;
  }
  const double ppq = value->as_number();
  if (!sonare::transport::valid_public_ppq(ppq)) {
    *error = std::string("scope.") + key + " is outside the representable PPQ range";
    return false;
  }
  *out = ppq;
  return true;
}

bool read_u32(const json::Value& root, const char* key, uint32_t* out, std::string* error) {
  const json::Value* value = root.find(key);
  if (value == nullptr) return true;
  if (!value->is_number()) {
    *error = std::string(key) + " must be a number";
    return false;
  }
  const double number = value->as_number();
  if (!std::isfinite(number) || number < 0.0 || number > 4294967295.0 ||
      number != std::floor(number)) {
    *error = std::string(key) + " must be a non-negative 32-bit integer";
    return false;
  }
  *out = static_cast<uint32_t>(number);
  return true;
}

/// Builds the AssistRequest. `params` is handed through verbatim: the seam keeps
/// it opaque and the modules own their own reading of it, so re-encoding it here
/// would put a second parser in front of the one that matters.
bool read_request(const char* request_json, assist::AssistRequest* out, bool* out_generator,
                  bool* out_harmonizer, std::string* error) {
  json::Value root;
  if (request_json == nullptr || *request_json == '\0') {
    *error = "request_json must name at least a params object with target_clip_id";
    return false;
  }
  try {
    root = json::parse_strict(request_json);
  } catch (const json::JsonError& parse_error) {
    *error = std::string("request_json is not valid JSON: ") + parse_error.what();
    return false;
  }
  if (!root.is_object()) {
    *error = "request_json must be a JSON object";
    return false;
  }
  if (!select_modules(root, out_generator, out_harmonizer, error)) return false;
  if (!read_u32(root, "seed", &out->seed, error)) return false;

  if (const json::Value* scope = root.find("scope")) {
    if (!scope->is_object()) {
      *error = "scope must be an object";
      return false;
    }
    if (!read_optional_ppq(*scope, "startPpq", &out->scope.start_ppq, error)) return false;
    if (!read_optional_ppq(*scope, "endPpq", &out->scope.end_ppq, error)) return false;
    if (const json::Value* ids = scope->find("trackIds")) {
      if (!ids->is_array()) {
        *error = "scope.trackIds must be an array";
        return false;
      }
      for (const json::Value& entry : ids->as_array()) {
        if (!entry.is_number()) {
          *error = "scope.trackIds must contain only numbers";
          return false;
        }
        out->scope.track_ids.push_back(static_cast<sonare::arrangement::TrackId>(entry.as_int()));
      }
    }
  }
  if (const json::Value* budget = root.find("budget")) {
    if (!budget->is_object()) {
      *error = "budget must be an object";
      return false;
    }
    if (!read_u32(*budget, "maxTimeMs", &out->budget.max_time_ms, error)) return false;
    if (!read_u32(*budget, "maxIterations", &out->budget.max_iterations, error)) return false;
  }
  if (const json::Value* params = root.find("params")) {
    if (!params->is_object()) {
      *error = "params must be an object";
      return false;
    }
    out->params_json = json::dump(*params);
  }
  return true;
}

const char* status_text(assist::AssistStatus status) noexcept {
  switch (status) {
    case assist::AssistStatus::kOk:
      return "ok";
    case assist::AssistStatus::kEmpty:
      return "empty";
    case assist::AssistStatus::kBudgetTruncated:
      return "budgetTruncated";
    case assist::AssistStatus::kDiscarded:
      return "discarded";
    case assist::AssistStatus::kRejected:
      return "rejected";
  }
  return "empty";
}

/// Describes the proposed commands. A command this surface cannot render as a
/// patch is reported by type name rather than dropped: silence would read as
/// "the module proposed nothing".
void describe_commands(const assist::AssistResult& result, json::Object* document) {
  json::Array patches;
  json::Array unrendered;
  for (const auto& command : result.commands) {
    const auto* patch_command = dynamic_cast<const sonare::arrangement::PatchMidiClip*>(command.get());
    if (patch_command == nullptr) {
      unrendered.push_back(json::Value(std::string(command->type_name())));
      continue;
    }
    const sonare::arrangement::MidiClipPatch& patch = patch_command->patch();
    json::Array added;
    for (const sonare::arrangement::MidiClipEvent& event : patch.add) {
      json::Object pod;
      pod["ppq"] = json::Value(event.ppq);
      pod["data0"] = json::Value(static_cast<double>(event.data0));
      pod["data1"] = json::Value(static_cast<double>(event.data1));
      added.push_back(json::Value(std::move(pod)));
    }
    json::Object entry;
    entry["clipId"] = json::Value(static_cast<double>(patch.clip_id));
    entry["add"] = json::Value(std::move(added));
    patches.push_back(json::Value(std::move(entry)));
  }
  (*document)["patches"] = json::Value(std::move(patches));
  (*document)["unrenderedCommands"] = json::Value(std::move(unrendered));
}

std::string build_document(const assist::AssistResult& result) {
  json::Object document;
  document["status"] = json::Value(std::string(status_text(result.diagnostics.status)));
  document["reason"] = json::Value(result.diagnostics.reason);
  document["iterationsConsumed"] =
      json::Value(static_cast<double>(result.diagnostics.iterations_consumed));
  document["slotsDiscarded"] = json::Value(static_cast<double>(result.diagnostics.slots_discarded));
  describe_commands(result, &document);
  json::Array payloads;
  for (const std::string& payload : result.candidate_payloads) {
    payloads.push_back(json::Value(payload));
  }
  document["payloads"] = json::Value(std::move(payloads));
  return json::dump(json::Value(std::move(document)));
}

SonareError run_assist(SonareProject* project, const char* request_json, bool apply,
                       char** out_json) {
  if (project == nullptr || out_json == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  *out_json = nullptr;

  assist::AssistRequest request;
  bool want_generator = false;
  bool want_harmonizer = false;
  std::string error;
  if (!read_request(request_json, &request, &want_generator, &want_harmonizer, &error)) {
    sonare_c_detail::set_last_error(error.c_str());
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  BuiltinModules modules;
  assist::AssistRegistry registry;
  registry.register_harmony_context(&modules.harmony);
  registry.register_dissonance_analyzer(&modules.dissonance);
  registry.register_judge(&modules.judge);
  if (want_generator) registry.register_generator(&modules.generator);
  if (want_harmonizer) registry.register_counterpoint(&modules.harmonizer);

  const sonare::arrangement::ProjectView view(project->history.project(),
                                              project->history.midi_content(), "sonare.builtin");
  assist::AssistResult result = assist::CompositionAssist(registry).run(view, request);

  // Described BEFORE applying. Applying moves the commands out of the result, so
  // building the document afterwards reported an empty `patches` on every
  // successful apply -- the caller could see that something committed but not
  // what, which is the one thing the document is for.
  const std::string document = build_document(result);

  // A refused request is an ERROR return, not a document reporting a quiet
  // nothing. The reason goes to the detailed-error channel so a caller that only
  // checks the code still learns which field it got wrong.
  if (result.diagnostics.status == assist::AssistStatus::kRejected) {
    sonare_c_detail::set_last_error(result.diagnostics.reason.c_str());
    *out_json = copy_string(document);
    return *out_json == nullptr ? SONARE_ERROR_OUT_OF_MEMORY : SONARE_ERROR_INVALID_PARAMETER;
  }

  // Applied as ONE transaction, so an assist run is one undo step rather than a
  // pile of per-module ones a user has to walk back individually.
  if (apply && !result.commands.empty()) {
    if (!project->history.apply_transaction(std::move(result.commands))) {
      return SONARE_ERROR_INVALID_STATE;
    }
  }

  *out_json = copy_string(document);
  return *out_json == nullptr ? SONARE_ERROR_OUT_OF_MEMORY : SONARE_OK;
}

}  // namespace
#endif

const char* sonare_assist_module_ids(void) {
#if defined(SONARE_WITH_ARRANGEMENT) && defined(SONARE_WITH_ASSIST)
  static thread_local std::string ids;
  ids = std::string(kGeneratorId) + "\n" + kHarmonizerId;
  return ids.c_str();
#else
  return nullptr;
#endif
}

SonareError sonare_project_assist_preview_json(const SonareProject* project,
                                               const char* request_json, char** out_json) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT) && defined(SONARE_WITH_ASSIST)
  SONARE_C_TRY
  // const_cast is safe on this path: run_assist is called with apply = false and
  // never reaches the history's mutating side. The parameter stays const because
  // a preview that could take a mutable project would read as one that might use it.
  return run_assist(const_cast<SonareProject*>(project), request_json, false, out_json);
  SONARE_C_CATCH
#else
  if (out_json) *out_json = nullptr;
  SONARE_C_STUB_NOT_SUPPORTED(project, request_json, out_json);
#endif
}

SonareError sonare_project_assist_apply_json(SonareProject* project, const char* request_json,
                                             char** out_json) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT) && defined(SONARE_WITH_ASSIST)
  SONARE_C_TRY
  return run_assist(project, request_json, true, out_json);
  SONARE_C_CATCH
#else
  if (out_json) *out_json = nullptr;
  SONARE_C_STUB_NOT_SUPPORTED(project, request_json, out_json);
#endif
}
