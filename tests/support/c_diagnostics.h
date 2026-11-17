/// @file c_diagnostics.h
/// @brief Reads the C ABI's last-call diagnostic list for assertions.

#pragma once

#include <sonare/sonare_c.h>

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

namespace sonare_test {

/// Every diagnostic the previous C call published, in order.
inline std::vector<SonareDiagnostic> last_diagnostics() {
  std::vector<SonareDiagnostic> out;
  for (size_t i = 0; i < sonare_last_diagnostic_count(); ++i) {
    SonareDiagnostic diagnostic;
    sonare_diagnostic_init(&diagnostic);
    REQUIRE(sonare_last_diagnostic(i, &diagnostic) == SONARE_OK);
    out.push_back(diagnostic);
  }
  return out;
}

inline bool has_diagnostic_code(const std::string& code) {
  for (const SonareDiagnostic& diagnostic : last_diagnostics()) {
    if (code == diagnostic.code) return true;
  }
  return false;
}

/// The messages of every diagnostic carrying @p code, in order.
inline std::vector<std::string> diagnostic_messages(const std::string& code) {
  std::vector<std::string> out;
  for (const SonareDiagnostic& diagnostic : last_diagnostics()) {
    if (code == diagnostic.code) out.emplace_back(diagnostic.message);
  }
  return out;
}

}  // namespace sonare_test
