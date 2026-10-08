#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "engine/insert_automation_id.h"
#include "engine/instrument_automation_id.h"
#include "engine/realtime_engine.h"
#include "engine/realtime_engine_internal.h"
#include "engine/track_mixer.h"
#include "engine/track_mixer_internal.h"
#include "mastering/api/insert_factory.h"
#include "rt/command.h"
#include "util/constants.h"
#include "util/json.h"

namespace sonare::engine {

#if defined(SONARE_WITH_MIXING)

using sonare::constants::kFloorDb;

bool RealtimeEngine::read_meter_target_insert_gain_reduction(uint32_t target_id, float* out,
                                                             size_t capacity,
                                                             size_t* out_count) const noexcept {
  using Kind = MeterTargetSlot::Kind;
  const MeterTargetSlot slot = decode_meter_target(target_id);
  switch (slot.kind) {
    case Kind::Master:
      *out_count = master_insert_gr_board_.read(out, capacity);
      return true;
    case Kind::Lane:
      *out_count = track_mixer_runtime_.lane_insert_gain_reduction(slot.index).read(out, capacity);
      return true;
    case Kind::Bus:
      *out_count = track_mixer_runtime_.bus_insert_gain_reduction(slot.index).read(out, capacity);
      return true;
    case Kind::InputMonitor:
      *out_count = 0;
      return true;
    case Kind::Invalid:
      break;
  }
  return false;
}

namespace {

constexpr uint32_t kEngineParamLaneMaster = 0xFFu;
constexpr uint32_t kEngineParamLaneBusBase = 0xFEu;

// A non-finite insert-parameter value has no meaning for any processor and is
// actively dangerous for the delay-based ones: std::clamp propagates NaN (every
// comparison is false, so it returns the value unchanged), and the result
// reaches a fractional delay read index. The check lives here, in the engine
// method every surface calls, rather than in one binding's wrapper -- the WASM
// bindings call these methods directly and never see the C ABI's copy of it.
bool insert_param_value_acceptable(float value) noexcept { return std::isfinite(value); }

enum class TrackLaneRemapResult : uint8_t {
  kUnchanged,
  kRemapped,
  kDrop,
};

TrackLaneRemapResult remap_track_lane_index(
    uint32_t old_lane, const std::array<uint32_t, TrackMixerRuntime::kMaxTrackLanes>& old_lane_ids,
    size_t old_lane_count,
    const std::array<uint32_t, TrackMixerRuntime::kMaxTrackLanes>& new_lane_ids,
    size_t new_lane_count, uint32_t* new_lane) noexcept {
  if (new_lane == nullptr || old_lane >= old_lane_count || old_lane >= old_lane_ids.size()) {
    return TrackLaneRemapResult::kDrop;
  }
  const uint32_t track_id = old_lane_ids[old_lane];
  if (track_id == 0) return TrackLaneRemapResult::kDrop;
  for (size_t lane = 0; lane < new_lane_count && lane < new_lane_ids.size(); ++lane) {
    if (new_lane_ids[lane] != track_id) continue;
    *new_lane = static_cast<uint32_t>(lane);
    return lane == old_lane ? TrackLaneRemapResult::kUnchanged : TrackLaneRemapResult::kRemapped;
  }
  return TrackLaneRemapResult::kDrop;
}

TrackLaneRemapResult remap_track_target_id(
    uint32_t target_id, const std::array<uint32_t, TrackMixerRuntime::kMaxTrackLanes>& old_lane_ids,
    size_t old_lane_count,
    const std::array<uint32_t, TrackMixerRuntime::kMaxTrackLanes>& new_lane_ids,
    size_t new_lane_count, uint32_t* remapped_id) noexcept {
  if (remapped_id == nullptr) return TrackLaneRemapResult::kDrop;
  // Insert ids name their track through the target table, so a reorder leaves them alone.
  if ((target_id & kEngineParamNamespaceMask) != kEngineParamNamespace) {
    return TrackLaneRemapResult::kUnchanged;
  }
  const uint32_t old_lane = (target_id & kEngineParamLaneMask) >> kEngineParamLaneShift;
  if (old_lane >= TrackMixerRuntime::kMaxTrackLanes) {
    return TrackLaneRemapResult::kUnchanged;
  }
  uint32_t new_lane = 0;
  const TrackLaneRemapResult result = remap_track_lane_index(
      old_lane, old_lane_ids, old_lane_count, new_lane_ids, new_lane_count, &new_lane);
  if (result == TrackLaneRemapResult::kRemapped) {
    *remapped_id = make_track_lane_param_id(new_lane, target_id & kEngineParamKindMask);
  }
  return result;
}

struct TrackLaneAutomationRemapContext {
  const std::array<uint32_t, TrackMixerRuntime::kMaxTrackLanes>& old_lane_ids;
  size_t old_lane_count = 0;
  const std::array<uint32_t, TrackMixerRuntime::kMaxTrackLanes>& new_lane_ids;
  size_t new_lane_count = 0;
};

bool remap_track_automation_target(void* opaque, uint32_t old_param_id,
                                   uint32_t* new_param_id) noexcept {
  if (opaque == nullptr) return false;
  const auto& context = *static_cast<const TrackLaneAutomationRemapContext*>(opaque);
  return remap_track_target_id(old_param_id, context.old_lane_ids, context.old_lane_count,
                               context.new_lane_ids, context.new_lane_count,
                               new_param_id) != TrackLaneRemapResult::kDrop;
}

TrackLaneRemapResult remap_track_command(
    rt::Command& command,
    const std::array<uint32_t, TrackMixerRuntime::kMaxTrackLanes>& old_lane_ids,
    size_t old_lane_count,
    const std::array<uint32_t, TrackMixerRuntime::kMaxTrackLanes>& new_lane_ids,
    size_t new_lane_count) noexcept {
  switch (command.type) {
    case rt::CommandType::kSetParam:
    case rt::CommandType::kSetParamSmoothed: {
      uint32_t remapped_id = command.target_id;
      const TrackLaneRemapResult result =
          remap_track_target_id(command.target_id, old_lane_ids, old_lane_count, new_lane_ids,
                                new_lane_count, &remapped_id);
      if (result == TrackLaneRemapResult::kRemapped) command.target_id = remapped_id;
      return result;
    }
    case rt::CommandType::kSetTrackInsertParam: {
      const uint32_t old_lane = (command.target_id >> 16u) & 0xFFu;
      uint32_t new_lane = 0;
      const TrackLaneRemapResult result = remap_track_lane_index(
          old_lane, old_lane_ids, old_lane_count, new_lane_ids, new_lane_count, &new_lane);
      if (result == TrackLaneRemapResult::kRemapped) {
        command.target_id = (command.target_id & ~0x00FF0000u) | (new_lane << 16u);
      }
      return result;
    }
    case rt::CommandType::kSetSoloMute:
    case rt::CommandType::kSetTrackMonitorMode: {
      uint32_t new_lane = 0;
      const TrackLaneRemapResult result = remap_track_lane_index(
          command.target_id, old_lane_ids, old_lane_count, new_lane_ids, new_lane_count, &new_lane);
      if (result == TrackLaneRemapResult::kRemapped) command.target_id = new_lane;
      return result;
    }
    default:
      return TrackLaneRemapResult::kUnchanged;
  }
}

// Reads @p processor_name's catalog entry for @p param_id out of
// insert_param_info_json (the same JSON the capability catalog and the C ABI's
// insert-param-info entry point read), and fills @p out from it. A null
// min/max/unit in the catalog is an unmeasured bound, not a zero one, so it
// maps to +/-infinity / empty string rather than being read as 0.
bool describe_insert_param_from_catalog(const std::string& processor_name, unsigned int param_id,
                                        automation::ParameterDescription* out) {
  util::json::Value parsed;
  try {
    parsed = util::json::parse(mastering::api::insert_param_info_json(processor_name));
  } catch (const util::json::JsonError&) {
    return false;
  }
  if (!parsed.is_array()) return false;
  for (const util::json::Value& entry : parsed.as_array()) {
    const util::json::Value* id_field = entry.find("id");
    if (id_field == nullptr || !id_field->is_number()) continue;
    if (static_cast<unsigned int>(id_field->as_int()) != param_id) continue;
    const util::json::Value* name_field = entry.find("name");
    out->name = name_field != nullptr && name_field->is_string() ? name_field->as_string() : "";
    const util::json::Value* min_field = entry.find("min");
    out->min_value = min_field != nullptr && min_field->is_number()
                         ? min_field->as_float()
                         : -std::numeric_limits<float>::infinity();
    const util::json::Value* max_field = entry.find("max");
    out->max_value = max_field != nullptr && max_field->is_number()
                         ? max_field->as_float()
                         : std::numeric_limits<float>::infinity();
    const util::json::Value* default_field = entry.find("default");
    if (default_field != nullptr && default_field->is_number()) {
      out->default_value = default_field->as_float();
    } else if (default_field != nullptr && default_field->is_bool()) {
      out->default_value = default_field->as_bool() ? 1.0f : 0.0f;
    } else {
      out->default_value = 0.0f;
    }
    const util::json::Value* unit_field = entry.find("unit");
    out->unit = unit_field != nullptr && unit_field->is_string() ? unit_field->as_string() : "";
    const util::json::Value* rt_safe_field = entry.find("rtSafe");
    out->rt_safe = rt_safe_field != nullptr && rt_safe_field->is_bool() && rt_safe_field->as_bool();
    out->default_curve = automation::CurveType::Linear;
    return true;
  }
  return false;
}

// Entries a rebuilt chain would need minted for (strip, owner): slots whose
// processor layout has no live entry yet. A same-layout rebuild needs none.
template <typename Chain>
size_t insert_entries_to_mint(const InsertAutomationTargetTable& table, InsertStripKind strip,
                              uint32_t owner_id, const Chain& chain, size_t slot_count) noexcept {
  size_t needed = 0;
  for (size_t slot = 0; slot < slot_count && slot <= kInsertIndexMask; ++slot) {
    const InsertAutomationTarget target{
        strip, owner_id, static_cast<unsigned int>(slot),
        insert_processor_layout(chain.insert_processor(static_cast<unsigned int>(slot)))};
    if (!target.processor.empty() && table.find(target) < 0) ++needed;
  }
  return needed;
}

struct InsertChainAdmission {
  const InsertAutomationTargetTable* table = nullptr;
  // False when the strip is not configured in the mixer, so its chain mints nothing yet.
  bool mints = false;
};

bool admit_track_chain(void* context, uint32_t track_id,
                       const mixing::ChannelStrip& strip) noexcept {
  const auto& admission = *static_cast<const InsertChainAdmission*>(context);
  if (!admission.mints) return true;
  return insert_entries_to_mint(*admission.table, InsertStripKind::kTrack, track_id, strip,
                                strip.num_pre_inserts() + strip.num_post_inserts()) <=
         admission.table->remaining();
}

bool admit_bus_chain(void* context, uint32_t bus_id, const mixing::FxBus& bus) noexcept {
  const auto& admission = *static_cast<const InsertChainAdmission*>(context);
  return insert_entries_to_mint(*admission.table, InsertStripKind::kBus, bus_id, bus,
                                bus.num_inserts()) <= admission.table->remaining();
}

}  // namespace

void RealtimeEngine::set_mixing_enabled(bool enabled) noexcept {
  mixing_enabled_.store(enabled, std::memory_order_relaxed);
  update_reported_graph_latency();
}

bool RealtimeEngine::reset_master_meter_integrated(int64_t render_frame) noexcept {
  rt::Command command{};
  command.type = rt::CommandType::kResetMasterMeterIntegrated;
  command.sample_time = render_frame;
  return push_command(command);
}

bool RealtimeEngine::bind_mixing_strip(mixing::ChannelStrip* strip) {
  if (strip != nullptr && monitor_runtime_.contains(strip)) {
    return false;
  }
  const bool owned = strip != nullptr && strip == owned_master_strip_.get();
  const size_t insert_count = owned ? master_strip_spec_.inserts.size() : 0;
  try {
    if (strip != nullptr && max_block_size_ > 0) {
      // Prepare before changing the raw pointer visible to the audio thread.
      strip->prepare(sample_rate_, max_block_size_);
    }
  } catch (...) {
    return false;
  }
  TrackMixerRuntime::PreparedMasterStripUpdate update;
  if (!track_mixer_runtime_.prepare_master_strip_update(owned ? strip : nullptr, insert_count,
                                                        &update)) {
    return false;
  }
  const bool bound = mixing_runtime_.bind(strip);
  if (strip != nullptr && !bound) return false;
  // bind(nullptr) reports false yet still clears the pointer, so always commit.
  track_mixer_runtime_.commit_master_strip_update(owned ? strip : nullptr, update);
  sync_insert_automation_targets();
  update_reported_graph_latency();
  return bound;
}

bool RealtimeEngine::set_master_strip(const mixing::api::Strip& strip_spec) {
  if (!strip_eq_acceptable(strip_spec.eq, sample_rate_)) return false;
  mixing::api::Strip next_spec;
  try {
    next_spec = strip_spec;
  } catch (...) {
    return false;
  }
  // In-place: an unchanged insert chain keeps the bound strip, and with it the
  // insert state (tails, envelopes), the insert automation and the EQ filter
  // state of every band the new spec leaves alone.
  if (owned_master_strip_ != nullptr && mixing_runtime_.strip() == owned_master_strip_.get() &&
      strip_inserts_equal(master_strip_spec_.inserts, strip_spec.inserts)) {
    if (!owned_master_strip_->try_set_channel_delay_samples(strip_spec.channel_delay_samples)) {
      return false;
    }
    apply_strip_scalars(*owned_master_strip_, strip_spec, master_strip_spec_);
    std::swap(master_strip_spec_, next_spec);
    update_reported_graph_latency();
    set_mixing_enabled(true);
    return true;
  }
  std::unique_ptr<mixing::ChannelStrip> strip;
  try {
    strip = make_channel_strip_from_spec(next_spec);
    if (strip == nullptr) return false;
    if (max_block_size_ > 0) strip->prepare(sample_rate_, max_block_size_);
  } catch (...) {
    return false;
  }
  if (!insert_automation_room(
          insert_entries_to_mint(insert_automation_targets_, InsertStripKind::kMaster, 0, *strip,
                                 strip->num_pre_inserts() + strip->num_post_inserts()))) {
    return false;
  }
  TrackMixerRuntime::PreparedMasterStripUpdate update;
  if (!track_mixer_runtime_.prepare_master_strip_update(strip.get(), next_spec.inserts.size(),
                                                        &update)) {
    return false;
  }
  // Keys stay on their insert index; an index the new chain lacks is dropped.
  if (!mixing_runtime_.bind(strip.get())) return false;
  // Control-thread-only: this std::move destroys the old master immediately (no
  // deferred reclaim), so the caller must not run process() concurrently.
  clear_master_insert_automations();
  owned_master_strip_ = std::move(strip);
  std::swap(master_strip_spec_, next_spec);
  track_mixer_runtime_.commit_master_strip_update(owned_master_strip_.get(), update);
  sync_insert_automation_targets();
  set_mixing_enabled(true);
  update_reported_graph_latency();
  return true;
}

bool RealtimeEngine::validate_track_lanes(const std::vector<TrackLaneConfig>& lanes) const {
  return track_mixer_runtime_.validate_track_lanes(lanes);
}

bool RealtimeEngine::validate_track_strip(const mixing::api::Strip& strip) const {
  if (!strip_eq_acceptable(strip.eq, sample_rate_)) return false;
  try {
    return make_channel_strip_from_spec(strip) != nullptr;
  } catch (...) {
    return false;
  }
}

bool RealtimeEngine::set_track_lanes(std::vector<TrackLaneConfig> lanes) {
  std::array<uint32_t, TrackMixerRuntime::kMaxTrackLanes> new_lane_ids{};
  const size_t new_lane_count = std::min(lanes.size(), new_lane_ids.size());
  size_t entering_inserts = 0;
  for (size_t lane_index = 0; lane_index < new_lane_count; ++lane_index) {
    new_lane_ids[lane_index] = lanes[lane_index].track_id;
    const auto old_end = track_lane_ids_.begin() + static_cast<std::ptrdiff_t>(track_lane_count_);
    if (std::find(track_lane_ids_.begin(), old_end, new_lane_ids[lane_index]) == old_end) {
      entering_inserts += track_mixer_runtime_.bound_track_insert_count(new_lane_ids[lane_index]);
    }
  }
  if (!insert_automation_room(entering_inserts)) return false;

  // Prepare the automation remap before lane routing changes; commit it once the mixer accepts.
  automation::AutomationEngine::PreparedLaneRemap automation_remap;
  TrackLaneAutomationRemapContext automation_context{track_lane_ids_, track_lane_count_,
                                                     new_lane_ids, new_lane_count};
  if (!automation_.prepare_lane_remap_control_quiescent(remap_track_automation_target,
                                                        &automation_context, &automation_remap)) {
    return false;
  }
  const bool ok = track_mixer_runtime_.set_track_lanes(std::move(lanes));
  if (ok) {
    automation_.commit_lane_remap_control_quiescent(std::move(automation_remap));
    // Remap queued commands and manual bases by track id so a lane reorder cannot retarget a strip.
    pending_.remove_if([&](rt::Command& command) noexcept {
      return remap_track_command(command, track_lane_ids_, track_lane_count_, new_lane_ids,
                                 new_lane_count) == TrackLaneRemapResult::kDrop;
    });

    const size_t queued = commands_.size_approx();
    for (size_t i = 0; i < queued; ++i) {
      rt::Command command{};
      if (!commands_.pop(command)) break;
      const TrackLaneRemapResult result = remap_track_command(
          command, track_lane_ids_, track_lane_count_, new_lane_ids, new_lane_count);
      if (result != TrackLaneRemapResult::kDrop) (void)commands_.push(command);
    }

    struct RemappedBase {
      uint32_t target_id = 0;
      float value = 0.0f;
    };
    std::array<RemappedBase, kParameterBaseTableMaxEntries> remapped_bases{};
    size_t remapped_count = 0;
    parameter_base_table_.erase_if([&](uint32_t target_id, float value) noexcept {
      uint32_t remapped_id = target_id;
      const TrackLaneRemapResult result =
          remap_track_target_id(target_id, track_lane_ids_, track_lane_count_, new_lane_ids,
                                new_lane_count, &remapped_id);
      if (result == TrackLaneRemapResult::kUnchanged) return false;
      if (result == TrackLaneRemapResult::kRemapped && remapped_count < remapped_bases.size()) {
        remapped_bases[remapped_count++] = {remapped_id, value};
      }
      // Erase both moved and removed old selectors. Moved values are
      // re-recorded below after the fixed table has completed its pass.
      return true;
    });
    for (size_t i = 0; i < remapped_count; ++i) {
      record_parameter_base(remapped_bases[i].target_id, remapped_bases[i].value);
    }

    track_lane_ids_ = new_lane_ids;
    track_lane_count_ = new_lane_count;
    // A removed track's ids are retired, and their queued edits and bases go with them.
    sync_insert_automation_targets();
    update_reported_graph_latency();
  }
  return ok;
}

bool RealtimeEngine::set_track_buses(std::vector<TrackBusConfig> buses) {
  const bool ok = track_mixer_runtime_.set_buses(std::move(buses));
  if (ok) {
    // A removed bus's ids are retired, and their queued edits and bases go with
    // them, so an unreachable id neither reports UnknownTarget later nor holds
    // base capacity.
    sync_insert_automation_targets();
    update_reported_graph_latency();
  }
  return ok;
}

bool RealtimeEngine::set_lane_sidechain(uint32_t track_id, unsigned int insert_index,
                                        uint32_t source_track_id) noexcept {
  const bool ok = track_mixer_runtime_.set_lane_sidechain(track_id, insert_index, source_track_id);
  if (ok) {
    update_reported_graph_latency();
  }
  return ok;
}

bool RealtimeEngine::set_bus_sidechain(uint32_t bus_id, unsigned int insert_index,
                                       SidechainSourceKind kind, uint32_t source_id) {
  const bool ok = track_mixer_runtime_.set_bus_sidechain(bus_id, insert_index, kind, source_id);
  if (ok) {
    update_reported_graph_latency();
  }
  return ok;
}

bool RealtimeEngine::set_master_sidechain(unsigned int insert_index, SidechainSourceKind kind,
                                          uint32_t source_id) {
  const bool ok = track_mixer_runtime_.set_master_sidechain(insert_index, kind, source_id);
  if (ok) {
    update_reported_graph_latency();
  }
  return ok;
}

bool RealtimeEngine::bind_track_strip(uint32_t track_id, mixing::ChannelStrip* strip) {
  uint32_t lane_position = 0;
  if (strip != nullptr && track_mixer_runtime_.track_lane_position(track_id, &lane_position) &&
      !insert_automation_room(
          insert_entries_to_mint(insert_automation_targets_, InsertStripKind::kTrack, track_id,
                                 *strip, strip->num_pre_inserts() + strip->num_post_inserts()))) {
    return false;
  }
  const bool ok = track_mixer_runtime_.bind_track_strip(track_id, strip);
  sync_insert_automation_targets();
  if (ok) {
    update_reported_graph_latency();
  }
  return ok;
}

bool RealtimeEngine::release_track_strip(uint32_t track_id) {
  const bool ok = track_mixer_runtime_.release_track_strip(track_id);
  sync_insert_automation_targets();
  if (ok) {
    update_reported_graph_latency();
  }
  return ok;
}

bool RealtimeEngine::set_track_strip(uint32_t track_id, const mixing::api::Strip& strip) {
  uint32_t lane_position = 0;
  InsertChainAdmission admission{
      &insert_automation_targets_,
      track_mixer_runtime_.track_lane_position(track_id, &lane_position)};
  const bool ok =
      track_mixer_runtime_.set_track_strip(track_id, strip, admit_track_chain, &admission);
  sync_insert_automation_targets();
  if (ok) {
    update_reported_graph_latency();
  }
  return ok;
}

bool RealtimeEngine::set_bus_strip(uint32_t bus_id, const mixing::api::Bus& bus) {
  InsertChainAdmission admission{&insert_automation_targets_, true};
  const bool ok = track_mixer_runtime_.set_bus_strip(bus_id, bus, admit_bus_chain, &admission);
  sync_insert_automation_targets();
  if (ok) {
    update_reported_graph_latency();
  }
  return ok;
}

bool RealtimeEngine::set_track_insert_bypassed(uint32_t track_id, unsigned int insert_index,
                                               bool bypassed, bool reset_on_bypass) noexcept {
  return track_mixer_runtime_.set_track_insert_bypassed(track_id, insert_index, bypassed,
                                                        reset_on_bypass);
}

bool RealtimeEngine::set_master_insert_bypassed(unsigned int insert_index, bool bypassed,
                                                bool reset_on_bypass) noexcept {
  return owned_master_strip_ != nullptr &&
         owned_master_strip_->set_insert_bypassed(insert_index, bypassed, reset_on_bypass);
}

bool RealtimeEngine::set_bus_insert_bypassed(uint32_t bus_id, unsigned int insert_index,
                                             bool bypassed, bool reset_on_bypass) noexcept {
  return track_mixer_runtime_.set_bus_insert_bypassed(bus_id, insert_index, bypassed,
                                                      reset_on_bypass);
}

InsertParamSetResult RealtimeEngine::set_track_insert_param_detailed(uint32_t track_id,
                                                                     unsigned int insert_index,
                                                                     const std::string& key,
                                                                     float value) noexcept {
  if (!insert_param_value_acceptable(value)) {
    return InsertParamSetResult::kInvalidTarget;
  }
  const int64_t id = resolve_track_insert_automation_id(track_id, insert_index, key);
  if (id < 0) return InsertParamSetResult::kInvalidTarget;
  rt::Command command;
  // Generic reserved id so apply_command records the manual base.
  command.type = rt::CommandType::kSetParam;
  command.target_id = static_cast<uint32_t>(id);
  command.sample_time = -1;  // block head / immediate
  command.arg.f = value;
  return push_command(command) ? InsertParamSetResult::kQueued : InsertParamSetResult::kQueueFull;
}

bool RealtimeEngine::set_track_insert_param(uint32_t track_id, unsigned int insert_index,
                                            const std::string& key, float value) noexcept {
  return set_track_insert_param_detailed(track_id, insert_index, key, value) ==
         InsertParamSetResult::kQueued;
}

bool RealtimeEngine::apply_track_insert_param_by_name_now(uint32_t track_id,
                                                          unsigned int insert_index,
                                                          const std::string& key,
                                                          float value) noexcept {
  const bool applied =
      track_mixer_runtime_.apply_track_insert_param_by_name_now(track_id, insert_index, key, value);
  if (applied) {
    const int64_t automation_id = resolve_track_insert_automation_id(track_id, insert_index, key);
    if (automation_id >= 0) record_parameter_base(static_cast<uint32_t>(automation_id), value);
  }
  return applied;
}

bool RealtimeEngine::restore_track_insert_param_by_name(uint32_t track_id,
                                                        unsigned int insert_index,
                                                        const std::string& key,
                                                        float value) noexcept {
  const bool applied =
      track_mixer_runtime_.restore_track_insert_param_by_name(track_id, insert_index, key, value);
  if (applied) {
    const int64_t automation_id = resolve_track_insert_automation_id(track_id, insert_index, key);
    if (automation_id >= 0) record_parameter_base(static_cast<uint32_t>(automation_id), value);
  }
  return applied;
}

bool RealtimeEngine::clear_track_insert_parameter_bases(uint32_t track_id) noexcept {
  uint32_t lane_position = 0;
  if (!track_mixer_runtime_.track_lane_position(track_id, &lane_position)) return false;
  InsertPurge purge;
  purge.strip = InsertStripKind::kTrack;
  purge.owner_id = track_id;
  purge.legacy_track_lane = static_cast<int>(lane_position);
  purge_insert_edits(purge);
  return true;
}

InsertParamSetResult RealtimeEngine::set_master_insert_param_detailed(unsigned int insert_index,
                                                                      const std::string& key,
                                                                      float value) noexcept {
  if (!insert_param_value_acceptable(value)) return InsertParamSetResult::kInvalidTarget;
  const int64_t id = resolve_master_insert_automation_id(insert_index, key);
  if (id < 0) return InsertParamSetResult::kInvalidTarget;
  rt::Command command;
  command.type = rt::CommandType::kSetParam;
  command.target_id = static_cast<uint32_t>(id);
  command.sample_time = -1;
  command.arg.f = value;
  return push_command(command) ? InsertParamSetResult::kQueued : InsertParamSetResult::kQueueFull;
}

bool RealtimeEngine::set_master_insert_param(unsigned int insert_index, const std::string& key,
                                             float value) noexcept {
  return set_master_insert_param_detailed(insert_index, key, value) ==
         InsertParamSetResult::kQueued;
}

bool RealtimeEngine::apply_master_insert_param_by_name_now(unsigned int insert_index,
                                                           const std::string& key,
                                                           float value) noexcept {
  if (owned_master_strip_ == nullptr || !insert_param_value_acceptable(value)) return false;
  const int id = owned_master_strip_->insert_parameter_id_for_key(insert_index, key);
  if (id < 0) return false;
  const int64_t automation_id = resolve_master_insert_automation_id(insert_index, key);
  const uint32_t base_id = automation_id >= 0 ? static_cast<uint32_t>(automation_id) : 0u;
  const bool applied = route_master_insert_param_smoothed(
      insert_index, static_cast<unsigned int>(id), value, base_id);
  if (applied && base_id != 0) record_parameter_base(base_id, value);
  return applied;
}

bool RealtimeEngine::restore_master_insert_param_by_name(unsigned int insert_index,
                                                         const std::string& key,
                                                         float value) noexcept {
  if (owned_master_strip_ == nullptr || !insert_param_value_acceptable(value)) return false;
  const int id = owned_master_strip_->insert_parameter_id_for_key(insert_index, key);
  if (id < 0) return false;
  const unsigned int param_id = static_cast<unsigned int>(id);
  const bool applied = owned_master_strip_->apply_insert_parameter(insert_index, param_id, value);
  if (applied) {
    // Retire the smoother only after the processor accepted the restore. A
    // rejected non-RT-safe or unknown parameter must leave its active target
    // assigned so the next automation update cannot silently reclaim it.
    for (MasterInsertAutoSlot& slot : master_insert_auto_slots_) {
      if (slot.assigned && slot.insert_index == insert_index && slot.param_id == param_id) {
        slot.active = false;
        slot.assigned = false;
      }
    }
    const int64_t automation_id = resolve_master_insert_automation_id(insert_index, key);
    if (automation_id >= 0) record_parameter_base(static_cast<uint32_t>(automation_id), value);
  }
  return applied;
}

void RealtimeEngine::clear_master_insert_parameter_bases() noexcept {
  InsertPurge purge;
  purge.strip = InsertStripKind::kMaster;
  purge.legacy_master = true;
  purge_insert_edits(purge);
}

InsertParamSetResult RealtimeEngine::set_bus_insert_param_detailed(uint32_t bus_id,
                                                                   unsigned int insert_index,
                                                                   const std::string& key,
                                                                   float value) noexcept {
  if (!insert_param_value_acceptable(value)) {
    return InsertParamSetResult::kInvalidTarget;
  }
  const int64_t id = resolve_bus_insert_automation_id(bus_id, insert_index, key);
  if (id < 0) return InsertParamSetResult::kInvalidTarget;
  rt::Command command;
  command.type = rt::CommandType::kSetParam;
  command.target_id = static_cast<uint32_t>(id);
  command.sample_time = -1;  // block head / immediate
  command.arg.f = value;
  return push_command(command) ? InsertParamSetResult::kQueued : InsertParamSetResult::kQueueFull;
}

bool RealtimeEngine::set_bus_insert_param(uint32_t bus_id, unsigned int insert_index,
                                          const std::string& key, float value) noexcept {
  return set_bus_insert_param_detailed(bus_id, insert_index, key, value) ==
         InsertParamSetResult::kQueued;
}

bool RealtimeEngine::apply_bus_insert_param_by_name_now(uint32_t bus_id, unsigned int insert_index,
                                                        const std::string& key,
                                                        float value) noexcept {
  const bool applied =
      track_mixer_runtime_.apply_bus_insert_param_by_name_now(bus_id, insert_index, key, value);
  if (applied) {
    const int64_t automation_id = resolve_bus_insert_automation_id(bus_id, insert_index, key);
    if (automation_id >= 0) record_parameter_base(static_cast<uint32_t>(automation_id), value);
  }
  return applied;
}

bool RealtimeEngine::restore_bus_insert_param_by_name(uint32_t bus_id, unsigned int insert_index,
                                                      const std::string& key,
                                                      float value) noexcept {
  const bool applied =
      track_mixer_runtime_.restore_bus_insert_param_by_name(bus_id, insert_index, key, value);
  if (applied) {
    const int64_t automation_id = resolve_bus_insert_automation_id(bus_id, insert_index, key);
    if (automation_id >= 0) record_parameter_base(static_cast<uint32_t>(automation_id), value);
  }
  return applied;
}

bool RealtimeEngine::clear_bus_insert_parameter_bases(uint32_t bus_id) noexcept {
  if (bus_id == 0) return false;
  const bool known =
      track_mixer_runtime_.configured_bus(bus_id) ||
      insert_automation_targets_.any_minted([bus_id](const InsertAutomationTarget& target) {
        return target.same_owner(InsertStripKind::kBus, bus_id);
      });
  if (!known) return false;
  // Reaches every id the identity held, including ids retired by a remove/re-add.
  InsertPurge purge;
  purge.strip = InsertStripKind::kBus;
  purge.owner_id = bus_id;
  purge_insert_edits(purge);
  return true;
}

const rt::ProcessorBase* RealtimeEngine::current_insert_processor(
    const InsertAutomationTarget& target) const noexcept {
  switch (target.strip) {
    case InsertStripKind::kTrack:
      return track_mixer_runtime_.track_insert_processor(target.owner_id, target.insert_index);
    case InsertStripKind::kBus:
      return track_mixer_runtime_.bus_insert_processor(target.owner_id, target.insert_index);
    case InsertStripKind::kMaster:
      return owned_master_strip_ != nullptr
                 ? owned_master_strip_->insert_processor(target.insert_index)
                 : nullptr;
  }
  return nullptr;
}

void RealtimeEngine::sync_insert_automation_targets() noexcept {
  const size_t retired =
      insert_automation_targets_.retire_if([this](const InsertAutomationTarget& target) noexcept {
        return insert_processor_layout(current_insert_processor(target)) != target.processor;
      });
  const auto mint = [this](InsertStripKind strip, uint32_t owner_id, size_t insert_count) {
    for (size_t slot = 0; slot < insert_count && slot <= kInsertIndexMask; ++slot) {
      InsertAutomationTarget target{strip, owner_id, static_cast<unsigned int>(slot), {}};
      target.processor = insert_processor_layout(current_insert_processor(target));
      if (!target.processor.empty()) (void)insert_automation_targets_.ensure(target);
    }
  };
  std::array<uint32_t, TrackMixerRuntime::kMaxTrackLanes> track_ids{};
  const size_t track_count =
      track_mixer_runtime_.copy_control_lane_track_ids(track_ids.data(), track_ids.size());
  for (size_t i = 0; i < track_count; ++i) {
    mint(InsertStripKind::kTrack, track_ids[i],
         track_mixer_runtime_.track_insert_count(track_ids[i]));
  }
  for (const TrackBusConfig& bus : track_mixer_runtime_.bus_configs()) {
    mint(InsertStripKind::kBus, bus.bus_id, track_mixer_runtime_.bus_insert_count(bus.bus_id));
  }
  if (owned_master_strip_ != nullptr) {
    mint(InsertStripKind::kMaster, 0,
         owned_master_strip_->num_pre_inserts() + owned_master_strip_->num_post_inserts());
  }
  if (retired == 0) return;
  // Edits and bases under a retired id can never apply again.
  InsertPurge purge;
  purge.retired_only = true;
  purge_insert_edits(purge);
}

int64_t RealtimeEngine::insert_automation_id(const InsertAutomationTarget& target,
                                             unsigned int param_id) const noexcept {
  if (target.insert_index > kInsertIndexMask || param_id > kInsertParamFieldMask) return -1;
  const int64_t selector = insert_automation_targets_.find(target);
  if (selector < 0) return -1;
  return make_insert_param_id(static_cast<uint32_t>(selector), target.insert_index, param_id);
}

int64_t RealtimeEngine::resolve_track_insert_automation_id(uint32_t track_id,
                                                           unsigned int insert_index,
                                                           const std::string& key) noexcept {
  size_t lane_index = 0;
  unsigned int param_id = 0;
  if (!track_mixer_runtime_.resolve_track_insert_param(track_id, insert_index, key, &lane_index,
                                                       &param_id)) {
    return -1;
  }
  InsertAutomationTarget target{InsertStripKind::kTrack, track_id, insert_index, {}};
  target.processor = insert_processor_layout(current_insert_processor(target));
  return insert_automation_id(target, param_id);
}

int64_t RealtimeEngine::resolve_master_insert_automation_id(unsigned int insert_index,
                                                            const std::string& key) noexcept {
  if (owned_master_strip_ == nullptr) return -1;
  const int id = owned_master_strip_->insert_parameter_id_for_key(insert_index, key);
  if (id < 0) return -1;
  InsertAutomationTarget target{InsertStripKind::kMaster, 0, insert_index, {}};
  target.processor = insert_processor_layout(current_insert_processor(target));
  return insert_automation_id(target, static_cast<unsigned int>(id));
}

int64_t RealtimeEngine::resolve_bus_insert_automation_id(uint32_t bus_id, unsigned int insert_index,
                                                         const std::string& key) noexcept {
  size_t bus_index = 0;
  unsigned int param_id = 0;
  if (!track_mixer_runtime_.resolve_bus_insert_param(bus_id, insert_index, key, &bus_index,
                                                     &param_id)) {
    return -1;
  }
  InsertAutomationTarget target{InsertStripKind::kBus, bus_id, insert_index, {}};
  target.processor = insert_processor_layout(current_insert_processor(target));
  return insert_automation_id(target, param_id);
}

bool RealtimeEngine::set_track_eq_band(uint32_t track_id, size_t band_index,
                                       const mastering::eq::EqBand& band) noexcept {
  const bool ok = track_mixer_runtime_.set_track_eq_band(track_id, band_index, band);
  if (ok) {
    update_reported_graph_latency();
  }
  return ok;
}

bool RealtimeEngine::set_track_pan(uint32_t track_id, float pan) noexcept {
  return track_mixer_runtime_.set_track_pan(track_id, pan);
}

bool RealtimeEngine::set_track_pan_law(uint32_t track_id, mixing::PanLaw law) noexcept {
  return track_mixer_runtime_.set_track_pan_law(track_id, law);
}

bool RealtimeEngine::set_track_pan_mode(uint32_t track_id, mixing::PanMode mode) noexcept {
  return track_mixer_runtime_.set_track_pan_mode(track_id, mode);
}

bool RealtimeEngine::set_track_dual_pan(uint32_t track_id, float left_pan,
                                        float right_pan) noexcept {
  return track_mixer_runtime_.set_track_dual_pan(track_id, left_pan, right_pan);
}

bool RealtimeEngine::set_track_surround_pan(uint32_t track_id,
                                            const mixing::SurroundPanParams& params) noexcept {
  return track_mixer_runtime_.set_track_surround_pan(track_id, params);
}

bool RealtimeEngine::set_bus_pan(uint32_t bus_id, float pan) noexcept {
  return track_mixer_runtime_.set_bus_pan(bus_id, pan);
}

bool RealtimeEngine::set_bus_pan_law(uint32_t bus_id, mixing::PanLaw law) noexcept {
  return track_mixer_runtime_.set_bus_pan_law(bus_id, law);
}

bool RealtimeEngine::set_bus_pan_mode(uint32_t bus_id, mixing::PanMode mode) noexcept {
  return track_mixer_runtime_.set_bus_pan_mode(bus_id, mode);
}

bool RealtimeEngine::set_bus_dual_pan(uint32_t bus_id, float left_pan, float right_pan) noexcept {
  return track_mixer_runtime_.set_bus_dual_pan(bus_id, left_pan, right_pan);
}

bool RealtimeEngine::set_bus_eq_band(uint32_t bus_id, size_t band_index,
                                     const mastering::eq::EqBand& band) noexcept {
  const bool ok = track_mixer_runtime_.set_bus_eq_band(bus_id, band_index, band);
  if (ok) {
    update_reported_graph_latency();
  }
  return ok;
}

bool RealtimeEngine::set_track_channel_delay_samples(uint32_t track_id,
                                                     int delay_samples) noexcept {
  return track_mixer_runtime_.set_track_channel_delay_samples(track_id, delay_samples);
}

bool RealtimeEngine::set_master_eq_band(size_t band_index,
                                        const mastering::eq::EqBand& band) noexcept {
  if (owned_master_strip_ == nullptr) return false;
  mixing::ChannelStrip& strip = *owned_master_strip_;
  mastering::eq::EqBand previous_band;
  mastering::eq::EqBand previous_spec_band;
  auto& spec_bands = master_strip_spec_.eq.bands;
  const size_t previous_spec_size = spec_bands.size();
  const bool had_spec_band = previous_spec_size > band_index;
  try {
    previous_band = strip.eq().band(band_index);
    if (had_spec_band) previous_spec_band = spec_bands[band_index];
    strip.set_eq_band(band_index, band);
    store_eq_band(master_strip_spec_.eq, band_index, band);
    update_reported_graph_latency();
    return true;
  } catch (...) {
    // A false return means nothing changed: put the stage and the spec back.
    try {
      strip.set_eq_band(band_index, previous_band);
    } catch (...) {
    }
    if (had_spec_band) {
      spec_bands[band_index] = previous_spec_band;
    } else if (spec_bands.size() > previous_spec_size) {
      spec_bands.resize(previous_spec_size);
    }
    return false;
  }
}

uint32_t RealtimeEngine::configure_scope_telemetry(int interval_frames, uint32_t band_count) {
  scope_interval_frames_.store(std::max(0, interval_frames), std::memory_order_relaxed);
  const uint32_t clamped = std::clamp<uint32_t>(band_count, 1, ScopeTelemetryRecord::kMaxBands);
  if (clamped != scope_band_count_) {
    scope_band_count_ = clamped;
    if (max_block_size_ > 0) {
      // Re-prepare the tap with the new band resolution. Control-thread only,
      // not concurrent with process() (same contract as prepare()).
      scope_tap_.prepare(
          sample_rate_, max_block_size_,
          8 * (TrackMixerRuntime::kMaxTrackLanes + TrackMixerRuntime::kMaxBusLanes + 2), 2048,
          scope_band_count_);
    }
  }
  // Before prepare(), the tap still carries its default band count. Return the
  // clamped configuration instead; RealtimeEngine::prepare() applies it when
  // the tap's allocation is made.
  return scope_band_count_;
}

bool RealtimeEngine::route_engine_parameter(uint32_t target_id, float value) noexcept {
  if (!parameter_target_reserved(target_id)) return false;
#if defined(SONARE_WITH_ARRANGEMENT)
  // Instrument-automation namespace: hosted instruments live outside the mixer
  // runtimes, so they are decoded first and served by their own slot table.
  if (is_instrument_param_id(target_id)) {
    return route_instrument_parameter(target_id, value);
  }
#endif
  // Insert-automation namespace: the strip field names a target entry, which
  // names the strip by identity. A retired entry, or a slot that now holds
  // another processor layout, is an unknown target rather than a retarget.
  if (is_insert_param_id(target_id)) {
    const InsertAutomationTarget* target = live_insert_target(target_id);
    if (target == nullptr || !insert_target_holds(*target)) return false;
    const unsigned int param_id = static_cast<unsigned int>(insert_param_param(target_id));
    switch (target->strip) {
      case InsertStripKind::kMaster:
        return route_master_insert_param_smoothed(target->insert_index, param_id, value, target_id);
      case InsertStripKind::kBus:
        return track_mixer_runtime_.route_bus_insert_param_smoothed_by_id(
            target->owner_id, target->insert_index, param_id, value);
      case InsertStripKind::kTrack:
        return track_mixer_runtime_.route_track_insert_param_smoothed_by_id(
            target->owner_id, target->insert_index, param_id, value);
    }
    return false;
  }
  const uint32_t lane = (target_id & kEngineParamLaneMask) >> kEngineParamLaneShift;
  const uint32_t kind = target_id & kEngineParamKindMask;
  if (lane == kEngineParamLaneMaster) {
    return mixing_runtime_.set_parameter(kind, value);
  }
  if (lane <= kEngineParamLaneBusBase &&
      lane > kEngineParamLaneBusBase - TrackMixerRuntime::kMaxBusLanes) {
    if (kind != TrackMixerRuntime::kFaderDb) return false;
    const uint32_t bus_index = kEngineParamLaneBusBase - lane;
    return track_mixer_runtime_.set_bus_gain_db_by_index(bus_index, value);
  }
  // Track lanes own only the typed fader and pan targets. Width remains a
  // standalone strip/mixer control and is intentionally not generated for a
  // track lane (set_lane_parameter also rejects it as a defensive boundary).
  if (kind != TrackMixerRuntime::kFaderDb && kind != TrackMixerRuntime::kPan) return false;
  return track_mixer_runtime_.set_lane_parameter(static_cast<size_t>(lane), kind, value);
}

bool RealtimeEngine::route_engine_parameter_thunk(void* context, uint32_t param_id,
                                                  float value) noexcept {
  return static_cast<RealtimeEngine*>(context)->route_engine_parameter(param_id, value);
}

bool RealtimeEngine::describe_reserved_parameter(uint32_t id,
                                                 automation::ParameterDescription* out) const {
  if (out == nullptr || !parameter_target_reserved(id)) return false;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (is_instrument_param_id(id)) {
    return describe_instrument_reserved_parameter(id, out);
  }
#endif
  if (is_insert_param_id(id)) {
    const InsertAutomationTarget* target = live_insert_target(id);
    if (target == nullptr ||
        insert_processor_layout(current_insert_processor(*target)) != target->processor) {
      return false;
    }
    const unsigned int insert_index = target->insert_index;
    const unsigned int param_id = static_cast<unsigned int>(insert_param_param(id));
    std::string processor_name;
    if (target->strip == InsertStripKind::kMaster) {
      // The retained spec describes the owned strip only; a strip bound since
      // through bind_mixing_strip has none.
      if (owned_master_strip_ == nullptr || mixing_runtime_.strip() != owned_master_strip_.get()) {
        return false;
      }
      const std::string* name = strip_insert_processor_name_at(master_strip_spec_, insert_index);
      if (name == nullptr) return false;
      processor_name = *name;
    } else if (target->strip == InsertStripKind::kBus) {
      if (!track_mixer_runtime_.bus_insert_processor_name_by_id(target->owner_id, insert_index,
                                                                &processor_name)) {
        return false;
      }
    } else if (!track_mixer_runtime_.track_insert_processor_name(target->owner_id, insert_index,
                                                                 &processor_name)) {
      return false;
    }
    return describe_insert_param_from_catalog(processor_name, param_id, out);
  }
  const uint32_t lane = (id & kEngineParamLaneMask) >> kEngineParamLaneShift;
  const uint32_t kind = id & kEngineParamKindMask;
  const bool is_master = lane == kEngineParamLaneMaster;
  const bool is_bus = !is_master && lane <= kEngineParamLaneBusBase &&
                      lane > kEngineParamLaneBusBase - TrackMixerRuntime::kMaxBusLanes;
  // Mirrors route_engine_parameter's own per-scope kind restriction: a bus
  // owns only its fader, a track lane owns fader and pan, and width is a
  // master-only control (see the comment there).
  if (is_bus) {
    if (kind != TrackMixerRuntime::kFaderDb) return false;
  } else if (!is_master) {
    if (kind != TrackMixerRuntime::kFaderDb && kind != TrackMixerRuntime::kPan) return false;
  }
  switch (kind) {
    case TrackMixerRuntime::kFaderDb:
      out->name = "faderDb";
      out->unit = "dB";
      if (is_master) {
        // GainProcessor::set_gain_db (reached by MixingRuntime::set_parameter)
        // only rejects a non-finite value; unlike the lane/bus fader setters
        // it applies no numeric clamp.
        out->min_value = -std::numeric_limits<float>::infinity();
        out->max_value = std::numeric_limits<float>::infinity();
      } else {
        // TrackMixerRuntime::set_lane_parameter / set_bus_gain_db_by_index.
        out->min_value = kFloorDb;
        out->max_value = TrackMixerRuntime::kMaxGainDb;
      }
      out->default_value = 0.0f;
      break;
    case TrackMixerRuntime::kPan:
      // PannerProcessor::set_pan's clamp_pan.
      out->name = "pan";
      out->unit = "";
      out->min_value = -1.0f;
      out->max_value = 1.0f;
      out->default_value = 0.0f;
      break;
    case TrackMixerRuntime::kWidth:
      // StereoWidthProcessor::set_width's clamp_width.
      out->name = "width";
      out->unit = "";
      out->min_value = 0.0f;
      out->max_value = 2.0f;
      out->default_value = 1.0f;
      break;
    default:
      return false;
  }
  out->rt_safe = true;
  out->default_curve = automation::CurveType::Linear;
  return true;
}

bool RealtimeEngine::constructed_insert_parameter_base(uint32_t target_id,
                                                       float* out_value) const noexcept {
  if (out_value == nullptr || !is_insert_param_id(target_id)) return false;
  const InsertAutomationTarget* target = live_insert_target(target_id);
  if (target == nullptr || !insert_target_holds(*target)) return false;
  const unsigned int insert_index = target->insert_index;
  const unsigned int param_id = static_cast<unsigned int>(insert_param_param(target_id));
  switch (target->strip) {
    case InsertStripKind::kMaster:
      return owned_master_strip_->constructed_insert_parameter_value(insert_index, param_id,
                                                                     out_value);
    case InsertStripKind::kBus:
      return track_mixer_runtime_.bus_insert_constructed_parameter_value(
          target->owner_id, insert_index, param_id, out_value);
    case InsertStripKind::kTrack:
      return track_mixer_runtime_.lane_insert_constructed_parameter_value(
          target->owner_id, insert_index, param_id, out_value);
  }
  return false;
}

const InsertAutomationTarget* RealtimeEngine::live_insert_target(
    uint32_t target_id) const noexcept {
  const InsertAutomationTarget* target =
      insert_automation_targets_.live(insert_param_strip(target_id));
  return target != nullptr && target->insert_index == insert_param_index(target_id) ? target
                                                                                    : nullptr;
}

bool RealtimeEngine::insert_target_holds(const InsertAutomationTarget& target) const noexcept {
  switch (target.strip) {
    case InsertStripKind::kMaster:
      return owned_master_strip_ != nullptr &&
             insert_processor_layout(owned_master_strip_->insert_processor(target.insert_index)) ==
                 target.processor;
    case InsertStripKind::kBus:
      return track_mixer_runtime_.bus_insert_holds(target.owner_id, target.insert_index,
                                                   target.processor);
    case InsertStripKind::kTrack:
      return track_mixer_runtime_.lane_insert_holds(target.owner_id, target.insert_index,
                                                    target.processor);
  }
  return false;
}

bool RealtimeEngine::restore_track_lane_parameter(uint32_t target_id) noexcept {
  if ((target_id & kEngineParamNamespaceMask) != kEngineParamNamespace) return false;
  const uint32_t lane = (target_id & kEngineParamLaneMask) >> kEngineParamLaneShift;
  if (lane >= TrackMixerRuntime::kMaxTrackLanes) return false;
  return track_mixer_runtime_.restore_lane_parameter(static_cast<size_t>(lane),
                                                     target_id & kEngineParamKindMask);
}

bool RealtimeEngine::insert_parameter_constructed_value(uint32_t target_id,
                                                        float* out_value) const noexcept {
  return constructed_insert_parameter_base(target_id, out_value);
}

bool RealtimeEngine::master_insert_target_resolves(unsigned int insert_index,
                                                   unsigned int param_id) const noexcept {
  if (owned_master_strip_ == nullptr) return false;
  const rt::ProcessorBase* processor = owned_master_strip_->insert_processor(insert_index);
  return processor != nullptr && processor->parameter_is_realtime_safe(param_id);
}

bool RealtimeEngine::route_master_insert_param_smoothed(unsigned int insert_index,
                                                        unsigned int param_id, float value,
                                                        uint32_t base_id) noexcept {
  // A missing strip or insert is refused before a slot is claimed or a base recorded.
  if (!std::isfinite(value) || !master_insert_target_resolves(insert_index, param_id)) {
    return false;
  }
  MasterInsertAutoSlot* free_slot = nullptr;
  MasterInsertAutoSlot* settled_match = nullptr;
  for (MasterInsertAutoSlot& slot : master_insert_auto_slots_) {
    if (slot.assigned && slot.insert_index == insert_index && slot.param_id == param_id) {
      if (slot.active) {
        slot.smoother.set_target(value);
        return true;
      }
      settled_match = &slot;
    }
    if (!slot.active && free_slot == nullptr) {
      free_slot = &slot;
    }
  }
  if (settled_match != nullptr) free_slot = settled_match;
  if (free_slot == nullptr) {
    ++master_insert_automation_overflow_count_;
    return false;
  }
  // Fresh slot: manual base, last applied, construction, target. Settled: last applied, live value.
  float baseline = value;
  if (settled_match != nullptr) {
    // Retained manual bases must not pull a live settled target back to an older edit.
    baseline = settled_match->smoother.current();
    float last_applied = baseline;
    if (owned_master_strip_ != nullptr &&
        owned_master_strip_->last_applied_insert_parameter_value(insert_index, param_id,
                                                                 &last_applied) &&
        std::isfinite(last_applied)) {
      baseline = last_applied;
    }
  } else {
    // The command handler records this call's manual base only afterwards.
    baseline = value;
    if (base_id == 0 || !parameter_base_lookup(base_id, &baseline) || !std::isfinite(baseline)) {
      const bool have_last_applied = owned_master_strip_ != nullptr &&
                                     owned_master_strip_->last_applied_insert_parameter_value(
                                         insert_index, param_id, &baseline);
      if (!have_last_applied || !std::isfinite(baseline)) {
        if (owned_master_strip_ == nullptr ||
            !owned_master_strip_->constructed_insert_parameter_value(insert_index, param_id,
                                                                     &baseline) ||
            !std::isfinite(baseline)) {
          baseline = value;
        }
      }
    }
  }
  free_slot->active = true;
  free_slot->assigned = true;
  free_slot->insert_index = insert_index;
  free_slot->param_id = param_id;
  free_slot->smoother.prepare(sample_rate_, 5.0f);
  free_slot->smoother.reset(baseline);
  free_slot->smoother.set_target(value);
  return true;
}

void RealtimeEngine::advance_master_insert_automations(int num_steps) noexcept {
  if (num_steps <= 0 || owned_master_strip_ == nullptr) return;
  constexpr float kSettleEpsilon = 1.0e-6f;
  for (MasterInsertAutoSlot& slot : master_insert_auto_slots_) {
    if (!slot.active) continue;
    const float value = slot.smoother.advance(num_steps);
    owned_master_strip_->apply_insert_parameter(slot.insert_index, slot.param_id, value);
    if (std::abs(slot.smoother.target() - value) <= kSettleEpsilon) {
      slot.smoother.reset(slot.smoother.target());
      slot.active = false;
    }
  }
}

void RealtimeEngine::settle_master_insert_automations() noexcept {
  for (MasterInsertAutoSlot& slot : master_insert_auto_slots_) {
    if (!slot.active) continue;
    const float target = slot.smoother.target();
    slot.smoother.reset(target);
    if (owned_master_strip_ != nullptr) {
      owned_master_strip_->apply_insert_parameter(slot.insert_index, slot.param_id, target);
    }
    slot.active = false;
  }
}

void RealtimeEngine::clear_master_insert_automations() noexcept {
  for (MasterInsertAutoSlot& slot : master_insert_auto_slots_) {
    slot.active = false;
    slot.assigned = false;
  }
}

bool RealtimeEngine::add_monitor_strip(mixing::ChannelStrip* strip) noexcept {
  if (strip != nullptr && mixing_runtime_.strip() == strip) {
    return false;
  }
  return monitor_runtime_.add_strip(strip);
}
SidechainRefusal RealtimeEngine::can_set_lane_sidechain(uint32_t track_id,
                                                        unsigned int insert_index,
                                                        uint32_t source_track_id) const noexcept {
  return track_mixer_runtime_.can_set_lane_sidechain(track_id, insert_index, source_track_id);
}

SidechainRefusal RealtimeEngine::can_set_bus_sidechain(uint32_t bus_id, unsigned int insert_index,
                                                       SidechainSourceKind kind,
                                                       uint32_t source_id) const noexcept {
  return track_mixer_runtime_.can_set_bus_sidechain(bus_id, insert_index, kind, source_id);
}

SidechainRefusal RealtimeEngine::can_set_master_sidechain(unsigned int insert_index,
                                                          SidechainSourceKind kind,
                                                          uint32_t source_id) const noexcept {
  return track_mixer_runtime_.can_set_master_sidechain(insert_index, kind, source_id);
}
#endif

}  // namespace sonare::engine
