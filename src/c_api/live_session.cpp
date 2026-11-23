#include <sonare/sonare_c_engine.h>

#include "c_api/sonare_c_internal.h"

#if defined(SONARE_WITH_LIVE_DEVICES) && defined(SONARE_WITH_ARRANGEMENT)
#define SONARE_C_LIVE_AVAILABLE
#include <algorithm>
#include <cstring>
#include <memory>
#include <string>

#include "c_api/engine_config_domains.h"
#include "host/live_session.h"

struct SonareLiveSession {
  sonare::host::LiveSession host;
};
#endif

using namespace sonare_c_detail;

int32_t sonare_has_live_devices(void) {
#if defined(SONARE_C_LIVE_AVAILABLE)
  return 1;
#else
  return 0;
#endif
}

void sonare_free_live_audio_device_list(SonareLiveAudioDeviceList* list) {
  if (list == nullptr) return;
  delete[] list->items;
  *list = {};
}

void sonare_free_live_midi_device_list(SonareLiveMidiDeviceList* list) {
  if (list == nullptr) return;
  delete[] list->items;
  *list = {};
}

void sonare_live_session_config_init(SonareLiveSessionConfig* config) {
  if (config == nullptr) return;
  *config = {};
  config->struct_size = sizeof(*config);
  config->block_size = sonare::engine::RealtimeEngine::kDefaultOfflineBlock;
}

#if defined(SONARE_C_LIVE_AVAILABLE)
namespace {
namespace host = sonare::host;
namespace check = sonare::util::check;
using Selection = host::LiveSession::DeviceSelection;

template <size_t N>
void copy_display_name(char (&out)[N], const std::string& name) {
  size_t length = std::min(name.size(), N - 1);
  while (length > 0 && length < name.size() &&
         (static_cast<unsigned char>(name[length]) & 0xc0u) == 0x80u) {
    --length;
  }
  std::memcpy(out, name.data(), length);
  out[length] = '\0';
}

template <size_t N>
void copy_uid(char (&out)[N], const std::string& uid) {
  if (uid.size() >= N) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidFormat,
                                  "a device UID exceeds the live-device record size");
  }
  std::memcpy(out, uid.data(), uid.size());
  out[uid.size()] = '\0';
}

host::LiveSession::DeviceSelector selector(SonareLiveDeviceSelect value, const char* uid,
                                           const char* subject, const char* uid_subject,
                                           bool for_midi = false) {
  switch (value) {
    case SONARE_LIVE_DEVICE_BY_UID:
      check::non_null(uid, uid_subject);
      check::non_empty(uid, uid_subject);
      return {Selection::kByUid, uid};
    case SONARE_LIVE_DEVICE_DEFAULT:
      if (for_midi) check::refuse(subject, "must select a MIDI UID or no MIDI input");
      break;
    case SONARE_LIVE_DEVICE_NONE:
      break;
    default:
      check::refuse(subject, sonare::util::refusal::kDeclaredValuePredicate);
  }
  if (uid != nullptr) {
    check::refuse_relation({uid_subject, subject}, "{0} must be NULL unless {1} selects a UID");
  }
  return {value == SONARE_LIVE_DEVICE_DEFAULT ? Selection::kDefault : Selection::kNone, {}};
}

SonareError open_error(host::LiveOpenResult result) {
  using Open = host::LiveOpenResult;
  using Code = sonare::ErrorCode;
  using Reason = sonare::ErrorReason;
  const auto unavailable = [](const char* subject) -> SonareError {
    throw sonare::SonareParameterRefusal::predicate_with_reason(
        Code::InvalidState, subject, "is unavailable", Reason::DeviceUnavailable);
  };
  switch (result) {
    case Open::kOk:
      return SONARE_OK;
    case Open::kInvalidArgument:
      check::refuse("config", "must name a valid live device configuration");
    case Open::kMidiInputUnavailable:
      return unavailable("config.midi_input");
    case Open::kAudioInputUnavailable:
      return unavailable("config.audio_input");
    case Open::kAudioOutputUnavailable:
    case Open::kAudioOutputStartFailed:
      return unavailable("config.audio_output");
    case Open::kAudioFormatRefused:
    case Open::kEngineRefusedFormat:
      throw sonare::SonareParameterRefusal::predicate_with_reason(
          Code::NotSupported, "config.sample_rate", "must be supported by the engine and devices",
          Reason::DeviceFormatRefused);
    case Open::kEngineAlreadyDriven:
      throw sonare::SonareParameterRefusal::predicate_with_reason(
          Code::InvalidState, "engine", "is driven by a live device", Reason::EngineDeviceDriven);
    case Open::kMidiInputSourceNotOwned:
      throw sonare::SonareException(Code::InvalidState,
                                    "engine must own its current MIDI source to replace it");
    case Open::kOutOfMemory:
      set_last_error(SONARE_ERROR_OUT_OF_MEMORY, "the MIDI capture queue could not be allocated");
      return SONARE_ERROR_OUT_OF_MEMORY;
  }
  set_last_error(SONARE_ERROR_INVALID_STATE, "the live session could not be opened");
  return SONARE_ERROR_INVALID_STATE;
}
}  // namespace
#endif

SonareError sonare_live_audio_devices(SonareLiveAudioDeviceList* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
#if defined(SONARE_C_LIVE_AVAILABLE)
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_TRY
  const auto devices = host::backends::CoreAudioDevice::devices();
  StagedOutput<SonareLiveAudioDeviceList, sonare_free_live_audio_device_list> result;
  if (!devices.empty()) result->items = new SonareLiveAudioDevice[devices.size()]{};
  for (const auto& device : devices) {
    auto& row = result->items[result->count++];
    copy_uid(row.uid, device.uid);
    copy_display_name(row.name, device.name);
    row.input_channels = device.input_channels;
    row.output_channels = device.output_channels;
    row.is_default_input = device.is_default_input ? 1 : 0;
    row.is_default_output = device.is_default_output ? 1 : 0;
    row.nominal_sample_rate = device.nominal_sample_rate;
  }
  result.publish(out);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return refuse_not_compiled_in(Feature::kLiveDevices, out);
#endif
}

SonareError sonare_live_midi_sources(SonareLiveMidiDeviceList* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
#if defined(SONARE_C_LIVE_AVAILABLE)
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_TRY
  const auto devices = host::backends::CoreMidiInput::sources();
  StagedOutput<SonareLiveMidiDeviceList, sonare_free_live_midi_device_list> result;
  if (!devices.empty()) result->items = new SonareLiveMidiDevice[devices.size()]{};
  for (const auto& device : devices) {
    auto& row = result->items[result->count++];
    copy_uid(row.uid, device.uid);
    copy_display_name(row.name, device.name);
  }
  result.publish(out);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return refuse_not_compiled_in(Feature::kLiveDevices, out);
#endif
}

SonareError sonare_live_session_create(SonareEngine* engine, const SonareLiveSessionConfig* config,
                                       SonareLiveSession** out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
#if defined(SONARE_C_LIVE_AVAILABLE)
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL(config);
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_TRY
  require_struct_size(config->struct_size, sizeof(*config), "config",
                      "sonare_live_session_config_init");
  host::LiveSession::SelectorConfig settings;
  settings.audio_output = selector(config->audio_output, config->audio_output_uid,
                                   "config.audio_output", "config.audio_output_uid");
  settings.audio_input = selector(config->audio_input, config->audio_input_uid,
                                  "config.audio_input", "config.audio_input_uid");
  settings.midi_input = selector(config->midi_input, config->midi_input_uid, "config.midi_input",
                                 "config.midi_input_uid", true);
  if (settings.audio_output.selection == Selection::kNone &&
      (settings.audio_input.selection == Selection::kNone ||
       settings.midi_input.selection != Selection::kNone)) {
    check::refuse("config.audio_output",
                  "must select audio output when there is no audio input "
                  "or MIDI input is selected");
  }
  if (config->sample_rate != 0) {
    check::integer_within(config->sample_rate, kEngineSampleRateDomain, "config.sample_rate");
  }
  check::integer_within(config->block_size, kEngineMaxBlockSizeDomain, "config.block_size");
  settings.sample_rate = config->sample_rate;
  settings.block_size = config->block_size;
  settings.destination_id = config->destination_id;
  engine->engine.require_quiescent();
  auto session = std::make_unique<SonareLiveSession>();
  const SonareError result = open_error(session->host.open(&engine->engine, settings));
  if (result == SONARE_OK) *out = session.release();
  return result;
  SONARE_C_CATCH
#else
  return refuse_not_compiled_in(Feature::kLiveDevices, engine, config, out);
#endif
}

void sonare_live_session_destroy(SonareLiveSession* session) {
#if defined(SONARE_C_LIVE_AVAILABLE)
  delete session;
#else
  ignore_args(session);
#endif
}

SonareError sonare_live_session_status(const SonareLiveSession* session,
                                       SonareLiveSessionStatus* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
#if defined(SONARE_C_LIVE_AVAILABLE)
  SONARE_C_REQUIRE_NONNULL(session);
  SONARE_C_REQUIRE_NONNULL(out);
  const auto status = session->host.status();
  *out = {status.running ? 1 : 0,      status.device_lost ? 1 : 0,   status.rate_changed ? 1 : 0,
          status.sample_rate,          status.output_block_size,     status.input_block_size,
          status.output_latency_ms,    status.input_latency_ms,      status.xrun_count,
          status.input_overrun_frames, status.input_underrun_frames, status.dropped_midi_events};
  return SONARE_OK;
#else
  return refuse_not_compiled_in(Feature::kLiveDevices, session, out);
#endif
}

SonareError sonare_live_session_captured_midi(SonareLiveSession* session,
                                              SonareMidiEventList* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
#if defined(SONARE_C_LIVE_AVAILABLE)
  SONARE_C_REQUIRE_NONNULL(session);
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_TRY
  const auto captured = session->host.captured_midi();
  StagedOutput<SonareMidiEventList, sonare_free_midi_event_list> result;
  if (!captured.empty()) result->items = new SonareMidiEventPod[captured.size()]{};
  for (const auto& event : captured) {
    result->items[result->count++] = {event.ppq, event.ump.words[0], event.ump.words[1], nullptr,
                                      0};
  }
  result.publish(out);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return refuse_not_compiled_in(Feature::kLiveDevices, session, out);
#endif
}
