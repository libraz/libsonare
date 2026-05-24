#pragma once

/// @file scene.h
/// @brief Pure-data mixer scene schema and JSON helpers.

#include <string>
#include <vector>

namespace sonare::mixing::api {

enum class InsertSlot {
  PreFader,
  PostFader,
};

enum class SendTiming {
  PreFader,
  PostFader,
};

struct Insert {
  InsertSlot slot = InsertSlot::PreFader;
  std::string processor_name;
  std::string params_json;
};

struct Send {
  std::string id;
  std::string destination_bus_id;
  float send_db = 0.0f;
  SendTiming timing = SendTiming::PostFader;
};

struct Strip {
  std::string id;
  float fader_db = 0.0f;
  float pan = 0.0f;
  float width = 1.0f;
  bool muted = false;
  bool soloed = false;
  bool solo_safe = false;
  std::vector<Insert> inserts;
  std::vector<Send> sends;
};

struct Bus {
  std::string id;
  std::string role = "aux";
};

struct VcaGroup {
  std::string id;
  float gain_db = 0.0f;
  std::vector<std::string> members;
};

struct Connection {
  std::string source;
  std::string destination;
};

struct Scene {
  int version = 1;
  std::vector<Strip> strips;
  std::vector<Bus> buses;
  std::vector<VcaGroup> vca_groups;
  std::vector<Connection> connections;
};

std::string scene_to_json(const Scene& scene);
Scene scene_from_json(const std::string& json);

}  // namespace sonare::mixing::api
