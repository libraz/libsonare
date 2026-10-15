#pragma once

/// @file automation_engine.h
/// @brief RT-safe application of prepared PPQ automation lanes.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "automation/automation_lane.h"
#include "automation/parameter.h"
#include "rt/processor_base.h"
#include "rt/rt_publisher.h"
#include "transport/tempo_map.h"
#include "transport/transport_state.h"
#include "util/constants.h"

namespace sonare::automation {

struct AutomationBoundaryList {
  static constexpr size_t kCapacity = 64;
  std::array<double, kCapacity> ppq{};
  size_t size = 0;
  bool overflowed = false;

  void clear() noexcept;
  bool add(double value) noexcept;
  void sort_unique() noexcept;
};

class AutomationEngine {
 public:
  /// Router callback for engine-reserved parameter ids. Returns true when the
  /// value was accepted by the engine-side target.
  using EngineParamRouter = bool (*)(void* context, uint32_t param_id, float value);
  /// Gate predicate selecting which ids route through the engine router. When
  /// set, it replaces the (id & mask) == match test, so a router can claim two
  /// disjoint id namespaces (e.g. mixer fader/pan plus insert automation). Pure
  /// and stateless: configured at prepare time, called on the audio thread.
  using EngineParamGate = bool (*)(uint32_t param_id);
  using ExternalTargetResolver = rt::ProcessorBase* (*)(void* context, uint32_t param_id) noexcept;
  /// Callback invoked by acquire_lanes() on the audio thread for a target whose
  /// lane held points before the newly adopted lane set and holds none now
  /// (dropped entirely or replaced by an empty lane). Lets the engine restore
  /// whatever value should apply once nothing is driving the target anymore.
  using LaneReleaseCallback = void (*)(void* context, uint32_t param_id);

  void prepare(double sample_rate, const transport::TempoMap* tempo_map);
  void set_tempo_map(const transport::TempoMap* tempo_map) noexcept { tempo_map_ = tempo_map; }
  /// Routes lane values whose target id matches `(id & id_mask) == id_match`
  /// through @p router instead of the bound-processor table, so reserved
  /// engine-namespace lanes (e.g. mixer fader/pan) reach their runtime without
  /// a ProcessorBase binding. The members are plain (non-atomic): configure at
  /// prepare time, before realtime processing starts, like prepare().
  void set_engine_param_router(EngineParamRouter router, void* context, uint32_t id_mask,
                               uint32_t id_match) noexcept {
    engine_param_router_ = router;
    engine_param_router_context_ = context;
    engine_param_id_mask_ = id_mask;
    engine_param_id_match_ = id_match;
  }
  /// Installs a gate predicate (see EngineParamGate). Once set, apply() and
  /// set_parameter() route an id through the engine router when @p gate returns
  /// true instead of testing (id & mask) == match; leaving it null preserves the
  /// single mask/match fast path bit-for-bit. Configure at prepare time.
  void set_engine_param_gate(EngineParamGate gate) noexcept { engine_param_gate_ = gate; }
  /// Publishes registered parameter metadata used to reject explicitly
  /// non-realtime-safe targets before any RT automation path reaches a
  /// processor. Unregistered ids keep the legacy unknown-target behavior.
  void set_parameter_metadata(std::vector<ParameterInfo> parameters);
  /// Adds a block-stable target source owned by another immutable runtime
  /// snapshot (for example GraphRuntime). Configure before processing starts.
  void set_external_target_resolver(ExternalTargetResolver resolver, void* context) noexcept {
    external_target_resolver_ = resolver;
    external_target_context_ = context;
  }
  /// Installs the release callback (see LaneReleaseCallback). Configure at
  /// prepare time, like the other router/resolver setters above.
  void set_lane_release_callback(LaneReleaseCallback callback, void* context) noexcept {
    lane_release_callback_ = callback;
    lane_release_context_ = context;
  }
  void set_lanes(std::vector<AutomationLane> lanes);
  /// Binds @p param_id to @p processor, reusing the slot already holding that
  /// id or the first cleared one. CONTROL thread. Publishes the whole target
  /// table as one snapshot, then waits until no reader still uses the table it
  /// replaced, so a processor this call unbinds may be destroyed once it
  /// returns. Must not be called from inside apply() or set_parameter().
  bool bind_target(uint32_t param_id, rt::ProcessorBase* processor);
  /// Unbinds every target, with the same publish-and-wait contract as bind_target().
  void clear_targets();
  /// Unbinds the slot holding @p param_id, leaving an interior gap later binds
  /// reuse. Same publish-and-wait contract as bind_target(). Returns true if a
  /// bound slot matched.
  bool unbind_target(uint32_t param_id);

  /// Adopt the latest published lane set on the audio thread. Call once at
  /// block start before apply / collect_boundaries. RT-safe, no alloc.
  ///
  /// When a release callback is installed (see set_lane_release_callback), a
  /// merge-walk over the previous and newly adopted lane sets (both sorted by
  /// target_param_id, see set_lanes) invokes it once for every target that
  /// held points before and holds none now -- dropped from the set entirely,
  /// or replaced by an empty lane (the no-op-apply path a cleared lane takes,
  /// see apply()). O(n), no allocation.
  void acquire_lanes() noexcept {
    lanes_.acquire([this](const std::vector<AutomationLane>* previous,
                          const std::vector<AutomationLane>* next) noexcept {
      notify_lane_targets_released(previous, next);
    });
  }

  void apply(const transport::TransportState& state, int sub_block_offset,
             int sub_block_len) noexcept;
  /// Immediately set a bound parameter to @p value, mirroring apply()'s
  /// per-lane resolution and RT-safety gating. Returns true on success;
  /// failures bump the same internal counters apply() uses.
  bool set_parameter(uint32_t param_id, float value) noexcept;
  void collect_boundaries(double block_start_ppq, double block_end_ppq,
                          AutomationBoundaryList* out) noexcept;

  size_t lane_count() const noexcept;
  uint32_t unknown_target_count() const noexcept {
    return unknown_target_count_.load(std::memory_order_relaxed);
  }
  uint32_t non_realtime_safe_rejection_count() const noexcept {
    return non_realtime_safe_rejection_count_.load(std::memory_order_relaxed);
  }
  uint32_t bind_target_overflow_count() const noexcept {
    return bind_target_overflow_count_.load(std::memory_order_relaxed);
  }
  uint32_t stale_lane_apply_count() const noexcept {
    return stale_lane_apply_count_.load(std::memory_order_relaxed);
  }
  // Number of blocks whose boundary collection overflowed AutomationBoundaryList
  // (kCapacity exceeded), so some breakpoint sub-block splits were dropped.
  uint32_t boundary_overflow_count() const noexcept {
    return boundary_overflow_count_.load(std::memory_order_relaxed);
  }

 private:
  static constexpr size_t kMaxTargets = 128;

  // param_id 0 is reserved as the invalid/none sentinel: an unbound slot keeps
  // the default (param_id 0, processor null) and must never match a real lane.
  struct TargetSlot {
    uint32_t param_id = 0;
    rt::ProcessorBase* processor = nullptr;
  };
  // Immutable once published, so a reader sees each slot's id and processor
  // from one generation. `bound` is the highest ever-occupied index + 1;
  // lookups skip cleared slots inside it rather than stopping at them.
  struct TargetTable {
    std::array<TargetSlot, kMaxTargets> slots{};
    size_t bound = 0;
  };

  // Resolves @p param_id through the external resolver, then @p table (may be
  // null). The caller keeps the pin on @p table while it uses the result.
  rt::ProcessorBase* target_for(const TargetTable* table, uint32_t param_id) const noexcept;
  // Publishes control_targets_ and waits for readers of the table it replaces.
  void publish_targets();
  bool registered_parameter_rejects_realtime(uint32_t param_id) const noexcept;
  // Merge-walk helper backing acquire_lanes()'s release notification (see
  // there). @p previous/@p next may each be null (no lane set adopted yet).
  void notify_lane_targets_released(const std::vector<AutomationLane>* previous,
                                    const std::vector<AutomationLane>* next) noexcept;
  // True when @p param_id should route through the engine router. Uses the gate
  // predicate when installed, otherwise the (id & mask) == match fast path.
  bool routes_to_engine(uint32_t param_id) const noexcept {
    return engine_param_router_ != nullptr &&
           (engine_param_gate_ != nullptr
                ? engine_param_gate_(param_id)
                : (param_id & engine_param_id_mask_) == engine_param_id_match_);
  }

  double sample_rate_ = ::sonare::constants::kDefaultDawSampleRate;
  const transport::TempoMap* tempo_map_ = nullptr;
  // Engine-parameter router state. Plain members by design: written once at
  // prepare time (see set_engine_param_router), read on the audio thread.
  EngineParamRouter engine_param_router_ = nullptr;
  void* engine_param_router_context_ = nullptr;
  uint32_t engine_param_id_mask_ = 0;
  uint32_t engine_param_id_match_ = 0;
  // Optional gate predicate; null keeps the mask/match fast path (see
  // set_engine_param_gate). Plain member: written once at prepare time.
  EngineParamGate engine_param_gate_ = nullptr;
  ExternalTargetResolver external_target_resolver_ = nullptr;
  void* external_target_context_ = nullptr;
  // Release-callback state. Plain members by design, like the router state
  // above: written once at prepare time, read on the audio thread.
  LaneReleaseCallback lane_release_callback_ = nullptr;
  void* lane_release_context_ = nullptr;
  rt::RtSnapshot<std::vector<ParameterInfo>> parameter_metadata_{};
  mutable rt::RtPublisher<std::vector<AutomationLane>> lanes_;
  std::atomic<size_t> lane_count_{0};
  rt::RtSnapshot<TargetTable> targets_{};
  TargetTable control_targets_{};  // control thread only
  std::atomic<uint32_t> unknown_target_count_{0};
  std::atomic<uint32_t> non_realtime_safe_rejection_count_{0};
  std::atomic<uint32_t> bind_target_overflow_count_{0};
  std::atomic<uint32_t> stale_lane_apply_count_{0};
  std::atomic<uint32_t> boundary_overflow_count_{0};
};

}  // namespace sonare::automation
