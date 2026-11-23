#pragma once

/// @file error_reason.h
/// @brief Stable reasons attached to coded library errors.

namespace sonare {

/// @brief A machine-readable cause for an error code with multiple causes.
enum class ErrorReason {
  MalformedJson,
  UnsupportedSchemaVersion,
  NotCompiledIn,
  NotPersistable,
  UnresolvedSource,
  UnresolvedSoundFont,
  DeviceUnavailable,
  DeviceFormatRefused,
  EngineDeviceDriven,
  ReleasedHandle,
};

/// @brief Returns the lower-camel-case token for @p reason.
inline const char* error_reason_token(ErrorReason reason) {
  switch (reason) {
    case ErrorReason::MalformedJson:
      return "malformedJson";
    case ErrorReason::UnsupportedSchemaVersion:
      return "unsupportedSchemaVersion";
    case ErrorReason::NotCompiledIn:
      return "notCompiledIn";
    case ErrorReason::NotPersistable:
      return "notPersistable";
    case ErrorReason::UnresolvedSource:
      return "unresolvedSource";
    case ErrorReason::UnresolvedSoundFont:
      return "unresolvedSoundFont";
    case ErrorReason::DeviceUnavailable:
      return "deviceUnavailable";
    case ErrorReason::DeviceFormatRefused:
      return "deviceFormatRefused";
    case ErrorReason::EngineDeviceDriven:
      return "engineDeviceDriven";
    case ErrorReason::ReleasedHandle:
      return "releasedHandle";
  }
  return nullptr;
}

}  // namespace sonare
