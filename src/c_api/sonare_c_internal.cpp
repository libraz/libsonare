#include "sonare_c_internal.h"

#include <cmath>
#include <optional>
#include <string>
#include <string_view>

#include "c_api/sonare_c_error_mapping.h"
#include "c_api/sonare_c_last_refusal.h"
#include "util/error_classification.h"
#include "util/json_schema.h"
#include "util/parameter_checks.h"
#include "util/refusal_predicates.h"

namespace sonare_c_detail {

namespace {

constexpr const char* kErrorDetailUnavailable = "Error detail unavailable";

const char*& last_error_fallback_storage() noexcept {
  static thread_local const char* fallback = nullptr;
  return fallback;
}

std::optional<sonare::SonareParameterRefusal>& last_refusal_storage() noexcept {
  static thread_local std::optional<sonare::SonareParameterRefusal> refusal;
  return refusal;
}

std::optional<sonare::ErrorReason>& last_error_reason_storage() noexcept {
  static thread_local std::optional<sonare::ErrorReason> reason;
  return reason;
}

const char*& current_entry_storage() noexcept {
  static thread_local const char* entry = nullptr;
  return entry;
}

std::string& last_refusal_entry_storage() noexcept {
  static thread_local std::string key;
  return key;
}

const char*& last_unavailable_entry_storage() noexcept {
  static thread_local const char* entry = nullptr;
  return entry;
}

void record_error_reason(std::optional<sonare::ErrorReason> reason) noexcept {
  last_error_reason_storage() = reason;
  const char* entry =
      reason == sonare::ErrorReason::NotCompiledIn ? current_entry_storage() : nullptr;
  constexpr std::string_view prefix = "sonare_";
  if (entry != nullptr && std::string_view(entry).substr(0, prefix.size()) == prefix) {
    entry += prefix.size();
  }
  last_unavailable_entry_storage() = entry;
}

/// The c-entry-name key of @p entry: no "sonare_" prefix, a second-form suffix folded away.
std::string c_entry_key(const char* entry) {
  std::string key(entry != nullptr ? entry : "");
  constexpr std::string_view kPrefix = "sonare_";
  if (key.compare(0, kPrefix.size(), kPrefix) == 0) key.erase(0, kPrefix.size());
  for (const std::string_view suffix :
       {"_with_progress", "_with_options", "_checked", "_ex2", "_ex", "_v2"}) {
    if (key.size() > suffix.size() &&
        key.compare(key.size() - suffix.size(), suffix.size(), suffix) == 0) {
      key.erase(key.size() - suffix.size());
      break;
    }
  }
  return key;
}

}  // namespace

std::string& last_error_storage() noexcept {
  static thread_local std::string storage;
  return storage;
}

SonareError& last_error_code_storage() noexcept {
  static thread_local SonareError code = SONARE_OK;
  return code;
}

const char* last_error_message() noexcept {
  const char* fallback = last_error_fallback_storage();
  return fallback != nullptr ? fallback : last_error_storage().c_str();
}

const char* last_error_reason() noexcept {
  const std::optional<sonare::ErrorReason>& reason = last_error_reason_storage();
  return reason.has_value() ? sonare::error_reason_token(*reason) : nullptr;
}

void set_last_error(SonareError code, const char* msg,
                    std::optional<sonare::ErrorReason> reason) noexcept {
  last_error_code_storage() = code;
  last_refusal_storage().reset();
  record_error_reason(reason);
  last_error_fallback_storage() = nullptr;
  try {
    last_error_storage().assign(msg != nullptr ? msg : "");
  } catch (...) {
    last_error_fallback_storage() = kErrorDetailUnavailable;
  }
}

void set_last_error_with_refusal(SonareError code, const char* msg,
                                 const sonare::SonareParameterRefusal& refusal,
                                 std::optional<sonare::ErrorReason> reason) noexcept {
  set_last_error(code, msg, reason);
  try {
    last_refusal_entry_storage() = c_entry_key(current_entry_storage());
    last_refusal_storage() = refusal;
  } catch (...) {
    last_refusal_storage().reset();
  }
}

void set_last_error_static(SonareError code, const char* msg,
                           std::optional<sonare::ErrorReason> reason) noexcept {
  last_error_code_storage() = code;
  last_refusal_storage().reset();
  record_error_reason(reason);
  last_error_fallback_storage() = msg;
}

bool enter_c_api(const char* entry) noexcept {
  const bool outermost = current_entry_storage() == nullptr;
  if (outermost) current_entry_storage() = entry;
  clear_last_error();
  return outermost;
}

void leave_c_api() noexcept { current_entry_storage() = nullptr; }

void clear_last_error() noexcept {
  last_error_code_storage() = SONARE_OK;
  last_refusal_storage().reset();
  record_error_reason(std::nullopt);
  last_error_storage().clear();
  last_error_fallback_storage() = nullptr;
}

std::vector<sonare::Diagnostic>& last_diagnostics_storage() noexcept {
  static thread_local std::vector<sonare::Diagnostic> storage;
  return storage;
}

void set_last_diagnostics(const std::vector<sonare::Diagnostic>& diagnostics) {
  last_diagnostics_storage() = diagnostics;
}

void clear_last_diagnostics() noexcept { last_diagnostics_storage().clear(); }

void add_last_warning(const char* code, std::string message) {
  last_diagnostics_storage().push_back(
      sonare::Diagnostic{sonare::Diagnostic::Severity::Warning, code, std::move(message)});
}

const sonare::SonareParameterRefusal* last_parameter_refusal() noexcept {
  const auto& refusal = last_refusal_storage();
  return refusal.has_value() ? &*refusal : nullptr;
}

sonare::SonareParameterRefusal relabel_struct_refusal(const sonare::SonareParameterRefusal& e,
                                                      const char* root) {
  return sonare::util::json::schema::relabel_under(e, root, sonare::ErrorCode::InvalidParameter);
}

size_t last_refusal_subject_count() noexcept {
  const sonare::SonareParameterRefusal* refusal = last_parameter_refusal();
  return refusal == nullptr ? 0 : refusal->subjects().size();
}

const char* last_refusal_entry() noexcept {
  if (last_parameter_refusal() == nullptr) return last_unavailable_entry_storage();
  return last_refusal_entry_storage().c_str();
}

const char* last_refusal_subject(size_t index) noexcept {
  const sonare::SonareParameterRefusal* refusal = last_parameter_refusal();
  if (refusal == nullptr) return nullptr;
  const std::vector<std::string>& subjects = refusal->subjects();
  if (index < subjects.size()) return subjects[index].c_str();
  return nullptr;
}

namespace {

/// The predicate stating @p domain divided by @p scale. Each bound is a computed one, rounded
/// inward, so a printed bound is itself accepted; the domain is no longer an integer one.
std::string scaled_domain_predicate(const sonare::RefusalDomain& domain, double scale) {
  namespace refusal = sonare::util::refusal;
  using refusal::Inward;
  const auto lo = [&] { return refusal::bound_text(*domain.lo / scale, Inward::Up); };
  const auto hi = [&] { return refusal::bound_text(*domain.hi / scale, Inward::Down); };
  if (domain.lo && domain.hi) {
    return std::string("must be finite and in ") + (domain.lo_included ? "[" : "(") + lo() + ", " +
           hi() + (domain.hi_included ? "]" : ")");
  }
  if (domain.lo) {
    return (domain.lo_included ? "must be finite and at least "
                               : "must be finite and greater than ") +
           lo();
  }
  return (domain.hi_included ? "must be finite and at most " : "must be finite and less than ") +
         hi();
}

}  // namespace

sonare::SonareParameterRefusal respelled_refusal(const sonare::SonareParameterRefusal& refusal,
                                                 const char* const* subjects, const double* scales,
                                                 size_t subject_count) {
  namespace check = sonare::util::check;
  if (subject_count != refusal.subjects().size()) {
    check::refuse("subject_count", "must equal the refusal's subject count");
  }
  if (subject_count > 0 && subjects == nullptr) check::refuse("subjects", "must not be NULL");
  for (size_t i = 0; i < subject_count; ++i) {
    if (subjects[i] == nullptr) check::refuse("subjects", "must not contain NULL");
    if (scales != nullptr) check::greater_than(scales[i], 0.0, "scales");
  }
  size_t next = 0;
  sonare::SonareParameterRefusal rendered = refusal.respelled(
      refusal.code(), [&](const std::string&) { return std::string(subjects[next++]); },
      refusal.caller_spelled());
  if (scales == nullptr || scales[0] == 1.0 || !rendered.domain() ||
      rendered.form() != sonare::SonareParameterRefusal::Form::Predicate) {
    return rendered;
  }
  sonare::RefusalDomain scaled = *rendered.domain();
  if (scaled.lo) *scaled.lo /= scales[0];
  if (scaled.hi) *scaled.hi /= scales[0];
  return sonare::SonareParameterRefusal::predicate(
      rendered.code(), rendered.subject(), scaled_domain_predicate(*rendered.domain(), scales[0]),
      scaled);
}

SonareError render_last_refusal(const char* const* subjects, const double* scales,
                                size_t subject_count, SonarePresenceSpelling spelling,
                                char** out_text) noexcept {
  if (out_text == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  *out_text = nullptr;
  const sonare::SonareParameterRefusal* refusal = last_parameter_refusal();
  if (refusal == nullptr) return SONARE_ERROR_INVALID_STATE;
  sonare::PresenceSpelling surface;
  switch (spelling) {
    case SONARE_PRESENCE_SPELLING_C:
      surface = sonare::PresenceSpelling::kC;
      break;
    case SONARE_PRESENCE_SPELLING_PYTHON:
      surface = sonare::PresenceSpelling::kPython;
      break;
    case SONARE_PRESENCE_SPELLING_JS:
      surface = sonare::PresenceSpelling::kJs;
      break;
    default:
      return SONARE_ERROR_INVALID_PARAMETER;
  }
  try {
    *out_text =
        copy_string(respelled_refusal(*refusal, subjects, scales, subject_count).text(surface));
    return SONARE_OK;
  } catch (const sonare::SonareParameterRefusal&) {
    // Not recorded: recording it would overwrite the refusal this accessor renders.
    return SONARE_ERROR_INVALID_PARAMETER;
  } catch (...) {
    return SONARE_ERROR_OUT_OF_MEMORY;
  }
}

SonareError map_sonare_exception(const SonareException& e) {
  return c_error_from_error_code(e.code());
}

SonareError record_current_exception() noexcept {
  try {
    throw;
  } catch (const sonare::SonareParameterRefusal& e) {
    const SonareError code = map_sonare_exception(e);
    set_last_error(code, e.what(), e.reason());
    try {
      last_refusal_entry_storage() = c_entry_key(current_entry_storage());
      last_refusal_storage() = e;
    } catch (...) {
      last_refusal_storage().reset();
      // The flat message is already recorded; a binding then renders it as C spells it.
    }
    return code;
  } catch (const SonareException& e) {
    const SonareError code = map_sonare_exception(e);
    set_last_error(code, e.what(), e.reason());
    return code;
  } catch (const std::exception& e) {
    const SonareError code = static_cast<SonareError>(sonare::error_code_for_std_exception(e));
    set_last_error(code, sonare::message_for_std_exception(e));
    return code;
  } catch (...) {
    set_last_error(SONARE_ERROR_UNKNOWN, kUnknownExceptionMessage);
    return SONARE_ERROR_UNKNOWN;
  }
}

SonareError refuse_null(const char* param) noexcept {
  try {
    throw sonare::SonareParameterRefusal::predicate(sonare::ErrorCode::InvalidParameter, param,
                                                    sonare::util::refusal::kNonNullPredicate);
  } catch (...) {
    return record_current_exception();
  }
}

SonareError refuse_null_when_count(const char* param, const char* count) noexcept {
  try {
    throw sonare::SonareParameterRefusal::relation(
        sonare::ErrorCode::InvalidParameter, {param, count},
        sonare::util::refusal::kNonNullWhenCountRelation);
  } catch (...) {
    return record_current_exception();
  }
}

SonareError validate_audio_params(const char* name, const float* samples, size_t length,
                                  int sample_rate) noexcept {
  clear_last_error();
  // Delegate to the shared core policy so the C ABI and the WASM bindings (which
  // bypass this translation unit) enforce identical empty/range/finite rules.
  try {
    sonare::validate_offline_audio_input(name, samples, length, sample_rate);
  } catch (...) {
    return record_current_exception();
  }
  return SONARE_OK;
}

SonareError validate_audio_params_window(const char* name, const float* samples, size_t length,
                                         int sample_rate, size_t scan_offset,
                                         size_t scan_count) noexcept {
  clear_last_error();
  try {
    sonare::validate_offline_audio_window(name, samples, length, sample_rate, scan_offset,
                                          scan_count);
  } catch (...) {
    return record_current_exception();
  }
  return SONARE_OK;
}

SonareGrooveType to_c_groove_type(const std::string& groove) {
  if (groove == "swing") return SONARE_GROOVE_SWING;
  if (groove == "shuffle") return SONARE_GROOVE_SHUFFLE;
  return SONARE_GROOVE_STRAIGHT;
}

SonareChordQuality to_c_chord_quality(ChordQuality quality) {
  switch (quality) {
    case ChordQuality::Major:
      return SONARE_CHORD_MAJOR;
    case ChordQuality::Minor:
      return SONARE_CHORD_MINOR;
    case ChordQuality::Diminished:
      return SONARE_CHORD_DIMINISHED;
    case ChordQuality::Augmented:
      return SONARE_CHORD_AUGMENTED;
    case ChordQuality::Dominant7:
      return SONARE_CHORD_DOMINANT7;
    case ChordQuality::Major7:
      return SONARE_CHORD_MAJOR7;
    case ChordQuality::Minor7:
      return SONARE_CHORD_MINOR7;
    case ChordQuality::Sus2:
      return SONARE_CHORD_SUS2;
    case ChordQuality::Sus4:
      return SONARE_CHORD_SUS4;
    case ChordQuality::Unknown:
      return SONARE_CHORD_UNKNOWN;
    case ChordQuality::Add9:
      return SONARE_CHORD_ADD9;
    case ChordQuality::MinorAdd9:
      return SONARE_CHORD_MINOR_ADD9;
    case ChordQuality::Dim7:
      return SONARE_CHORD_DIM7;
    case ChordQuality::HalfDim7:
      return SONARE_CHORD_HALF_DIM7;
    case ChordQuality::Major9:
      return SONARE_CHORD_MAJOR9;
    case ChordQuality::Dominant9:
      return SONARE_CHORD_DOMINANT9;
    case ChordQuality::Sus2Add4:
      return SONARE_CHORD_SUS2_ADD4;
    case ChordQuality::Major6:
      return SONARE_CHORD_MAJOR6;
    case ChordQuality::Minor6:
      return SONARE_CHORD_MINOR6;
    case ChordQuality::MinorMajor7:
      return SONARE_CHORD_MINOR_MAJOR7;
    case ChordQuality::Dominant7Sus4:
      return SONARE_CHORD_DOMINANT7_SUS4;
    case ChordQuality::Dominant11:
      return SONARE_CHORD_DOMINANT11;
    case ChordQuality::Dominant13:
      return SONARE_CHORD_DOMINANT13;
    case ChordQuality::Dominant7b9:
      return SONARE_CHORD_DOMINANT7_FLAT9;
    case ChordQuality::Dominant7s9:
      return SONARE_CHORD_DOMINANT7_SHARP9;
  }
  return SONARE_CHORD_UNKNOWN;
}

}  // namespace sonare_c_detail
