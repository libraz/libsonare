// A refusal of one insert parameter leads with that parameter's key, so a document layer can
// place it under the path the parameter sits at. Probes every cataloged insert one parameter at a
// time and counts the refusals that do not lead with the key.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <sstream>
#include <string>
#include <vector>

#include "mastering/api/insert_factory.h"
#include "util/exception.h"
#include "util/json.h"

namespace {

namespace json = sonare::util::json;
using sonare::mastering::api::insert_factory_names;
using sonare::mastering::api::insert_param_info_json;
using sonare::mastering::api::make_insert;

// The count may only fall.
constexpr std::size_t kKeylessCeiling = 1471;

constexpr double kBeyondBound = 1.0e6;
constexpr const char* kMakeInsertPrefix = "make_insert: ";
constexpr const char* kUnreadNote = "does not read parameter(s)";

std::string number_text(double value) {
  std::ostringstream out;
  out.precision(17);
  out << value;
  return out.str();
}

struct Tally {
  std::size_t read = 0;
  std::size_t keyless = 0;
};

void probe(const std::string& insert, const std::string& key, const std::string& value_text,
           Tally* tally) {
  const std::string params = "{\"" + key + "\":" + value_text + "}";
  try {
    (void)make_insert(insert, params);
  } catch (const sonare::SonareException& e) {
    std::string message = e.what();
    if (message.rfind(kMakeInsertPrefix, 0) == 0)
      message.erase(0, std::string(kMakeInsertPrefix).size());
    if (message.find(kUnreadNote) != std::string::npos) return;
    ++tally->read;
    if (message.rfind(key + " ", 0) != 0) ++tally->keyless;
  }
}

}  // namespace

TEST_CASE("an insert parameter refusal leads with the parameter key",
          "[mastering][insert_catalog][.][slow]") {
  Tally tally;
  for (const std::string& insert : insert_factory_names()) {
    const json::Value info = json::parse_strict(insert_param_info_json(insert));
    if (!info.is_array()) continue;
    for (const json::Value& parameter : info.as_array()) {
      const json::Value* name = parameter.find("name");
      const json::Value* type = parameter.find("type");
      if (name == nullptr || type == nullptr || !name->is_string() || !type->is_string()) continue;
      const std::string key = name->as_string();
      if (type->as_string() == "number") {
        probe(insert, key, "\"x\"", &tally);
        const json::Value* max = parameter.find("max");
        const json::Value* min = parameter.find("min");
        if (max != nullptr && max->is_number()) {
          probe(insert, key, number_text(max->as_number() + kBeyondBound), &tally);
        }
        if (min != nullptr && min->is_number()) {
          probe(insert, key, number_text(min->as_number() - kBeyondBound), &tally);
        }
        probe(insert, key, "1e300", &tally);
      } else if (type->as_string() == "boolean") {
        probe(insert, key, "\"x\"", &tally);
        probe(insert, key, "5", &tally);
      }
    }
  }
  WARN("refusals read: " << tally.read << ", keyless: " << tally.keyless);
  REQUIRE(tally.read > 0);
  CHECK(tally.keyless <= kKeylessCeiling);
}
