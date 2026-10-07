#pragma once

/// @file scene.h
/// @brief Pure-data mixer scene schema and JSON helpers.

#include <string>
#include <utility>
#include <vector>

#include "core/channel_layout.h"
#include "mastering/eq/eq_band.h"

// Forward-declared rather than including util/json.h: this header is the
// pure-data schema and is pulled into the arrangement compile path, which has no
// business depending on the JSON parser. The macro guard mirrors util/json.h's
// own so the two namespaces cannot desync.
#ifndef SONARE_JSON_NAMESPACE
#define SONARE_JSON_NAMESPACE sonare::util::json
#endif
namespace SONARE_JSON_NAMESPACE {
class Value;
}  // namespace SONARE_JSON_NAMESPACE

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
// azimuth/divergence/lfe; elevation/distance are reserved. Applied wherever the
// strip's main output meets a destination wider than two planes: the engine's
// lane scatter and the mixer graph's strip scatter (a project bounce at 6/8
// channels). The public stereo mixer builds its master at two planes, so there
// it has no effect. Mirrors
// mixing::SurroundPanParams without pulling the realtime DSP header into the
// pure-data scene schema.
struct SurroundPan {
  float azimuth = 0.0f;
  float elevation = 0.0f;
  float divergence = 0.0f;
  float lfe = 0.0f;
  float distance = 1.0f;
};

// Meter configuration for a strip's pre/post taps. Fixed when the strip is
// built (the meters size their buffers in prepare()), so it travels with the
// scene rather than through a setter. Defaults reproduce the historical full
// configuration; a scene that leaves it alone serializes no metering object at
// all and stays byte-identical.
struct StripMetering {
  bool enabled = true;
  bool lufs = true;
  bool true_peak = true;
  // Requested true-peak oversample factor. The realtime meter resolves it as
  // 2 -> 2x, 8..16 -> 8x, and any other value (0, 1, 3..7) -> 4x.
  int true_peak_oversample = 4;
};

// A strip or bus's dedicated equalizer. `enabled` gates the stage without
// discarding the bands; `bands[i]` is that slot's band.
struct StripEq {
  bool enabled = true;
  std::vector<mastering::eq::EqBand> bands;
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
  // Input channel layout of the source feeding this strip. Stored and
  // round-tripped only: a strip processes two planes, and a wider destination is
  // reached through the surround pan above.
  ChannelLayout source_layout = ChannelLayout::Stereo;
  // Surround pan position, used when this strip feeds a surround bus. Serialized
  // only when non-default (see scene_json) so existing stereo scenes are
  // byte-identical.
  SurroundPan surround_pan;
  // What this strip's meters measure. Serialized only when non-default so
  // existing scenes are byte-identical.
  StripMetering metering;
  std::vector<Insert> inserts;
  std::vector<Send> sends;
  // Omitted from the document at its default, so an existing scene stays byte-identical.
  StripEq eq;
};

struct Bus {
  Bus() = default;
  Bus(std::string id_, std::string role_ = "aux") : id(std::move(id_)), role(std::move(role_)) {}

  std::string id;
  std::string role = "aux";
  // Channel layout of this bus. A 5.1/7.1 bus processes that many planes; mono
  // and stereo buses process two. The master (role == "master") is instead built
  // at the render's output width, and its layout is the widest output allowed.
  ChannelLayout layout = ChannelLayout::Stereo;
  // Output processing applied to the bus's summed signal, mirroring a Strip's
  // input trim / stereo width / polarity. Trim and polarity run before the
  // insert chain; width runs after it. All three are no-ops at their defaults
  // (0 dB / width 1 / no invert), so a bus that never sets them is bit-identical.
  float input_trim_db = 0.0f;
  float width = 1.0f;
  bool polarity_invert_left = false;
  bool polarity_invert_right = false;
  // Pan, as on Strip. A non-default pan or width on a layout wider than two
  // channels is rejected.
  float pan = 0.0f;
  int pan_mode = 0;  // 0 = balance (matches SONARE_PAN_MODE_*).
  float dual_pan_left = -1.0f;
  float dual_pan_right = 1.0f;
  int pan_law = 0;  // 0 = Const3dB (matches PanLaw enum order).
  std::vector<Insert> inserts;
  // Dedicated bus EQ, applied before inserts. Omitted at its default, like Strip's.
  StripEq eq;
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

/// Parses a scene document. When `warnings` is non-null, every key the reader does not
/// consume is appended as `unknown scene key '<path>'` (array indices are the original
/// positions). Keys starting with `$` or `x-` are exempt at every level, and insert
/// `params` bags are not walked. Unknown keys never fail the parse.
Scene scene_from_json(const std::string& json, std::vector<std::string>* warnings = nullptr);

/// Canonical field paths for one Scene document, rooted at the document object
/// (no leading `scene.`). Array item fields use `[]`, e.g. `strips[].id`. Kept
/// literal, not composed, so a checker outside this language can parse it; see
/// @ref sonare::mixing::assistant::mix_assistant_result_schema_paths for the
/// copy this is duplicated into under a `scene.` prefix.
const std::vector<std::string>& scene_schema_paths();

/// Walks an ALREADY-PARSED scene document. `scene_from_json` is this plus a
/// parse. A caller that has parsed a larger document containing a scene should
/// use this rather than dumping the sub-tree back to text and re-parsing it:
/// the round trip costs a second parse and puts it under whatever resource
/// limits that second parse happens to carry, instead of the ones the enclosing
/// document was admitted under.
Scene scene_from_value(const SONARE_JSON_NAMESPACE::Value& value,
                       std::vector<std::string>* warnings = nullptr);

}  // namespace sonare::mixing::api
