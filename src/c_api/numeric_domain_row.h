#pragma once

/// @file numeric_domain_row.h
/// @brief The row type of the numeric-domain table, shared by every partition of it.

#include <cstddef>

#include "util/parameter_checks.h"

namespace sonare_c_detail {

/// @brief One refused parameter of one C entry and the domain it is checked against.
struct NumericDomainRow {
  /// C function name without the `sonare_` prefix.
  const char* entry;
  /// The refusal subject; an array index is written empty (`instruments[].config.gain`).
  const char* param;
  sonare::util::check::NumericDomain domain;
  /// A further predicate the validator applies with the domain, or nullptr: `even`, `odd` or
  /// `power_of_two` replace the domain's text; any other name is a relation the text omits.
  const char* stricter;
};

/// @brief One partition of the table: rows sorted by (entry, param).
struct NumericDomainTable {
  const char* name;
  const NumericDomainRow* rows;
  std::size_t count;
};

template <std::size_t N>
constexpr NumericDomainTable numeric_domain_table(const char* name,
                                                  const NumericDomainRow (&rows)[N]) {
  return {name, rows, N};
}

}  // namespace sonare_c_detail
