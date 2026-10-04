#pragma once

/// @file part_rig.h
/// @brief Per-part rig selection: what sits between a part's voices and the mix.
///
/// Layering: depends on nothing but the standard library, the same layer as
/// articulation_mode.h. Processor names and parameters stay strings here; the
/// layer that owns an insert factory resolves them.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sonare::midi {

/// Where a part's rig comes from.
enum class PartRigMode : uint8_t {
  /// The player's default rig for the part's program (pass-through when none).
  kBank = 0,
  /// No rig: the voice's direct output (DI).
  kNone = 1,
  /// An explicit chain, replacing the default rig.
  kChain = 2,
};

/// One stage of an explicit chain: an insert processor name and its parameters
/// as a JSON object string.
struct PartRigStage {
  std::string processor;
  std::string params_json;
};

/// A part's rig selection.
struct PartRig {
  PartRigMode mode = PartRigMode::kBank;
  /// Non-empty only for kChain.
  std::vector<PartRigStage> stages;
};

/// Most stages one chain may hold.
constexpr size_t kMaxPartRigStages = 8;
/// Part number addressing the destination default instead of one part.
constexpr uint8_t kPartRigAllParts = 0xFF;
/// Largest latency, in samples at 48 kHz, one chain stage may report.
constexpr int kMaxPartRigLatencySamples = 256;

/// Shape check shared by every entry point: part range, mode range, stage count,
/// and stages present exactly for kChain. Processor names and parameters are
/// validated by the layer that owns the insert factory.
inline bool validate_part_rig(uint8_t part, const PartRig& rig) noexcept {
  if (part >= 16 && part != kPartRigAllParts) return false;
  if (static_cast<uint8_t>(rig.mode) > static_cast<uint8_t>(PartRigMode::kChain)) return false;
  if (rig.stages.size() > kMaxPartRigStages) return false;
  if (rig.mode != PartRigMode::kChain) return rig.stages.empty();
  if (rig.stages.empty()) return false;
  for (const PartRigStage& stage : rig.stages) {
    if (stage.processor.empty()) return false;
  }
  return true;
}

}  // namespace sonare::midi
