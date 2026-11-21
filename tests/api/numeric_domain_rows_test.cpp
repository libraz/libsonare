/// @file numeric_domain_rows_test.cpp
/// @brief The numeric-domain partitions together: no (entry, parameter) has two rows.

#include "c_api/numeric_domain_rows.h"

#include <catch2/catch_test_macros.hpp>
#include <set>
#include <string>

TEST_CASE("no (entry, parameter) has rows in two numeric-domain partitions",
          "[numeric_domains][c_api]") {
  std::set<std::string> seen;
  for (const auto& table : sonare_c_detail::kNumericDomainTables) {
    for (std::size_t i = 0; i < table.count; ++i) {
      const std::string key = std::string(table.rows[i].entry) + " " + table.rows[i].param;
      INFO(table.name << ": " << key);
      CHECK(seen.insert(key).second);
    }
  }
}
