#include "playback/config.h"

#include <cmath>
#include <initializer_list>
#include <limits>
#include <string>

#include "playback/layout_convert.h"
#include "util/exception.h"
#include "util/json_budget.h"
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

struct EarlierKey {
  const char* old_key;
  const char* new_key;
};

constexpr EarlierKey kEarlierKeys[] = {
    {"channel_map", "channelMap"},
    {"bass_management", "bassManagement"},
    {"crossover_hz", "crossoverHz"},
    {"lfe_gain_db", "lfeGainDb"},
    {"distance_m", "distanceM"},
    {"trim_db", "trimDb"},
    {"lfe_mix_db", "lfeMixDb"},
    {"dialogue_level_db", "dialogueLevelDb"},
    {"center_width", "centerWidth"},
    {"front_ambience", "frontAmbience"},
    {"lfe_from_upmix", "lfeFromUpmix"},
    {"program_lufs", "programLufs"},
    {"target_lufs", "targetLufs"},
    {"night_mode", "nightMode"},
    {"mix_db", "mixDb"},
    {"head_tracking", "headTracking"},
    {"output_limiter", "outputLimiter"},
    {"ceiling_db", "ceilingDb"},
};

struct EarlierRole {
  const char* old_name;
  const char* new_name;
};

constexpr EarlierRole kEarlierRoles[] = {
    {"L", "left"},
    {"R", "right"},
    {"C", "center"},
    {"LFE", "lfe"},
    {"Ls", "leftSurround"},
    {"Rs", "rightSurround"},
    {"Lss", "leftSideSurround"},
    {"Rss", "rightSideSurround"},
};

/// Refuses an earlier role token by name; `path` is the location it was written at.
void refuse_earlier_role(const std::string& token, const std::string& path) {
  for (const EarlierRole& earlier : kEarlierRoles) {
    if (token == earlier.old_name) {
      fail(path + " is an earlier spelling; write " + earlier.new_name);
    }
  }
}

void check_keys(const Value& object, const std::string& path,
                std::initializer_list<const char*> keys) {
  if (!object.is_object())
    fail((path.empty() ? std::string("configuration") : path) + " must be an object");
  for (const auto& entry : object.as_object()) {
    bool known = false;
    for (const char* key : keys) known = known || entry.first == key;
    if (known) continue;
    for (const EarlierKey& earlier : kEarlierKeys) {
      if (entry.first == earlier.old_key) {
        fail(join(path, entry.first) + " is an earlier spelling; write " + earlier.new_key);
      }
    }
    fail("unknown key: " + join(path, entry.first));
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

std::string read_string(const Value& value, const std::string& key) {
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
  check_keys(input, "input", {"layout", "channelMap"});
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
  const Value* map = input.find("channelMap");
  if (map == nullptr) return;
  if (!map->is_array()) fail("input.channelMap must be an array");
  if (prepare.input_layout == InputLayout::Auto) {
    fail("input.channelMap requires a fixed input.layout");
  }
  const ChannelLayout layout = to_channel_layout(prepare.input_layout);
  const auto& items = map->as_array();
  const int length = static_cast<int>(items.size());
  if (length != channel_count(layout)) {
    fail("input.channelMap length " + std::to_string(length) +
         " does not match layout channel count " + std::to_string(channel_count(layout)));
  }
  for (int i = 0; i < length; ++i) {
    const std::string key = "input.channelMap[" + std::to_string(i) + "]";
    SpeakerRole role = SpeakerRole::L;
    const std::string token = read_string(items[static_cast<size_t>(i)], key);
    refuse_earlier_role(token, "input.channelMap." + token);
    if (!parse_speaker_role(token, &role)) {
      fail(key + " is not a speaker role");
    }
    prepare.channel_map[static_cast<size_t>(i)] = role;
  }
  validate_channel_map(layout, prepare.channel_map.data(), length);
  prepare.has_channel_map = true;
}

void parse_speaker(const Value& speaker, SpeakerRole role, RendererConfig& config) {
  const std::string path = std::string("target.speakers.") + speaker_role_name(role);
  check_keys(speaker, path, {"distanceM", "trimDb", "size"});
  SpeakerPrepare& prepare = config.prepare.speakers[static_cast<size_t>(role)];
  if (const Value* distance = speaker.find("distanceM")) {
    prepare.has_distance = true;
    prepare.distance_m = read_float(*distance, path + ".distanceM");
  }
  if (const Value* trim = speaker.find("trimDb")) {
    config.realtime.trim_db[static_cast<size_t>(role)] = read_float(*trim, path + ".trimDb");
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
  check_keys(target, "target", {"kind", "layout", "speakers", "bassManagement"});
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
      refuse_earlier_role(entry.first, "target.speakers." + entry.first);
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

  if (const Value* bass = target.find("bassManagement")) {
    check_keys(*bass, "target.bassManagement",
               {"enabled", "crossoverHz", "subwoofer", "lfeGainDb"});
    BassManagementConfig& bm = prepare.bass_management;
    if (const Value* v = bass->find("enabled")) {
      bm.enabled = read_bool(*v, "target.bassManagement.enabled");
    }
    if (const Value* v = bass->find("crossoverHz")) {
      bm.crossover_hz = read_float(*v, "target.bassManagement.crossoverHz");
    }
    if (const Value* v = bass->find("subwoofer")) {
      bm.subwoofer = read_bool(*v, "target.bassManagement.subwoofer");
    }
    if (const Value* v = bass->find("lfeGainDb")) {
      config.realtime.lfe_gain_db = read_float(*v, "target.bassManagement.lfeGainDb");
    }
  }
}

void parse_sections(const Value& root, RendererConfig& config) {
  RealtimeConfig& rt = config.realtime;
  if (const Value* v = root.find("lfeMixDb")) rt.lfe_mix_db = read_float(*v, "lfeMixDb");
  if (const Value* v = root.find("dialogueLevelDb")) {
    rt.dialogue_level_db = read_float(*v, "dialogueLevelDb");
  }
  if (const Value* upmix = root.find("upmix")) {
    check_keys(*upmix, "upmix", {"enabled", "centerWidth", "frontAmbience", "lfeFromUpmix"});
    if (const Value* v = upmix->find("enabled")) rt.upmix_enabled = read_bool(*v, "upmix.enabled");
    if (const Value* v = upmix->find("centerWidth")) {
      rt.upmix_center_width = read_float(*v, "upmix.centerWidth");
    }
    if (const Value* v = upmix->find("frontAmbience")) {
      rt.upmix_front_ambience = read_float(*v, "upmix.frontAmbience");
    }
    if (const Value* v = upmix->find("lfeFromUpmix")) {
      rt.upmix_lfe_from_upmix = read_bool(*v, "upmix.lfeFromUpmix");
    }
  }
  if (const Value* loudness = root.find("loudness")) {
    check_keys(*loudness, "loudness", {"programLufs", "targetLufs"});
    if (const Value* v = loudness->find("programLufs")) {
      rt.has_program_lufs = true;
      rt.program_lufs = read_float(*v, "loudness.programLufs");
    }
    if (const Value* v = loudness->find("targetLufs")) {
      rt.target_lufs = read_float(*v, "loudness.targetLufs");
    }
  }
  if (const Value* night = root.find("nightMode")) {
    check_keys(*night, "nightMode", {"amount"});
    if (const Value* v = night->find("amount")) {
      rt.night_amount = read_float(*v, "nightMode.amount");
    }
  }
  if (const Value* room = root.find("room")) {
    check_keys(*room, "room", {"preset", "mixDb", "enabled"});
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
      if (!found) fail("room.preset must be one of none, livingRoom, homeTheater, screeningRoom");
    }
    if (const Value* v = room->find("mixDb")) rt.room_mix_db = read_float(*v, "room.mixDb");
    if (const Value* v = room->find("enabled")) rt.room_enabled = read_bool(*v, "room.enabled");
  }
  if (const Value* head = root.find("headTracking")) {
    check_keys(*head, "headTracking", {"enabled"});
    if (const Value* v = head->find("enabled")) {
      rt.head_tracking_enabled = read_bool(*v, "headTracking.enabled");
    }
  }
  if (const Value* limiter = root.find("outputLimiter")) {
    check_keys(*limiter, "outputLimiter", {"enabled", "ceilingDb"});
    if (const Value* v = limiter->find("enabled")) {
      rt.limiter_enabled = read_bool(*v, "outputLimiter.enabled");
    }
    if (const Value* v = limiter->find("ceilingDb")) {
      rt.limiter_ceiling_db = read_float(*v, "outputLimiter.ceilingDb");
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
      return "livingRoom";
    case RoomPreset::HomeTheater:
      return "homeTheater";
    case RoomPreset::ScreeningRoom:
      return "screeningRoom";
  }
  return "";
}

RendererConfig parse_renderer_config(std::string_view json) {
  // The field readers and validate_renderer_config also serve callers that hand over a
  // RendererConfig struct, where a refusal is the caller's argument. Everything refused while
  // reading this document is its content.
  try {
    Value root;
    try {
      root = util::json::admit_strict(std::string(json));
    } catch (const util::json::JsonError& e) {
      fail(std::string("invalid configuration JSON: ") + e.what());
    }
    if (const Value* version = root.is_object() ? root.find("version") : nullptr) {
      if (!version->is_number() || version->as_number() != 2.0) fail("version must be 2");
    }
    check_keys(root, "",
               {"version", "input", "target", "lfeMixDb", "upmix", "dialogueLevelDb", "loudness",
                "nightMode", "room", "headTracking", "outputLimiter"});
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
  } catch (const SonareException& e) {
    if (e.code() != ErrorCode::InvalidParameter) throw;
    throw SonareException(ErrorCode::InvalidFormat, e.what());
  }
}

std::string serialize_renderer_config(const RendererConfig& config) {
  const PrepareConfig& p = config.prepare;
  const RealtimeConfig& rt = config.realtime;
  std::string out = "{\"version\":2,\"input\":{\"layout\":\"";
  out += input_layout_name(p.input_layout);
  out += '"';
  if (p.has_channel_map && p.input_layout != InputLayout::Auto) {
    out += ",\"channelMap\":[";
    const int count = channel_count(to_channel_layout(p.input_layout));
    for (int i = 0; i < count; ++i) {
      if (i > 0) out += ',';
      out += '"';
      out += speaker_role_name(p.channel_map[static_cast<size_t>(i)]);
      out += '"';
    }
    out += ']';
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
      out += "\":{";
      if (sp.has_distance) out += "\"distanceM\":" + float_text(sp.distance_m) + ',';
      out += "\"trimDb\":" + float_text(rt.trim_db[static_cast<size_t>(role)]);
      out += ",\"size\":\"";
      out += sp.size == SpeakerSize::Small ? "small" : "large";
      out += "\"}";
    }
  }
  out += "},\"bassManagement\":{\"enabled\":";
  out += bool_text(p.bass_management.enabled);
  out += ",\"crossoverHz\":" + float_text(p.bass_management.crossover_hz);
  out += ",\"subwoofer\":";
  out += bool_text(p.bass_management.subwoofer);
  out += ",\"lfeGainDb\":" + float_text(rt.lfe_gain_db);
  out += "}},\"lfeMixDb\":" + float_text(rt.lfe_mix_db);
  out += ",\"upmix\":{\"enabled\":";
  out += bool_text(rt.upmix_enabled);
  out += ",\"centerWidth\":" + float_text(rt.upmix_center_width);
  out += ",\"frontAmbience\":" + float_text(rt.upmix_front_ambience);
  out += ",\"lfeFromUpmix\":";
  out += bool_text(rt.upmix_lfe_from_upmix);
  out += "},\"dialogueLevelDb\":" + float_text(rt.dialogue_level_db);
  out += ",\"loudness\":{";
  if (rt.has_program_lufs) out += "\"programLufs\":" + float_text(rt.program_lufs) + ',';
  out += "\"targetLufs\":" + float_text(rt.target_lufs);
  out += "},\"nightMode\":{\"amount\":" + float_text(rt.night_amount);
  out += "},\"room\":{\"preset\":\"";
  out += room_preset_name(p.room_preset);
  out += "\",\"mixDb\":" + float_text(rt.room_mix_db);
  out += ",\"enabled\":";
  out += bool_text(rt.room_enabled);
  out += "},\"headTracking\":{\"enabled\":";
  out += bool_text(rt.head_tracking_enabled);
  out += "},\"outputLimiter\":{\"enabled\":";
  out += bool_text(rt.limiter_enabled);
  out += ",\"ceilingDb\":" + float_text(rt.limiter_ceiling_db);
  out += "}}";
  return out;
}

void validate_renderer_config(const RendererConfig& config) {
  const PrepareConfig& p = config.prepare;
  const RealtimeConfig& rt = config.realtime;

  if (p.has_channel_map) {
    if (p.input_layout == InputLayout::Auto) {
      fail("input.channelMap requires a fixed input.layout");
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
    if (sp.has_distance) check_range(sp.distance_m, 0.1f, 30.0f, speaker_key(role, "distanceM"));
    check_range(trim, -20.0f, 20.0f, speaker_key(role, "trimDb"));
  }

  const BassManagementConfig& bm = p.bass_management;
  check_range(bm.crossover_hz, 40.0f, 200.0f, "target.bassManagement.crossoverHz");
  check_range(rt.lfe_gain_db, -10.0f, 15.0f, "target.bassManagement.lfeGainDb");
  if (speakers && bm.enabled) {
    if (bm.subwoofer && p.target_layout == ChannelLayout::Stereo) {
      fail("target.bassManagement.subwoofer: stereo speakers have no LFE plane");
    }
    const auto size_of = [&](SpeakerRole role) {
      return p.speakers[static_cast<size_t>(role)].size;
    };
    if (!bm.subwoofer && (size_of(SpeakerRole::L) == SpeakerSize::Small ||
                          size_of(SpeakerRole::R) == SpeakerSize::Small)) {
      fail("target.bassManagement.subwoofer=false needs large left and right speakers");
    }
  }

  check_range(rt.lfe_mix_db, -60.0f, 10.0f, "lfeMixDb");
  check_range(rt.upmix_center_width, 0.05f, 1.0f, "upmix.centerWidth");
  check_range(rt.upmix_front_ambience, 0.0f, 1.0f, "upmix.frontAmbience");
  check_range(rt.dialogue_level_db, -12.0f, 12.0f, "dialogueLevelDb");
  if (rt.has_program_lufs) check_range(rt.program_lufs, -70.0f, 0.0f, "loudness.programLufs");
  check_range(rt.target_lufs, -40.0f, -5.0f, "loudness.targetLufs");
  check_range(rt.night_amount, 0.0f, 1.0f, "nightMode.amount");
  check_range(rt.room_mix_db, -30.0f, 6.0f, "room.mixDb");
  check_range(rt.limiter_ceiling_db, -12.0f, 0.0f, "outputLimiter.ceilingDb");
}

std::string first_prepare_difference(const PrepareConfig& current, const PrepareConfig& next) {
  if (current.input_layout != next.input_layout) return "input.layout";
  if (current.has_channel_map != next.has_channel_map) return "input.channelMap";
  if (current.has_channel_map) {
    const int count = channel_count(to_channel_layout(current.input_layout));
    for (int i = 0; i < count; ++i) {
      if (current.channel_map[static_cast<size_t>(i)] != next.channel_map[static_cast<size_t>(i)]) {
        return "input.channelMap";
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
      return speaker_key(role, "distanceM");
    }
    if (a.size != b.size) return speaker_key(role, "size");
  }
  const BassManagementConfig& a = current.bass_management;
  const BassManagementConfig& b = next.bass_management;
  if (a.enabled != b.enabled) return "target.bassManagement.enabled";
  if (a.crossover_hz != b.crossover_hz) return "target.bassManagement.crossoverHz";
  if (a.subwoofer != b.subwoofer) return "target.bassManagement.subwoofer";
  if (current.room_preset != next.room_preset) return "room.preset";
  return {};
}

int output_channel_count(const PrepareConfig& config) noexcept {
  return config.target_kind == TargetKind::Speakers ? channel_count(config.target_layout) : 2;
}

}  // namespace sonare::playback
