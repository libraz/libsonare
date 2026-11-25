#pragma once

/// @file exception.h
/// @brief Exception classes for libsonare.

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "util/error_reason.h"
#include "util/number_format.h"
#include "util/types.h"

namespace sonare {

/// @brief Base exception class for libsonare errors.
class SonareException : public std::runtime_error {
 public:
  /// @brief Constructs exception with error code.
  /// @param code Error code
  explicit SonareException(ErrorCode code) : std::runtime_error(error_message(code)), code_(code) {}

  /// @brief Constructs exception with error code and custom message.
  /// @param code Error code
  /// @param message Custom error message
  SonareException(ErrorCode code, const std::string& message)
      : std::runtime_error(message), code_(code) {}

  /// @brief Constructs an exception with an error code, message and reason.
  SonareException(ErrorCode code, const std::string& message, std::optional<ErrorReason> reason)
      : std::runtime_error(message), code_(code), reason_(reason) {}

  /// @brief Returns the error code.
  ErrorCode code() const { return code_; }

  /// @brief Returns the machine-readable cause, when one is assigned.
  const std::optional<ErrorReason>& reason() const { return reason_; }

 private:
  ErrorCode code_;
  std::optional<ErrorReason> reason_;
};

/// @brief The numeric domain a value refusal states, kept beside its text so a binding that
///        converted the value from another unit can state the domain in that unit.
struct RefusalDomain {
  std::optional<double> lo;  ///< Lower bound, or none for a domain open below.
  std::optional<double> hi;  ///< Upper bound, or none for a domain open above.
  bool lo_included = true;
  bool hi_included = true;
};

/// @brief How a surface words a pointer or optional value being left out or supplied.
/// @details A refusal text writes presence through the tokens in refusal_predicates.h
///          (`{must_be_absent}`, `{is_present}`, ...) rather than through the word NULL, so each
///          surface renders it in its own terms: C as NULL, Python as None, JS as omitted / given.
enum class PresenceSpelling { kC, kPython, kJs };

/// @brief The phrase @p token (a presence token's name, without braces) reads as in @p spelling,
///        or nullptr when @p token names no presence token.
inline const char* presence_phrase(const std::string& token, PresenceSpelling spelling) {
  struct Row {
    const char* token;
    const char* c;
    const char* python;
    const char* js;
  };
  static constexpr Row kRows[] = {
      {"must_be_absent", "must be NULL", "must be None", "must be omitted"},
      {"must_be_present", "must not be NULL", "must not be None", "must be given"},
      {"must_not_both_be_absent", "must not both be NULL", "must not both be None",
       "must not both be omitted"},
      {"give_either", "give", "give", "give"},
      {"is_absent", "is NULL", "is None", "is omitted"},
      {"is_present", "is not NULL", "is not None", "is given"},
  };
  for (const Row& row : kRows) {
    if (token != row.token) continue;
    switch (spelling) {
      case PresenceSpelling::kC:
        return row.c;
      case PresenceSpelling::kPython:
        return row.python;
      case PresenceSpelling::kJs:
        return row.js;
    }
  }
  return nullptr;
}

/// @brief Whether @p pattern requires a value to be left out or supplied: it carries a
///        required-presence or required-absence token. A presence
///        token in a condition (`{is_absent}`, `{is_present}`) requires nothing of its own.
inline bool requires_presence(const std::string& pattern) noexcept {
  for (const char* token :
       {"{must_be_absent}", "{must_be_present}", "{must_not_both_be_absent}", "{give_either}"}) {
    if (pattern.find(token) != std::string::npos) return true;
  }
  return false;
}

/// @brief A refusal of one or more parameters that carries their subject keys as data.
/// @details The layer that knows where a parameter sits in a document, or how its caller spells
///          it, rethrows the refusal with each subject rewritten. what() reads
///          "<subject> <predicate>", "unknown key <subject>" with " (expected <expected>)" when
///          the accepted keys are known, or a relation text naming every subject it constrains.
class SonareParameterRefusal : public SonareException {
 public:
  /// @brief WrongType reads as a Predicate does. It and UnknownKey are shape errors, refusing the
  ///        request's form rather than a value's domain (TypeError on the JS surfaces), and so is
  ///        any refusal whose requirement is a presence token (see shape_error()).
  enum class Form { Predicate, WrongType, UnknownKey, Relation };

  /// @brief A refusal reading "<subject> <predicate>".
  static SonareParameterRefusal predicate(ErrorCode code, const std::string& subject,
                                          const std::string& predicate) {
    return SonareParameterRefusal(code, Form::Predicate, {subject}, predicate, std::string(),
                                  false);
  }

  /// @brief A refusal reading "<subject> <predicate>" with a machine-readable reason.
  static SonareParameterRefusal predicate_with_reason(ErrorCode code, const std::string& subject,
                                                      const std::string& predicate,
                                                      ErrorReason reason) {
    return SonareParameterRefusal(code, Form::Predicate, {subject}, predicate, std::string(), false,
                                  reason);
  }

  /// @brief A refusal reading "<subject> <predicate>" where @p predicate states @p domain.
  static SonareParameterRefusal predicate(ErrorCode code, const std::string& subject,
                                          const std::string& predicate,
                                          const RefusalDomain& domain) {
    SonareParameterRefusal refusal = SonareParameterRefusal::predicate(code, subject, predicate);
    refusal.domain_ = domain;
    return refusal;
  }

  /// @brief A refusal reading "<subject> <predicate>" for a value of the wrong type.
  static SonareParameterRefusal wrong_type(ErrorCode code, const std::string& subject,
                                           const std::string& predicate) {
    return SonareParameterRefusal(code, Form::WrongType, {subject}, predicate, std::string(),
                                  false);
  }

  /// @brief A refusal reading "unknown key <subject>", listing the keys the reader takes when
  ///        @p expected is set.
  /// @param expected The accepted sibling keys as one comma-separated list. They are leaf names
  ///        already in the caller's spelling, so a respelling leaves them as they are.
  static SonareParameterRefusal unknown(ErrorCode code, const std::string& subject,
                                        const std::string& expected = std::string()) {
    return unknown_key_argument(code, subject, std::string(), expected);
  }

  /// @brief A string argument whose value is an undeclared key; record the argument's name
  ///        and print the supplied key. An empty @p key prints @p subject.
  static SonareParameterRefusal unknown_key_argument(ErrorCode code, const std::string& subject,
                                                     const std::string& key,
                                                     const std::string& expected) {
    return SonareParameterRefusal(code, Form::UnknownKey, {subject}, key, expected, false);
  }

  /// @brief A refusal relating several parameters; @p text writes subject @c i as "{i}".
  static SonareParameterRefusal relation(ErrorCode code, std::vector<std::string> subjects,
                                         const std::string& text) {
    return SonareParameterRefusal(code, Form::Relation, std::move(subjects), text, std::string(),
                                  false);
  }

  Form form() const { return form_; }
  /// @brief Whether the refusal is of the request's shape rather than of a value's domain: an
  ///        unknown key, a wrong type, or a value required to be left out or supplied. It is read
  ///        off the text, so every call site of a presence template gets the same verdict.
  bool shape_error() const noexcept {
    return form_ == Form::WrongType || form_ == Form::UnknownKey || requires_presence(predicate_);
  }
  /// @brief The first subject.
  const std::string& subject() const { return subjects_.front(); }
  /// @brief Every subject, in the order the text names them.
  const std::vector<std::string>& subjects() const { return subjects_; }
  /// @brief The predicate or relation template, with presence tokens rendered by what().
  ///        For UnknownKey, this holds a supplied key value when it differs from the subject.
  const std::string& predicate() const { return predicate_; }
  /// @brief The accepted keys an unknown-key refusal lists, or empty.
  const std::string& expected() const { return expected_; }
  /// @brief The numeric domain the predicate states, when it states one.
  const std::optional<RefusalDomain>& domain() const { return domain_; }
  /// @brief Whether the subjects are already in the caller's spelling.
  bool caller_spelled() const { return caller_spelled_; }

  /// @brief The text what() reads with its presence tokens rendered in @p spelling.
  std::string text(PresenceSpelling spelling) const {
    return compose(form_, subjects_, predicate_, expected_, spelling);
  }

  /// @brief This refusal marked as already in the caller's spelling.
  SonareParameterRefusal as_caller_spelled() const {
    return respelled(code(), [](const std::string& subject) { return subject; }, true);
  }

  /// @brief This refusal with every subject passed through @p spell.
  /// @param caller_spelled Whether the result is in the caller's spelling; once set it stays set.
  template <typename Spell>
  SonareParameterRefusal respelled(ErrorCode code, const Spell& spell, bool caller_spelled) const {
    std::vector<std::string> subjects;
    subjects.reserve(subjects_.size());
    for (const std::string& subject : subjects_) subjects.push_back(spell(subject));
    return with_subjects(code, std::move(subjects), caller_spelled);
  }

 private:
  [[gnu::noinline]] SonareParameterRefusal with_subjects(ErrorCode code,
                                                         std::vector<std::string>&& subjects,
                                                         bool caller_spelled) const {
    SonareParameterRefusal refusal(code, form_, std::move(subjects), predicate_, expected_,
                                   caller_spelled_ || caller_spelled,
                                   code == this->code() ? reason() : std::nullopt);
    refusal.domain_ = domain_;
    return refusal;
  }

  SonareParameterRefusal(ErrorCode code, Form form, std::vector<std::string> subjects,
                         std::string predicate, std::string expected, bool caller_spelled,
                         std::optional<ErrorReason> reason = std::nullopt)
      : SonareException(code, compose(form, subjects, predicate, expected, PresenceSpelling::kC),
                        reason),
        form_(form),
        subjects_(std::move(subjects)),
        predicate_(std::move(predicate)),
        expected_(std::move(expected)),
        caller_spelled_(caller_spelled) {}

  static std::string compose(Form form, const std::vector<std::string>& subjects,
                             const std::string& predicate, const std::string& expected,
                             PresenceSpelling spelling) {
    if (form == Form::Predicate || form == Form::WrongType) {
      return subjects.front() + " " + render(predicate, {}, spelling);
    }
    if (form == Form::UnknownKey) {
      std::string text = "unknown key " + (predicate.empty() ? subjects.front() : predicate);
      if (!expected.empty()) {
        text += " (expected " + expected + ")";
      }
      return text;
    }
    return render(predicate, subjects, spelling);
  }

  /// @brief @p pattern with each "{i}" replaced by subject i and each presence token by its
  ///        phrase in @p spelling; any other brace text is kept as written.
  static std::string render(const std::string& pattern, const std::vector<std::string>& subjects,
                            PresenceSpelling spelling) {
    std::string text;
    for (std::size_t at = 0; at < pattern.size(); ++at) {
      const std::size_t close = pattern.find('}', at);
      if (pattern[at] == '{' && close != std::string::npos) {
        const std::string name = pattern.substr(at + 1, close - at - 1);
        if (!name.empty() && name.find_first_not_of("0123456789") == std::string::npos) {
          const std::size_t index = std::stoul(name);
          if (index < subjects.size()) {
            text += subjects[index];
            at = close;
            continue;
          }
        } else if (const char* phrase = presence_phrase(name, spelling)) {
          text += phrase;
          at = close;
          continue;
        }
      }
      text += pattern[at];
    }
    return text;
  }

  Form form_;
  std::vector<std::string> subjects_;
  std::string predicate_;
  std::string expected_;
  bool caller_spelled_;
  std::optional<RefusalDomain> domain_;
};

/// @def SONARE_CHECK
/// @brief Throws SonareException if condition is false.
#define SONARE_CHECK(cond, code)   \
  do {                             \
    if (!(cond)) {                 \
      throw SonareException(code); \
    }                              \
  } while (0)

/// @def SONARE_CHECK_MSG
/// @brief Throws SonareException with custom message if condition is false.
#define SONARE_CHECK_MSG(cond, code, msg) \
  do {                                    \
    if (!(cond)) {                        \
      throw SonareException(code, msg);   \
    }                                     \
  } while (0)

/// @def SONARE_CHECK_RANGE
/// @brief Throws InvalidParameter with a detailed inclusive range message.
#define SONARE_CHECK_RANGE(name, value, min_value, max_value)                                 \
  do {                                                                                        \
    const auto sonare_check_range_value = (value);                                            \
    const auto sonare_check_range_min = (min_value);                                          \
    const auto sonare_check_range_max = (max_value);                                          \
    if (!(sonare_check_range_value >= sonare_check_range_min &&                               \
          sonare_check_range_value <= sonare_check_range_max)) {                              \
      throw SonareException(ErrorCode::InvalidParameter,                                      \
                            ::sonare::util::to_text(name) + " must be in [" +                 \
                                ::sonare::util::to_text(sonare_check_range_min) + ", " +      \
                                ::sonare::util::to_text(sonare_check_range_max) + "], got " + \
                                ::sonare::util::to_text(sonare_check_range_value));           \
    }                                                                                         \
  } while (0)

}  // namespace sonare
