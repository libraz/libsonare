#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "engine/insert_automation_id.h"
#include "engine/realtime_engine.h"
#include "engine/realtime_engine_internal.h"
#include "rt/command.h"

namespace sonare::engine {

#if defined(SONARE_WITH_MIXING)
namespace {

constexpr uint32_t kEngineParamLaneMaster = 0xFFu;
constexpr uint32_t kEngineParamLaneBusBase = 0xFEu;

}  // namespace

void RealtimeEngine::set_mixing_enabled(bool enabled) noexcept {
  mixing_enabled_.store(enabled, std::memory_order_relaxed);
  update_reported_graph_latency();
}

bool RealtimeEngine::bind_mixing_strip(mixing::ChannelStrip* strip) {
  if (strip != nullptr && monitor_runtime_.contains(strip)) {
    return false;
  }
  const bool bound = mixing_runtime_.bind(strip);
  if (bound && max_block_size_ > 0) {
    // Re-prepare so the freshly bound strip sees the engine's sample rate and
    // block size. bind() runs on the control thread, so allocation is allowed.
    mixing_runtime_.prepare(sample_rate_, max_block_size_);
  }
  if (bound) {
    update_reported_graph_latency();
  }
  return bound;
}

bool RealtimeEngine::set_master_strip(const mixing::api::Strip& strip_spec) {
  std::unique_ptr<mixing::ChannelStrip> strip;
  try {
    strip = make_channel_strip_from_spec(strip_spec);
  } catch (...) {
    return false;
  }
  if (!strip) return false;
  // Control-thread-only, not concurrent with process() (see RealtimeEngine's
  // thread-safety contract). This std::move destroys the previously bound master
  // strip immediately -- there is no deferred reclaim -- and rebinds the raw
  // pointer the audio thread reads, so a concurrent render would use freed
  // memory. The caller must quiesce process() around this call.
  clear_master_insert_automations();
  owned_master_strip_ = std::move(strip);
  const bool bound = bind_mixing_strip(owned_master_strip_.get());
  if (bound) {
    set_mixing_enabled(true);
  }
  return bound;
}

bool RealtimeEngine::set_track_lanes(std::vector<TrackLaneConfig> lanes) {
  const bool ok = track_mixer_runtime_.set_track_lanes(std::move(lanes));
  if (ok) {
    update_reported_graph_latency();
  }
  return ok;
}

bool RealtimeEngine::set_track_buses(std::vector<TrackBusConfig> buses) {
  const bool ok = track_mixer_runtime_.set_buses(std::move(buses));
  if (ok) {
    update_reported_graph_latency();
  }
  return ok;
}

bool RealtimeEngine::bind_track_strip(uint32_t track_id, mixing::ChannelStrip* strip) {
  const bool ok = track_mixer_runtime_.bind_track_strip(track_id, strip);
  if (ok) {
    update_reported_graph_latency();
  }
  return ok;
}

bool RealtimeEngine::set_track_strip(uint32_t track_id, const mixing::api::Strip& strip) {
  const bool ok = track_mixer_runtime_.set_track_strip(track_id, strip);
  if (ok) {
    update_reported_graph_latency();
  }
  return ok;
}

bool RealtimeEngine::set_bus_strip(uint32_t bus_id, const mixing::api::Bus& bus) {
  const bool ok = track_mixer_runtime_.set_bus_strip(bus_id, bus);
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
  size_t lane_index = 0;
  unsigned int param_id = 0;
  if (!track_mixer_runtime_.resolve_track_insert_param(track_id, insert_index, key, &lane_index,
                                                       &param_id)) {
    return InsertParamSetResult::kInvalidTarget;
  }
  if (lane_index > 0xFFu || insert_index > 0xFFu || param_id > 0xFFu) {
    return InsertParamSetResult::kInvalidTarget;
  }
  rt::Command command;
  command.type = rt::CommandType::kSetTrackInsertParam;
  command.target_id = (static_cast<uint32_t>(lane_index) << 16) | ((insert_index & 0xFFu) << 8) |
                      (param_id & 0xFFu);
  command.sample_time = -1;  // block head / immediate
  command.arg.f = value;
  return push_command(command) ? InsertParamSetResult::kQueued : InsertParamSetResult::kQueueFull;
}

bool RealtimeEngine::set_track_insert_param(uint32_t track_id, unsigned int insert_index,
                                            const std::string& key, float value) noexcept {
  return set_track_insert_param_detailed(track_id, insert_index, key, value) ==
         InsertParamSetResult::kQueued;
}

InsertParamSetResult RealtimeEngine::set_master_insert_param_detailed(unsigned int insert_index,
                                                                      const std::string& key,
                                                                      float value) noexcept {
  if (owned_master_strip_ == nullptr) {
    return InsertParamSetResult::kInvalidTarget;
  }
  const int id = owned_master_strip_->insert_parameter_id_for_key(insert_index, key);
  if (id < 0) {
    return InsertParamSetResult::kInvalidTarget;
  }
  const unsigned int param_id = static_cast<unsigned int>(id);
  if (insert_index > 0xFFu || param_id > 0xFFu) {
    return InsertParamSetResult::kInvalidTarget;
  }
  rt::Command command;
  command.type = rt::CommandType::kSetMasterInsertParam;
  command.target_id = ((insert_index & 0xFFu) << 8) | (param_id & 0xFFu);
  command.sample_time = -1;  // block head / immediate
  command.arg.f = value;
  return push_command(command) ? InsertParamSetResult::kQueued : InsertParamSetResult::kQueueFull;
}

bool RealtimeEngine::set_master_insert_param(unsigned int insert_index, const std::string& key,
                                             float value) noexcept {
  return set_master_insert_param_detailed(insert_index, key, value) ==
         InsertParamSetResult::kQueued;
}

InsertParamSetResult RealtimeEngine::set_bus_insert_param_detailed(uint32_t bus_id,
                                                                   unsigned int insert_index,
                                                                   const std::string& key,
                                                                   float value) noexcept {
  size_t bus_index = 0;
  unsigned int param_id = 0;
  if (!track_mixer_runtime_.resolve_bus_insert_param(bus_id, insert_index, key, &bus_index,
                                                     &param_id)) {
    return InsertParamSetResult::kInvalidTarget;
  }
  if (bus_index >= TrackMixerRuntime::kMaxBusLanes || insert_index > 0xFFu || param_id > 0xFFu) {
    return InsertParamSetResult::kInvalidTarget;
  }
  // Route through the reserved insert-automation id over the generic kSetParam
  // command: apply_command already forwards reserved ids to the smoothed insert
  // router, so a manual set glides exactly like an automated breakpoint without a
  // dedicated command type (no engine ABI change).
  const uint32_t selector = kInsertStripBusBase - static_cast<uint32_t>(bus_index);
  rt::Command command;
  command.type = rt::CommandType::kSetParam;
  command.target_id = make_insert_param_id(selector, insert_index, param_id);
  command.sample_time = -1;  // block head / immediate
  command.arg.f = value;
  return push_command(command) ? InsertParamSetResult::kQueued : InsertParamSetResult::kQueueFull;
}

bool RealtimeEngine::set_bus_insert_param(uint32_t bus_id, unsigned int insert_index,
                                          const std::string& key, float value) noexcept {
  return set_bus_insert_param_detailed(bus_id, insert_index, key, value) ==
         InsertParamSetResult::kQueued;
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
  if (lane_index > kInsertStripMask || insert_index > kInsertIndexMask ||
      param_id > kInsertParamFieldMask) {
    return -1;
  }
  return make_insert_param_id(static_cast<uint32_t>(lane_index), insert_index, param_id);
}

int64_t RealtimeEngine::resolve_master_insert_automation_id(unsigned int insert_index,
                                                            const std::string& key) noexcept {
  if (owned_master_strip_ == nullptr) {
    return -1;
  }
  const int id = owned_master_strip_->insert_parameter_id_for_key(insert_index, key);
  if (id < 0) {
    return -1;
  }
  const unsigned int param_id = static_cast<unsigned int>(id);
  if (insert_index > kInsertIndexMask || param_id > kInsertParamFieldMask) {
    return -1;
  }
  return make_insert_param_id(kInsertStripMaster, insert_index, param_id);
}

int64_t RealtimeEngine::resolve_bus_insert_automation_id(uint32_t bus_id, unsigned int insert_index,
                                                         const std::string& key) noexcept {
  size_t bus_index = 0;
  unsigned int param_id = 0;
  if (!track_mixer_runtime_.resolve_bus_insert_param(bus_id, insert_index, key, &bus_index,
                                                     &param_id)) {
    return -1;
  }
  if (bus_index >= TrackMixerRuntime::kMaxBusLanes || insert_index > kInsertIndexMask ||
      param_id > kInsertParamFieldMask) {
    return -1;
  }
  const uint32_t selector = kInsertStripBusBase - static_cast<uint32_t>(bus_index);
  return make_insert_param_id(selector, insert_index, param_id);
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

bool RealtimeEngine::set_track_channel_delay_samples(uint32_t track_id,
                                                     int delay_samples) noexcept {
  const bool ok = track_mixer_runtime_.set_track_channel_delay_samples(track_id, delay_samples);
  if (ok) {
    update_reported_graph_latency();
  }
  return ok;
}

bool RealtimeEngine::set_master_eq_band(size_t band_index,
                                        const mastering::eq::EqBand& band) noexcept {
  if (owned_master_strip_ == nullptr) return false;
  try {
    owned_master_strip_->set_eq_band(band_index, band);
    update_reported_graph_latency();
    return true;
  } catch (...) {
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
  // Insert-automation namespace: decode (strip selector, insert, param) and set
  // the matching per-target smoother. Master inserts use this engine's slot
  // table; lane/bus inserts use the track mixer's. The strip selector is an
  // integer index, so a stale target resolves to a no-op (dangling-safe).
  if (is_insert_param_id(target_id)) {
    const uint32_t strip = insert_param_strip(target_id);
    const unsigned int insert_index = static_cast<unsigned int>(insert_param_index(target_id));
    const unsigned int param_id = static_cast<unsigned int>(insert_param_param(target_id));
    if (strip == kInsertStripMaster) {
      return route_master_insert_param_smoothed(insert_index, param_id, value);
    }
    if (strip <= kInsertStripBusBase &&
        strip > kInsertStripBusBase - TrackMixerRuntime::kMaxBusLanes) {
      const size_t bus_index = kInsertStripBusBase - strip;
      return track_mixer_runtime_.route_bus_insert_param_smoothed(bus_index, insert_index, param_id,
                                                                  value);
    }
    return track_mixer_runtime_.route_lane_insert_param_smoothed(static_cast<size_t>(strip),
                                                                 insert_index, param_id, value);
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

bool RealtimeEngine::route_master_insert_param_smoothed(unsigned int insert_index,
                                                        unsigned int param_id,
                                                        float value) noexcept {
  if (!std::isfinite(value)) return false;
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
  if (settled_match != nullptr) {
    settled_match->active = true;
    settled_match->smoother.set_target(value);
    return true;
  }
  if (free_slot == nullptr) {
    ++master_insert_automation_overflow_count_;
    return false;
  }
  free_slot->active = true;
  free_slot->assigned = true;
  free_slot->insert_index = insert_index;
  free_slot->param_id = param_id;
  // Snap to the first observed value so the smoother does not glide up from 0.
  free_slot->smoother.prepare(sample_rate_, 5.0f);
  free_slot->smoother.reset(value);
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
#endif

}  // namespace sonare::engine
