#pragma once

/// @file sonare_c_refusal_record.h
/// @brief Internal: a parameter refusal as plain parts, which a binding carries on the JS error it
///        threw and rebuilds when a facade re-renders the refusal under the names its caller wrote.
/// @details The parts hold every field the refusal's text is composed from, so a rebuilt refusal
///          renders the same text and respells the same way as the one that was thrown. Not
///          exported.

#include <optional>
#include <string>
#include <vector>

#include "util/exception.h"

namespace sonare_c_detail {

/// @brief The fields of a SonareParameterRefusal, with its form spelled as a token.
struct RefusalParts {
  sonare::ErrorCode code = sonare::ErrorCode::InvalidParameter;
  std::string form;  ///< "predicate", "wrongType", "unknownKey" or "relation".
  std::vector<std::string> subjects;
  std::string predicate;
  std::string expected;
  bool caller_spelled = false;
  std::optional<sonare::RefusalDomain> domain;
};

inline const char* refusal_form_token(sonare::SonareParameterRefusal::Form form) {
  using Form = sonare::SonareParameterRefusal::Form;
  switch (form) {
    case Form::Predicate:
      return "predicate";
    case Form::WrongType:
      return "wrongType";
    case Form::UnknownKey:
      return "unknownKey";
    case Form::Relation:
      return "relation";
  }
  return "predicate";
}

inline RefusalParts refusal_parts(const sonare::SonareParameterRefusal& refusal) {
  RefusalParts parts;
  parts.code = refusal.code();
  parts.form = refusal_form_token(refusal.form());
  parts.subjects = refusal.subjects();
  parts.predicate = refusal.predicate();
  parts.expected = refusal.expected();
  parts.caller_spelled = refusal.caller_spelled();
  parts.domain = refusal.domain();
  return parts;
}

/// @brief The refusal @p parts describe.
/// @throws SonareParameterRefusal(InvalidParameter) on an unknown form or a form whose subjects
///         are missing.
inline sonare::SonareParameterRefusal refusal_from_parts(const RefusalParts& parts) {
  using sonare::SonareParameterRefusal;
  const sonare::ErrorCode invalid = sonare::ErrorCode::InvalidParameter;
  if (parts.subjects.empty()) {
    throw SonareParameterRefusal::predicate(invalid, "refusal.subjects", "must not be empty");
  }
  const auto spelled = [&parts](const SonareParameterRefusal& refusal) {
    return parts.caller_spelled ? refusal.as_caller_spelled() : refusal;
  };
  const std::string& subject = parts.subjects.front();
  if (parts.form == "predicate") {
    return spelled(
        parts.domain
            ? SonareParameterRefusal::predicate(parts.code, subject, parts.predicate, *parts.domain)
            : SonareParameterRefusal::predicate(parts.code, subject, parts.predicate));
  }
  if (parts.form == "wrongType") {
    return spelled(SonareParameterRefusal::wrong_type(parts.code, subject, parts.predicate));
  }
  if (parts.form == "unknownKey") {
    return spelled(SonareParameterRefusal::unknown_key_argument(parts.code, subject,
                                                                parts.predicate, parts.expected));
  }
  if (parts.form == "relation") {
    return spelled(SonareParameterRefusal::relation(parts.code, parts.subjects, parts.predicate));
  }
  throw SonareParameterRefusal::predicate(invalid, "refusal.form",
                                          "must be one of predicate, wrongType, unknownKey, "
                                          "relation");
}

}  // namespace sonare_c_detail
