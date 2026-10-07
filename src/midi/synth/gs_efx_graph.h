#pragma once

/// @file gs_efx_graph.h
/// @brief The allocation-owned GS EFX unit graph shared by MIDI and audio.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "midi/synth/gs_efx_bindings.h"
#include "midi/synth/gs_layer.h"
#include "rt/delay_line.h"
#include "rt/fractional_delay.h"
#include "rt/processor_base.h"

namespace sonare::midi::synth {

using GsEfxStageFactory = std::function<std::unique_ptr<rt::ProcessorBase>(
    std::string_view name, std::string_view json_params)>;

/// A fixed, control-thread prepared delay used to keep a bypassed EFX stage
/// on the same timeline as its active child.  The delay is also primed while
/// the child is active, so switching a stage off does not open a cold delay
/// line on the audio thread.  The two channel lanes and all fractional scratch
/// are owned by the realised unit; process/prime never allocate.
struct Sf2EfxDelayRt {
  void prepare(int delay_samples_q8);
  void reset() noexcept;
  void process(float* left, float* right, int num_samples) const noexcept;
  void prime(const float* left, const float* right, int num_samples) const noexcept;

  int delay_samples_q8() const noexcept { return delay_samples_q8_; }

 private:
  void process_lane(float* samples, int num_samples, size_t lane) const noexcept;
  void prime_lane(const float* samples, int num_samples, size_t lane) const noexcept;

  int delay_samples_q8_ = 0;
  mutable std::array<rt::DelayLine, 2> integer_{};
  mutable std::array<std::vector<float>, 2> fractional_{};
  mutable std::array<size_t, 2> fractional_write_{};
};

/// One stage of a realised insertion unit, at the same position as the stage
/// gs_efx_insert_chain (or the classic unit) describes, so a queued update
/// addresses it by index. The identity fields are read by the control thread
/// only; `fade`, `enabled_now` and `dry_delay` belong to the audio thread.
struct Sf2EfxStageRt {
  /// Null where the factory could not build the stage; the position is kept
  /// and nothing runs there.
  std::unique_ptr<rt::ProcessorBase> proc;
  std::string name;
  uint8_t branch = kGsEfxBranchFront;
  uint8_t ordinal = 0;
  /// The child's Q8 latency cached at prepare; zero in the classic realization.
  int latency_samples_q8 = 0;
  /// The enable target used by the AUDIO-side plan application. CONTROL only
  /// initializes this field while constructing a fresh snapshot; it never
  /// reads or updates the published value.
  mutable bool enabled_target = true;
  /// AUDIO thread: 1 runs the stage, 0 passes its input through the latency
  /// matched dry delay with the processor's state frozen, in between crossfades
  /// dry to wet.
  mutable float fade = 1.0f;
  /// AUDIO thread: the enable state `fade` is moving toward.
  mutable bool enabled_now = true;
  /// AUDIO thread: keeps the bypassed path on the child's timeline.
  mutable Sf2EfxDelayRt dry_delay{};
};

/// One control destination resolved against an insertion stage. A modern
/// destination reads the slot byte through @c binding; a classic one (null @c
/// binding) takes the byte itself in slot @c param_id.
struct Sf2EfxControlDest {
  uint8_t stage_index = 0;
  uint32_t param_id = 0;
  const GsEfxBindingRow* binding = nullptr;
};

/// A descriptor resolved against a published legacy unit. Unlike a prepared
/// node's strict plan, this is partial: a custom processor may expose only a
/// subset of the generated GS rows, and the rows without a safe descriptor are
/// simply left on the already realised processor.
struct Sf2EfxLegacyParamDest {
  GsEfxBindingRow row{};
  uint8_t stage_index = 0;
  uint32_t param_id = 0;
};

/// One selector/switch rule resolved against a published legacy unit. The
/// stage indices are 0xFF where the published chain has no corresponding
/// processor, which keeps partial custom graphs valid.
struct Sf2EfxLegacyEnablePlan {
  GsEfxEnable rule{};
  GsEfxEnableStageIndices stage_indices = kGsEfxUnmappedStageIndices;
};

/// One EFX CONTROL fanout resolved against published unit 0. Unlike
/// Sf2EfxControlRt, this plan is retained even when the snapshot's original
/// source byte was zero, so a later scheduled source write can activate the
/// already validated destinations without rebuilding the graph.
struct Sf2EfxLegacyControlPlan {
  uint8_t slot = 0;
  uint8_t lo = 0;
  uint8_t hi = 0;
  uint8_t states = 0;
  uint8_t n_dest = 0;
  std::array<Sf2EfxControlDest, 4> dest{};
};

/// One realised insertion unit: its stages in chain order and the scratch its
/// parallel halves and fades run in.
struct Sf2EfxUnitRt {
  GsEfxRealization realization = GsEfxRealization::kModern;
  std::vector<Sf2EfxStageRt> stages;
  /// Dry copy, half A and half B, each stereo x the render chunk. Allocated on
  /// the control thread; the audio thread writes it through the const snapshot.
  mutable std::vector<float> scratch;
  /// Fade movement per sample: a whole fade takes kSf2EfxFadeMs.
  float fade_step = 1.0f;
  /// Q8 graph latency: front + the slower half + back; zero for classic.
  int latency_samples_q8 = 0;
  /// Extra tail the fractional compensation delays add beyond the floored latency.
  int latency_compensation_tail_samples = 0;
  /// AUDIO thread: delays the faster half onto the slower half's timeline.
  mutable Sf2EfxDelayRt half_a_alignment{};
  mutable Sf2EfxDelayRt half_b_alignment{};
  /// CONTROL-built partial plan for candidate-less prepared deltas. These
  /// vectors belong to the immutable published snapshot; AUDIO only iterates
  /// them and calls already-validated realtime-safe setters. Only unit 0
  /// carries @c legacy_controls.
  std::vector<Sf2EfxLegacyParamDest> legacy_param_dests;
  std::vector<Sf2EfxLegacyEnablePlan> legacy_enable_plans;
  std::vector<uint8_t> legacy_default_enabled;
  std::array<Sf2EfxLegacyControlPlan, 2> legacy_controls{};
  std::array<uint8_t, 20> legacy_classic_slots{};
  uint8_t legacy_classic_slot_count = 0;
};

/// Length of the linear crossfade a stage's enable switch takes.
inline constexpr float kSf2EfxFadeMs = 5.0f;

/// EFX CONTROL 1 or 2 of unit 0 (40 03 1B-1E), resolved when the unit is built:
/// the source of part @c part moves slot @c slot (the type's `+` or `#` slot)
/// away from its base byte within [@c lo, @c hi], scaled by the depth. Built
/// only where the source is a controller and the type marks a slot; otherwise
/// @c n_dest is 0 and the control is inert. The identity fields are read by the
/// control thread; the three mutable ones belong to the audio thread.
struct Sf2EfxControlRt {
  uint8_t part = 0;
  uint8_t source = 0;    ///< Raw CONTROL SOURCE byte: 01-5F CC1-95, 60 CAf, 61 bend.
  uint8_t depth = 0x40;  ///< Raw CONTROL DEPTH byte; 40 is no modulation.
  uint8_t slot = 0;
  uint8_t lo = 0;  ///< Lowest byte the slot takes.
  uint8_t hi = 0;  ///< Highest byte the slot takes.
  /// How many evenly spaced bytes across [lo, hi] the slot prints as states
  /// (00/7F is two); 0 where every byte between is a value.
  uint8_t states = 0;
  uint8_t n_dest = 0;
  std::array<Sf2EfxControlDest, 4> dest{};
  /// AUDIO thread: the slot's unmodulated byte, moved by kControlBase.
  mutable uint8_t base_byte = 0;
  /// AUDIO thread: the byte the destinations last received.
  mutable uint8_t applied_byte = 0;
  /// AUDIO thread: an update rewrote a destination, so the next block writes it
  /// whether or not the modulated byte moved.
  mutable bool dirty = false;
};

/// How one binding row resolved against a realised unit. Each caller decides
/// which outcomes skip the row and which reject the whole plan.
enum class Sf2EfxRowResolution : uint8_t {
  kInvalidRow,         ///< Stage or key index outside the generated name tables.
  kNoStage,            ///< No stage with the row's name and ordinal.
  kNoProcessor,        ///< The stage holds no processor.
  kNoRealtimeControl,  ///< No descriptor for the key, or it is not realtime-safe.
  kResolved,
};

/// The stage and realtime-safe parameter one binding row drives.
struct Sf2EfxRowTarget {
  Sf2EfxRowResolution status = Sf2EfxRowResolution::kInvalidRow;
  int stage_index = -1;
  uint32_t param_id = 0;
};

/// Index of the stage named @p name at @p ordinal in @p unit, or -1.
int sf2_find_efx_stage(const Sf2EfxUnitRt& unit, std::string_view name, uint8_t ordinal) noexcept;

/// CONTROL thread: resolve @p row to its stage and the first descriptor with
/// the row's key, which must be realtime-safe. Allocates.
Sf2EfxRowTarget sf2_resolve_efx_row(const Sf2EfxUnitRt& unit, const GsEfxBindingRow& row);

/// CONTROL thread: EFX CONTROL @p control (0 drives the type's `+` slot, 1 its
/// `#` slot) of EFX type @p type, resolved against @p unit over @p rows. A
/// classic unit reads the slot's wire byte, so its one destination is the slot
/// itself; a modern unit takes every realtime-safe control the slot's rows
/// reach. n_dest is 0 where the type marks no slot or nothing resolved.
/// Allocates.
Sf2EfxLegacyControlPlan sf2_resolve_efx_control(const Sf2EfxUnitRt& unit, const GsEfxRowView& rows,
                                                uint16_t type, size_t control);

/// CONTROL thread: realise one insertion unit. kModern builds @p stages
/// through @p factory, keeping a null processor where a stage cannot be built;
/// kClassic ignores @p stages and makes the type's classic unit the only stage,
/// holding @p efx's bytes. Type 00 00 (Thru) realises no stage either way.
/// Allocates.
Sf2EfxUnitRt sf2_build_efx_unit(const GsEfx& efx, const std::vector<GsEfxStage>& stages,
                                GsEfxRealization realization, const GsEfxStageFactory& factory,
                                double sample_rate, int max_block);

/// CONTROL thread: prepare child processors and all shared delay state for a
/// realised unit.  Repreparing a unit resets its delay storage and is required
/// before the next audio block when the host rate or block bound changes.
void sf2_prepare_efx_unit(Sf2EfxUnitRt& unit, double sample_rate, int max_block);

/// CONTROL/audio quiescent boundary: reset every child, stage dry delay, and
/// parallel alignment delay owned by a realised unit.
void sf2_reset_efx_unit(Sf2EfxUnitRt& unit) noexcept;

/// Cached realised-unit latency in Q8 samples.  A classic unit and Thru are
/// explicitly zero even when a future classic child grows an internal delay;
/// that realization remains intentionally uncompensated.
int sf2_efx_unit_latency_samples_q8(const Sf2EfxUnitRt& unit) noexcept;

/// Tail of a realised unit's serial/parallel graph, including any fractional
/// latency compensation support beyond the floored graph latency.
int sf2_efx_unit_tail_samples(const Sf2EfxUnitRt& unit) noexcept;

/// AUDIO thread: run @p unit in place over @p n frames (n <= the max_block it
/// was built for): the front stages, then each half on its own copy of their
/// output summed back, then the back stages. Allocation-free.
void sf2_run_efx_unit(const Sf2EfxUnitRt& unit, float* left, float* right, int n) noexcept;

}  // namespace sonare::midi::synth
