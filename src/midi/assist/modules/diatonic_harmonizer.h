#pragma once

/// @file diatonic_harmonizer.h
/// @brief Built-in ICounterpointEngine: derives a harmony voice from a line
///        already in the project.
///
/// It moves each source note a fixed number of SCALE STEPS, not a fixed number
/// of semitones, so "a third below" follows the key rather than alternating
/// between a major and a minor third by accident. Every derived note then goes
/// through the placement judge, and one the judge refuses is dropped with its
/// reason recorded rather than forced in.
///
/// Proposes only, and proposes content rather than structure: the result is a
/// PatchMidiClip against a clip the request NAMES, and a request naming none
/// comes back empty saying so. The family's contract -- rule-based, applied by
/// the caller, seeded rather than clocked -- is in sonare_c_assist.h.
///
/// Control/offline thread only.

#include <vector>

#include "arrangement/project_view.h"
#include "midi/assist/i_counterpoint_engine.h"
#include "midi/assist/modules/generator_params.h"
#include "midi/assist/modules/placement_judge.h"

namespace sonare::midi::assist::modules {

struct DiatonicHarmonizerConfig {
  /// Scale steps the derived voice sits from the source. Negative is below.
  /// -2 is a third below, which is the default because it is the one interval
  /// that stays inside the chord for most of a diatonic line.
  int interval_steps = -2;
};

class DiatonicHarmonizer final : public ICounterpointEngine {
 public:
  explicit DiatonicHarmonizer(DiatonicHarmonizerConfig config = {}) noexcept : config_(config) {}

  const char* module_id() const noexcept override { return kModuleId; }

  /// @brief Derives one harmony voice from the clip @p request names.
  /// @details `request.params_json` is read as @ref GeneratorParams: it must
  ///          name `target_clip_id`, may name a separate `source_clip_id`, and
  ///          may narrow the voice with `low_note` / `high_note` and scale the
  ///          derived velocities.
  ///
  ///          `cantus` narrows the voice further when it carries one: its first
  ///          entry's range wins over the params, because a host that modelled
  ///          the voice explicitly has said more than a default.
  ///
  ///          A note is dropped, with the reason recorded, when the key at its
  ///          position is unknown (there are no steps to move through), when the
  ///          transposition leaves the MIDI range, or when the judge refuses it.
  /// @return An AssistResult carrying at most one PatchMidiClip, a JSON
  ///         candidate payload of the per-note decisions, and diagnostics.
  ///         Never throws for a malformed request -- it comes back empty with a
  ///         reason, because a throw is discarded by the driver and the reason
  ///         would be lost with it.
  AssistResult derive(const arrangement::ProjectView& view, const AssistRequest& request,
                      const std::vector<VoiceModel>& cantus,
                      const AssistQueryContext& queries) override;

  static constexpr const char* kModuleId = "sonare.builtin.diatonic_harmonizer";

 private:
  DiatonicHarmonizerConfig config_{};
};

}  // namespace sonare::midi::assist::modules
