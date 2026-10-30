#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <new>
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

namespace sonare::engine {

#if defined(SONARE_WITH_MIXING)

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

// Positional commands (solo/mute, monitor, the legacy track insert command) carry a
// lane index; automation ids name their track through the target table instead.
TrackLaneRemapResult remap_track_command(
    rt::Command& command,
    const std::array<uint32_t, TrackMixerRuntime::kMaxTrackLanes>& old_lane_ids,
    size_t old_lane_count,
    const std::array<uint32_t, TrackMixerRuntime::kMaxTrackLanes>& new_lane_ids,
    size_t new_lane_count) noexcept {
  switch (command.type) {
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
  const auto refuse_argument = [this] {
    return report_mixer_edit(MixerEditStatus::kInvalidArgument);
  };
  const auto refuse_state = [this] { return report_mixer_edit(MixerEditStatus::kInvalidState); };
  const auto refuse_out_of_memory = [this] {
    return report_mixer_edit(MixerEditStatus::kOutOfMemory);
  };
  if (!strip_eq_acceptable(strip_spec.eq, sample_rate_)) return refuse_argument();
  mixing::api::Strip next_spec;
  try {
    next_spec = strip_spec;
  } catch (...) {
    return refuse_out_of_memory();
  }
  // In-place: an unchanged insert chain keeps the bound strip, and with it the
  // insert state (tails, envelopes), the insert automation and the EQ filter
  // state of every band the new spec leaves alone.
  if (owned_master_strip_ != nullptr && mixing_runtime_.strip() == owned_master_strip_.get() &&
      strip_inserts_equal(master_strip_spec_.inserts, strip_spec.inserts)) {
    if (!owned_master_strip_->try_set_channel_delay_samples(strip_spec.channel_delay_samples)) {
      return refuse_argument();
    }
    apply_strip_scalars(*owned_master_strip_, strip_spec, master_strip_spec_);
    std::swap(master_strip_spec_, next_spec);
    update_reported_graph_latency();
    set_mixing_enabled(true);
    return report_mixer_edit(MixerEditStatus::kApplied);
  }
  std::unique_ptr<mixing::ChannelStrip> strip;
  try {
    strip = make_channel_strip_from_spec(next_spec, sample_rate_);
    if (strip == nullptr) return refuse_argument();
    if (max_block_size_ > 0) strip->prepare(sample_rate_, max_block_size_);
  } catch (const std::bad_alloc&) {
    return refuse_out_of_memory();
  } catch (...) {
    return refuse_argument();
  }
  if (!insert_automation_room(
          insert_entries_to_mint(insert_automation_targets_, InsertStripKind::kMaster, 0, *strip,
                                 strip->num_pre_inserts() + strip->num_post_inserts()))) {
    return refuse_state();
  }
  TrackMixerRuntime::PreparedMasterStripUpdate update;
  if (!track_mixer_runtime_.prepare_master_strip_update(strip.get(), next_spec.inserts.size(),
                                                        &update)) {
    return update.pdc.out_of_memory ? refuse_out_of_memory() : refuse_state();
  }
  // Keys stay on their insert index; an index the new chain lacks is dropped.
  if (!mixing_runtime_.bind(strip.get())) return refuse_state();
  // Control-thread-only: this std::move destroys the old master immediately (no
  // deferred reclaim), so the caller must not run process() concurrently.
  clear_master_insert_automations();
  owned_master_strip_ = std::move(strip);
  std::swap(master_strip_spec_, next_spec);
  track_mixer_runtime_.commit_master_strip_update(owned_master_strip_.get(), update);
  sync_insert_automation_targets();
  set_mixing_enabled(true);
  update_reported_graph_latency();
  return report_mixer_edit(MixerEditStatus::kApplied);
}

bool RealtimeEngine::validate_track_lanes(const std::vector<TrackLaneConfig>& lanes) const {
  return track_mixer_runtime_.validate_track_lanes(lanes);
}

bool RealtimeEngine::validate_track_strip(const mixing::api::Strip& strip) const {
  if (!strip_eq_acceptable(strip.eq, sample_rate_)) return false;
  try {
    return make_channel_strip_from_spec(strip, sample_rate_) != nullptr;
  } catch (const std::bad_alloc&) {
    throw;
  } catch (...) {
    return false;
  }
}

bool RealtimeEngine::set_track_lanes(std::vector<TrackLaneConfig> lanes) {
  // The list's own content is judged before the capacity earlier calls used up.
  if (!track_mixer_runtime_.validate_track_lanes(lanes)) {
    return report_mixer_edit(MixerEditStatus::kInvalidArgument);
  }
  std::array<uint32_t, TrackMixerRuntime::kMaxTrackLanes> new_lane_ids{};
  const size_t new_lane_count = std::min(lanes.size(), new_lane_ids.size());
  size_t entering_inserts = 0;
  for (size_t lane_index = 0; lane_index < new_lane_count; ++lane_index) {
    new_lane_ids[lane_index] = lanes[lane_index].track_id;
    const auto old_end = track_lane_ids_.begin() + static_cast<std::ptrdiff_t>(track_lane_count_);
    if (std::find(track_lane_ids_.begin(), old_end, new_lane_ids[lane_index]) == old_end) {
      // An entering track mints its fader/pan entry plus one per insert slot.
      entering_inserts +=
          1 + track_mixer_runtime_.bound_track_insert_count(new_lane_ids[lane_index]);
    }
  }
  if (!insert_automation_room(entering_inserts)) {
    return report_mixer_edit(MixerEditStatus::kInvalidState);
  }

  const bool ok = track_mixer_runtime_.set_track_lanes(std::move(lanes));
  if (ok) {
    // Remap queued positional commands by track id so a lane reorder cannot retarget a strip.
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

    track_lane_ids_ = new_lane_ids;
    track_lane_count_ = new_lane_count;
    // A removed track's ids are retired, and their queued edits and bases go with them.
    sync_insert_automation_targets();
    update_reported_graph_latency();
  }
  return report_track_mixer_edit(ok);
}

namespace {

// Copies the published lane list into @p out; false when @p track_id has no lane in it.
bool copy_lanes_holding(const std::vector<TrackLaneConfig>* published, uint32_t track_id,
                        std::vector<TrackLaneConfig>* out, TrackLaneConfig** out_lane) {
  if (published == nullptr) return false;
  *out = *published;
  for (TrackLaneConfig& lane : *out) {
    if (lane.track_id == track_id) {
      *out_lane = &lane;
      return true;
    }
  }
  return false;
}

}  // namespace

TrackLaneEditResult RealtimeEngine::track_lane_edit_result(bool applied) const noexcept {
  if (applied) return TrackLaneEditResult::kApplied;
  switch (last_mixer_edit_status()) {
    case MixerEditStatus::kInvalidState:
      return TrackLaneEditResult::kStateRefused;
    case MixerEditStatus::kOutOfMemory:
      return TrackLaneEditResult::kOutOfMemory;
    default:
      return TrackLaneEditResult::kRefused;
  }
}

TrackLaneEditResult RealtimeEngine::set_track_sends(uint32_t track_id,
                                                    std::vector<TrackLaneConfig::Send> sends) {
  std::vector<TrackLaneConfig> lanes;
  TrackLaneConfig* lane = nullptr;
  if (!copy_lanes_holding(track_mixer_runtime_.control_lanes(), track_id, &lanes, &lane)) {
    return TrackLaneEditResult::kUnknownTrack;
  }
  lane->sends = std::move(sends);
  return track_lane_edit_result(set_track_lanes(std::move(lanes)));
}

TrackLaneEditResult RealtimeEngine::set_track_output_bus(uint32_t track_id,
                                                         uint32_t output_bus_id) {
  std::vector<TrackLaneConfig> lanes;
  TrackLaneConfig* lane = nullptr;
  if (!copy_lanes_holding(track_mixer_runtime_.control_lanes(), track_id, &lanes, &lane)) {
    return TrackLaneEditResult::kUnknownTrack;
  }
  lane->output_bus_id = output_bus_id;
  return track_lane_edit_result(set_track_lanes(std::move(lanes)));
}

bool RealtimeEngine::set_track_buses(std::vector<TrackBusConfig> buses) {
  if (!track_mixer_runtime_.validate_buses(buses)) {
    return report_mixer_edit(MixerEditStatus::kInvalidArgument);
  }
  // An entering bus mints its fader entry; its slots are minted when its strip is set.
  size_t entering = 0;
  for (const TrackBusConfig& bus : buses) {
    if (!track_mixer_runtime_.configured_bus(bus.bus_id)) ++entering;
  }
  if (!insert_automation_room(entering)) return report_mixer_edit(MixerEditStatus::kInvalidState);
  const bool ok = track_mixer_runtime_.set_buses(std::move(buses));
  if (ok) {
    // A removed bus's ids are retired, and their queued edits and bases go with
    // them, so an unreachable id neither reports UnknownTarget later nor holds
    // base capacity.
    sync_insert_automation_targets();
    update_reported_graph_latency();
  }
  return report_track_mixer_edit(ok);
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
  return report_track_mixer_edit(ok);
}

bool RealtimeEngine::set_bus_strip(uint32_t bus_id, const mixing::api::Bus& bus) {
  InsertChainAdmission admission{&insert_automation_targets_, true};
  const bool ok = track_mixer_runtime_.set_bus_strip(bus_id, bus, admit_bus_chain, &admission);
  sync_insert_automation_targets();
  if (ok) {
    update_reported_graph_latency();
  }
  return report_track_mixer_edit(ok);
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

bool RealtimeEngine::master_insert_target_resolves(unsigned int insert_index,
                                                   unsigned int param_id) const noexcept {
  if (owned_master_strip_ == nullptr) return false;
  const rt::ProcessorBase* processor = owned_master_strip_->insert_processor(insert_index);
  return processor != nullptr && processor->parameter_is_realtime_safe(param_id);
}

bool RealtimeEngine::route_master_insert_param_smoothed(unsigned int insert_index,
                                                        unsigned int param_id, float value,
                                                        uint32_t base_id) noexcept {
  // A missing strip or insert, or a value the processor refuses, is refused before
  // a slot is claimed or a base recorded.
  if (!std::isfinite(value) || !master_insert_target_resolves(insert_index, param_id) ||
      !owned_master_strip_->insert_parameter_accepts(insert_index, param_id, value)) {
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
