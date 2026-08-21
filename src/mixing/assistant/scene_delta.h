#pragma once

/// @file scene_delta.h
/// @brief Composable scene edits produced by the mixing assistant's decision
///        stages.
///
/// @details **Offline / control thread only.**
///
/// @details Each decision domain returns @ref SceneDelta values rather than
///          writing a @ref mixing::api::Scene directly. Five domains run
///          independently and would otherwise silently overwrite each other:
///          the delta form makes the interaction explicit and gives every
///          change a reason string that becomes one line of the human-readable
///          explanation.
///
/// @details Generation and application stay separate. @ref apply_deltas takes
///          the base scene by const reference and returns a new one; it never
///          edits in place. A caller who wants the suggestion applied does that
///          as its own step, with the original scene still intact.

#include <optional>
#include <string>
#include <vector>

#include "mixing/api/scene.h"

namespace sonare::mixing::assistant {

/// @brief Decision domain a delta belongs to, and its application order.
/// @details The order is fixed and enumerator values are the sort key. A later
///          domain may rely on the earlier ones already being in the scene:
///          structure exists before anything routes to it, gain is staged before
///          EQ decides how much to carve, and the spatial pass runs last so it
///          has the final say on position.
enum class DeltaDomain {
  Structure = 0,  ///< Buses, VCA groups, connections, sends.
  Gain = 1,       ///< Input trim and fader.
  Eq = 2,         ///< Equalization inserts.
  Dynamics = 3,   ///< Compression, gating, ducking inserts.
  Image = 4,      ///< Pan, width, polarity, alignment delay.
};

/// @brief camelCase identifier for a delta domain.
const char* delta_domain_to_string(DeltaDomain domain) noexcept;

/// @brief One atomic scene edit with the reason it was made.
/// @details Every optional field distinguishes "not touched" from "set to
///          zero". Holding plain values instead would make a delta that leaves
///          pan alone indistinguishable from one that centres it, and whichever
///          delta applied last would flatten every other domain's decision.
///
///          One delta carries one decision, so @ref reason describes exactly
///          the change in it. Domains emit a vector of small deltas rather than
///          one large one.
struct SceneDelta {
  DeltaDomain domain = DeltaDomain::Gain;

  /// @brief Lower-case English declarative sentence explaining the change.
  /// @details Becomes one line of the result's explanation verbatim, so it is
  ///          written for the person reading the suggestion.
  std::string reason;

  /// @brief Strip the change applies to. Empty for a purely structural delta.
  /// @details A strip named here that is not yet in the scene is appended, so a
  ///          suggestion can be built against an empty scene.
  std::string strip_id;

  /// @name Additive fields
  /// @brief Summed across every delta that sets them, then clamped once.
  /// @details Gain staging sets an absolute-target trim and the balance pass
  ///          adds a class-relative offset on top; both are real decisions and
  ///          neither should erase the other.
  /// @{
  std::optional<float> input_trim_db;
  std::optional<float> fader_db;
  std::optional<float> vca_offset_db;
  /// @}

  /// @name Last-writer-wins fields
  /// @brief The last delta in application order that sets one decides it.
  /// @details These describe a position rather than an amount, so summing them
  ///          is meaningless. The spatial domain applies last and therefore has
  ///          the final say.
  /// @{
  std::optional<float> pan;
  std::optional<float> width;
  std::optional<bool> polarity_invert_left;
  std::optional<bool> polarity_invert_right;
  std::optional<int> channel_delay_samples;
  /// @}

  /// @name Append-only collections
  /// @brief Added, never replaced. A duplicate is dropped and reported.
  /// @details Two domains asking for the same processor in the same slot on the
  ///          same strip would otherwise stack two copies of it, which sounds
  ///          like a bug and reads like a feature. Dropping the second and
  ///          saying so in the notes is the recoverable behaviour.
  /// @{
  std::vector<api::Insert> inserts;
  std::vector<api::Send> sends;
  std::vector<api::Bus> buses;
  std::vector<api::VcaGroup> vca_groups;
  std::vector<api::Connection> connections;
  /// @}
};

/// @brief Applies deltas to a copy of @p base and returns the result.
/// @details Deltas are sorted stably by @ref SceneDelta::domain first, so the
///          caller may pass them in any order and still get the fixed
///          application order. Within a domain, the caller's order is preserved.
///
///          Ranges are clamped once after everything has been summed, not per
///          delta. Clamping each contribution separately would hide the fact
///          that the total overshot.
///
/// @param base Scene to build on. Never modified.
/// @param deltas Changes to apply, in any order.
/// @param notes Optional sink for messages about dropped duplicates and
///        clamped values. Appended to, never cleared.
/// @return A new scene with the deltas applied.
api::Scene apply_deltas(const api::Scene& base, const std::vector<SceneDelta>& deltas,
                        std::vector<std::string>* notes = nullptr);

/// @brief Lower clamp for a suggested input trim, in dB.
/// @details A staging correction, not a creative move. The range spans a hot
///          tracked signal to a very quiet one; anything past it means the track
///          should have been excluded rather than rescued.
inline constexpr float kMinSuggestedTrimDb = -24.0f;
/// @brief Upper clamp for a suggested input trim, in dB. See @ref kMinSuggestedTrimDb.
inline constexpr float kMaxSuggestedTrimDb = 24.0f;
/// @brief Lower clamp for a suggested fader position, in dB.
/// @details Far enough down to park a part without muting it, which is a
///          different decision the assistant does not make on its own.
inline constexpr float kMinSuggestedFaderDb = -40.0f;
/// @brief Upper clamp for a suggested fader position, in dB.
/// @details Console practice keeps the fader near unity and stages level at the
///          input, so a suggestion needing more than this is a staging problem.
inline constexpr float kMaxSuggestedFaderDb = 12.0f;
/// @brief Upper clamp for suggested stereo width.
/// @details Matching the imager's useful range; beyond it the mono fold starts
///          losing material.
inline constexpr float kMaxSuggestedWidth = 2.0f;
/// @brief Upper clamp for a suggested alignment delay, in samples.
/// @details The scene schema itself rejects anything larger.
inline constexpr int kMaxSuggestedDelaySamples = 192000;

}  // namespace sonare::mixing::assistant
