#include "playback/config.h"

#include <cmath>
#include <initializer_list>
#include <limits>
#include <string>

#include "playback/layout_convert.h"
#include "util/exception.h"
#include "util/json.h"
#include "util/number_format.h"

namespace sonare::playback {

namespace {

using util::json::Value;

[[noreturn]] void fail(const std::string& message) {
  throw SonareException(ErrorCode::InvalidParameter, message);
}

std::string join(const std::string& path, const std::string& key) {
  return path.empty() ? key : path + "." + key;
}

void check_keys(const Value& object, const std::string& path,
                std::initializer_list<const char*> keys) {
  if (!object.is_object())
    fail((path.empty() ? std::string("configuration") : path) + " must be an object");
  for (const auto& entry : object.as_object()) {
    bool known = false;
    for (const char* key : keys) known = known || entry.first == key;
    if (!known) fail("unknown key: " + join(path, entry.first));
  }
}

/// A JSON number narrowed to float; a value no float can hold is refused.
float read_float(const Value& value, const std::string& key) {
  if (!value.is_number()) fail(key + " must be a number");
  const double number = value.as_number();
  if (!std::isfinite(number) ||
      std::fabs(number) > static_cast<double>(std::numeric_limits<float>::max())) {
    fail(key + " is not representable as a 32-bit float");
  }
  return static_cast<float>(number);
}

bool read_bool(const Value& value, const std::string& key) {
  if (!value.is_bool()) fail(key + " must be a boolean");
  return value.as_bool();
}

const std::string& read_string(const Value& value, const std::string& key) {
  if (!value.is_string()) fail(key + " must be a string");
  return value.as_string();
}

void check_range(float value, float lo, float hi, const std::string& key) {
  if (!(value >= lo && value <= hi)) {
    fail(key + " must be in [" + util::to_text(lo) + ", " + util::to_text(hi) + "], got " +
         util::to_text(value));
  }
}

bool parse_bed_layout(const std::string& text, ChannelLayout* out) {
  if (text == "mono") {
    *out = ChannelLayout::Mono;
  } else if (text == "stereo") {
    *out = ChannelLayout::Stereo;
  } else if (text == "5.1") {
    *out = ChannelLayout::FivePointOne;
  } else if (text == "7.1") {
    *out = ChannelLayout::SevenPointOne;
  } else {
    return false;
  }
  return true;
}

const char* bed_layout_name(ChannelLayout layout) {
  switch (layout) {
    case ChannelLayout::Mono:
      return "mono";
    case ChannelLayout::Stereo:
      return "stereo";
    case ChannelLayout::FivePointOne:
      return "5.1";
    case ChannelLayout::SevenPointOne:
      return "7.1";
  }
  return "stereo";
}

bool layout_has_speaker(ChannelLayout layout, SpeakerRole role) {
  if (role == SpeakerRole::LFE) return false;
  const SpeakerRole* roles = speaker_roles(layout);
  for (int i = 0; i < channel_count(layout); ++i) {
    if (roles[i] == role) return true;
  }
  return false;
}

std::string speaker_key(SpeakerRole role, const char* field) {
  return std::string("target.speakers.") + speaker_role_name(role) + "." + field;
}

void parse_input(const Value& input, PrepareConfig& prepare) {
  check_keys(input, "input", {"layout", "channel_map"});
  if (const Value* layout = input.find("layout")) {
    const std::string& text = read_string(*layout, "input.layout");
    ChannelLayout bed = ChannelLayout::Stereo;
    if (text == "auto") {
      prepare.input_layout = InputLayout::Auto;
    } else if (parse_bed_layout(text, &bed)) {
      prepare.input_layout = bed == ChannelLayout::Mono           ? InputLayout::Mono
                             : bed == ChannelLayout::Stereo       ? InputLayout::Stereo
                             : bed == ChannelLayout::FivePointOne ? InputLayout::FivePointOne
                                                                  : InputLayout::SevenPointOne;
    } else {
      fail("input.layout must be one of auto, mono, stereo, 5.1, 7.1");
    }
  }
  const Value* map = input.find("channel_map");
  if (map == nullptr || map->is_null()) return;
  if (!map->is_array()) fail("input.channel_map must be an array or null");
  if (prepare.input_layout == InputLayout::Auto) {
    fail("input.channel_map requires a fixed input.layout");
  }
  const ChannelLayout layout = to_channel_layout(prepare.input_layout);
  const auto& items = map->as_array();
  const int length = static_cast<int>(items.size());
  if (length != channel_count(layout)) {
    fail("input.channel_map length " + std::to_string(length) +
         " does not match layout channel count " + std::to_string(channel_count(layout)));
  }
  for (int i = 0; i < length; ++i) {
    const std::string key = "input.channel_map[" + std::to_string(i) + "]";
    SpeakerRole role = SpeakerRole::L;
    if (!parse_speaker_role(read_string(items[static_cast<size_t>(i)], key), &role)) {
      fail(key + " is not a speaker role");
    }
    prepare.channel_map[static_cast<size_t>(i)] = role;
  }
  validate_channel_map(layout, prepare.channel_map.data(), length);
  prepare.has_channel_map = true;
}

void parse_speaker(const Value& speaker, SpeakerRole role, RendererConfig& config) {
  const std::string path = std::string("target.speakers.") + speaker_role_name(role);
  check_keys(speaker, path, {"distance_m", "trim_db", "size"});
  SpeakerPrepare& prepare = config.prepare.speakers[static_cast<size_t>(role)];
  if (const Value* distance = speaker.find("distance_m")) {
    prepare.has_distance = !distance->is_null();
    if (prepare.has_distance) prepare.distance_m = read_float(*distance, path + ".distance_m");
  }
  if (const Value* trim = speaker.find("trim_db")) {
    config.realtime.trim_db[static_cast<size_t>(role)] = read_float(*trim, path + ".trim_db");
  }
  if (const Value* size = speaker.find("size")) {
    const std::string& text = read_string(*size, path + ".size");
    if (text == "large") {
      prepare.size = SpeakerSize::Large;
    } else if (text == "small") {
      prepare.size = SpeakerSize::Small;
    } else {
      fail(path + ".size must be large or small");
    }
  }
}

void parse_target(const Value& target, RendererConfig& config) {
  check_keys(target, "target", {"kind", "layout", "speakers", "bass_management"});
  PrepareConfig& prepare = config.prepare;
  if (const Value* kind = target.find("kind")) {
    const std::string& text = read_string(*kind, "target.kind");
    if (text == "headphones") {
      prepare.target_kind = TargetKind::Headphones;
    } else if (text == "speakers") {
      prepare.target_kind = TargetKind::Speakers;
    } else {
      fail("target.kind must be headphones or speakers");
    }
  }
  const Value* layout = target.find("layout");
  if (prepare.target_kind == TargetKind::Speakers) {
    if (layout == nullptr) fail("target.layout is required for speakers");
    const std::string& text = read_string(*layout, "target.layout");
    if (!parse_bed_layout(text, &prepare.target_layout) ||
        prepare.target_layout == ChannelLayout::Mono) {
      fail("target.layout must be one of stereo, 5.1, 7.1");
    }
  } else if (layout != nullptr) {
    fail("target.layout is not allowed for headphones");
  }

  if (const Value* speakers = target.find("speakers")) {
    if (!speakers->is_object()) fail("target.speakers must be an object");
    for (const auto& entry : speakers->as_object()) {
      SpeakerRole role = SpeakerRole::L;
      if (!parse_speaker_role(entry.first, &role) || role == SpeakerRole::LFE) {
        fail("unknown key: target.speakers." + entry.first);
      }
      if (prepare.target_kind != TargetKind::Speakers ||
          !layout_has_speaker(prepare.target_layout, role)) {
        fail("target.speakers." + entry.first + " is not a speaker of the output layout");
      }
      parse_speaker(entry.second, role, config);
    }
  }

  if (const Value* bass = target.find("bass_management")) {
    check_keys(*bass, "target.bass_management",
               {"enabled", "crossover_hz", "subwoofer", "lfe_gain_db"});
    BassManagementConfig& bm = prepare.bass_management;
    if (const Value* v = bass->find("enabled")) {
      bm.enabled = read_bool(*v, "target.bass_management.enabled");
    }
    if (const Value* v = bass->find("crossover_hz")) {
      bm.crossover_hz = read_float(*v, "target.bass_management.crossover_hz");
    }
    if (const Value* v = bass->find("subwoofer")) {
      bm.subwoofer = read_bool(*v, "target.bass_management.subwoofer");
    }
    if (const Value* v = bass->find("lfe_gain_db")) {
      config.realtime.lfe_gain_db = read_float(*v, "target.bass_management.lfe_gain_db");
    }
  }
}

void parse_sections(const Value& root, RendererConfig& config) {
  RealtimeConfig& rt = config.realtime;
  if (const Value* v = root.find("lfe_mix_db")) rt.lfe_mix_db = read_float(*v, "lfe_mix_db");
  if (const Value* v = root.find("dialogue_level_db")) {
    rt.dialogue_level_db = read_float(*v, "dialogue_level_db");
  }
  if (const Value* upmix = root.find("upmix")) {
    check_keys(*upmix, "upmix", {"enabled", "center_width", "front_ambience", "lfe_from_upmix"});
    if (const Value* v = upmix->find("enabled")) rt.upmix_enabled = read_bool(*v, "upmix.enabled");
    if (const Value* v = upmix->find("center_width")) {
      rt.upmix_center_width = read_float(*v, "upmix.center_width");
    }
    if (const Value* v = upmix->find("front_ambience")) {
      rt.upmix_front_ambience = read_float(*v, "upmix.front_ambience");
    }
    if (const Value* v = upmix->find("lfe_from_upmix")) {
      rt.upmix_lfe_from_upmix = read_bool(*v, "upmix.lfe_from_upmix");
    }
  }
  if (const Value* loudness = root.find("loudness")) {
    check_keys(*loudness, "loudness", {"program_lufs", "target_lufs"});
    if (const Value* v = loudness->find("program_lufs")) {
      rt.has_program_lufs = !v->is_null();
      if (rt.has_program_lufs) rt.program_lufs = read_float(*v, "loudness.program_lufs");
    }
    if (const Value* v = loudness->find("target_lufs")) {
      rt.target_lufs = read_float(*v, "loudness.target_lufs");
    }
  }
  if (const Value* night = root.find("night_mode")) {
    check_keys(*night, "night_mode", {"amount"});
    if (const Value* v = night->find("amount")) {
      rt.night_amount = read_float(*v, "night_mode.amount");
    }
  }
  if (const Value* room = root.find("room")) {
    check_keys(*room, "room", {"preset", "mix_db", "enabled"});
    if (const Value* v = room->find("preset")) {
      const std::string& text = read_string(*v, "room.preset");
      bool found = false;
      for (RoomPreset preset : {RoomPreset::None, RoomPreset::LivingRoom, RoomPreset::HomeTheater,
                                RoomPreset::ScreeningRoom}) {
        if (text == room_preset_name(preset)) {
          config.prepare.room_preset = preset;
          found = true;
        }
      }
      if (!found)
        fail("room.preset must be one of none, living_room, home_theater, screening_room");
    }
    if (const Value* v = room->find("mix_db")) rt.room_mix_db = read_float(*v, "room.mix_db");
    if (const Value* v = room->find("enabled")) rt.room_enabled = read_bool(*v, "room.enabled");
  }
  if (const Value* head = root.find("head_tracking")) {
    check_keys(*head, "head_tracking", {"enabled"});
    if (const Value* v = head->find("enabled")) {
      rt.head_tracking_enabled = read_bool(*v, "head_tracking.enabled");
    }
  }
  if (const Value* limiter = root.find("output_limiter")) {
    check_keys(*limiter, "output_limiter", {"enabled", "ceiling_db"});
    if (const Value* v = limiter->find("enabled")) {
      rt.limiter_enabled = read_bool(*v, "output_limiter.enabled");
    }
    if (const Value* v = limiter->find("ceiling_db")) {
      rt.limiter_ceiling_db = read_float(*v, "output_limiter.ceiling_db");
    }
  }
}

/// Shortest decimal text that reads back as the same float.
std::string float_text(float value) {
  std::string text;
  for (int digits = 6; digits <= std::numeric_limits<float>::max_digits10; ++digits) {
    text = util::format_general(static_cast<double>(value), digits);
    double back = 0.0;
    if (util::parse_double(text.data(), text.data() + text.size(), &back) &&
        static_cast<float>(back) == value) {
      break;
    }
  }
  return text;
}

const char* bool_text(bool value) { return value ? "true" : "false"; }

}  // namespace

bool parse_speaker_role(std::string_view name, SpeakerRole* out) noexcept {
  for (SpeakerRole role : {SpeakerRole::L, SpeakerRole::R, SpeakerRole::C, SpeakerRole::LFE,
                           SpeakerRole::Ls, SpeakerRole::Rs, SpeakerRole::Lss, SpeakerRole::Rss}) {
    if (name == speaker_role_name(role)) {
      *out = role;
      return true;
    }
  }
  return false;
}

const char* input_layout_name(InputLayout layout) noexcept {
  switch (layout) {
    case InputLayout::Auto:
      return "auto";
    case InputLayout::Mono:
      return "mono";
    case InputLayout::Stereo:
      return "stereo";
    case InputLayout::FivePointOne:
      return "5.1";
    case InputLayout::SevenPointOne:
      return "7.1";
  }
  return "";
}

const char* room_preset_name(RoomPreset preset) noexcept {
  switch (preset) {
    case RoomPreset::None:
      return "none";
    case RoomPreset::LivingRoom:
      return "living_room";
    case RoomPreset::HomeTheater:
      return "home_theater";
    case RoomPreset::ScreeningRoom:
      return "screening_room";
  }
  return "";
}

RendererConfig parse_renderer_config(std::string_view json) {
  Value root;
  try {
    root = util::json::parse_strict(std::string(json));
  } catch (const util::json::JsonError& e) {
    fail(std::string("invalid configuration JSON: ") + e.what());
  }
  check_keys(root, "",
             {"input", "target", "lfe_mix_db", "upmix", "dialogue_level_db", "loudness",
              "night_mode", "room", "head_tracking", "output_limiter"});
  RendererConfig config;
  if (const Value* input = root.find("input")) parse_input(*input, config.prepare);
  if (const Value* target = root.find("target")) {
    parse_target(*target, config);
  } else {
    parse_target(Value(util::json::Object{}), config);
  }
  parse_sections(root, config);
  validate_renderer_config(config);
  return config;
}

std::string serialize_renderer_config(const RendererConfig& config) {
  const PrepareConfig& p = config.prepare;
  const RealtimeConfig& rt = config.realtime;
  std::string out = "{\"input\":{\"layout\":\"";
  out += input_layout_name(p.input_layout);
  out += "\",\"channel_map\":";
  if (p.has_channel_map && p.input_layout != InputLayout::Auto) {
    out += '[';
    const int count = channel_count(to_channel_layout(p.input_layout));
    for (int i = 0; i < count; ++i) {
      if (i > 0) out += ',';
      out += '"';
      out += speaker_role_name(p.channel_map[static_cast<size_t>(i)]);
      out += '"';
    }
    out += ']';
  } else {
    out += "null";
  }
  out += "},\"target\":{\"kind\":\"";
  const bool speakers = p.target_kind == TargetKind::Speakers;
  out += speakers ? "speakers\"" : "headphones\"";
  if (speakers) {
    out += ",\"layout\":\"";
    out += bed_layout_name(p.target_layout);
    out += '"';
  }
  out += ",\"speakers\":{";
  if (speakers) {
    bool first = true;
    const SpeakerRole* roles = speaker_roles(p.target_layout);
    for (int i = 0; i < channel_count(p.target_layout); ++i) {
      const SpeakerRole role = roles[i];
      if (role == SpeakerRole::LFE) continue;
      const SpeakerPrepare& sp = p.speakers[static_cast<size_t>(role)];
      if (!first) out += ',';
      first = false;
      out += '"';
      out += speaker_role_name(role);
      out += "\":{\"distance_m\":";
      out += sp.has_distance ? float_text(sp.distance_m) : std::string("null");
      out += ",\"trim_db\":" + float_text(rt.trim_db[static_cast<size_t>(role)]);
      out += ",\"size\":\"";
      out += sp.size == SpeakerSize::Small ? "small" : "large";
      out += "\"}";
    }
  }
  out += "},\"bass_management\":{\"enabled\":";
  out += bool_text(p.bass_management.enabled);
  out += ",\"crossover_hz\":" + float_text(p.bass_management.crossover_hz);
  out += ",\"subwoofer\":";
  out += bool_text(p.bass_management.subwoofer);
  out += ",\"lfe_gain_db\":" + float_text(rt.lfe_gain_db);
  out += "}},\"lfe_mix_db\":" + float_text(rt.lfe_mix_db);
  out += ",\"upmix\":{\"enabled\":";
  out += bool_text(rt.upmix_enabled);
  out += ",\"center_width\":" + float_text(rt.upmix_center_width);
  out += ",\"front_ambience\":" + float_text(rt.upmix_front_ambience);
  out += ",\"lfe_from_upmix\":";
  out += bool_text(rt.upmix_lfe_from_upmix);
  out += "},\"dialogue_level_db\":" + float_text(rt.dialogue_level_db);
  out += ",\"loudness\":{\"program_lufs\":";
  out += rt.has_program_lufs ? float_text(rt.program_lufs) : std::string("null");
  out += ",\"target_lufs\":" + float_text(rt.target_lufs);
  out += "},\"night_mode\":{\"amount\":" + float_text(rt.night_amount);
  out += "},\"room\":{\"preset\":\"";
  out += room_preset_name(p.room_preset);
  out += "\",\"mix_db\":" + float_text(rt.room_mix_db);
  out += ",\"enabled\":";
  out += bool_text(rt.room_enabled);
  out += "},\"head_tracking\":{\"enabled\":";
  out += bool_text(rt.head_tracking_enabled);
  out += "},\"output_limiter\":{\"enabled\":";
  out += bool_text(rt.limiter_enabled);
  out += ",\"ceiling_db\":" + float_text(rt.limiter_ceiling_db);
  out += "}}";
  return out;
}

void validate_renderer_config(const RendererConfig& config) {
  const PrepareConfig& p = config.prepare;
  const RealtimeConfig& rt = config.realtime;

  if (p.has_channel_map) {
    if (p.input_layout == InputLayout::Auto) {
      fail("input.channel_map requires a fixed input.layout");
    }
    const ChannelLayout layout = to_channel_layout(p.input_layout);
    validate_channel_map(layout, p.channel_map.data(), channel_count(layout));
  }

  const bool speakers = p.target_kind == TargetKind::Speakers;
  if (speakers && p.target_layout == ChannelLayout::Mono) {
    fail("target.layout must be one of stereo, 5.1, 7.1");
  }
  for (int r = 0; r < kSpeakerRoleCount; ++r) {
    const auto role = static_cast<SpeakerRole>(r);
    const SpeakerPrepare& sp = p.speakers[static_cast<size_t>(r)];
    const float trim = rt.trim_db[static_cast<size_t>(r)];
    if (!speakers || !layout_has_speaker(p.target_layout, role)) {
      if (sp.has_distance || sp.size != SpeakerSize::Large || trim != 0.0f) {
        fail(std::string("target.speakers.") + speaker_role_name(role) +
             " is not a speaker of the output layout");
      }
      continue;
    }
    if (sp.has_distance) check_range(sp.distance_m, 0.1f, 30.0f, speaker_key(role, "distance_m"));
    check_range(trim, -20.0f, 20.0f, speaker_key(role, "trim_db"));
  }

  const BassManagementConfig& bm = p.bass_management;
  check_range(bm.crossover_hz, 40.0f, 200.0f, "target.bass_management.crossover_hz");
  check_range(rt.lfe_gain_db, -10.0f, 15.0f, "target.bass_management.lfe_gain_db");
  if (speakers && bm.enabled) {
    if (bm.subwoofer && p.target_layout == ChannelLayout::Stereo) {
      fail("target.bass_management.subwoofer: stereo speakers have no LFE plane");
    }
    const auto size_of = [&](SpeakerRole role) {
      return p.speakers[static_cast<size_t>(role)].size;
    };
    if (!bm.subwoofer && (size_of(SpeakerRole::L) == SpeakerSize::Small ||
                          size_of(SpeakerRole::R) == SpeakerSize::Small)) {
      fail("target.bass_management.subwoofer=false needs large L and R speakers");
    }
  }

  check_range(rt.lfe_mix_db, -60.0f, 10.0f, "lfe_mix_db");
  check_range(rt.upmix_center_width, 0.05f, 1.0f, "upmix.center_width");
  check_range(rt.upmix_front_ambience, 0.0f, 1.0f, "upmix.front_ambience");
  check_range(rt.dialogue_level_db, -12.0f, 12.0f, "dialogue_level_db");
  if (rt.has_program_lufs) check_range(rt.program_lufs, -70.0f, 0.0f, "loudness.program_lufs");
  check_range(rt.target_lufs, -40.0f, -5.0f, "loudness.target_lufs");
  check_range(rt.night_amount, 0.0f, 1.0f, "night_mode.amount");
  check_range(rt.room_mix_db, -30.0f, 6.0f, "room.mix_db");
  check_range(rt.limiter_ceiling_db, -12.0f, 0.0f, "output_limiter.ceiling_db");
}

std::string first_prepare_difference(const PrepareConfig& current, const PrepareConfig& next) {
  if (current.input_layout != next.input_layout) return "input.layout";
  if (current.has_channel_map != next.has_channel_map) return "input.channel_map";
  if (current.has_channel_map) {
    const int count = channel_count(to_channel_layout(current.input_layout));
    for (int i = 0; i < count; ++i) {
      if (current.channel_map[static_cast<size_t>(i)] != next.channel_map[static_cast<size_t>(i)]) {
        return "input.channel_map";
      }
    }
  }
  if (current.target_kind != next.target_kind) return "target.kind";
  if (current.target_kind == TargetKind::Speakers && current.target_layout != next.target_layout) {
    return "target.layout";
  }
  for (int r = 0; r < kSpeakerRoleCount; ++r) {
    const auto role = static_cast<SpeakerRole>(r);
    const SpeakerPrepare& a = current.speakers[static_cast<size_t>(r)];
    const SpeakerPrepare& b = next.speakers[static_cast<size_t>(r)];
    if (a.has_distance != b.has_distance || (a.has_distance && a.distance_m != b.distance_m)) {
      return speaker_key(role, "distance_m");
    }
    if (a.size != b.size) return speaker_key(role, "size");
  }
  const BassManagementConfig& a = current.bass_management;
  const BassManagementConfig& b = next.bass_management;
  if (a.enabled != b.enabled) return "target.bass_management.enabled";
  if (a.crossover_hz != b.crossover_hz) return "target.bass_management.crossover_hz";
  if (a.subwoofer != b.subwoofer) return "target.bass_management.subwoofer";
  if (current.room_preset != next.room_preset) return "room.preset";
  return {};
}

int output_channel_count(const PrepareConfig& config) noexcept {
  return config.target_kind == TargetKind::Speakers ? channel_count(config.target_layout) : 2;
}

}  // namespace sonare::playback
