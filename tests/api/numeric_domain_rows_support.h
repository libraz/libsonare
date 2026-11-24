#pragma once

/// @file numeric_domain_rows_support.h
/// @brief Drives a numeric-domain partition against its C entries: each row's domain is the one
///        its entry refuses with, under the row's subject.

#include <sonare/sonare_c.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "c_api/numeric_domain_row.h"
#include "support/audio_fixtures.h"
#include "util/refusal_predicates.h"

namespace sonare::test::numeric_domains {

using sonare::util::check::NumberKind;
using sonare::util::check::NumericDomain;
using sonare_c_detail::NumericDomainRow;
using sonare_c_detail::NumericDomainTable;

/// Calls one C entry with `param` set to `value` and every other argument at a working default.
using Probe = std::function<SonareError(const std::string& param, double value)>;
/// A value the row's domain accepts and the probe can run on.
using InsideValue = std::function<double(const NumericDomainRow&)>;

struct ProbeField {
  const char* name;
  double value;
};

/// Applies fixed sibling fields before sweeping the selected field.
template <typename SetField>
void set_probe_fields(const std::string& param, double value,
                      std::initializer_list<ProbeField> siblings, SetField set) {
  for (const ProbeField& field : siblings) {
    if (param != field.name) set(field.name, field.value);
  }
  set(param, value);
}

inline constexpr int kAnalysisRate = 22050;

inline const std::vector<float>& analysis_samples() {
  static const std::vector<float> samples =
      sonare::test::generate_sine(kAnalysisRate / 2, 440.0f, kAnalysisRate);
  return samples;
}

inline const std::vector<uint8_t>& probe_sound_font() {
  static const auto bytes = [] {
    std::ifstream file("tests/fixtures/sf2/minimal_gs.sf2", std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), {});
  }();
  REQUIRE_FALSE(bytes.empty());
  return bytes;
}

/// A named row whose tested value reaches a declared operation-level refusal.
struct InsideRefusal {
  const char* entry;
  const char* param;
  SonareError code;
  const char* subject;
  const char* message_part;
  bool applies = true;
};

inline bool is_param(const std::string& param, const char* name) { return param == name; }

/// The refusal subject a row names, with each `[]` read as the first element.
inline std::string subject_of(const NumericDomainRow& row) {
  std::string subject = row.param;
  for (std::size_t at = subject.find("[]"); at != std::string::npos; at = subject.find("[]")) {
    subject.replace(at, 2, "[0]");
  }
  return subject;
}

/// A value outside the domain: below lo by 1 for an integer (by 2 when lo - 1 is the domain's
/// sentinel, which the entry reads as "not given"), lo itself when excluded, else past hi.
inline double outside_value(const NumericDomain& d) {
  if (d.kind == NumberKind::Integer) {
    const double below = *d.lo - 1.0;
    return d.sentinel && *d.sentinel == below ? below - 1.0 : below;
  }
  if (!d.lo_included) return *d.lo;
  if (d.hi) return *d.hi + std::max(1e-3 * std::fabs(*d.hi), 1e-6);
  return *d.lo - 1.0;
}

/// The domain's lowest accepted value, or a midpoint when lo is excluded.
inline double lowest_inside_value(const NumericDomainRow& row) {
  const NumericDomain& d = row.domain;
  if (d.kind == NumberKind::Integer) return *d.lo;
  if (!d.lo_included) return d.hi ? (*d.lo + *d.hi) / 2.0 : *d.lo + 1.0;
  return *d.lo;
}

/// The predicate the refusal states: the domain's, or the stricter form that replaces it. A
/// stricter name outside these three (a relation to another argument) leaves the domain's text.
inline std::string expected_predicate(const NumericDomainRow& row) {
  namespace refusal = sonare::util::refusal;
  const std::string stricter = row.stricter != nullptr ? row.stricter : "";
  if (stricter == "even" || stricter == "odd" || stricter == "power_of_two") {
    const auto lo = static_cast<long long>(*row.domain.lo);
    const auto hi = static_cast<long long>(*row.domain.hi);
    if (stricter == "even") return refusal::even_integer_predicate(lo, hi);
    if (stricter == "odd") return refusal::odd_integer_predicate(lo, hi);
    return refusal::power_of_two_predicate(lo, hi);
  }
  return sonare::util::check::domain_predicate(row.domain);
}

inline std::string last_subject() {
  if (sonare_last_refusal_subject_count() == 0) return "";
  const char* subject = sonare_last_refusal_subject(0);
  return subject != nullptr ? subject : "";
}

inline bool last_refusal_has_subject(const std::string& expected) {
  for (std::size_t i = 0; i < sonare_last_refusal_subject_count(); ++i) {
    const char* subject = sonare_last_refusal_subject(i);
    if (subject != nullptr && expected == subject) return true;
  }
  return false;
}

inline std::string row_key(const NumericDomainRow& row) {
  return std::string(row.entry) + '\n' + row.param;
}

inline void require_sorted(const NumericDomainTable& table) {
  REQUIRE(table.count > 0);
  for (std::size_t i = 1; i < table.count; ++i) {
    INFO(table.name << ": " << row_key(table.rows[i]));
    CHECK(row_key(table.rows[i - 1]) < row_key(table.rows[i]));
  }
}

/// Every row's outside value is refused as an invalid parameter, under the row's subject, with the
/// domain's predicate in the message.
inline void require_outside_refused(const NumericDomainTable& table,
                                    const std::map<std::string, Probe>& probes) {
  for (std::size_t i = 0; i < table.count; ++i) {
    const NumericDomainRow& row = table.rows[i];
    INFO(row.entry << " " << row.param);
    const auto probe = probes.find(row.entry);
    REQUIRE(probe != probes.end());
    const SonareError err = probe->second(row.param, outside_value(row.domain));
    REQUIRE(err == SONARE_ERROR_INVALID_PARAMETER);
    const std::string message = sonare_last_error_message();
    const bool istft_geometry_pair =
        std::string(row.entry) == "istft" &&
        (std::string(row.param) == "n_fft" || std::string(row.param) == "hop_length");
    if (istft_geometry_pair) {
      REQUIRE(sonare_last_refusal_subject_count() == 2);
      CHECK(last_refusal_has_subject("n_fft"));
      CHECK(last_refusal_has_subject("hop_length"));
      CHECK(message.find("when paired with") != std::string::npos);
    } else {
      CHECK(last_subject() == subject_of(row));
    }
    CHECK(message.find(expected_predicate(row)) != std::string::npos);
  }
}

/// Every row admits its inside value with compatible sibling arguments.
inline void require_inside_accepted(const NumericDomainTable& table,
                                    const std::map<std::string, Probe>& probes,
                                    const InsideValue& inside = lowest_inside_value,
                                    std::initializer_list<InsideRefusal> refusals = {}) {
  for (std::size_t i = 0; i < table.count; ++i) {
    const NumericDomainRow& row = table.rows[i];
    INFO(row.entry << " " << row.param);
    const auto probe = probes.find(row.entry);
    REQUIRE(probe != probes.end());
    const double value = inside(row);
    const SonareError err = probe->second(row.param, value);
    INFO("inside value " << value << ", subject " << last_subject() << ", "
                         << sonare_last_error_message());
    const auto refusal = std::find_if(refusals.begin(), refusals.end(), [&](const auto& item) {
      return item.applies && is_param(row.entry, item.entry) && is_param(row.param, item.param);
    });
    if (refusal == refusals.end()) {
      CHECK(err == SONARE_OK);
    } else {
      CHECK(err == refusal->code);
      CHECK(last_subject() == refusal->subject);
      CHECK(std::string(sonare_last_error_message()).find(refusal->message_part) !=
            std::string::npos);
    }
  }
}

}  // namespace sonare::test::numeric_domains
