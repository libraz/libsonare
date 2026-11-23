#include "playback/config.h"

#include <array>
#include <initializer_list>
#include <limits>
#include <string>
#include <vector>

#include "util/exception.h"
#include "util/json_budget.h"
#include "util/json_schema.h"
#include "util/number_format.h"
#include "util/numeric_validation.h"

namespace sonare::playback {

namespace {

using util::json::Value;
namespace schema = util::json::schema;

using Refusal = SonareParameterRefusal;
constexpr ErrorCode kDocumentError = ErrorCode::InvalidFormat;
constexpr int kRendererConfigVersion = 2;

[[noreturn]] void refuse_at(const std::string& path, const std::string& predicate,
                            ErrorCode code = kDocumentError) {
  throw Refusal::predicate(code, path, predicate).as_caller_spelled();
}

[[noreturn]] void refuse_kind(const std::string& path, schema::Kind kind) {
  if (path.empty()) throw SonareException(kDocumentError, schema::must_be(path, kind));
  throw Refusal::wrong_type(kDocumentError, path, std::string("must be ") + schema::kind_text(kind))
      .as_caller_spelled();
}

[[noreturn]] void refuse_unknown(const std::string& path, const std::string& expected) {
  throw Refusal::unknown(kDocumentError, path, std::string(), expected).as_caller_spelled();
}

void validate_renderer_values(const RendererConfig& config, ErrorCode code);

std::string speaker_path(SpeakerRole role) {
  return schema::child_path(schema::child_path(schema::child_path("", "target"), "speakers"),
                            speaker_role_name(role));
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

/// Refuses an earlier role token, naming the full replacement path for a key.
void refuse_earlier_role(const std::string& token, const std::string& path,
                         const std::string* parent) {
  for (const EarlierRole& earlier : kEarlierRoles) {
    if (token != earlier.old_name) continue;
    if (parent == nullptr) {
      refuse_at(path, std::string("is an earlier spelling; write ") + earlier.new_name);
    }
    throw Refusal::relation(kDocumentError, {path, schema::child_path(*parent, earlier.new_name)},
                            "{0} is an earlier spelling; write {1}")
        .as_caller_spelled();
  }
}

void check_keys(const Value& object, const std::string& path,
                std::initializer_list<const char*> keys) {
  if (!object.is_object()) refuse_kind(path, schema::Kind::Object);
  for (const auto& entry : object.as_object()) {
    bool known = false;
    for (const char* key : keys) known = known || entry.first == key;
    if (known) continue;
    const std::string at = schema::child_path(path, entry.first);
    for (const EarlierKey& earlier : kEarlierKeys) {
      if (entry.first == earlier.old_key) {
        throw Refusal::relation(kDocumentError, {at, schema::child_path(path, earlier.new_key)},
                                "{0} is an earlier spelling; write {1}")
            .as_caller_spelled();
      }
    }
    refuse_unknown(at, schema::expected_keys(std::vector<std::string>(keys.begin(), keys.end())));
  }
}

/// A JSON number narrowed to float; a value no float can hold is refused.
float read_float(const Value& value, const std::string& key) {
  if (!value.is_number()) refuse_kind(key, schema::Kind::Number);
  float converted = 0.0f;
  if (!numeric::checked_float_cast(value.as_number(), &converted)) {
    refuse_at(key, schema::kFloatRangePredicate);
  }
  return converted;
}

bool read_bool(const Value& value, const std::string& key) {
  if (!value.is_bool()) refuse_kind(key, schema::Kind::Boolean);
  return value.as_bool();
}

std::string read_string(const Value& value, const std::string& key) {
  if (!value.is_string()) refuse_kind(key, schema::Kind::String);
  return value.as_string();
}

void check_range(ErrorCode code, float value, double lo, double hi, const std::string& key) {
  if (!(value >= lo && value <= hi)) {
    throw Refusal::predicate(code, key, schema::range_predicate(lo, hi), {lo, hi})
        .as_caller_spelled();
  }
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
  return schema::child_path(speaker_path(role), field);
}

std::string bass_key(const char* field) {
  return schema::child_path(schema::child_path(schema::child_path("", "target"), "bassManagement"),
                            field);
}

std::string input_key(const char* field) { return schema::child_path("input", field); }

constexpr const char* kNeedsFixedLayout = "requires a fixed input.layout";
constexpr const char* kTargetLayoutDomain = "must be one of stereo, 5.1, 7.1";

/// Refuses a channel map that does not name each role of the fixed input layout exactly once.
void check_channel_map(const PrepareConfig& prepare, ErrorCode code) {
  const std::string map_path = input_key("channelMap");
  const ChannelLayout layout = to_channel_layout(prepare.input_layout);
  const int count = channel_count(layout);
  // With one entry per channel, distinct roles of the layout cover every role it requires.
  std::array<bool, kSpeakerRoleCount> allowed{};
  std::array<bool, kSpeakerRoleCount> seen{};
  const SpeakerRole* roles = speaker_roles(layout);
  for (int i = 0; i < count; ++i) allowed[static_cast<size_t>(roles[i])] = true;
  for (int i = 0; i < count; ++i) {
    const size_t role = static_cast<size_t>(prepare.channel_map[static_cast<size_t>(i)]);
    const std::string key = schema::index_path(map_path, static_cast<size_t>(i));
    if (role >= allowed.size()) refuse_at(key, "must be a speaker role", code);
    if (!allowed[role]) refuse_at(key, "must be a role of the input layout", code);
    if (seen[role]) refuse_at(key, "must be distinct from earlier channel roles", code);
    seen[role] = true;
  }
}

void parse_input(const Value& input, PrepareConfig& prepare) {
  check_keys(input, "input", {"layout", "channelMap"});
  if (const Value* layout = input.find("layout")) {
    const std::string& text = read_string(*layout, input_key("layout"));
    ChannelLayout bed = ChannelLayout::Stereo;
    if (text == "auto") {
      prepare.input_layout = InputLayout::Auto;
    } else if (channel_layout_from_string(text, bed)) {
      prepare.input_layout = bed == ChannelLayout::Mono           ? InputLayout::Mono
                             : bed == ChannelLayout::Stereo       ? InputLayout::Stereo
                             : bed == ChannelLayout::FivePointOne ? InputLayout::FivePointOne
                                                                  : InputLayout::SevenPointOne;
    } else {
      refuse_at(input_key("layout"), "must be one of auto, mono, stereo, 5.1, 7.1");
    }
  }
  const Value* map = input.find("channelMap");
  if (map == nullptr) return;
  const std::string map_path = input_key("channelMap");
  if (!map->is_array()) refuse_kind(map_path, schema::Kind::Array);
  if (prepare.input_layout == InputLayout::Auto) refuse_at(map_path, kNeedsFixedLayout);
  const ChannelLayout layout = to_channel_layout(prepare.input_layout);
  const auto& items = map->as_array();
  const int length = static_cast<int>(items.size());
  if (length != channel_count(layout)) {
    refuse_at(map_path, "must hold " + std::to_string(channel_count(layout)) +
                            " entries, one per channel of the layout");
  }
  for (int i = 0; i < length; ++i) {
    const std::string key = schema::index_path(map_path, static_cast<size_t>(i));
    SpeakerRole role = SpeakerRole::L;
    const std::string token = read_string(items[static_cast<size_t>(i)], key);
    refuse_earlier_role(token, key, nullptr);
    if (!parse_speaker_role(token, &role)) refuse_at(key, "must be a speaker role");
    prepare.channel_map[static_cast<size_t>(i)] = role;
  }
  check_channel_map(prepare, kDocumentError);
  prepare.has_channel_map = true;
}

void parse_speaker(const Value& speaker, SpeakerRole role, RendererConfig& config) {
  const std::string path = speaker_path(role);
  check_keys(speaker, path, {"distanceM", "trimDb", "size"});
  SpeakerPrepare& prepare = config.prepare.speakers[static_cast<size_t>(role)];
  if (const Value* distance = speaker.find("distanceM")) {
    prepare.has_distance = true;
    prepare.distance_m = read_float(*distance, speaker_key(role, "distanceM"));
  }
  if (const Value* trim = speaker.find("trimDb")) {
    config.realtime.trim_db[static_cast<size_t>(role)] =
        read_float(*trim, speaker_key(role, "trimDb"));
  }
  if (const Value* size = speaker.find("size")) {
    const std::string& text = read_string(*size, speaker_key(role, "size"));
    if (text == "large") {
      prepare.size = SpeakerSize::Large;
    } else if (text == "small") {
      prepare.size = SpeakerSize::Small;
    } else {
      refuse_at(speaker_key(role, "size"), "must be large or small");
    }
  }
}

void parse_target(const Value& target, RendererConfig& config) {
  check_keys(target, "target", {"kind", "layout", "speakers", "bassManagement"});
  PrepareConfig& prepare = config.prepare;
  if (const Value* kind = target.find("kind")) {
    const std::string& text = read_string(*kind, schema::child_path("target", "kind"));
    if (text == "headphones") {
      prepare.target_kind = TargetKind::Headphones;
    } else if (text == "speakers") {
      prepare.target_kind = TargetKind::Speakers;
    } else {
      refuse_at(schema::child_path("target", "kind"), "must be headphones or speakers");
    }
  }
  const Value* layout = target.find("layout");
  if (prepare.target_kind == TargetKind::Speakers) {
    const std::string layout_path = schema::child_path("target", "layout");
    if (layout == nullptr) {
      throw Refusal::wrong_type(kDocumentError, layout_path, "must be given for speakers")
          .as_caller_spelled();
    }
    const std::string& text = read_string(*layout, layout_path);
    if (!channel_layout_from_string(text, prepare.target_layout) ||
        prepare.target_layout == ChannelLayout::Mono) {
      refuse_at(layout_path, kTargetLayoutDomain);
    }
  } else if (layout != nullptr) {
    throw Refusal::wrong_type(kDocumentError, schema::child_path("target", "layout"),
                              "must be left out for headphones")
        .as_caller_spelled();
  }

  if (const Value* speakers = target.find("speakers")) {
    const std::string speakers_path = schema::child_path("target", "speakers");
    if (!speakers->is_object()) refuse_kind(speakers_path, schema::Kind::Object);
    for (const auto& entry : speakers->as_object()) {
      SpeakerRole role = SpeakerRole::L;
      const std::string entry_path = schema::child_path(speakers_path, entry.first);
      refuse_earlier_role(entry.first, entry_path, &speakers_path);
      if (!parse_speaker_role(entry.first, &role) || role == SpeakerRole::LFE) {
        std::vector<std::string> roles;
        for (SpeakerRole named : {SpeakerRole::L, SpeakerRole::R, SpeakerRole::C, SpeakerRole::Ls,
                                  SpeakerRole::Rs, SpeakerRole::Lss, SpeakerRole::Rss}) {
          roles.emplace_back(speaker_role_name(named));
        }
        refuse_unknown(entry_path, schema::expected_keys(roles));
      }
      if (prepare.target_kind != TargetKind::Speakers ||
          !layout_has_speaker(prepare.target_layout, role)) {
        refuse_at(entry_path, "must be a speaker of the output layout");
      }
      parse_speaker(entry.second, role, config);
    }
  }

  if (const Value* bass = target.find("bassManagement")) {
    check_keys(*bass, schema::child_path("target", "bassManagement"),
               {"enabled", "crossoverHz", "subwoofer", "lfeGainDb"});
    BassManagementConfig& bm = prepare.bass_management;
    if (const Value* v = bass->find("enabled")) {
      bm.enabled = read_bool(*v, bass_key("enabled"));
    }
    if (const Value* v = bass->find("crossoverHz")) {
      bm.crossover_hz = read_float(*v, bass_key("crossoverHz"));
    }
    if (const Value* v = bass->find("subwoofer")) {
      bm.subwoofer = read_bool(*v, bass_key("subwoofer"));
    }
    if (const Value* v = bass->find("lfeGainDb")) {
      config.realtime.lfe_gain_db = read_float(*v, bass_key("lfeGainDb"));
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
    if (const Value* v = upmix->find("enabled"))
      rt.upmix_enabled = read_bool(*v, schema::child_path("upmix", "enabled"));
    if (const Value* v = upmix->find("centerWidth")) {
      rt.upmix_center_width = read_float(*v, schema::child_path("upmix", "centerWidth"));
    }
    if (const Value* v = upmix->find("frontAmbience")) {
      rt.upmix_front_ambience = read_float(*v, schema::child_path("upmix", "frontAmbience"));
    }
    if (const Value* v = upmix->find("lfeFromUpmix")) {
      rt.upmix_lfe_from_upmix = read_bool(*v, schema::child_path("upmix", "lfeFromUpmix"));
    }
  }
  if (const Value* loudness = root.find("loudness")) {
    check_keys(*loudness, "loudness", {"programLufs", "targetLufs"});
    if (const Value* v = loudness->find("programLufs")) {
      rt.has_program_lufs = true;
      rt.program_lufs = read_float(*v, schema::child_path("loudness", "programLufs"));
    }
    if (const Value* v = loudness->find("targetLufs")) {
      rt.target_lufs = read_float(*v, schema::child_path("loudness", "targetLufs"));
    }
  }
  if (const Value* night = root.find("nightMode")) {
    check_keys(*night, "nightMode", {"amount"});
    if (const Value* v = night->find("amount")) {
      rt.night_amount = read_float(*v, schema::child_path("nightMode", "amount"));
    }
  }
  if (const Value* room = root.find("room")) {
    check_keys(*room, "room", {"preset", "mixDb", "enabled"});
    if (const Value* v = room->find("preset")) {
      const std::string& text = read_string(*v, schema::child_path("room", "preset"));
      bool found = false;
      for (RoomPreset preset : {RoomPreset::None, RoomPreset::LivingRoom, RoomPreset::HomeTheater,
                                RoomPreset::ScreeningRoom}) {
        if (text == room_preset_name(preset)) {
          config.prepare.room_preset = preset;
          found = true;
        }
      }
      if (!found) {
        refuse_at(schema::child_path("room", "preset"),
                  "must be one of none, livingRoom, homeTheater, screeningRoom");
      }
    }
    if (const Value* v = room->find("mixDb"))
      rt.room_mix_db = read_float(*v, schema::child_path("room", "mixDb"));
    if (const Value* v = room->find("enabled"))
      rt.room_enabled = read_bool(*v, schema::child_path("room", "enabled"));
  }
  if (const Value* head = root.find("headTracking")) {
    check_keys(*head, "headTracking", {"enabled"});
    if (const Value* v = head->find("enabled")) {
      rt.head_tracking_enabled = read_bool(*v, schema::child_path("headTracking", "enabled"));
    }
  }
  if (const Value* limiter = root.find("outputLimiter")) {
    check_keys(*limiter, "outputLimiter", {"enabled", "ceilingDb"});
    if (const Value* v = limiter->find("enabled")) {
      rt.limiter_enabled = read_bool(*v, schema::child_path("outputLimiter", "enabled"));
    }
    if (const Value* v = limiter->find("ceilingDb")) {
      rt.limiter_ceiling_db = read_float(*v, schema::child_path("outputLimiter", "ceilingDb"));
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
  Value root;
  try {
    root = util::json::admit_strict(std::string(json));
  } catch (const util::json::JsonDuplicateKeyError& e) {
    throw SonareException(kDocumentError, e.what());
  } catch (const util::json::JsonResourceError& e) {
    throw SonareException(kDocumentError, e.what());
  } catch (const util::json::JsonError& e) {
    throw SonareException(
        kDocumentError,
        schema::refusal("the document", std::string("must be valid JSON: ") + e.what()));
  }
  if (const Value* version = root.is_object() ? root.find("version") : nullptr) {
    if (!version->is_number()) refuse_kind("version", schema::Kind::Number);
    if (version->as_number() != kRendererConfigVersion) {
      refuse_at("version", "must be " + std::to_string(kRendererConfigVersion));
    }
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
  validate_renderer_values(config, kDocumentError);
  return config;
}

std::string serialize_renderer_config(const RendererConfig& config) {
  const PrepareConfig& p = config.prepare;
  const RealtimeConfig& rt = config.realtime;
  std::string out =
      "{\"version\":" + std::to_string(kRendererConfigVersion) + ",\"input\":{\"layout\":\"";
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
    out += channel_layout_to_string(p.target_layout);
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

namespace {

void validate_renderer_values(const RendererConfig& config, ErrorCode code) {
  const PrepareConfig& p = config.prepare;
  const RealtimeConfig& rt = config.realtime;

  const bool speakers = p.target_kind == TargetKind::Speakers;
  if (speakers && p.target_layout == ChannelLayout::Mono) {
    refuse_at(schema::child_path("target", "layout"), kTargetLayoutDomain, code);
  }
  for (int r = 0; r < kSpeakerRoleCount; ++r) {
    const auto role = static_cast<SpeakerRole>(r);
    const SpeakerPrepare& sp = p.speakers[static_cast<size_t>(r)];
    const float trim = rt.trim_db[static_cast<size_t>(r)];
    if (!speakers || !layout_has_speaker(p.target_layout, role)) {
      if (sp.has_distance || sp.size != SpeakerSize::Large || trim != 0.0f) {
        refuse_at(speaker_path(role), "must be a speaker of the output layout", code);
      }
      continue;
    }
    if (sp.has_distance)
      check_range(code, sp.distance_m, 0.1, 30.0, speaker_key(role, "distanceM"));
    check_range(code, trim, -20.0, 20.0, speaker_key(role, "trimDb"));
  }

  const BassManagementConfig& bm = p.bass_management;
  check_range(code, bm.crossover_hz, 40.0, 200.0, bass_key("crossoverHz"));
  check_range(code, rt.lfe_gain_db, -10.0, 15.0, bass_key("lfeGainDb"));
  if (speakers && bm.enabled) {
    if (bm.subwoofer && p.target_layout == ChannelLayout::Stereo) {
      throw Refusal::relation(code, {bass_key("subwoofer"), schema::child_path("target", "layout")},
                              "{0} must be false when {1} is stereo, which has no LFE plane")
          .as_caller_spelled();
    }
    const auto size_of = [&](SpeakerRole role) {
      return p.speakers[static_cast<size_t>(role)].size;
    };
    if (!bm.subwoofer && (size_of(SpeakerRole::L) == SpeakerSize::Small ||
                          size_of(SpeakerRole::R) == SpeakerSize::Small)) {
      throw Refusal::relation(code,
                              {bass_key("subwoofer"), speaker_key(SpeakerRole::L, "size"),
                               speaker_key(SpeakerRole::R, "size")},
                              "{0} must be true unless {1} and {2} are large")
          .as_caller_spelled();
    }
  }

  check_range(code, rt.lfe_mix_db, -60.0, 10.0, "lfeMixDb");
  check_range(code, rt.upmix_center_width, 0.05, 1.0, schema::child_path("upmix", "centerWidth"));
  check_range(code, rt.upmix_front_ambience, 0.0, 1.0,
              schema::child_path("upmix", "frontAmbience"));
  check_range(code, rt.dialogue_level_db, -12.0, 12.0, "dialogueLevelDb");
  if (rt.has_program_lufs)
    check_range(code, rt.program_lufs, -70.0, 0.0, schema::child_path("loudness", "programLufs"));
  check_range(code, rt.target_lufs, -40.0, -5.0, schema::child_path("loudness", "targetLufs"));
  check_range(code, rt.night_amount, 0.0, 1.0, schema::child_path("nightMode", "amount"));
  check_range(code, rt.room_mix_db, -30.0, 6.0, schema::child_path("room", "mixDb"));
  check_range(code, rt.limiter_ceiling_db, -12.0, 0.0,
              schema::child_path("outputLimiter", "ceilingDb"));
}

}  // namespace

void validate_renderer_config(const RendererConfig& config) {
  constexpr ErrorCode code = ErrorCode::InvalidParameter;
  const PrepareConfig& p = config.prepare;
  if (p.has_channel_map) {
    if (p.input_layout == InputLayout::Auto) {
      refuse_at(input_key("channelMap"), kNeedsFixedLayout, code);
    }
    check_channel_map(p, code);
  }
  validate_renderer_values(config, code);
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
