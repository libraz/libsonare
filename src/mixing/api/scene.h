#pragma once

/// @file scene.h
/// @brief Pure-data mixer scene schema and JSON helpers.

#include <string>
#include <utility>
#include <vector>

#include "core/channel_layout.h"

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
  Insert() = default;
  Insert(InsertSlot slot_, std::string processor_name_, std::string params_json_,
         std::string sidechain_key_ = {})
      : slot(slot_),
        processor_name(std::move(processor_name_)),
        params_json(std::move(params_json_)),
        sidechain_key(std::move(sidechain_key_)) {}

  InsertSlot slot = InsertSlot::PreFader;
  std::string processor_name;
  std::string params_json;
  std::string sidechain_key;
};

struct Send {
  std::string id;
  std::string destination_bus_id;
  float send_db = 0.0f;
  SendTiming timing = SendTiming::PostFader;
};

// Surround pan position for a strip feeding a >2-channel bus. Phase 1 honors
// azimuth/divergence/lfe; elevation/distance are reserved. Stored but inert
// until the surround DSP path applies it (see surround design). Mirrors
// mixing::SurroundPanParams without pulling the realtime DSP header into the
// pure-data scene schema.
struct SurroundPan {
  float azimuth = 0.0f;
  float elevation = 0.0f;
  float divergence = 0.0f;
  float lfe = 0.0f;
  float distance = 1.0f;
};

struct Strip {
  std::string id;
  float input_trim_db = 0.0f;
  float fader_db = 0.0f;
  float vca_offset_db = 0.0f;
  float pan = 0.0f;
  float width = 1.0f;
  bool muted = false;
  bool soloed = false;
  bool solo_safe = false;
  int pan_mode = 0;  // 0 = balance (matches SONARE_PAN_MODE_*).
  // Default to the panner's identity dual-pan routing (hard L / hard R) so a
  // DualPan strip without explicit dual-pan values preserves the stereo image
  // instead of collapsing to mono. Matches PannerProcessor's runtime defaults.
  float dual_pan_left = -1.0f;
  float dual_pan_right = 1.0f;
  bool polarity_invert_left = false;
  bool polarity_invert_right = false;
  int pan_law = 0;  // 0 = Const3dB (matches PanLaw enum order).
  int channel_delay_samples = 0;
  // Input channel layout of the source feeding this strip. A mono/stereo source
  // can feed a surround bus; the panner upmixes it (see surround design). Stored
  // but inert until the surround DSP path lands.
  ChannelLayout source_layout = ChannelLayout::Stereo;
  // Surround pan position, used when this strip feeds a surround bus. Serialized
  // only when non-default (see scene_json) so existing stereo scenes are
  // byte-identical.
  SurroundPan surround_pan;
  std::vector<Insert> inserts;
  std::vector<Send> sends;
};

struct Bus {
  Bus() = default;
  Bus(std::string id_, std::string role_ = "aux") : id(std::move(id_)), role(std::move(role_)) {}

  std::string id;
  std::string role = "aux";
  // Channel layout of this bus. The master bus (role == "master") carries the
  // project output layout. Defaults to stereo; surround layouts are stored but
  // inert until the surround DSP path lands.
  ChannelLayout layout = ChannelLayout::Stereo;
  std::vector<Insert> inserts;
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
