#pragma once

/// @file render_path_record.h
/// @brief Development diagnostic: the signal path an offline render realised.
///
/// A recorder is attached to one player or synth for one offline render, whose
/// pending changes realise inline at the top of a block, so what it records is
/// deterministic. Two layers are kept: the realised topology (each part's rig
/// chain and route, each fed insertion unit's stages and their enable state),
/// sampled at the top of every process() call and stored only when it changes;
/// and every GS EFX parameter byte an inline SysEx moved. Frames are the device
/// render frames the engine hands the instrument through set_transport().
///
/// Not part of any public surface. An instrument with no recorder attached pays
/// one null-pointer test per block and records nothing, so live paths are
/// untouched.

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "midi/synth/gs_efx_graph.h"

namespace sonare::midi::synth {

/// One part as the block renders it.
struct RenderPathPart {
  uint8_t part = 0;
  int program = 0;
  int bank = 0;
  /// "sf2", "model" or "none": what voices the part's notes.
  const char* backend = "none";
  /// "part", "destination", "bank" or "none": which entry selected the rig.
  const char* rig_source = "none";
  /// The part's realised rig stages, in signal order.
  std::vector<std::string> stages;
  /// Stages the rig asked for and the factory refused.
  std::vector<std::string> skipped;
  /// The insertion unit the part merges into, or -1.
  int unit = -1;
  int mono_prefix = 0;
  /// "pre_rig" (CC sends from the dry voice), "post_unit" (the unit's own sends)
  /// or "none" (no system effects).
  const char* send_tap = "none";

  bool operator==(const RenderPathPart& o) const;
  bool operator!=(const RenderPathPart& o) const { return !(*this == o); }
};

/// One fed insertion unit as the block renders it.
struct RenderPathUnit {
  uint8_t unit = 0;
  uint16_t type = 0;
  GsEfxRealization realization = GsEfxRealization::kModern;
  std::vector<std::string> stages;
  /// Whether each stage in `stages` is switched in.
  std::vector<bool> enabled;
  /// Stages present in position only, because the factory refused them.
  std::vector<std::string> skipped;

  bool operator==(const RenderPathUnit& o) const;
  bool operator!=(const RenderPathUnit& o) const { return !(*this == o); }
};

struct RenderPathTopology {
  std::vector<RenderPathPart> parts;
  std::vector<RenderPathUnit> units;

  bool operator==(const RenderPathTopology& o) const {
    return parts == o.parts && units == o.units;
  }
  bool operator!=(const RenderPathTopology& o) const { return !(*this == o); }
};

struct RenderPathEvent {
  enum class Kind : uint8_t { kTopology, kParam };
  Kind kind = Kind::kTopology;
  int64_t frame = 0;
  /// kTopology only.
  RenderPathTopology topology;
  /// kParam only: the unit, the index into its 20 parameter bytes, the new byte.
  uint8_t unit = 0;
  uint8_t slot = 0;
  uint8_t value = 0;
};

class RenderPathRecorder {
 public:
  /// The device frame the next records take effect at. A frame earlier than one
  /// already seen means the instrument started a second render pass; recording
  /// stops there and the record is marked incomplete.
  void set_block_frame(int64_t frame);
  int64_t block_frame() const noexcept { return frame_; }
  /// False once recording has stopped.
  bool recording() const noexcept { return !stopped_; }

  /// Appends @p topology unless it equals the last one recorded.
  void record_topology(RenderPathTopology topology);
  void record_param(uint8_t unit, uint8_t slot, uint8_t value);

  /// Called by the EFX stage as it builds a snapshot: forget the previous
  /// build's refusals, then note each stage of @p part the factory refused.
  void begin_snapshot_build() noexcept;
  void note_refused_stage(int part, std::string_view name);
  const std::vector<std::string>& refused_stages(int part) const {
    return refused_[static_cast<size_t>(part & 0x0F)];
  }

  /// Marks the record incomplete; the first reason is kept.
  void mark_incomplete(std::string_view reason);
  bool complete() const noexcept { return reason_.empty(); }
  const std::string& reason() const noexcept { return reason_; }

  const std::vector<RenderPathEvent>& events() const noexcept { return events_; }

  /// The record as one JSON document (schema 1).
  std::string to_json(std::string_view library_version) const;

 private:
  std::vector<RenderPathEvent> events_;
  std::array<std::vector<std::string>, 16> refused_{};
  std::string reason_;
  int64_t frame_ = 0;
  int64_t max_frame_ = 0;
  bool stopped_ = false;
  bool has_topology_ = false;
};

/// SHA-256 of tools/bank-versions.json as it stood when the library was built,
/// in lower-case hex, or nullptr when the build had no registry to read.
const char* bank_registry_digest() noexcept;

#if defined(SONARE_TUNING) && SONARE_TUNING
/// Tuning builds: the path SONARE_RENDER_PATH_DUMP names, or empty.
std::string render_path_dump_path();
/// Tuning builds: writes @p recorder's JSON to @p path. False when it cannot.
bool write_render_path_dump(const std::string& path, const RenderPathRecorder& recorder,
                            std::string_view library_version);
#endif

}  // namespace sonare::midi::synth
