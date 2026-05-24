#include <cctype>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#include "mixing/api/scene.h"

namespace sonare::mixing::api {
namespace {

std::string escape_json(const std::string& value) {
  std::ostringstream out;
  for (char c : value) {
    switch (c) {
      case '"':
        out << "\\\"";
        break;
      case '\\':
        out << "\\\\";
        break;
      case '\n':
        out << "\\n";
        break;
      default:
        out << c;
        break;
    }
  }
  return out.str();
}

const char* to_string(InsertSlot slot) { return slot == InsertSlot::PreFader ? "pre" : "post"; }

const char* to_string(SendTiming timing) { return timing == SendTiming::PreFader ? "pre" : "post"; }

InsertSlot insert_slot_from_string(const std::string& value) {
  if (value == "pre") return InsertSlot::PreFader;
  if (value == "post") return InsertSlot::PostFader;
  throw std::invalid_argument("unknown insert slot: " + value);
}

SendTiming send_timing_from_string(const std::string& value) {
  if (value == "pre") return SendTiming::PreFader;
  if (value == "post") return SendTiming::PostFader;
  throw std::invalid_argument("unknown send timing: " + value);
}

void write_key(std::ostringstream& out, const char* key) { out << '"' << key << "\":"; }

class Parser {
 public:
  explicit Parser(std::string text) : text_(std::move(text)) {}

  Scene parse_scene() {
    Scene scene;
    expect('{');
    while (!consume('}')) {
      const std::string key = parse_string();
      expect(':');
      if (key == "version") {
        scene.version = static_cast<int>(parse_number());
        if (scene.version != 1) {
          throw std::invalid_argument("unsupported scene JSON version");
        }
      } else if (key == "strips") {
        scene.strips = parse_strips();
      } else if (key == "buses") {
        scene.buses = parse_buses();
      } else if (key == "vcaGroups") {
        scene.vca_groups = parse_vca_groups();
      } else if (key == "connections") {
        scene.connections = parse_connections();
      } else {
        throw std::invalid_argument("unknown scene field: " + key);
      }
      consume(',');
    }
    skip_ws();
    if (pos_ != text_.size()) {
      throw std::invalid_argument("trailing data after scene JSON");
    }
    return scene;
  }

 private:
  std::vector<Strip> parse_strips() {
    std::vector<Strip> out;
    expect('[');
    while (!consume(']')) {
      Strip strip;
      expect('{');
      while (!consume('}')) {
        const std::string key = parse_string();
        expect(':');
        if (key == "id")
          strip.id = parse_string();
        else if (key == "faderDb")
          strip.fader_db = parse_float();
        else if (key == "pan")
          strip.pan = parse_float();
        else if (key == "width")
          strip.width = parse_float();
        else if (key == "muted")
          strip.muted = parse_bool();
        else if (key == "soloed")
          strip.soloed = parse_bool();
        else if (key == "soloSafe")
          strip.solo_safe = parse_bool();
        else if (key == "inserts")
          strip.inserts = parse_inserts();
        else if (key == "sends")
          strip.sends = parse_sends();
        else
          throw std::invalid_argument("unknown strip field: " + key);
        consume(',');
      }
      out.push_back(std::move(strip));
      consume(',');
    }
    return out;
  }

  std::vector<Insert> parse_inserts() {
    std::vector<Insert> out;
    expect('[');
    while (!consume(']')) {
      Insert insert;
      expect('{');
      while (!consume('}')) {
        const std::string key = parse_string();
        expect(':');
        if (key == "slot")
          insert.slot = insert_slot_from_string(parse_string());
        else if (key == "processor")
          insert.processor_name = parse_string();
        else if (key == "params")
          insert.params_json = parse_string();
        else
          throw std::invalid_argument("unknown insert field: " + key);
        consume(',');
      }
      out.push_back(std::move(insert));
      consume(',');
    }
    return out;
  }

  std::vector<Send> parse_sends() {
    std::vector<Send> out;
    expect('[');
    while (!consume(']')) {
      Send send;
      expect('{');
      while (!consume('}')) {
        const std::string key = parse_string();
        expect(':');
        if (key == "id")
          send.id = parse_string();
        else if (key == "destinationBusId")
          send.destination_bus_id = parse_string();
        else if (key == "sendDb")
          send.send_db = parse_float();
        else if (key == "timing")
          send.timing = send_timing_from_string(parse_string());
        else
          throw std::invalid_argument("unknown send field: " + key);
        consume(',');
      }
      out.push_back(std::move(send));
      consume(',');
    }
    return out;
  }

  std::vector<Bus> parse_buses() {
    std::vector<Bus> out;
    expect('[');
    while (!consume(']')) {
      Bus bus;
      expect('{');
      while (!consume('}')) {
        const std::string key = parse_string();
        expect(':');
        if (key == "id")
          bus.id = parse_string();
        else if (key == "role")
          bus.role = parse_string();
        else
          throw std::invalid_argument("unknown bus field: " + key);
        consume(',');
      }
      out.push_back(std::move(bus));
      consume(',');
    }
    return out;
  }

  std::vector<VcaGroup> parse_vca_groups() {
    std::vector<VcaGroup> out;
    expect('[');
    while (!consume(']')) {
      VcaGroup group;
      expect('{');
      while (!consume('}')) {
        const std::string key = parse_string();
        expect(':');
        if (key == "id")
          group.id = parse_string();
        else if (key == "gainDb")
          group.gain_db = parse_float();
        else if (key == "members")
          group.members = parse_string_array();
        else
          throw std::invalid_argument("unknown vca group field: " + key);
        consume(',');
      }
      out.push_back(std::move(group));
      consume(',');
    }
    return out;
  }

  std::vector<Connection> parse_connections() {
    std::vector<Connection> out;
    expect('[');
    while (!consume(']')) {
      Connection connection;
      expect('{');
      while (!consume('}')) {
        const std::string key = parse_string();
        expect(':');
        if (key == "source")
          connection.source = parse_string();
        else if (key == "destination")
          connection.destination = parse_string();
        else
          throw std::invalid_argument("unknown connection field: " + key);
        consume(',');
      }
      out.push_back(std::move(connection));
      consume(',');
    }
    return out;
  }

  std::vector<std::string> parse_string_array() {
    std::vector<std::string> out;
    expect('[');
    while (!consume(']')) {
      out.push_back(parse_string());
      consume(',');
    }
    return out;
  }

  bool parse_bool() {
    skip_ws();
    if (text_.compare(pos_, 4, "true") == 0) {
      pos_ += 4;
      return true;
    }
    if (text_.compare(pos_, 5, "false") == 0) {
      pos_ += 5;
      return false;
    }
    throw std::invalid_argument("expected bool");
  }

  float parse_float() { return static_cast<float>(parse_number()); }

  double parse_number() {
    skip_ws();
    const size_t start = pos_;
    if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) ++pos_;
    while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
    if (pos_ < text_.size() && text_[pos_] == '.') {
      ++pos_;
      while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
    }
    if (start == pos_) throw std::invalid_argument("expected number");
    return std::stod(text_.substr(start, pos_ - start));
  }

  std::string parse_string() {
    skip_ws();
    expect('"');
    std::string out;
    while (pos_ < text_.size()) {
      const char c = text_[pos_++];
      if (c == '"') return out;
      if (c == '\\') {
        if (pos_ >= text_.size()) throw std::invalid_argument("bad string escape");
        const char esc = text_[pos_++];
        out.push_back(esc == 'n' ? '\n' : esc);
      } else {
        out.push_back(c);
      }
    }
    throw std::invalid_argument("unterminated string");
  }

  bool consume(char expected) {
    skip_ws();
    if (pos_ < text_.size() && text_[pos_] == expected) {
      ++pos_;
      return true;
    }
    return false;
  }

  void expect(char expected) {
    if (!consume(expected)) {
      throw std::invalid_argument("unexpected JSON token");
    }
  }

  void skip_ws() {
    while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) ++pos_;
  }

  std::string text_;
  size_t pos_ = 0;
};

}  // namespace

std::string scene_to_json(const Scene& scene) {
  std::ostringstream out;
  out << std::setprecision(9);
  out << "{";
  write_key(out, "version");
  out << scene.version << ",";
  write_key(out, "strips");
  out << "[";
  for (size_t i = 0; i < scene.strips.size(); ++i) {
    const auto& strip = scene.strips[i];
    if (i > 0) out << ",";
    out << "{";
    write_key(out, "id");
    out << "\"" << escape_json(strip.id) << "\",";
    write_key(out, "faderDb");
    out << strip.fader_db << ",";
    write_key(out, "pan");
    out << strip.pan << ",";
    write_key(out, "width");
    out << strip.width << ",";
    write_key(out, "muted");
    out << (strip.muted ? "true" : "false") << ",";
    write_key(out, "soloed");
    out << (strip.soloed ? "true" : "false") << ",";
    write_key(out, "soloSafe");
    out << (strip.solo_safe ? "true" : "false") << ",";
    write_key(out, "inserts");
    out << "[";
    for (size_t j = 0; j < strip.inserts.size(); ++j) {
      const auto& insert = strip.inserts[j];
      if (j > 0) out << ",";
      out << "{\"slot\":\"" << to_string(insert.slot) << "\",\"processor\":\""
          << escape_json(insert.processor_name) << "\",\"params\":\""
          << escape_json(insert.params_json) << "\"}";
    }
    out << "],";
    write_key(out, "sends");
    out << "[";
    for (size_t j = 0; j < strip.sends.size(); ++j) {
      const auto& send = strip.sends[j];
      if (j > 0) out << ",";
      out << "{\"id\":\"" << escape_json(send.id) << "\",\"destinationBusId\":\""
          << escape_json(send.destination_bus_id) << "\",\"sendDb\":" << send.send_db
          << ",\"timing\":\"" << to_string(send.timing) << "\"}";
    }
    out << "]}";
  }
  out << "],";
  write_key(out, "buses");
  out << "[";
  for (size_t i = 0; i < scene.buses.size(); ++i) {
    const auto& bus = scene.buses[i];
    if (i > 0) out << ",";
    out << "{\"id\":\"" << escape_json(bus.id) << "\",\"role\":\"" << escape_json(bus.role)
        << "\"}";
  }
  out << "],";
  write_key(out, "vcaGroups");
  out << "[";
  for (size_t i = 0; i < scene.vca_groups.size(); ++i) {
    const auto& group = scene.vca_groups[i];
    if (i > 0) out << ",";
    out << "{\"id\":\"" << escape_json(group.id) << "\",\"gainDb\":" << group.gain_db
        << ",\"members\":[";
    for (size_t j = 0; j < group.members.size(); ++j) {
      if (j > 0) out << ",";
      out << "\"" << escape_json(group.members[j]) << "\"";
    }
    out << "]}";
  }
  out << "],";
  write_key(out, "connections");
  out << "[";
  for (size_t i = 0; i < scene.connections.size(); ++i) {
    const auto& connection = scene.connections[i];
    if (i > 0) out << ",";
    out << "{\"source\":\"" << escape_json(connection.source) << "\",\"destination\":\""
        << escape_json(connection.destination) << "\"}";
  }
  out << "]}";
  return out.str();
}

Scene scene_from_json(const std::string& json) { return Parser(json).parse_scene(); }

}  // namespace sonare::mixing::api
