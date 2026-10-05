#pragma once

/// @file part_fx_stage.h
/// @brief Part buses, rig chains and GS insertion units shared by the players.
///
/// A player sums each bussed part's voices into that part's bus; the stage runs
/// the part's rig chain on it in place, and runs each insertion unit once over
/// the sum of the parts the file routed into it. Mixing the results, system
/// sends, master-EQ bypass and source attribution stay with the player.
///
/// A part's rig is resolved in this order: its own entry, the destination
/// default (part kPartRigAllParts), the bank-rig switch, then the bank's default
/// for the part's program. A rig carrying `saturation.ampSim` is fed a mono
/// pickup through its last amplifier and the part pan is restored after it.
///
/// Threads: snapshots are built on the CONTROL thread and handed to the audio
/// thread through rt::RtPublisher; parameter-only GS edits travel through a
/// wait-free queue. An offline host realises on the render thread instead, so
/// the EFX mirror and the rig entries belong to whichever thread realises.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "midi/part_rig.h"
#include "midi/synth/gs_efx_graph.h"
#include "midi/synth/gs_layer.h"
#include "rt/processor_base.h"
#include "rt/rt_publisher.h"

namespace sonare::midi::synth {

/// Frames per bus chunk; a player renders its voices into the buses this many
/// frames at a time.
inline constexpr int kPartFxChunkFrames = 256;

/// @name EFX CONTROL SOURCE bytes
/// 01-5F name CC1-95, 60 channel aftertouch and 61 the bend; 00 and 62-7F are off.
/// @{
inline constexpr uint8_t kEfxSourceAftertouch = 0x60;
inline constexpr uint8_t kEfxSourceBend = 0x61;
/// @}

/// The byte @p control puts its slot at with its source at @p position: the
/// base's place in [lo, hi] moved by depth x position, clamped to the range and
/// rounded to the nearest byte the slot takes, so a two-state slot switches at
/// half. The depth is centred on 40 over 64 steps.
uint8_t efx_control_byte(const Sf2EfxControlRt& control, float position) noexcept;

/// A realised set of part chains and insertion units, handed to the audio
/// thread as one immutable-lifetime snapshot. Built entirely on the CONTROL
/// thread, so the audio thread only reads it (running the processors, which a
/// `const unique_ptr` still allows) and never allocates, frees, or races the
/// builder.
struct PartFxSnapshot {
  /// The rig chain run in place on each part's bus.
  std::array<std::vector<std::unique_ptr<rt::ProcessorBase>>, 16> chains{};
  /// The processor name of each stage in chains, in order.
  std::array<std::vector<std::string>, 16> stage_names{};
  /// Whether the part sums through its bus rather than straight to the mix.
  std::array<bool, 16> part_bussed{};
  /// The file's insertion effects, one chain per unit rather than one per part:
  /// parts assigned to a unit sum into it and it runs once (docs/gs.md). Unit 0
  /// is the spec one; 1-15 are the extension's.
  std::array<Sf2EfxUnitRt, kGsEfxUnitCount> units{};
  /// EFX CONTROL 1 and 2, which only unit 0 has.
  std::array<Sf2EfxControlRt, 2> controls{};
  /// Which build this is. A queued update carries the generation it was
  /// resolved against and is dropped by any other snapshot.
  uint32_t generation = 0;
  /// The unit each part merges into after its own chain, or kNoUnit.
  static constexpr uint8_t kNoUnit = 0xFF;
  std::array<uint8_t, 16> part_unit{};
  /// Whether a unit has any part feeding it. A unit nothing feeds is not run.
  std::array<bool, kGsEfxUnitCount> unit_fed{};
  bool any_unit = false;
  bool any_bussed = false;
  /// Stages of the part's chain, through its last amplifier, that take the
  /// mono pickup; 0 for a stereo chain.
  std::array<uint8_t, 16> mono_prefix{};
  /// Bussed by the part's own rig, excluding GS unit assignments, so an overlay
  /// can re-derive routing without reading CONTROL-owned state.
  std::array<bool, 16> host_part_bussed{};
  /// The GS state the units were built from.
  std::array<GsEfx, kGsEfxUnitCount> gs_efx_state{};
  std::array<uint8_t, 16> gs_part_assign{};
};

/// Per-unit replacements for the snapshot's units: a non-null entry runs in
/// place of that unit.
using PartFxUnitOverrides = std::array<const Sf2EfxUnitRt*, kGsEfxUnitCount>;

/// What the stage reads from the player that owns the parts.
class PartFxHost {
 public:
  /// Where the controller @p source (an EFX CONTROL SOURCE byte) sits on
  /// @p part: 0..1, or -1..+1 for the bend.
  virtual float part_controller_position(int part, uint8_t source) const noexcept = 0;
  /// The part's pan in voice pan units, restored after a mono rig prefix.
  virtual float part_pan_units(int part) const noexcept = 0;

 protected:
  ~PartFxHost() = default;
};

struct PartFxStageConfig {
  /// Builds a streaming processor from a name and JSON params. Without one the
  /// stage is disabled: no buses, no chains, no units.
  GsEfxStageFactory insert_factory;
  /// Whether a part with no entry takes the bank's default rig.
  bool bank_rig_binding = true;
  GsEfxRealization realization = GsEfxRealization::kModern;
};

class PartFxStage {
 public:
  /// Saved EFX mirror for a rollback around an allocating rebuild.
  struct Checkpoint {
    std::array<GsEfx, kGsEfxUnitCount> efx{};
    std::array<uint8_t, 16> assign{};
    bool dirty = false;
  };
  /// The rig entries: one per part and, last, the destination default.
  struct RigTable {
    std::array<PartRig, 17> rigs{};
    std::array<bool, 17> present{};
  };

  PartFxStage() : PartFxStage(PartFxStageConfig{}) {}
  explicit PartFxStage(PartFxStageConfig config);
  ~PartFxStage();
  PartFxStage(PartFxStage&&) noexcept;
  PartFxStage& operator=(PartFxStage&&) noexcept;

  bool enabled() const noexcept { return static_cast<bool>(config_.insert_factory); }

  /// CONTROL thread: allocate the buses at @p sample_rate. Allocates.
  void prepare(double sample_rate);
  /// CONTROL thread at a quiescent boundary: clear the EFX mirror and drop any
  /// queued parameter update.
  void clear_mirror();
  /// CONTROL thread: build and publish a fresh snapshot under the next
  /// generation. Allocates.
  void publish();

  /// CONTROL thread: store @p rig for @p part (kPartRigAllParts for the
  /// destination default). Returns false when it fails validate_part_rig or a
  /// built chain stage exceeds 256 samples at the 48 kHz reference rate. A
  /// factory or processor prepare may throw. Does not publish.
  bool set_part_rig(uint8_t part, const PartRig& rig);
  const RigTable& rig_table() const noexcept { return rigs_; }
  void restore_rig_table(RigTable table) noexcept;

  /// AUDIO thread: record @p id as the bank rig @p part's program binds.
  /// Returns true when the change reaches a built chain, so a rebuild is owed.
  bool publish_part_rig(int part, uint8_t id) noexcept;
  uint8_t part_rig_id(int part) const noexcept;

  // EFX mirror.
  const std::array<GsEfx, kGsEfxUnitCount>& efx() const noexcept { return efx_; }
  const std::array<uint8_t, 16>& part_assign() const noexcept { return assign_; }
  /// Raised when the mirror or a bound rig changed since the last realise.
  bool dirty() const noexcept { return dirty_; }
  void mark_dirty() noexcept { dirty_ = true; }
  void clear_dirty() noexcept { dirty_ = false; }
  Checkpoint checkpoint() const noexcept { return {efx_, assign_, dirty_}; }
  void restore(const Checkpoint& saved) noexcept;
  /// Offline render thread: a GS/GM reset selects Thru and clears the part
  /// switches.
  void clear_efx() noexcept;
  /// Offline render thread: GS 40 4x 22 for part slot @p part.
  void assign_part(uint8_t part, uint8_t value) noexcept;
  /// Offline render thread: an EFX-block write (40 03 xx / 40 3u xx). Returns
  /// true when it was one.
  bool apply_unit_sysex(const uint8_t* data, size_t size) noexcept;
  /// CONTROL thread: apply the EFX content of a GS SysEx to the mirror alone.
  void mirror_sysex(const uint8_t* data, size_t size) noexcept;
  /// CONTROL thread: apply the EFX content of a GS SysEx to the mirror. Returns
  /// true when a full rebuild is required, false when the message was handled
  /// without one (a parameter-only edit queued for the audio thread, or not an
  /// EFX message). May throw on allocation.
  bool apply_control_sysex(const uint8_t* data, size_t size);

  GsEfxRealization realization() const noexcept { return config_.realization; }
  void set_realization(GsEfxRealization realization) noexcept { config_.realization = realization; }
  /// Rows the units are realised over in place of the generated tables, or null.
  const GsEfxRowView* rows() const noexcept { return rows_; }
  void set_rows(const GsEfxRowView* rows) noexcept { rows_ = rows; }
  /// rows(), or the generated tables.
  const GsEfxRowView& row_view() const noexcept;
  /// The modern stage list of @p efx over row_view().
  std::vector<GsEfxStage> efx_stages(const GsEfx& efx) const;
  /// CONTROL thread: a unit for @p efx under the current realisation, built and
  /// prepared at the stage's rate. Allocates.
  Sf2EfxUnitRt build_unit(const GsEfx& efx) const;

  /// AUDIO thread: the snapshot adopted by the last acquire().
  const PartFxSnapshot* current() const noexcept { return pub_->current(); }
  void acquire() noexcept { pub_->acquire(); }
  /// CONTROL thread: the last published snapshot.
  const PartFxSnapshot* control_current() const noexcept { return pub_->control_current().get(); }
  /// How many snapshots have been published.
  uint32_t generation() const noexcept { return generation_; }

  /// AUDIO thread: apply every pending parameter update to the current units.
  void drain_param_updates() noexcept;
  /// AUDIO thread, after drain_param_updates(): move each EFX CONTROL's slot to
  /// where its source now puts it.
  void apply_controls(const PartFxHost& host) noexcept;
  /// AUDIO thread: apply the published unit's retained plan for @p target in
  /// place. EFX CONTROL reads @p control_part; a negative part skips CONTROL
  /// and the enable rules.
  void apply_legacy_plan(size_t unit, const GsEfx& target, int control_part,
                         const PartFxHost& host) noexcept;

  /// Buses (AUDIO thread). Present only while enabled().
  bool has_buses() const noexcept { return !part_bus_.empty(); }
  float* bus_l(int part) noexcept {
    return part_bus_.data() + static_cast<size_t>(part) * 2 * kPartFxChunkFrames;
  }
  float* bus_r(int part) noexcept { return bus_l(part) + kPartFxChunkFrames; }
  float* unit_bus_l(size_t unit) noexcept {
    return unit_bus_.data() + unit * 2 * kPartFxChunkFrames;
  }
  float* unit_bus_r(size_t unit) noexcept { return unit_bus_l(unit) + kPartFxChunkFrames; }
  void add_mono(int part, int i, float s) noexcept { bus_l(part)[i] += s; }
  void add_stereo(int part, int i, float l, float r) noexcept {
    float* bus = bus_l(part);
    bus[i] += l;
    bus[kPartFxChunkFrames + i] += r;
  }
  void clear_part_buses() noexcept;
  void clear_unit_buses() noexcept;
  bool has_unit_buses() const noexcept { return !unit_bus_.empty(); }

  /// AUDIO thread: run each bussed part's chain in place on its bus. A part
  /// with a mono prefix runs it on the left leg alone and has its pan restored
  /// from @p host after it.
  void run_part_chains(int n, const std::array<bool, 16>& bussed,
                       const std::array<uint8_t, 16>& mono_prefix, const PartFxHost& host) noexcept;
  /// AUDIO thread: run each fed unit once in place on its bus.
  void run_units(int n, const std::array<bool, kGsEfxUnitCount>& fed,
                 const PartFxUnitOverrides* overrides) noexcept;

  /// The longest tail of the published chains and units, in samples.
  int tail_samples() const noexcept { return tail_samples_; }
  /// Every published processor's discard count added together. RT-safe.
  uint64_t discard_sum(const PartFxUnitOverrides* overrides) const noexcept;

 private:
  /// What an EfxParamUpdate does to its stage.
  enum class EfxUpdateKind : uint8_t {
    kParam,        ///< set_parameter(param_id, value) on a modern stage.
    kEnable,       ///< Turn the stage on (value != 0) or off, through its fade.
    kClassicByte,  ///< Write byte `value` into slot `param_id` of a classic unit.
    kControlBase,  ///< Byte `value` is the new base of EFX CONTROL `stage_index`.
  };

  /// A pending GS EFX update handed from the control thread to the audio
  /// thread, addressed to stage @c stage_index of insertion unit @c unit in the
  /// snapshot of generation @c generation.
  struct EfxParamUpdate {
    EfxUpdateKind kind = EfxUpdateKind::kParam;
    uint8_t unit = 0;
    uint8_t stage_index = 0;
    uint32_t param_id = 0;
    float value = 0.0f;
    uint32_t generation = 0;
  };

  /// Wait-free single-producer (control thread) / single-consumer (audio thread)
  /// ring of pending EFX updates. A parameter-only GS EFX edit is resolved to
  /// updates on the control thread and applied on the audio thread (serialized
  /// with process()), so a live insert processor is never mutated across threads
  /// and its DSP state (reverb/delay tails) is never rebuilt away.
  class EfxParamQueue {
   public:
    /// Capacity (power of two). When the ring is full a push fails. Any failed
    /// enqueue turns the edit into a rebuild, which bakes the whole
    /// EFX mirror in. This applies to kParam too: one translated edit can
    /// produce several records, and applying only a prefix would leave a stage
    /// partly on the old generation.
    static constexpr size_t kCapacity = 128;
    static_assert((kCapacity & (kCapacity - 1)) == 0, "kCapacity must be a power of two");

    /// CONTROL thread. Reserve and publish all records atomically from the
    /// consumer's point of view. Returns false, queueing nothing, when they do
    /// not all fit.
    bool push_batch(const EfxParamUpdate* updates, size_t count) noexcept {
      if (updates == nullptr || count == 0 || count > kCapacity) return false;
      const size_t head = head_.load(std::memory_order_relaxed);
      const size_t tail = tail_.load(std::memory_order_acquire);
      if (head - tail > kCapacity - count) return false;
      for (size_t i = 0; i < count; ++i) {
        slots_[(head + i) & (kCapacity - 1)] = updates[i];
      }
      head_.store(head + count, std::memory_order_release);
      return true;
    }

    /// AUDIO thread. Returns false when the ring is empty.
    bool pop(EfxParamUpdate& out) noexcept {
      const size_t tail = tail_.load(std::memory_order_relaxed);
      const size_t head = head_.load(std::memory_order_acquire);
      if (head == tail) return false;
      out = slots_[tail & (kCapacity - 1)];
      tail_.store(tail + 1, std::memory_order_release);
      return true;
    }

   private:
    std::array<EfxParamUpdate, kCapacity> slots_{};
    alignas(64) std::atomic<size_t> head_{0};  // control thread (producer)
    alignas(64) std::atomic<size_t> tail_{0};  // audio thread (consumer)
  };

  /// The rig in force for @p part and, for kChain, the entry carrying it.
  PartRigMode effective_mode(int part, const PartRig** entry) const noexcept;
  /// Recompute bank_parts_ from the entries.
  void refresh_bank_parts() noexcept;
  std::shared_ptr<PartFxSnapshot> build_snapshot() const;
  /// Resolve unit 0's EFX CONTROL 1/2 onto @p out's unit 0, which must already
  /// be built.
  void build_controls(PartFxSnapshot& out) const;
  /// Retain the realtime-safe subset of a realised unit's binding rows, so a
  /// candidate-less overlay delta can edit it in place.
  void build_legacy_plan(Sf2EfxUnitRt& unit, size_t unit_index, const GsEfx& efx) const;
  /// Resolve a parameter-only edit of @p unit against the published unit and
  /// enqueue the updates. Returns true when a full rebuild is required instead.
  bool enqueue_param_updates(size_t unit, const std::array<uint8_t, 20>& previous_params);

  PartFxStageConfig config_;
  double sample_rate_ = 0.0;
  const GsEfxRowView* rows_ = nullptr;
  /// GS insertion-effect unit state and the per-part assign byte (40 4x 22),
  /// as the wire wrote them.
  std::array<GsEfx, kGsEfxUnitCount> efx_{};
  std::array<uint8_t, 16> assign_{};
  bool dirty_ = false;
  RigTable rigs_{};
  /// 16 parts x stereo x kPartFxChunkFrames.
  std::vector<float> part_bus_;
  /// 16 units x stereo x kPartFxChunkFrames.
  std::vector<float> unit_bus_;
  uint32_t generation_ = 0;
  int tail_samples_ = 0;
  /// Held by unique_ptr because RtPublisher and the atomics are not movable
  /// while the stage is.
  std::unique_ptr<rt::RtPublisher<PartFxSnapshot>> pub_ =
      std::make_unique<rt::RtPublisher<PartFxSnapshot>>();
  std::unique_ptr<EfxParamQueue> queue_ = std::make_unique<EfxParamQueue>();
  /// Bank rig id per part, four bits each: written by the AUDIO thread when a
  /// program moves, read by the builder, all sixteen in one word so the builder
  /// sees them as they stood at one instant.
  std::unique_ptr<std::atomic<uint64_t>> part_rig_ids_ = std::make_unique<std::atomic<uint64_t>>(0);
  /// One bit per part whose rig in force is the bank's, so the AUDIO thread can
  /// tell whether a program change reaches a chain.
  std::unique_ptr<std::atomic<uint32_t>> bank_parts_ = std::make_unique<std::atomic<uint32_t>>(0);
};

}  // namespace sonare::midi::synth
