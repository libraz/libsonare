/// @file dump_numeric_domains.cpp
/// @brief Prints every partition of src/c_api/numeric_domain_rows.h as JSON, sorted by (entry,
/// param).
///
/// Needs only headers: c++ -std=c++17 -I include -I src tools/api/dump_numeric_domains.cpp

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <optional>
#include <vector>

#include "c_api/numeric_domain_rows.h"

namespace {

using sonare::util::check::NumberKind;

void print_bound(const std::optional<double>& bound, bool integer) {
  if (!bound) {
    std::printf("null");
  } else if (integer) {
    std::printf("%lld", static_cast<long long>(*bound));
  } else if (*bound == 0.0 || *bound == 1.0) {
    std::printf("%d", static_cast<int>(*bound));
  } else {
    std::printf("%.17g", *bound);
  }
}

}  // namespace

int main() {
  std::vector<const sonare_c_detail::NumericDomainRow*> rows;
  for (const auto& table : sonare_c_detail::kNumericDomainTables) {
    for (std::size_t i = 0; i < table.count; ++i) rows.push_back(&table.rows[i]);
  }
  std::sort(rows.begin(), rows.end(), [](const auto* a, const auto* b) {
    const int by_entry = std::strcmp(a->entry, b->entry);
    return by_entry != 0 ? by_entry < 0 : std::strcmp(a->param, b->param) < 0;
  });
  std::printf("{\n  \"rows\": [\n");
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const auto& row = *rows[i];
    const bool integer = row.domain.kind == NumberKind::Integer;
    std::printf("    {\n");
    std::printf("      \"entry\": \"%s\",\n", row.entry);
    std::printf("      \"param\": \"%s\",\n", row.param);
    std::printf("      \"kind\": \"%s\",\n", integer ? "integer" : "real");
    std::printf("      \"lo\": ");
    print_bound(row.domain.lo, integer);
    std::printf(",\n      \"hi\": ");
    print_bound(row.domain.hi, integer);
    std::printf(",\n      \"lo_included\": %s,\n", row.domain.lo_included ? "true" : "false");
    std::printf("      \"hi_included\": %s,\n", row.domain.hi_included ? "true" : "false");
    if (row.stricter != nullptr) {
      std::printf("      \"stricter\": \"%s\"\n", row.stricter);
    } else {
      std::printf("      \"stricter\": null\n");
    }
    std::printf("    }%s\n", i + 1 < rows.size() ? "," : "");
  }
  std::printf("  ]\n}\n");
  return 0;
}
