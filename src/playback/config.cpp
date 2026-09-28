#include "playback/config.h"

#include "util/exception.h"

namespace sonare::playback {

bool parse_speaker_role(std::string_view name, SpeakerRole* out) noexcept {
  (void)name;
  (void)out;
  return false;
}

const char* input_layout_name(InputLayout layout) noexcept {
  (void)layout;
  return "";
}

const char* room_preset_name(RoomPreset preset) noexcept {
  (void)preset;
  return "";
}

RendererConfig parse_renderer_config(std::string_view json) {
  (void)json;
  throw SonareException(ErrorCode::NotImplemented, "playback config parsing is not implemented");
}

std::string serialize_renderer_config(const RendererConfig& config) {
  (void)config;
  return "{}";
}

void validate_renderer_config(const RendererConfig& config) { (void)config; }

std::string first_prepare_difference(const PrepareConfig& current, const PrepareConfig& next) {
  (void)current;
  (void)next;
  return {};
}

int output_channel_count(const PrepareConfig& config) noexcept {
  (void)config;
  return 0;
}

}  // namespace sonare::playback
