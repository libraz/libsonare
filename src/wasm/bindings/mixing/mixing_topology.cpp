/// @file mixing_topology.cpp
/// @brief Embind scene-based mixer facade: bus / VCA-group / scene serialization.

#ifdef __EMSCRIPTEN__

#include "mixing_wasm.h"

#if defined(SONARE_WITH_MIXING) && defined(SONARE_WITH_GRAPH)

// Adds a channel strip to the mixer topology. metering is the strip's optional
// scene-JSON "metering" object; its field names and defaults are the document's,
// so a strip added here and one declared in a scene describe the same thing.
// Marks the routing graph dirty; call compile (or process) to rebuild.
void MixerWasm::addStrip(std::string id, val metering) {
  // Refuse a wrong-typed bag the way the typed*Property readers below refuse a
  // wrong-typed field. Absent is the omitted default; an array is excluded by
  // name because `typeof []` is "object" and would otherwise read as a bag.
  if (!metering.isUndefined() && !metering.isNull() &&
      (metering.typeOf().as<std::string>() != "object" ||
       val::global("Array").call<bool>("isArray", metering))) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "addStrip: metering must be a plain object");
  }
  const bool enabled = typedBoolProperty(metering, "enabled", true);
  const bool lufs = typedBoolProperty(metering, "lufs", true);
  const bool true_peak = typedBoolProperty(metering, "truePeak", true);
  // 0 selects the library default (4x), the sentinel the C ABI documents.
  const int true_peak_oversample = typedIntProperty(metering, "truePeakOversample", 0);
  // Returns the strip pointer rather than a SonareError, so NULL is the whole
  // failure signal and the detail is in the thread-local error slot. The pointer
  // is mixer-owned and never reaches JS: strips are addressed by index or id.
  if (sonare_mixer_add_strip_ex(mixer_, id.c_str(), enabled ? 1 : 0, lufs ? 1 : 0,
                                true_peak ? 1 : 0, true_peak_oversample) == nullptr) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidState,
        std::string("failed to add strip: ") + sonare_last_error_message());
  }
}

// Adds a bus to the mixer topology. role is one of "master", "aux", "submix"
// (empty defaults to "aux"). Marks the routing graph dirty; call compile (or
// process) to rebuild.
void MixerWasm::addBus(std::string id, std::string role) {
  SonareError err = sonare_mixer_add_bus(mixer_, id.c_str(), role.empty() ? nullptr : role.c_str());
  if (err != SONARE_OK) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                  std::string("failed to add bus: ") + sonare_error_message(err));
  }
}

void MixerWasm::removeBus(std::string id) {
  SonareError err = sonare_mixer_remove_bus(mixer_, id.c_str());
  if (err != SONARE_OK) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidState,
        std::string("failed to remove bus: ") + sonare_error_message(err));
  }
}

size_t MixerWasm::busCount() const {
  size_t count = 0;
  SonareError err = sonare_mixer_bus_count(mixer_, &count);
  if (err != SONARE_OK) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidState,
        std::string("failed to read bus count: ") + sonare_error_message(err));
  }
  return count;
}

// Adds a VCA group with the given gain offset. members is an array of strip-id
// strings (may be empty).
void MixerWasm::addVcaGroup(std::string id, const val& gain_db_val, val members) {
  const float gain_db = checkedFloatFromVal(gain_db_val, "gainDb");
  std::vector<std::string> member_storage;
  std::vector<const char*> member_ptrs;
  if (!members.isUndefined() && !members.isNull()) {
    const int count = static_cast<int>(wasmArrayLikeLength(members, "members"));
    member_storage.reserve(static_cast<size_t>(count));
    member_ptrs.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
      member_storage.push_back(members[i].as<std::string>());
    }
    for (const auto& member : member_storage) {
      member_ptrs.push_back(member.c_str());
    }
  }
  SonareError err = sonare_mixer_add_vca_group(mixer_, id.c_str(), gain_db,
                                               member_ptrs.empty() ? nullptr : member_ptrs.data(),
                                               member_ptrs.size());
  if (err != SONARE_OK) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidState,
        std::string("failed to add VCA group: ") + sonare_error_message(err));
  }
}

void MixerWasm::removeVcaGroup(std::string id) {
  SonareError err = sonare_mixer_remove_vca_group(mixer_, id.c_str());
  if (err != SONARE_OK) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidState,
        std::string("failed to remove VCA group: ") + sonare_error_message(err));
  }
}

void MixerWasm::setVcaGroupGainDb(std::string id, const val& gain_db_val) {
  const float gain_db = checkedFloatFromVal(gain_db_val, "gainDb");
  SonareError err = sonare_mixer_set_vca_group_gain_db(mixer_, id.c_str(), gain_db);
  if (err != SONARE_OK) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidState,
        std::string("failed to set VCA group gain: ") + sonare_error_message(err));
  }
}

void MixerWasm::setVcaGroupMembers(std::string id, val members) {
  std::vector<std::string> member_storage;
  std::vector<const char*> member_ptrs;
  const int count = static_cast<int>(wasmArrayLikeLength(members, "members"));
  member_storage.reserve(static_cast<size_t>(count));
  member_ptrs.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) member_storage.push_back(members[i].as<std::string>());
  for (const auto& member : member_storage) member_ptrs.push_back(member.c_str());
  const SonareError err = sonare_mixer_set_vca_group_members(
      mixer_, id.c_str(), member_ptrs.empty() ? nullptr : member_ptrs.data(), member_ptrs.size());
  if (err != SONARE_OK) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidState,
        std::string("failed to set VCA group members: ") + sonare_error_message(err));
  }
}

size_t MixerWasm::vcaGroupCount() const {
  size_t count = 0;
  SonareError err = sonare_mixer_vca_group_count(mixer_, &count);
  if (err != SONARE_OK) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidState,
        std::string("failed to read VCA group count: ") + sonare_error_message(err));
  }
  return count;
}

std::string MixerWasm::toSceneJson() const {
  char* json = nullptr;
  SonareError err = sonare_mixer_to_scene_json(mixer_, &json);
  if (err != SONARE_OK || json == nullptr) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidState,
        std::string("failed to serialize mixer scene: ") + sonare_error_message(err));
  }
  std::string out(json);
  sonare_free_string(json);
  return out;
}

void registerMixerTopology(class_<MixerWasm>& cls) {
  cls.function("addStrip", &MixerWasm::addStrip)
      .function("addBus", &MixerWasm::addBus)
      .function("removeBus", &MixerWasm::removeBus)
      .function("busCount", &MixerWasm::busCount)
      .function("addVcaGroup", &MixerWasm::addVcaGroup)
      .function("setVcaGroupGainDb", &MixerWasm::setVcaGroupGainDb)
      .function("setVcaGroupMembers", &MixerWasm::setVcaGroupMembers)
      .function("removeVcaGroup", &MixerWasm::removeVcaGroup)
      .function("vcaGroupCount", &MixerWasm::vcaGroupCount)
      .function("toSceneJson", &MixerWasm::toSceneJson);
}

#endif  // SONARE_WITH_MIXING && SONARE_WITH_GRAPH

#endif  // __EMSCRIPTEN__
