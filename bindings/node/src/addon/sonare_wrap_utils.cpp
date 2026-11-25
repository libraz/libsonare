#include "sonare_wrap_utils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "analysis/music_analyzer.h"
#include "c_api/sonare_c_error_mapping.h"
#include "c_api/sonare_c_last_refusal.h"
#include "c_api/sonare_c_refusal_record.h"
#include "c_entry_names.h"
#include "core/audio.h"
#include "effects/hpss.h"
#include "field_spelling.h"
#include "sonare_wrap_options.h"
#include "util/constants.h"
#include "util/error_classification.h"
#include "util/error_reason.h"
#include "util/parameter_checks.h"
#include "util/refusal_predicates.h"

namespace sonare_node {

using sonare::constants::kDefaultNFft;

std::string RefusalText(const sonare::SonareParameterRefusal& refusal) {
  return refusal.text(sonare::PresenceSpelling::kJs);
}

std::string ErrorMessageForCode(SonareError err) {
  // A recorded parameter refusal is rendered in request keys from its subjects, never parsed.
  if (const std::optional<sonare::SonareParameterRefusal> shown = ShownLastRefusal()) {
    return RefusalText(*shown);
  }
  const char* detail = sonare_last_error_message();
  if (detail != nullptr && detail[0] != '\0') {
    return detail + sonare_c_entries::CEntryPrefixLength(detail);
  }
  return sonare_error_message(err);
}

std::string ExceptionMessage(const std::exception& e) {
  if (const auto* refusal = dynamic_cast<const sonare::SonareParameterRefusal*>(&e)) {
    return RefusalText(sonare::util::check::camel_spelled(*refusal));
  }
  const char* what = dynamic_cast<const sonare::SonareException*>(&e) != nullptr
                         ? e.what()
                         : sonare::message_for_std_exception(e);
  return what + sonare_c_entries::CEntryPrefixLength(what);
}

bool IsShapeRefusal(const std::exception& e) {
  const auto* refusal = dynamic_cast<const sonare::SonareParameterRefusal*>(&e);
  return refusal != nullptr && refusal->shape_error();
}

bool LastRefusalIsShape() {
  const sonare::SonareParameterRefusal* refusal = sonare_c_detail::last_parameter_refusal();
  return refusal != nullptr && refusal->shape_error();
}

std::optional<sonare::SonareParameterRefusal> ShownLastRefusal() {
  return sonare_field_spelling::node::SpelledLastRefusal();
}

std::string LastRefusalEntry() {
  const char* entry = sonare_last_refusal_entry();
  return entry != nullptr ? entry : "";
}

namespace {

Napi::Object RefusalObject(Napi::Env env, const sonare::SonareParameterRefusal& refusal) {
  const sonare_c_detail::RefusalParts parts = sonare_c_detail::refusal_parts(refusal);
  Napi::Object out = Napi::Object::New(env);
  out.Set("code", static_cast<double>(static_cast<int>(parts.code)));
  out.Set("form", parts.form);
  Napi::Array subjects = Napi::Array::New(env, parts.subjects.size());
  for (uint32_t i = 0; i < parts.subjects.size(); ++i) subjects.Set(i, parts.subjects[i]);
  out.Set("subjects", subjects);
  out.Set("predicate", parts.predicate);
  out.Set("expected", parts.expected);
  out.Set("callerSpelled", parts.caller_spelled);
  out.Set("text", RefusalText(refusal));
  if (parts.domain) {
    Napi::Object domain = Napi::Object::New(env);
    if (parts.domain->lo) domain.Set("lo", *parts.domain->lo);
    if (parts.domain->hi) domain.Set("hi", *parts.domain->hi);
    domain.Set("loIncluded", parts.domain->lo_included);
    domain.Set("hiIncluded", parts.domain->hi_included);
    out.Set("domain", domain);
  }
  return out;
}

// Reads the refusal record renderRefusal was handed; false leaves the refusal pending in JS.
bool RefusalFromObject(Napi::Env env, const Napi::Object& object,
                       sonare_c_detail::RefusalParts* parts) {
  int code = 0;
  Napi::Array subjects;
  if (!RequiredIntProperty(env, object, "code", &code) ||
      !RequiredArrayValue(env, object.Get("subjects"), "subjects", &subjects)) {
    return false;
  }
  parts->code = static_cast<sonare::ErrorCode>(code);
  const uint32_t subject_count = node_list_length(subjects, "subjects");
  for (uint32_t i = 0; i < subject_count; ++i) {
    std::string subject;
    if (!RequiredStringValue(env, subjects.Get(i), "subjects", &subject)) return false;
    parts->subjects.push_back(std::move(subject));
  }
  parts->form = StringProperty(object, "form", "");
  parts->predicate = StringProperty(object, "predicate", "");
  parts->expected = StringProperty(object, "expected", "");
  parts->caller_spelled = BoolProperty(object, "callerSpelled", false);
  const Napi::Value domain = object.Get("domain");
  if (!domain.IsUndefined() && !domain.IsNull()) {
    Napi::Object d;
    if (!RequiredObjectValue(env, domain, "domain", &d)) return false;
    sonare::RefusalDomain parsed;
    for (auto [key, bound] : {std::pair{"lo", &parsed.lo}, std::pair{"hi", &parsed.hi}}) {
      const Napi::Value given = d.Get(key);
      if (given.IsUndefined() || given.IsNull()) continue;
      double number{};
      if (!RequiredDoubleValue(env, given, key, &number)) return false;
      *bound = number;
    }
    parsed.lo_included = BoolProperty(d, "loIncluded", true);
    parsed.hi_included = BoolProperty(d, "hiIncluded", true);
    parts->domain = parsed;
  }
  return true;
}

}  // namespace

void AttachRefusal(Napi::Env env, Napi::Object error, const sonare::SonareParameterRefusal& shown,
                   const std::string& c_entry) {
  Napi::Object record = RefusalObject(env, shown);
  record.Set("cEntry", c_entry.empty() ? env.Null() : Napi::String::New(env, c_entry));
  error.DefineProperty(Napi::PropertyDescriptor::Value(Napi::Symbol::For(env, "libsonare.refusal"),
                                                       record, napi_configurable));
}

Napi::Value RenderRefusal(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  Napi::Object record;
  Napi::Array names;
  sonare_c_detail::RefusalParts parts;
  if (!RequiredObjectValue(env, info[0], "refusal", &record) ||
      !RequiredArrayValue(env, info[1], "subjects", &names) ||
      !RefusalFromObject(env, record, &parts)) {
    return env.Undefined();
  }
  const sonare::SonareParameterRefusal refusal = sonare_c_detail::refusal_from_parts(parts);
  std::vector<std::string> subjects;
  const uint32_t name_count = node_list_length(names, "subjects");
  for (uint32_t i = 0; i < name_count; ++i) {
    std::string subject;
    if (!RequiredStringValue(env, names.Get(i), "subjects", &subject)) return env.Undefined();
    subjects.push_back(std::move(subject));
  }
  std::vector<const char*> pointers;
  for (const std::string& subject : subjects) pointers.push_back(subject.c_str());
  std::vector<double> scales;
  Napi::Array factors;
  if (!RequiredArrayValue(env, info[2], "scales", &factors)) return env.Undefined();
  const uint32_t factor_count = node_list_length(factors, "scales");
  for (uint32_t i = 0; i < factor_count; ++i) {
    double scale = 0.0;
    if (!RequiredDoubleValue(env, factors.Get(i), "scales", &scale)) return env.Undefined();
    scales.push_back(scale);
  }
  const sonare::SonareParameterRefusal rendered = sonare_c_detail::respelled_refusal(
      refusal, pointers.data(), scales.empty() ? nullptr : scales.data(), pointers.size());
  Napi::Object out = Napi::Object::New(env);
  out.Set("message", RefusalText(rendered));
  out.Set("refusal", RefusalObject(env, rendered));
  return out;
  SONARE_NODE_CATCH(env)
}

const char* ErrorCodeName(SonareError err) {
  switch (err) {
    case SONARE_OK:
      return "Ok";
    case SONARE_ERROR_FILE_NOT_FOUND:
      return "FileNotFound";
    case SONARE_ERROR_INVALID_FORMAT:
      return "InvalidFormat";
    case SONARE_ERROR_DECODE_FAILED:
      return "DecodeFailed";
    case SONARE_ERROR_INVALID_PARAMETER:
      return "InvalidParameter";
    case SONARE_ERROR_OUT_OF_MEMORY:
      return "OutOfMemory";
    case SONARE_ERROR_NOT_SUPPORTED:
      return "NotSupported";
    case SONARE_ERROR_INVALID_STATE:
      return "InvalidState";
    case SONARE_ERROR_CANCELLED:
      return "Cancelled";
    case SONARE_ERROR_ENCODE_FAILED:
      return "EncodeFailed";
    case SONARE_ERROR_ABI_MISMATCH:
      return "AbiMismatch";
    case SONARE_ERROR_STALE_RESULT:
      return "StaleResult";
    case SONARE_ERROR_UNKNOWN:
    default:
      return "Unknown";
  }
}

SonareError CErrorFromException(const sonare::SonareException& e) {
  return sonare_c_detail::c_error_from_error_code(e.code());
}

sonare::ErrorCode CodeFromCError(SonareError err) {
  return sonare_c_detail::error_code_from_c_error(err);
}

namespace {

bool IsKnownErrorReason(const char* reason) {
  if (reason == nullptr) return true;
  constexpr std::array<sonare::ErrorReason, 10> kReasons = {
      sonare::ErrorReason::MalformedJson,      sonare::ErrorReason::UnsupportedSchemaVersion,
      sonare::ErrorReason::NotCompiledIn,      sonare::ErrorReason::NotPersistable,
      sonare::ErrorReason::UnresolvedSource,   sonare::ErrorReason::UnresolvedSoundFont,
      sonare::ErrorReason::DeviceUnavailable,  sonare::ErrorReason::DeviceFormatRefused,
      sonare::ErrorReason::EngineDeviceDriven, sonare::ErrorReason::ReleasedHandle,
  };
  return std::any_of(kReasons.begin(), kReasons.end(), [reason](sonare::ErrorReason candidate) {
    return std::strcmp(reason, sonare::error_reason_token(candidate)) == 0;
  });
}

std::string UnknownErrorReasonMessage(const char* reason) {
  return "unknown error reason token: " + std::string(reason);
}

void SetSonareErrorProperties(Napi::Env env, Napi::Object error, SonareError err,
                              const char* reason) {
  error.Set("name", Napi::String::New(env, "SonareError"));
  error.Set("code", Napi::Number::New(env, static_cast<double>(static_cast<int>(err))));
  error.Set("codeName", Napi::String::New(env, ErrorCodeName(err)));
  if (reason != nullptr) {
    error.Set("reason", Napi::String::New(env, reason));
  } else {
    error.Delete("reason");
  }
}

}  // namespace

Napi::Error MakeSonareError(Napi::Env env, SonareError err, const std::string& message,
                            const char* reason) {
  return MakeSonareError(env, err, message, false, reason);
}

Napi::Error MakeSonareError(Napi::Env env, SonareError err, const std::string& message,
                            bool shape_refusal, const char* reason) {
  std::string translated_message = message;
  if (!IsKnownErrorReason(reason)) {
    err = SONARE_ERROR_INVALID_FORMAT;
    shape_refusal = false;
    translated_message = UnknownErrorReasonMessage(reason);
    reason = nullptr;
  }
  if (err == SONARE_ERROR_INVALID_PARAMETER) {
    if (shape_refusal) return Napi::TypeError::New(env, translated_message);
    return Napi::RangeError::New(env, translated_message);
  }
  Napi::Error error = Napi::Error::New(env, translated_message);
  SetSonareErrorProperties(env, error.Value(), err, reason);
  return error;
}

namespace {

/// Throws a failed C-ABI call with the refusal, entry and reason captured by its caller.
void ThrowWithLastRefusal(Napi::Env env, SonareError err, const std::string& message,
                          const char* reason, bool typed_document = false) {
  const auto shown = sonare_field_spelling::node::SpelledLastRefusal({}, typed_document);
  const bool reclassified = typed_document && err == SONARE_ERROR_INVALID_FORMAT && shown &&
                            shown->code() == sonare::ErrorCode::InvalidParameter;
  Napi::Error error = MakeSonareError(env, reclassified ? SONARE_ERROR_INVALID_PARAMETER : err,
                                      reclassified ? RefusalText(*shown) : message,
                                      shown && shown->shape_error(), reason);
  if (shown && IsKnownErrorReason(reason)) {
    AttachRefusal(env, error.Value().As<Napi::Object>(), *shown, LastRefusalEntry());
  }
  error.ThrowAsJavaScriptException();
}

}  // namespace

void ThrowSonareError(Napi::Env env, SonareError err, const std::string& prefix,
                      bool typed_document) {
  if (env.IsExceptionPending()) return;
  const char* reason = sonare_last_error_reason();
  ThrowWithLastRefusal(env, err, prefix + ErrorMessageForCode(err), reason, typed_document);
}

void ThrowSonareErrorPreservingRefusal(Napi::Env env, SonareError err, const std::string& prefix) {
  if (env.IsExceptionPending()) return;
  if (err == SONARE_OK) {
    err = sonare_last_error_code();
    if (err == SONARE_OK) err = SONARE_ERROR_UNKNOWN;
  }
  const char* reason = sonare_last_error_reason();
  const sonare::SonareParameterRefusal* refusal = sonare_c_detail::last_parameter_refusal();
  if (refusal == nullptr) {
    ThrowWithLastRefusal(env, err, prefix + ErrorMessageForCode(err), reason);
    return;
  }
  const sonare::SonareParameterRefusal shown = refusal->as_caller_spelled();
  // The code stays the C call's: a document refusal is InvalidFormat, not an argument error.
  Napi::Error error =
      MakeSonareError(env, err, prefix + RefusalText(shown), shown.shape_error(), reason);
  const Napi::Object value = error.Value().As<Napi::Object>();
  AttachRefusal(env, value, shown, LastRefusalEntry());
  error.ThrowAsJavaScriptException();
}

void ThrowSonareErrorFlattening(Napi::Env env, SonareError err, const char* c_root) {
  if (env.IsExceptionPending()) return;
  const std::optional<sonare::SonareParameterRefusal> shown =
      sonare_field_spelling::node::SpelledLastRefusal(c_root);
  if (!shown) {
    ThrowSonareError(env, err);
    return;
  }
  const char* reason = sonare_last_error_reason();
  Napi::Error error = MakeSonareError(env, err, RefusalText(*shown), shown->shape_error(), reason);
  const Napi::Object value = error.Value().As<Napi::Object>();
  if (IsKnownErrorReason(reason)) AttachRefusal(env, value, *shown, LastRefusalEntry());
  error.ThrowAsJavaScriptException();
}

void ThrowSonareErrorMessage(Napi::Env env, SonareError err, const std::string& message) {
  if (env.IsExceptionPending()) return;
  MakeSonareError(env, err, message).ThrowAsJavaScriptException();
}

void ThrowHandleClosed(Napi::Env env, const char* what) {
  ThrowSonareErrorMessage(env, SONARE_ERROR_INVALID_STATE,
                          std::string(what) + " has been destroyed");
}

void MarkPendingForeign(Napi::Env env) {
  if (!env.IsExceptionPending()) return;
  Napi::Error error = env.GetAndClearPendingException();
  const Napi::Value value = error.Value();
  if (value.IsObject()) {
    value.As<Napi::Object>().DefineProperty(
        Napi::PropertyDescriptor::Value(Napi::Symbol::For(env, "libsonare.foreignError"),
                                        Napi::Boolean::New(env, true), napi_configurable));
    // A frozen value keeps no mark; the caller's own throw still wins over the failed define.
    if (env.IsExceptionPending()) env.GetAndClearPendingException();
  }
  error.ThrowAsJavaScriptException();
}

void ThrowCurrentException(Napi::Env env) {
  try {
    throw;
  } catch (const Napi::Error& e) {
    e.ThrowAsJavaScriptException();
  } catch (const std::exception& e) {
    ThrowStdException(env, e);
  } catch (...) {
    ThrowSonareErrorMessage(env, SONARE_ERROR_UNKNOWN, "Unknown error");
  }
}

Napi::Promise RejectedFromPending(Napi::Env env) {
  // Cleared first: napi_create_promise refuses to run while an exception is pending.
  const Napi::Error pending = env.GetAndClearPendingException();
  Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);
  deferred.Reject(pending.Value());
  return deferred.Promise();
}

void ThrowLastSonareError(Napi::Env env, const std::string& prefix, SonareError fallback) {
  SonareError code = sonare_last_error_code();
  if (code == SONARE_OK) code = fallback;
  if (env.IsExceptionPending()) return;
  const char* reason = sonare_last_error_reason();
  ThrowWithLastRefusal(env, code, prefix + ErrorMessageForCode(code), reason);
}

Napi::Value JoinedNamesToArray(Napi::Env env, const char* joined, const std::string& what) {
  Napi::Array out = Napi::Array::New(env);
  if (joined == nullptr || joined[0] == '\0') {
    if (sonare_last_error_code() != SONARE_OK) {
      ThrowLastSonareError(env, what + ": ", SONARE_ERROR_UNKNOWN);
      return env.Undefined();
    }
    return out;
  }
  const std::string names(joined);
  uint32_t index = 0;
  size_t start = 0;
  while (start <= names.size()) {
    const size_t end = names.find('\n', start);
    out.Set(index++,
            names.substr(start, end == std::string::npos ? std::string::npos : end - start));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return out;
}

void ThrowStdException(Napi::Env env, const std::exception& e) {
  if (const auto* coded = dynamic_cast<const sonare::SonareException*>(&e)) {
    if (env.IsExceptionPending()) return;
    const char* reason = coded->reason() ? sonare::error_reason_token(*coded->reason()) : nullptr;
    Napi::Error error = MakeSonareError(env, CErrorFromException(*coded), ExceptionMessage(e),
                                        IsShapeRefusal(e), reason);
    if (const auto* refusal = dynamic_cast<const sonare::SonareParameterRefusal*>(&e)) {
      AttachRefusal(env, error.Value(), sonare::util::check::camel_spelled(*refusal), "");
    }
    error.ThrowAsJavaScriptException();
    return;
  }
  ThrowSonareErrorMessage(env, static_cast<SonareError>(sonare::error_code_for_std_exception(e)),
                          ExceptionMessage(e));
}

void DecorateSonareError(Napi::Env env, Napi::Object error, SonareError err, bool shape_refusal,
                         const std::optional<sonare::SonareParameterRefusal>& shown,
                         const std::string& c_entry, const char* reason) {
  if (!IsKnownErrorReason(reason)) {
    const Napi::Object translated = MakeSonareError(env, err, "", shape_refusal, reason).Value();
    for (const char* property : {"message", "name", "code", "codeName"}) {
      error.Set(property, translated.Get(property));
    }
    error.Delete("reason");
    return;
  }
  if (shown) AttachRefusal(env, error, *shown, c_entry);
  if (err == SONARE_ERROR_INVALID_PARAMETER) {
    // The async worker built a plain Error; re-class it as the TypeError or RangeError the
    // synchronous path throws, so both report a refused argument alike.
    Napi::Object proto = env.Global()
                             .Get(shape_refusal ? "TypeError" : "RangeError")
                             .As<Napi::Object>()
                             .Get("prototype")
                             .As<Napi::Object>();
    Napi::Object object_ctor = env.Global().Get("Object").As<Napi::Object>();
    object_ctor.Get("setPrototypeOf").As<Napi::Function>().Call(object_ctor, {error, proto});
    return;
  }
  SetSonareErrorProperties(env, error, err, reason);
}

bool ReadWholeHertzSampleRate(Napi::Env env, const Napi::Value& value, int* out) {
  if (out == nullptr || !value.IsNumber()) {
    Napi::TypeError::New(env, "sampleRate must be a number").ThrowAsJavaScriptException();
    return false;
  }
  const double sample_rate = value.As<Napi::Number>().DoubleValue();
  if (!sonare::is_supported_sample_rate(sample_rate)) {
    Napi::RangeError::New(
        env, "sampleRate " + sonare::util::refusal::integer_range_predicate(
                                 sonare::kMinAudioSampleRate, sonare::kMaxAudioSampleRate))
        .ThrowAsJavaScriptException();
    return false;
  }
  *out = static_cast<int>(sample_rate);
  return true;
}

bool ReadMeterCandidateNumerators(const Napi::Object& object, const char* key,
                                  int (&numerators)[SONARE_MAX_METER_CANDIDATE_NUMERATORS],
                                  size_t* count) {
  Napi::Env env = object.Env();
  const Napi::Value value = object.Get(key);
  if (value.IsUndefined() || value.IsNull()) return true;
  if (!value.IsArray()) {
    Napi::TypeError::New(env, std::string(key) + " must be an array").ThrowAsJavaScriptException();
    return false;
  }

  const Napi::Array array = value.As<Napi::Array>();
  const uint32_t length = node_list_length(array, std::string(key));
  if (length > static_cast<uint32_t>(SONARE_MAX_METER_CANDIDATE_NUMERATORS)) {
    Napi::RangeError::New(env, std::string(key) + " must hold at most " +
                                   std::to_string(SONARE_MAX_METER_CANDIDATE_NUMERATORS) +
                                   " entries")
        .ThrowAsJavaScriptException();
    return false;
  }

  // Staged locally so a rejected entry leaves the defaults untouched; the
  // zero-initialized tail also clears the default entries past the new count.
  int staged[SONARE_MAX_METER_CANDIDATE_NUMERATORS] = {};
  for (uint32_t i = 0; i < length; ++i) {
    if (!RequiredIntValue(env, array.Get(i), std::string(key) + "[" + std::to_string(i) + "]",
                          &staged[i])) {
      return false;
    }
  }
  std::copy(std::begin(staged), std::end(staged), numerators);
  *count = length;
  return true;
}

bool ReadMusicAnalyzeOptions(const Napi::Value& value, SonareMusicAnalyzeOptions* options) {
  if (options == nullptr) return false;
  sonare_music_analyze_options_init(options);
  if (!value.IsObject()) return false;

  const Napi::Object object = value.As<Napi::Object>();
  // Every field below is a plain quantity or flag -- none is documented (in
  // analysis.ts's MusicAnalyzeOptions) as reading a wrong-typed value as
  // "unspecified" -- so all of them belong on the refuse-by-name *Property
  // family, matching nFft/hopLength rather than diverging from them.
  options->n_fft = IntProperty(object, "nFft", options->n_fft);
  options->hop_length =
      DomainProperty(object, "hopLength", options->hop_length, sonare::kMusicCountDomain);
  options->bpm_min = FloatProperty(object, "bpmMin", options->bpm_min);
  options->bpm_max = FloatProperty(object, "bpmMax", options->bpm_max);
  options->start_bpm = FloatProperty(object, "startBpm", options->start_bpm);
  options->use_triads_only =
      BoolProperty(object, "useTriadsOnly", options->use_triads_only != 0) ? 1 : 0;
  options->use_hpss = BoolProperty(object, "useHpss", options->use_hpss != 0) ? 1 : 0;
  options->chroma_high_pass_hz =
      FloatProperty(object, "chromaHighPassHz", options->chroma_high_pass_hz);
  options->use_bass_weighted =
      BoolProperty(object, "useBassWeighted", options->use_bass_weighted != 0) ? 1 : 0;
  options->chroma_hop_multiplier = DomainProperty(
      object, "chromaHopMultiplier", options->chroma_hop_multiplier, sonare::kMusicCountDomain);
  options->use_chord_hmm = BoolProperty(object, "useChordHmm", options->use_chord_hmm != 0) ? 1 : 0;
  options->use_chord_key_context =
      BoolProperty(object, "useChordKeyContext", options->use_chord_key_context != 0) ? 1 : 0;
  options->chord_hmm_beam_width =
      DomainProperty(object, "chordHmmBeamWidth", options->chord_hmm_beam_width,
                     sonare::kMusicChordBeamWidthDomain);
  options->detect_chord_inversions =
      BoolProperty(object, "detectChordInversions", options->detect_chord_inversions != 0) ? 1 : 0;
  options->adaptive_tempo =
      BoolProperty(object, "adaptiveTempo", options->adaptive_tempo != 0) ? 1 : 0;
  options->tempo_update_interval_beats =
      DomainProperty(object, "tempoUpdateIntervalBeats", options->tempo_update_interval_beats,
                     sonare::kMusicCountDomain);
  options->compute_tempo_curve =
      BoolProperty(object, "computeTempoCurve", options->compute_tempo_curve != 0) ? 1 : 0;
  options->meter_denominator = IntProperty(object, "meterDenominator", options->meter_denominator);
  // Absent keeps the defaults' measured tuning; a given number is used as given.
  bool tuning_auto = false;
  options->tuning_cents =
      AutoFiniteFloatProperty(object, "tuningCents", options->tuning_cents, &tuning_auto);
  const Napi::Value tuning_value = object.Get("tuningCents");
  if (!tuning_value.IsUndefined() && !tuning_value.IsNull()) {
    options->tuning_auto = tuning_auto ? 1 : 0;
  }
  return ReadMeterCandidateNumerators(object, "meterCandidateNumerators",
                                      options->meter_candidate_numerators,
                                      &options->meter_candidate_numerator_count);
}

bool ReadHpssArguments(Napi::Env env, const Napi::CallbackInfo& info, HpssArguments* out) {
  const sonare::HpssConfig defaults;
  if (!OptionalIntArg(env, info, 2, "kernelHarmonic", defaults.kernel_size_harmonic,
                      &out->kernel_harmonic) ||
      !OptionalIntArg(env, info, 3, "kernelPercussive", defaults.kernel_size_percussive,
                      &out->kernel_percussive) ||
      !OptionalIntArg(env, info, 4, "nFft", kDefaultNFft, &out->n_fft) ||
      !OptionalResynthesisHopArg(env, info, 5, &out->hop_length) ||
      !OptionalBoolArg(env, info, 6, "hardMask", false, &out->hard_mask)) {
    return false;
  }
  return true;
}

Napi::Object MidiEventToObject(Napi::Env env, const SonareMidiEventPod& event) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("ppq", Napi::Number::New(env, event.ppq));
  out.Set("word0", Napi::Number::New(env, event.word0));
  out.Set("word1", Napi::Number::New(env, event.word1));
  return out;
}

bool ValidateMatrixDims(Napi::Env env, int rows, int cols, size_t length) {
  if (rows < 0 || cols < 0) {
    Napi::RangeError::New(env, "dimensions must be integers in [0, " + node_int_max_text() + "]")
        .ThrowAsJavaScriptException();
    return false;
  }
  const size_t expected = static_cast<size_t>(rows) * static_cast<size_t>(cols);
  if (expected != length) {
    Napi::RangeError::New(env, "rows*cols (" + std::to_string(expected) +
                                   ") must equal input length (" + std::to_string(length) + ")")
        .ThrowAsJavaScriptException();
    return false;
  }
  return true;
}

const char* PitchClassNameLocal(Napi::Env env, SonarePitchClass pc) {
  static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  const int idx = static_cast<int>(pc);
  if (idx < SONARE_PITCH_C || idx > SONARE_PITCH_B) {
    throw MakeSonareError(env, SONARE_ERROR_INVALID_FORMAT,
                          "unknown pitch class ordinal: " + std::to_string(idx));
  }
  return names[idx];
}

// Resolves a pitch class given as an ordinal in [0, 12) or as its name ("C", "C#", ..., "B").
SonarePitchClass PitchClassFromValue(const Napi::Value& value, const char* what) {
  const Napi::Env env = value.Env();
  if (value.IsNumber()) {
    const int ordinal = node_narrow_int(env, value, what);
    if (ordinal < SONARE_PITCH_C || ordinal > SONARE_PITCH_B) {
      throw Napi::RangeError::New(env, std::string(what) + " must be in [0, 12)");
    }
    return static_cast<SonarePitchClass>(ordinal);
  }
  if (!value.IsString()) {
    throw Napi::TypeError::New(env, std::string(what) + " must be a pitch class name or number");
  }
  const std::string name = node_narrow_string(env, value, what);
  for (int pc = SONARE_PITCH_C; pc <= SONARE_PITCH_B; ++pc) {
    if (name == PitchClassNameLocal(env, static_cast<SonarePitchClass>(pc))) {
      return static_cast<SonarePitchClass>(pc);
    }
  }
  throw Napi::RangeError::New(env,
                              std::string(what) + " is not a pitch class name: '" + name + "'");
}

const char* ModeNameLocal(SonareMode mode) {
  switch (mode) {
    case SONARE_MODE_MAJOR:
      return "major";
    case SONARE_MODE_MINOR:
      return "minor";
    case SONARE_MODE_DORIAN:
      return "dorian";
    case SONARE_MODE_PHRYGIAN:
      return "phrygian";
    case SONARE_MODE_LYDIAN:
      return "lydian";
    case SONARE_MODE_MIXOLYDIAN:
      return "mixolydian";
    case SONARE_MODE_LOCRIAN:
      return "locrian";
    default:
      return "unknown";
  }
}

const char* ChordQualityName(SonareChordQuality quality) {
  switch (quality) {
    case SONARE_CHORD_MAJOR:
      return "major";
    case SONARE_CHORD_MINOR:
      return "minor";
    case SONARE_CHORD_DIMINISHED:
      return "diminished";
    case SONARE_CHORD_AUGMENTED:
      return "augmented";
    case SONARE_CHORD_DOMINANT7:
      return "dominant7";
    case SONARE_CHORD_MAJOR7:
      return "major7";
    case SONARE_CHORD_MINOR7:
      return "minor7";
    case SONARE_CHORD_SUS2:
      return "sus2";
    case SONARE_CHORD_SUS4:
      return "sus4";
    case SONARE_CHORD_UNKNOWN:
      return "unknown";
    case SONARE_CHORD_ADD9:
      return "add9";
    case SONARE_CHORD_MINOR_ADD9:
      return "minorAdd9";
    case SONARE_CHORD_DIM7:
      return "dim7";
    case SONARE_CHORD_HALF_DIM7:
      return "halfDim7";
    case SONARE_CHORD_MAJOR9:
      return "major9";
    case SONARE_CHORD_DOMINANT9:
      return "dominant9";
    case SONARE_CHORD_SUS2_ADD4:
      return "sus2Add4";
    case SONARE_CHORD_MAJOR6:
      return "major6";
    case SONARE_CHORD_MINOR6:
      return "minor6";
    case SONARE_CHORD_MINOR_MAJOR7:
      return "minorMajor7";
    case SONARE_CHORD_DOMINANT7_SUS4:
      return "dominant7Sus4";
    case SONARE_CHORD_DOMINANT11:
      return "dominant11";
    case SONARE_CHORD_DOMINANT13:
      return "dominant13";
    case SONARE_CHORD_DOMINANT7_FLAT9:
      return "dominant7Flat9";
    case SONARE_CHORD_DOMINANT7_SHARP9:
      return "dominant7Sharp9";
  }
  return "unknown";
}

SonareChordQuality ChordQualityFromValue(const Napi::Value& value, const char* what) {
  const Napi::Env env = value.Env();
  if (value.IsNumber()) {
    const int ordinal = node_narrow_int(env, value, what);
    if (ordinal < 0 || ordinal >= SONARE_CHORD_QUALITY_COUNT) {
      throw Napi::RangeError::New(env, std::string(what) + " is not a chord quality");
    }
    return static_cast<SonareChordQuality>(ordinal);
  }
  if (!value.IsString()) {
    throw Napi::TypeError::New(env, std::string(what) + " must be a chord quality name or number");
  }
  const std::string name = node_narrow_string(env, value, what);
  for (int quality = 0; quality < SONARE_CHORD_QUALITY_COUNT; ++quality) {
    if (name == ChordQualityName(static_cast<SonareChordQuality>(quality))) {
      return static_cast<SonareChordQuality>(quality);
    }
  }
  throw Napi::RangeError::New(env, std::string(what) + " is not a chord quality: '" + name + "'");
}

Napi::Object KeyToObject(Napi::Env env, SonarePitchClass root, SonareMode mode, float confidence) {
  Napi::Object key = Napi::Object::New(env);
  const int mode_ordinal = static_cast<int>(mode);
  if (mode_ordinal < SONARE_MODE_MAJOR || mode_ordinal > SONARE_MODE_LOCRIAN) {
    throw MakeSonareError(env, SONARE_ERROR_INVALID_FORMAT,
                          "unknown key mode ordinal: " + std::to_string(mode_ordinal));
  }
  std::string root_name = PitchClassNameLocal(env, root);
  std::string mode_name = ModeNameLocal(mode);
  key.Set("root", Napi::Number::New(env, static_cast<double>(root)));
  key.Set("mode", Napi::String::New(env, mode_name));
  key.Set("confidence", Napi::Number::New(env, static_cast<double>(confidence)));
  key.Set("name", Napi::String::New(env, root_name + " " + mode_name));
  const std::string short_name =
      mode == SONARE_MODE_MAJOR
          ? root_name
          : (mode == SONARE_MODE_MINOR ? root_name + "m" : root_name + " " + mode_name);
  key.Set("shortName", Napi::String::New(env, short_name));
  return key;
}

std::vector<sonare::mastering::api::Param> ParamsFromObject(
    const Napi::Object& object, const std::vector<std::string>& skip_keys) {
  std::vector<sonare::mastering::api::Param> params;
  Napi::Env env = object.Env();
  Napi::Array names = object.GetPropertyNames();
  const uint32_t name_count = node_list_length(names, "params");
  for (uint32_t index = 0; index < name_count; ++index) {
    Napi::Value key_value = names.Get(index);
    Napi::Value value = object.Get(key_value);
    const std::string key = node_narrow_string(env, key_value, "parameter key");
    if (std::find(skip_keys.begin(), skip_keys.end(), key) != skip_keys.end()) continue;
    if (value.IsNumber()) {
      params.push_back({key, value.As<Napi::Number>().DoubleValue()});
    } else if (value.IsBoolean()) {
      params.push_back({key, value.As<Napi::Boolean>().Value() ? 1.0 : 0.0});
    } else {
      Napi::TypeError::New(
          env, "Parameter '" + key + "' must be a number or boolean (got an unsupported type)")
          .ThrowAsJavaScriptException();
      return params;
    }
  }
  return params;
}

std::vector<SonareMasteringParam> MasteringCParams(
    const std::vector<sonare::mastering::api::Param>& params) {
  std::vector<SonareMasteringParam> out;
  out.reserve(params.size());
  for (const auto& param : params) {
    out.push_back({param.key.c_str(), param.value});
  }
  return out;
}

bool EnrichFullAnalysisObject(Napi::Env env, Napi::Object result, Napi::Error* error) {
  // The full-analysis JSON carries chord ordinals only; add the spelled names detectChords
  // reports.
  Napi::Value chords_val = result.Get("chords");
  if (chords_val.IsArray()) {
    Napi::Array chords = chords_val.As<Napi::Array>();
    const uint32_t chord_count = node_list_length(chords, "chords");
    for (uint32_t i = 0; i < chord_count; ++i) {
      Napi::Value entry = chords.Get(i);
      if (!entry.IsObject()) continue;
      Napi::Object chord = entry.As<Napi::Object>();
      Napi::Value root = chord.Get("root");
      Napi::Value bass = chord.Get("bass");
      if (root.IsNumber()) {
        chord.Set("rootName",
                  Napi::String::New(
                      env, PitchClassNameLocal(env, static_cast<SonarePitchClass>(
                                                        node_narrow_int(env, root, "root")))));
      }
      if (bass.IsNumber()) {
        chord.Set("bassName",
                  Napi::String::New(
                      env, PitchClassNameLocal(env, static_cast<SonarePitchClass>(
                                                        node_narrow_int(env, bass, "bass")))));
      }
    }
  }

  Napi::Value key_val = result.Get("key");
  if (key_val.IsObject()) {
    Napi::Object key_obj = key_val.As<Napi::Object>();
    Napi::Value root = key_obj.Get("root");
    Napi::Value mode = key_obj.Get("mode");
    Napi::Value confidence = key_obj.Get("confidence");
    if (root.IsNumber() && mode.IsNumber()) {
      result.Set("key", KeyToObject(
                            env, static_cast<SonarePitchClass>(node_narrow_int(env, root, "root")),
                            static_cast<SonareMode>(node_narrow_int(env, mode, "mode")),
                            confidence.IsNumber() ? node_narrow_float(env, confidence, "confidence")
                                                  : 0.0f));
    }
  }

  if (env.IsExceptionPending()) {
    if (error != nullptr) *error = env.GetAndClearPendingException();
    return false;
  }
  return true;
}

Napi::Value ParseJsonObjectAndFree(Napi::Env env, char* json, const char* failure_message) {
  // Parse on the main thread using the JS engine's built-in parser (V8 is not
  // thread-safe), then release the heap-allocated C string either way.
  Napi::Object json_global = env.Global().Get("JSON").As<Napi::Object>();
  Napi::Function json_parse = json_global.Get("parse").As<Napi::Function>();
  Napi::Value parsed = json_parse.Call({Napi::String::New(env, json != nullptr ? json : "")});
  sonare_free_string(json);

  if (env.IsExceptionPending() || !parsed.IsObject()) {
    if (!env.IsExceptionPending()) {
      ThrowSonareErrorMessage(env, SONARE_ERROR_INVALID_FORMAT, failure_message);
    }
    return env.Undefined();
  }
  return parsed;
}

Napi::Value FullAnalysisJsonToObject(Napi::Env env, const float* data, size_t length,
                                     int sample_rate, const SonareMusicAnalyzeOptions* options) {
  char* json_str = nullptr;
  SonareError err = sonare_analyze_json(data, length, sample_rate, options, &json_str);
  if (err != SONARE_OK) {
    // Same classification as the async analyze path (RangeError for a refused
    // argument, coded SonareError otherwise).
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }

  Napi::Value parsed = ParseJsonObjectAndFree(env, json_str, "Failed to parse analysis JSON");
  if (env.IsExceptionPending()) return env.Undefined();

  Napi::Object result = parsed.As<Napi::Object>();
  Napi::Error enrich_error;
  if (!EnrichFullAnalysisObject(env, result, &enrich_error)) {
    enrich_error.ThrowAsJavaScriptException();
    return env.Undefined();
  }

  return result;
}

bool ReadChannelInputs(Napi::Env env, const Napi::Value& value, ChannelInputs* inputs) {
  Napi::Array channels;
  if (!RequiredPlaneListValue(env, value, "channels", &channels)) return false;
  const uint32_t count = node_list_length(channels, "channels");
  inputs->arrays.reserve(count);
  inputs->ptrs.reserve(count);
  for (uint32_t index = 0; index < count; ++index) {
    Napi::Float32Array channel;
    if (!RequiredFloat32ArrayValue(env, channels.Get(index),
                                   "channels[" + std::to_string(index) + "]", &channel)) {
      return false;
    }
    inputs->arrays.push_back(channel);
    const size_t length = inputs->arrays.back().ElementLength();
    if (index == 0) {
      inputs->length = length;
    } else if (length != inputs->length) {
      Napi::RangeError::New(
          env, "channels[" + std::to_string(index) + "] must have the same length as channels[0]")
          .ThrowAsJavaScriptException();
      return false;
    }
    inputs->ptrs.push_back(sonare_node::ElementData(inputs->arrays.back()));
  }
  return true;
}

Napi::Object MixMeterSnapshotToObject(Napi::Env env, const SonareMixMeterSnapshot& snapshot) {
  Napi::Object out = Napi::Object::New(env);
  SetMeasured(out, "correlation", snapshot.correlation);
  SetMeasured(out, "monoCompatWidth", snapshot.mono_compat_width);
  out.Set("monoCompatPeak", snapshot.mono_compat_peak);
  out.Set("monoCompatSideRms", snapshot.mono_compat_side_rms);
  out.Set("likelyMonoCompatible", snapshot.likely_mono_compatible != 0);
  SetMeasured(out, "momentaryLufs", snapshot.momentary_lufs);
  SetMeasured(out, "shortTermLufs", snapshot.short_term_lufs);
  SetMeasured(out, "integratedLufs", snapshot.integrated_lufs);
  out.Set("gainReductionDb", snapshot.gain_reduction_db);
  SetMeasured(out, "maxTruePeakDbtp", snapshot.max_true_peak_dbtp);
  out.Set("seq", Napi::Number::New(env, static_cast<double>(snapshot.seq)));

  // Expose only the valid planes and keep unmeasured entries in place.
  // Clamp the count so a bad value cannot index past the fixed buffers.
  int channel_count = snapshot.num_channels;
  if (channel_count < 0) channel_count = 0;
  if (channel_count > SONARE_MAX_METER_CHANNELS) channel_count = SONARE_MAX_METER_CHANNELS;
  out.Set("numChannels", Napi::Number::New(env, channel_count));
  Napi::Array peak_db = Napi::Array::New(env, channel_count);
  Napi::Array rms_db = Napi::Array::New(env, channel_count);
  Napi::Array true_peak_db = Napi::Array::New(env, channel_count);
  for (int ch = 0; ch < channel_count; ++ch) {
    peak_db.Set(ch, MeasuredNumber(env, snapshot.peak_db[ch]));
    rms_db.Set(ch, MeasuredNumber(env, snapshot.rms_db[ch]));
    true_peak_db.Set(ch, MeasuredNumber(env, snapshot.true_peak_dbtp[ch]));
  }
  out.Set("peakDb", peak_db);
  out.Set("rmsDb", rms_db);
  out.Set("truePeakDbtp", true_peak_db);
  return out;
}

}  // namespace sonare_node
