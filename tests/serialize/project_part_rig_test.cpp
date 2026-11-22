/// @file project_part_rig_test.cpp
/// @brief Per-part rig persistence: model, edit history, project JSON, C ABI.

#include <sonare/sonare_c.h>

#include <catch2/catch_test_macros.hpp>
#include <string>

#include "arrangement/edit_command.h"
#include "arrangement/edit_history.h"
#include "arrangement/edit_model.h"
#include "arrangement/retained_bytes.h"
#include "midi/part_rig.h"
#include "serialize/project_serializer.h"
#include "util/json.h"

namespace {

namespace json = sonare::util::json;
using sonare::midi::PartRig;
using sonare::midi::PartRigMode;
using sonare::midi::PartRigStage;

constexpr const char* kChainJson =
    R"([{"processor":"saturation.softClipper","params":{}},)"
    R"({"processor":"saturation.softClipper","params":{"drive":0.5}}])";

struct ProjectHandle {
  SonareProject* p = nullptr;
  ProjectHandle() { REQUIRE(sonare_project_create(&p) == SONARE_OK); }
  ~ProjectHandle() { sonare_project_destroy(p); }
  ProjectHandle(const ProjectHandle&) = delete;
  ProjectHandle& operator=(const ProjectHandle&) = delete;
};

std::string serialize(const SonareProject* project) {
  char* out = nullptr;
  REQUIRE(sonare_project_serialize(project, &out, nullptr) == SONARE_OK);
  std::string s(out);
  sonare_free_string(out);
  return s;
}

double version_of(const std::string& text) {
  return json::parse(text).find("version")->as_number();
}

}  // namespace

TEST_CASE("empty project is written at the current schema version", "[serialize][part_rig]") {
  ProjectHandle h;
  const std::string text = serialize(h.p);
  CHECK(version_of(text) == sonare::serialize::SONARE_PROJECT_SCHEMA_VERSION);
  CHECK(json::parse(text).find("partRigs") == nullptr);
}

TEST_CASE("part rigs round-trip", "[serialize][part_rig]") {
  ProjectHandle h;
  REQUIRE(sonare_project_set_part_rig(h.p, 5, 2, SONARE_PART_RIG_CHAIN, kChainJson) == SONARE_OK);
  REQUIRE(sonare_project_set_part_rig(h.p, 5, SONARE_PART_RIG_ALL_PARTS, SONARE_PART_RIG_NONE,
                                      nullptr) == SONARE_OK);
  REQUIRE(sonare_project_set_part_rig(h.p, 1, 0, SONARE_PART_RIG_BANK, nullptr) == SONARE_OK);

  const std::string text = serialize(h.p);
  CHECK(version_of(text) == sonare::serialize::SONARE_PROJECT_SCHEMA_VERSION);

  const json::Value root = json::parse(text);
  const json::Value* rigs = root.find("partRigs");
  REQUIRE(rigs != nullptr);
  REQUIRE(rigs->as_array().size() == 3);
  // Canonical order: (destination_id, part).
  const json::Value& first = rigs->as_array()[0];
  CHECK(first.find("destinationId")->as_number() == 1.0);
  CHECK(first.find("part")->as_number() == 0.0);
  CHECK(first.find("mode")->as_string() == "bank");
  CHECK(first.find("inserts") == nullptr);
  const json::Value& chain = rigs->as_array()[1];
  CHECK(chain.find("part")->as_number() == 2.0);
  CHECK(chain.find("mode")->as_string() == "chain");
  REQUIRE(chain.find("inserts")->as_array().size() == 2);
  CHECK(chain.find("inserts")->as_array()[1].find("processor")->as_string() ==
        "saturation.softClipper");
  const json::Value* drive_params = chain.find("inserts")->as_array()[1].find("params");
  REQUIRE(drive_params != nullptr);
  REQUIRE(drive_params->is_object());
  CHECK(drive_params->find("drive")->as_number() == 0.5);
  const json::Value& dest_default = rigs->as_array()[2];
  CHECK(dest_default.find("part")->as_number() == 255.0);
  CHECK(dest_default.find("mode")->as_string() == "none");

  SonareProject* loaded = nullptr;
  REQUIRE(sonare_project_deserialize(text.data(), text.size(), &loaded, nullptr) == SONARE_OK);
  CHECK(serialize(loaded) == text);
  sonare_project_destroy(loaded);
}

TEST_CASE("part rig getter reports presence, mode and inserts", "[serialize][part_rig]") {
  ProjectHandle h;
  int mode = -1;
  char* inserts = nullptr;
  int present = -1;
  REQUIRE(sonare_project_get_part_rig(h.p, 7, 3, &mode, &inserts, &present) == SONARE_OK);
  CHECK(present == 0);
  CHECK(inserts == nullptr);

  REQUIRE(sonare_project_set_part_rig(h.p, 7, 3, SONARE_PART_RIG_CHAIN, kChainJson) == SONARE_OK);
  REQUIRE(sonare_project_get_part_rig(h.p, 7, 3, &mode, &inserts, &present) == SONARE_OK);
  CHECK(present == 1);
  CHECK(mode == SONARE_PART_RIG_CHAIN);
  REQUIRE(inserts != nullptr);
  const json::Value arr = json::parse(inserts);
  sonare_free_string(inserts);
  REQUIRE(arr.as_array().size() == 2);
  CHECK(arr.as_array()[0].find("processor")->as_string() == "saturation.softClipper");

  // A non-chain entry reports no inserts.
  REQUIRE(sonare_project_set_part_rig(h.p, 7, 3, SONARE_PART_RIG_NONE, nullptr) == SONARE_OK);
  inserts = nullptr;
  REQUIRE(sonare_project_get_part_rig(h.p, 7, 3, &mode, &inserts, &present) == SONARE_OK);
  CHECK(present == 1);
  CHECK(mode == SONARE_PART_RIG_NONE);
  CHECK(inserts == nullptr);

  CHECK(sonare_project_get_part_rig(h.p, 7, 3, nullptr, &inserts, &present) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_project_get_part_rig(h.p, 7, 3, &mode, &inserts, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_project_get_part_rig(h.p, 7, 16, &mode, &inserts, &present) ==
        SONARE_ERROR_INVALID_PARAMETER);
}

TEST_CASE("part rig set refuses malformed input", "[serialize][part_rig]") {
  ProjectHandle h;
  const std::string before = serialize(h.p);
  const SonareError bad = SONARE_ERROR_INVALID_PARAMETER;

  CHECK(sonare_project_set_part_rig(nullptr, 1, 0, SONARE_PART_RIG_NONE, nullptr) == bad);
  CHECK(sonare_project_set_part_rig(h.p, 1, 16, SONARE_PART_RIG_NONE, nullptr) == bad);
  CHECK(sonare_project_set_part_rig(h.p, 1, 254, SONARE_PART_RIG_NONE, nullptr) == bad);
  CHECK(sonare_project_set_part_rig(h.p, 1, 0, 3, nullptr) == bad);
  CHECK(sonare_project_set_part_rig(h.p, 1, 0, -1, nullptr) == bad);
  // Inserts belong to chain only, and chain needs at least one.
  CHECK(sonare_project_set_part_rig(h.p, 1, 0, SONARE_PART_RIG_NONE, kChainJson) == bad);
  CHECK(sonare_project_set_part_rig(h.p, 1, 0, SONARE_PART_RIG_BANK, kChainJson) == bad);
  CHECK(sonare_project_set_part_rig(h.p, 1, 0, SONARE_PART_RIG_CHAIN, nullptr) == bad);
  CHECK(sonare_project_set_part_rig(h.p, 1, 0, SONARE_PART_RIG_CHAIN, "[]") == bad);
  // Malformed inserts.
  CHECK(sonare_project_set_part_rig(h.p, 1, 0, SONARE_PART_RIG_CHAIN, "not json") == bad);
  CHECK(sonare_project_set_part_rig(h.p, 1, 0, SONARE_PART_RIG_CHAIN, "{}") == bad);
  CHECK(sonare_project_set_part_rig(h.p, 1, 0, SONARE_PART_RIG_CHAIN, R"([{"params":{}}])") == bad);
  CHECK(sonare_project_set_part_rig(
            h.p, 1, 0, SONARE_PART_RIG_CHAIN,
            R"([{"processor":"saturation.softClipper","params":"{\"drive\":0.5}"}])") == bad);
  CHECK(sonare_project_set_part_rig(h.p, 1, 0, SONARE_PART_RIG_CHAIN, "[1]") == bad);
  CHECK(sonare_project_set_part_rig(h.p, 1, 0, SONARE_PART_RIG_CHAIN, R"([{"processor":""}])") ==
        bad);

  // More stages than a chain may hold.
  std::string nine = "[";
  for (int i = 0; i < 9; ++i) {
    if (i > 0) nine += ",";
    nine += R"({"processor":"saturation.softClipper","params":{}})";
  }
  nine += "]";
  CHECK(sonare_project_set_part_rig(h.p, 1, 0, SONARE_PART_RIG_CHAIN, nine.c_str()) == bad);

#if defined(SONARE_WITH_MASTERING)
  CHECK(sonare_project_set_part_rig(h.p, 1, 0, SONARE_PART_RIG_CHAIN,
                                    R"([{"processor":"no.such.processor","params":{}}])") == bad);
  CHECK(sonare_project_set_part_rig(h.p, 1, 0, SONARE_PART_RIG_CHAIN,
                                    R"([{"processor":"saturation.softClipper","params":"{"}])") ==
        bad);
#endif

  CHECK(serialize(h.p) == before);
}

TEST_CASE("part rig edits undo and redo", "[serialize][part_rig]") {
  ProjectHandle h;
  const std::string empty = serialize(h.p);

  REQUIRE(sonare_project_set_part_rig(h.p, 5, 0, SONARE_PART_RIG_NONE, nullptr) == SONARE_OK);
  const std::string one = serialize(h.p);
  REQUIRE(sonare_project_set_part_rig(h.p, 5, 0, SONARE_PART_RIG_CHAIN, kChainJson) == SONARE_OK);
  const std::string replaced = serialize(h.p);
  REQUIRE(sonare_project_clear_part_rig(h.p, 5, 0) == SONARE_OK);
  CHECK(serialize(h.p) == empty);

  REQUIRE(sonare_project_undo(h.p) == SONARE_OK);
  CHECK(serialize(h.p) == replaced);
  REQUIRE(sonare_project_undo(h.p) == SONARE_OK);
  CHECK(serialize(h.p) == one);
  REQUIRE(sonare_project_undo(h.p) == SONARE_OK);
  CHECK(serialize(h.p) == empty);
  CHECK(version_of(serialize(h.p)) == sonare::serialize::SONARE_PROJECT_SCHEMA_VERSION);

  REQUIRE(sonare_project_redo(h.p) == SONARE_OK);
  CHECK(serialize(h.p) == one);
  REQUIRE(sonare_project_redo(h.p) == SONARE_OK);
  CHECK(serialize(h.p) == replaced);
  REQUIRE(sonare_project_redo(h.p) == SONARE_OK);
  CHECK(serialize(h.p) == empty);
}

TEST_CASE("clearing an absent part rig is a no-op", "[serialize][part_rig]") {
  ProjectHandle h;
  CHECK(sonare_project_clear_part_rig(h.p, 9, 0) == SONARE_OK);
  CHECK(sonare_project_clear_part_rig(h.p, 9, 16) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_project_clear_part_rig(nullptr, 9, 0) == SONARE_ERROR_INVALID_PARAMETER);
  // Nothing was recorded for undo.
  CHECK(sonare_project_undo(h.p) != SONARE_OK);
}

TEST_CASE("part rig entries survive without a referencing track", "[serialize][part_rig]") {
  ProjectHandle h;
  REQUIRE(sonare_project_set_part_rig(h.p, 4242, 0, SONARE_PART_RIG_NONE, nullptr) == SONARE_OK);
  const std::string text = serialize(h.p);
  SonareProject* loaded = nullptr;
  REQUIRE(sonare_project_deserialize(text.data(), text.size(), &loaded, nullptr) == SONARE_OK);
  int mode = -1;
  int present = 0;
  REQUIRE(sonare_project_get_part_rig(loaded, 4242, 0, &mode, nullptr, &present) == SONARE_OK);
  CHECK(present == 1);
  CHECK(mode == SONARE_PART_RIG_NONE);
  sonare_project_destroy(loaded);
}

TEST_CASE("part rig decode honours the schema version and shape", "[serialize][part_rig]") {
  using sonare::serialize::project_from_json;
  const char* body =
      R"("partRigs":[{"destinationId":3,"part":1,"mode":"none"},)"
      R"({"destinationId":3,"part":16,"mode":"none"},)"
      R"({"destinationId":3,"part":2,"mode":"bogus"},)"
      R"({"destinationId":3,"part":4,"mode":"chain"},)"
      R"({"destinationId":3,"part":5,"mode":"bank","inserts":[{"processor":"a","params":{}}]},)"
      R"({"destinationId":3,"part":6,"mode":"chain",)"
      R"("inserts":[{"processor":"x.y","params":{}}]}]})";

  const auto v5 = project_from_json(std::string(R"({"version":7,)") + body);
  REQUIRE(v5.ok());
  // Only the well-formed entries load; the rest are dropped with warnings.
  REQUIRE(v5.project->part_rigs().size() == 2);
  CHECK(v5.project->part_rigs()[0].part == 1);
  CHECK(v5.project->part_rigs()[0].rig.mode == PartRigMode::kNone);
  CHECK(v5.project->part_rigs()[1].part == 6);
  CHECK(v5.project->part_rigs()[1].rig.stages[0].processor == "x.y");
  CHECK_FALSE(v5.diagnostics.empty());

  // An earlier or later document is refused, never read.
  for (const char* version : {"4", "5", "6", "8"}) {
    const auto other = project_from_json(std::string(R"({"version":)") + version + "," + body);
    CHECK_FALSE(other.ok());
    CHECK(other.diagnostics.front().code == "unsupported_schema_version");
  }
}

TEST_CASE("part rig decode bounds malformed-entry diagnostics", "[serialize][part_rig]") {
  using sonare::serialize::project_from_json;

  std::string document = R"({"version":7,"partRigs":[)";
  for (int i = 0; i < 130; ++i) {
    if (i > 0) document += ",";
    document += R"({"destinationId":1,"part":16,"mode":"none"})";
  }
  document += "]}";

  const auto result = project_from_json(document);
  REQUIRE(result.ok());
  REQUIRE(result.project->part_rigs().empty());

  std::size_t invalid_part_rig_count = 0;
  std::size_t truncated_count = 0;
  std::string truncated_message;
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == "invalid_part_rig") {
      ++invalid_part_rig_count;
    } else if (diagnostic.code == "decode_diagnostics_truncated") {
      ++truncated_count;
      truncated_message = diagnostic.message;
    }
  }
  CHECK(invalid_part_rig_count == 128);
  CHECK(truncated_count == 1);
  CHECK(result.diagnostics.size() == 129);
  CHECK(truncated_message == "2 additional decode diagnostics were suppressed");
}

TEST_CASE("Project part rig model keeps one entry per key in canonical order",
          "[serialize][part_rig][arrangement]") {
  namespace arr = sonare::arrangement;
  arr::Project project;
  PartRig none;
  none.mode = PartRigMode::kNone;
  CHECK(project.set_part_rig({9, 3, none}));
  CHECK(project.set_part_rig({2, 0xFF, none}));
  CHECK(project.set_part_rig({2, 1, none}));
  PartRig chain;
  chain.mode = PartRigMode::kChain;
  chain.stages.push_back({"saturation.softClipper", "{}"});
  CHECK(project.set_part_rig({2, 1, chain}));
  REQUIRE(project.part_rigs().size() == 3);
  CHECK(project.part_rigs()[0].part == 1);
  CHECK(project.part_rigs()[1].part == 0xFF);
  CHECK(project.part_rigs()[2].destination_id == 9);
  CHECK(project.find_part_rig(2, 1)->rig.mode == PartRigMode::kChain);
  CHECK(project.find_part_rig(2, 2) == nullptr);

  // Shape violations are refused by the setter.
  CHECK_FALSE(project.set_part_rig({1, 16, none}));
  PartRig bad_chain;
  bad_chain.mode = PartRigMode::kChain;
  CHECK_FALSE(project.set_part_rig({1, 0, bad_chain}));
  CHECK(project.remove_part_rig(2, 1));
  CHECK_FALSE(project.remove_part_rig(2, 1));

  // Memory accounting covers the container and the command payload.
  const std::size_t base = sonare::arrangement::retained::dynamic_bytes(project);
  CHECK(project.set_part_rig({4, 0, chain}));
  CHECK(sonare::arrangement::retained::dynamic_bytes(project) > base);
  arr::SetPartRig command(1, 0, chain);
  CHECK(command.retained_bytes() > sizeof(command));
}
