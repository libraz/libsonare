#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "engine/strip_spec.h"
#include "engine/track_mixer.h"
#include "engine/track_mixer_internal.h"
#include "mastering/api/insert_factory.h"
#include "mastering/api/named_processor.h"
#include "mixing/channel_strip_eq.h"
#include "mixing/pan_law.h"
#include "util/constants.h"
#include "util/db.h"

namespace sonare::engine {

using sonare::constants::kFloorDb;

namespace {

bool bus_pan_is_default(const mixing::api::Bus& bus) noexcept {
  const mixing::api::Bus defaults;
  return bus.pan == defaults.pan && bus.pan_mode == defaults.pan_mode &&
         bus.pan_law == defaults.pan_law && bus.dual_pan_left == defaults.dual_pan_left &&
         bus.dual_pan_right == defaults.dual_pan_right;
}

// Pan and width are stereo-image operations, refused on a bus wider than two channels.
bool bus_stereo_image_is_default(const mixing::api::Bus& bus) noexcept {
  return bus_pan_is_default(bus) && bus.width == mixing::api::Bus{}.width;
}

bool layout_wider_than_stereo(ChannelLayout layout) noexcept {
  return channel_count(layout) > TrackMixerRuntime::kMaxLaneChannels;
}

}  // namespace

bool TrackMixerRuntime::set_buses(std::vector<TrackBusConfig> buses) {
  last_refusal_ = MixerRefusal::kArgument;
  if (!bus_config_valid(buses)) return false;
  // Bus state follows the bus id, not the slot: source[i] is the slot new bus i
  // held before, or -1 for a bus declared now.
  std::array<int, kMaxBusLanes> source{};
  source.fill(-1);
  std::array<bool, kMaxBusLanes> kept{};
  std::array<bool, kMaxBusLanes> moved_out{};
  std::array<ChannelLayout, kMaxBusLanes> previous_layout{};
  std::array<float, kMaxBusLanes> previous_gain_db{};
  for (size_t index = 0; index < bus_configs_.size() && index < previous_layout.size(); ++index) {
    previous_layout[index] = bus_configs_[index].layout;
    previous_gain_db[index] = bus_configs_[index].gain_db;
  }
  bool any_move = false;
  for (size_t index = 0; index < buses.size(); ++index) {
    const int previous = configured_bus_index(buses[index].bus_id);
    if (previous < 0) continue;
    // Pan and width are refused on a wider bus rather than silently reset.
    if (layout_wider_than_stereo(buses[index].layout) &&
        !bus_stereo_image_is_default(bus_states_[static_cast<size_t>(previous)].spec)) {
      return refuse_from_state();
    }
    source[index] = previous;
    if (previous == static_cast<int>(index)) {
      kept[index] = true;
    } else {
      moved_out[static_cast<size_t>(previous)] = true;
      any_move = true;
    }
  }
  // Bus and master keys whose bus (source or target) is not in the new list, or
  // whose insert the target no longer has, are dropped; the rest stay edges of
  // the graph the new list has to keep acyclic.
  const auto declared = [&buses](uint32_t bus_id) {
    return std::any_of(buses.begin(), buses.end(),
                       [bus_id](const TrackBusConfig& bus) { return bus.bus_id == bus_id; });
  };
  const size_t binding_count = sidechains_.count;
  std::array<bool, kMaxSidechainBindings> drop{};
  std::array<KeyEdge, kMaxSidechainBindings> key_edges{};
  size_t key_edge_count = 0;
  for (size_t i = 0; i < binding_count; ++i) {
    const SidechainBinding& binding = sidechains_.bindings[i];
    const auto target_kind = static_cast<SidechainTargetKind>(binding.target_kind);
    if (target_kind == SidechainTargetKind::Lane) continue;
    const uint32_t target_id = binding.target_id;
    const uint32_t source_id = binding.source_id;
    const bool bus_source = binding.source_kind == static_cast<uint8_t>(SidechainSourceKind::Bus);
    if (bus_source && !declared(source_id)) drop[i] = true;
    if (target_kind == SidechainTargetKind::Bus) {
      const int previous = configured_bus_index(target_id);
      const mixing::FxBus* fx =
          previous >= 0 ? bus_states_[static_cast<size_t>(previous)].bus.get() : nullptr;
      if (!declared(target_id) || fx == nullptr || binding.insert_index >= fx->num_inserts()) {
        drop[i] = true;
      }
      if (!drop[i] && bus_source) key_edges[key_edge_count++] = KeyEdge{source_id, target_id};
    }
  }
  std::array<size_t, kMaxBusLanes> order{};
  if (!validate_bus_graph(buses, key_edges.data(), key_edge_count, &order)) return false;
  // Every lane output and lane send has to land on a bus the new list keeps.
  static const std::vector<TrackLaneConfig> kNoLanes;
  const std::vector<TrackLaneConfig>* current_lanes = lanes_.control_current().get();
  const std::vector<TrackLaneConfig>& lanes = current_lanes ? *current_lanes : kNoLanes;
  for (const TrackLaneConfig& lane : lanes) {
    if (lane.output_bus_id != 0 && !declared(lane.output_bus_id)) return refuse_from_state();
    for (const TrackLaneConfig::Send& send : lane.sends) {
      if (!declared(send.bus_id)) return refuse_from_state();
    }
  }
  // The alignment the new graph needs has to fit the delay lines.
  BusGraphView view;
  view.buses = &buses;
  view.order = order;
  view.skip_binding = drop;
  build_routes(buses, drop, &view.routes);
  for (size_t index = 0; index < buses.size(); ++index) {
    const mixing::FxBus* fx =
        source[index] >= 0 ? bus_states_[static_cast<size_t>(source[index])].bus.get() : nullptr;
    view.bus[index] = fx;
    view.latency_q8[index] = fx != nullptr ? fx->latency_samples_q8() : 0;
  }
  PdcPlan plan;
  if (!plan_pdc(lanes, view, &plan)) return refuse_from_state();
  std::array<int, kMaxBusLanes> bank_sources{};
  bank_sources.fill(-1);
  std::array<bool, kMaxBusLanes> bank_used{};
  for (size_t index = 0; index < kMaxBusLanes; ++index) {
    int previous = index < buses.size() ? source[index] : -1;
    if (previous < 0 || previous >= static_cast<int>(kMaxBusLanes) ||
        bank_used[static_cast<size_t>(previous)]) {
      previous = -1;
      for (size_t candidate = 0; candidate < kMaxBusLanes; ++candidate) {
        if (!bank_used[candidate]) {
          previous = static_cast<int>(candidate);
          break;
        }
      }
    }
    if (previous < 0) continue;
    bank_sources[index] = previous;
    bank_used[static_cast<size_t>(previous)] = true;
  }
  // Stage every lane send table before the bus table changes, resolving strips by
  // track identity so a lane reorder cannot make the staging positional.
  std::array<mixing::ChannelStrip*, kMaxTrackLanes> lane_send_strips{};
  std::array<std::unique_ptr<mixing::ChannelStrip>, kMaxTrackLanes> staged_lane_owned;
  std::array<mixing::ChannelStrip::PreparedSends, kMaxTrackLanes> prepared_lane_sends;
  std::array<bool, kMaxTrackLanes> replace_lane_sends{};
  size_t staged_lane_owned_count = 0;
  try {
    for (size_t lane_index = 0; lane_index < lanes.size(); ++lane_index) {
      const TrackLaneConfig& config = lanes[lane_index];
      mixing::ChannelStrip* strip = bound_strip_for(config.track_id);
      if (strip == nullptr) strip = owned_strip_for(config.track_id);
      if (strip == nullptr) {
        for (const LaneState& state : lane_states_) {
          if (state.track_id == config.track_id) {
            strip = state.strip;
            break;
          }
        }
      }
      if (strip == nullptr && !config.sends.empty()) {
        if (owned_strips_.size() + staged_lane_owned_count >= kMaxTrackLanes) {
          return refuse_from_state();
        }
        auto owned = make_seed_strip();
        strip = owned.get();
        staged_lane_owned[lane_index] = std::move(owned);
        ++staged_lane_owned_count;
      }
      lane_send_strips[lane_index] = strip;
      if (strip == nullptr) continue;
      // set_buses() leaves the lane snapshot and its strip identity intact.
      // Preserve the existing send processor in that case, since replacing it
      // would clear queued send automation even though no lane send changed.
      const bool retain_sends = staged_lane_owned[lane_index] == nullptr &&
                                lane_states_[lane_index].track_id == config.track_id &&
                                lane_states_[lane_index].strip == strip &&
                                strip->num_sends() == config.sends.size();
      if (retain_sends) continue;
      replace_lane_sends[lane_index] = true;
      std::array<mixing::SendConfig, mixing::ChannelStrip::kMaxSends> configs{};
      for (size_t send_index = 0; send_index < config.sends.size(); ++send_index) {
        const TrackLaneConfig::Send& send = config.sends[send_index];
        configs[send_index] =
            mixing::SendConfig{send.enabled ? send.level_db : kFloorDb, send.timing, 5.0f};
      }
      if (!strip->prepare_sends(configs.data(), config.sends.size(),
                                prepared_lane_sends[lane_index])) {
        // The sends were validated with the list, so only an allocation fails here.
        return refuse_out_of_memory();
      }
    }
    owned_strips_.reserve(owned_strips_.size() + staged_lane_owned_count);
    track_strip_bindings_.reserve(track_strip_bindings_.size() + staged_lane_owned_count);
  } catch (const std::bad_alloc&) {
    return refuse_out_of_memory();
  } catch (...) {
    return false;
  }

  // Prepare new FxBus objects up front so the commit only moves pointers and sets scalars.
  std::array<std::unique_ptr<mixing::FxBus>, kMaxBusLanes> staged_buses;
  try {
    for (size_t index = 0; index < buses.size(); ++index) {
      const int previous = source[index];
      const bool missing =
          previous < 0 || bus_states_[static_cast<size_t>(previous)].bus == nullptr;
      if (!missing) continue;
      auto bus = std::make_unique<mixing::FxBus>(static_cast<int>(kMaxTrackLanes));
      bus->set_channel_layout(buses[index].layout);
      if (max_block_size_ > 0) bus->prepare(sample_rate_, max_block_size_);
      staged_buses[index] = std::move(bus);
    }
  } catch (const std::bad_alloc&) {
    return refuse_out_of_memory();
  } catch (...) {
    return false;
  }

  // Stage PDC replacement storage now; commit_pdc_updates() applies scalar plans after the remap.
  std::array<int, kMaxTrackLanes> pdc_sources{};
  std::array<bool, kMaxTrackLanes> pdc_reset{};
  for (size_t index = 0; index < kMaxTrackLanes; ++index) {
    pdc_sources[index] = static_cast<int>(index);
    pdc_reset[index] = false;
  }
  PreparedPdc prepared_pdc;
  if (!prepare_pdc_updates(plan, pdc_sources, pdc_reset, &prepared_pdc, &bank_sources)) {
    return refuse_pdc(prepared_pdc);
  }

  decltype(bus_sends_) sends{};
  try {
    for (size_t index = 0; index < buses.size(); ++index) {
      for (size_t send_index = 0; send_index < buses[index].sends.size(); ++send_index) {
        const TrackLaneConfig::Send& send = buses[index].sends[send_index];
        auto processor = std::make_unique<mixing::SendProcessor>(
            mixing::SendConfig{send.enabled ? send.level_db : kFloorDb, send.timing, 5.0f});
        if (max_block_size_ > 0) processor->prepare(sample_rate_, max_block_size_);
        sends[index][send_index] = std::move(processor);
      }
    }
  } catch (const std::bad_alloc&) {
    return refuse_out_of_memory();
  } catch (...) {
    return false;
  }
  std::unique_ptr<std::array<BusState, kMaxBusLanes>> staging;
  try {
    if (any_move) staging = std::make_unique<std::array<BusState, kMaxBusLanes>>();
    for (size_t index = 0; index < bus_states_.size(); ++index) {
      if (kept[index]) continue;
      if (moved_out[index]) {
        (*staging)[index].eq.prepare(sample_rate_, max_block_size_);
        stage_bus_state(bus_states_[index], (*staging)[index]);
      }
    }
  } catch (const std::bad_alloc&) {
    return refuse_out_of_memory();
  } catch (...) {
    return false;
  }
  // Every throwing step is behind us; live ownership moves only from here on.
  for (size_t index = 0; index < bus_states_.size(); ++index) {
    if (kept[index] || !moved_out[index]) continue;
    (*staging)[index].bus = std::move(bus_states_[index].bus);
    (*staging)[index].spec = std::move(bus_states_[index].spec);
  }

  // Alignment banks move with their bus id; edge histories survive only while routes are unchanged.
  const auto resolves_to_bus = [](const std::vector<TrackBusConfig>& configs, uint32_t bus_id) {
    return std::any_of(configs.begin(), configs.end(),
                       [bus_id](const TrackBusConfig& config) { return config.bus_id == bus_id; });
  };
  const auto same_destination = [&](uint32_t old_id, uint32_t next_id) {
    const bool old_bus = old_id != 0 && resolves_to_bus(bus_configs_, old_id);
    const bool next_bus = next_id != 0 && resolves_to_bus(buses, next_id);
    return old_bus == next_bus && (!old_bus || old_id == next_id);
  };
  const auto routes_unchanged = [&](size_t index) {
    const int previous = source[index];
    if (previous < 0) return false;
    const TrackBusConfig& old = bus_configs_[static_cast<size_t>(previous)];
    const TrackBusConfig& next = buses[index];
    if (old.layout != next.layout || old.sends.size() != next.sends.size() ||
        !same_destination(old.output_bus_id, next.output_bus_id)) {
      return false;
    }
    for (size_t send = 0; send < old.sends.size(); ++send) {
      if (old.sends[send].timing != next.sends[send].timing ||
          !same_destination(old.sends[send].bus_id, next.sends[send].bus_id)) {
        return false;
      }
    }
    return true;
  };

  std::array<mixing::AlignmentDelay, kMaxBusLanes> old_bus_pdc = std::move(bus_pdc_delays_);
  std::array<mixing::AlignmentDelay, kMaxBusLanes> next_bus_pdc;
  std::array<mixing::AlignmentDelay, kMaxBusLanes * kBusEdgesPerBus> old_edges =
      std::move(bus_edge_delays_);
  std::array<mixing::AlignmentDelay, kMaxBusLanes * kBusEdgesPerBus> next_edges;
  for (size_t index = 0; index < kMaxBusLanes; ++index) {
    const int previous = bank_sources[index];
    if (previous < 0) continue;
    const bool retained_bus = index < buses.size() && source[index] >= 0;
    const bool retained_edges = retained_bus && routes_unchanged(index);
    next_bus_pdc[index] = std::move(old_bus_pdc[static_cast<size_t>(previous)]);
    if (!retained_bus) next_bus_pdc[index].reset();
    for (size_t edge = 0; edge < kBusEdgesPerBus; ++edge) {
      next_edges[index * kBusEdgesPerBus + edge] =
          std::move(old_edges[static_cast<size_t>(previous) * kBusEdgesPerBus + edge]);
      if (!retained_edges) next_edges[index * kBusEdgesPerBus + edge].reset();
    }
  }
  bus_pdc_delays_ = std::move(next_bus_pdc);
  bus_edge_delays_ = std::move(next_edges);
  for (size_t index = 0; index < bus_states_.size(); ++index) {
    if (!kept[index]) retire_bus_state(bus_states_[index]);
  }
  bus_configs_ = std::move(buses);
  for (InsertGainReductionBoard& board : bus_insert_gr_boards_) board.clear();
  bus_sends_ = std::move(sends);
  for (size_t lane_index = 0; lane_index < lanes.size(); ++lane_index) {
    if (staged_lane_owned[lane_index] == nullptr) continue;
    mixing::ChannelStrip* raw = staged_lane_owned[lane_index].get();
    owned_strips_.push_back(
        OwnedStrip{lanes[lane_index].track_id, std::move(staged_lane_owned[lane_index]), {}});
    record_track_strip_binding(lanes[lane_index].track_id, raw);
  }
  for (size_t lane_index = 0; lane_index < lanes.size(); ++lane_index) {
    if (lane_send_strips[lane_index] != nullptr && replace_lane_sends[lane_index]) {
      lane_send_strips[lane_index]->commit_sends(prepared_lane_sends[lane_index]);
    }
  }
  for (size_t i = binding_count; i > 0; --i) {
    if (drop[i - 1]) remove_sidechain_binding(i - 1);
  }
  // Keep a live bus smoother across a positional reorder. Only buses that no
  // longer exist are retired; the slot is keyed by bus_id and resolves its
  // current render index on each advance.
  for (InsertAutoSlot& slot : insert_auto_slots_) {
    if (!slot.assigned || !slot.is_bus) continue;
    if (configured_bus_index(slot.bus_id) < 0) {
      slot.active = false;
      slot.assigned = false;
    }
  }
  for (size_t index = 0; index < bus_configs_.size(); ++index) {
    BusState& state = bus_states_[index];
    if (!kept[index]) {
      if (source[index] >= 0) {
        adopt_bus_state((*staging)[static_cast<size_t>(source[index])], state);
      }
    }
    state.bus_id = bus_configs_[index].bus_id;
    if (source[index] >= 0) {
      // A retained bus may have active gain/trim/width ramps. Keep their
      // current and target values through a same-order republish or an
      // identity reorder; only a changed TrackBusConfig gain is a new target.
      if (previous_gain_db[static_cast<size_t>(source[index])] != bus_configs_[index].gain_db) {
        state.gain.set_target(db_to_linear(bus_configs_[index].gain_db));
      }
    } else {
      state.gain.prepare(sample_rate_, 5.0f);
      state.gain.reset(db_to_linear(bus_configs_[index].gain_db));
      state.input_trim_gain.prepare(sample_rate_, 5.0f);
      if (max_block_size_ > 0) {
        state.width.prepare(sample_rate_, max_block_size_);
      }
    }
    if (!state.bus) {
      state.bus = std::move(staged_buses[index]);
    }
    // Dedicated EQ state is per output plane. A retained bus can change width
    // under the same identity, so a plane skipped while the bus was narrower
    // must not resume with its old filter history when that plane returns.
    // Reorders keep the history because the bus state follows its id.
    if (source[index] >= 0 &&
        previous_layout[static_cast<size_t>(source[index])] != bus_configs_[index].layout) {
      state.eq.reset();
    }
    state.bus->set_channel_layout(bus_configs_[index].layout);
    // Existing FxBus objects were prepared before this transaction. Newly
    // declared slots were prepared into staged_buses[] above.
  }
  refresh_bus_graph();
  commit_pdc_updates(prepared_pdc);
  return true;
}

void TrackMixerRuntime::stage_bus_state(const BusState& from, BusState& to) {
  to.bus_id = from.bus_id;
  to.gain = from.gain;
  to.input_trim_gain = from.input_trim_gain;
  to.width.copy_state_from(from.width);
  to.polarity_left.store(from.polarity_left.load(std::memory_order_relaxed),
                         std::memory_order_relaxed);
  to.polarity_right.store(from.polarity_right.load(std::memory_order_relaxed),
                          std::memory_order_relaxed);
  to.panner.copy_state_from(from.panner);
  // The state belongs to the bus identity, so a positional move must carry it
  // across with the rest of the retained bus state. set_buses() resets it after
  // this transfer only when the destination layout changes.
  to.eq = from.eq;
  to.eq_enabled.store(from.eq_enabled.load(std::memory_order_relaxed), std::memory_order_relaxed);
  to.eq_active.store(from.eq_active.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

void TrackMixerRuntime::adopt_bus_state(BusState& from, BusState& to) noexcept {
  to.bus_id = from.bus_id;
  to.gain = from.gain;
  to.input_trim_gain = from.input_trim_gain;
  to.width.copy_state_from(from.width);
  to.polarity_left.store(from.polarity_left.load(std::memory_order_relaxed),
                         std::memory_order_relaxed);
  to.polarity_right.store(from.polarity_right.load(std::memory_order_relaxed),
                          std::memory_order_relaxed);
  to.panner.copy_state_from(from.panner);
  to.eq = std::move(from.eq);
  to.eq_enabled.store(from.eq_enabled.load(std::memory_order_relaxed), std::memory_order_relaxed);
  to.eq_active.store(from.eq_active.load(std::memory_order_relaxed), std::memory_order_relaxed);
  to.bus = std::move(from.bus);
  to.spec = std::move(from.spec);
}

void TrackMixerRuntime::retire_bus_state(BusState& state) {
  state.bus_id = 0;
  state.gain.reset(1.0f);
  state.input_trim_gain.reset(1.0f);
  state.width.set_width(1.0f);
  state.width.reset();
  state.polarity_left.store(1.0f, std::memory_order_relaxed);
  state.polarity_right.store(1.0f, std::memory_order_relaxed);
  state.spec = mixing::api::Bus{};
  apply_bus_pan(state, state.spec);
  state.panner.reset();
  state.eq.clear();
  state.eq.reset();
  state.eq_enabled.store(true, std::memory_order_relaxed);
  state.eq_active.store(false, std::memory_order_relaxed);
  state.bus.reset();
}

void TrackMixerRuntime::apply_bus_pan(BusState& state, const mixing::api::Bus& bus) noexcept {
  state.panner.set_pan_mode(to_pan_mode(bus.pan_mode));
  state.panner.set_pan_law(mixing::pan_law_from_index(bus.pan_law));
  state.panner.set_pan(bus.pan);
  state.panner.set_dual_pan(bus.dual_pan_left, bus.dual_pan_right);
}

void TrackMixerRuntime::refresh_bus_eq_active(BusState& state) noexcept {
  bool active = false;
  for (size_t band = 0; band < mastering::eq::ParametricEq::kMaxBands; ++band) {
    active = active || state.eq.band(band).enabled;
  }
  state.eq_active.store(active, std::memory_order_relaxed);
}

bool TrackMixerRuntime::set_bus_strip(uint32_t bus_id, const mixing::api::Bus& bus,
                                      BusAdmission admit, void* admit_context) {
  last_refusal_ = MixerRefusal::kArgument;
  const int found = configured_bus_index(bus_id);
  if (found < 0) return false;
  const size_t bus_index = static_cast<size_t>(found);
  BusState* state = &bus_states_[bus_index];
  // Validate everything before applying anything. The width check reads the
  // engine's own layout, not the spec's, which may be omitted.
  if (!std::isfinite(bus.pan) || !std::isfinite(bus.dual_pan_left) ||
      !std::isfinite(bus.dual_pan_right) ||
      (layout_wider_than_stereo(bus_configs_[bus_index].layout) &&
       !bus_stereo_image_is_default(bus)) ||
      !strip_eq_acceptable(bus.eq, sample_rate_)) {
    return false;
  }
  std::unique_ptr<mixing::FxBus> fx;
  const bool rebuild = !strip_inserts_equal(state->spec.inserts, bus.inserts,
                                            /*canonical_track_order=*/false);
  if (rebuild) {
    fx = std::make_unique<mixing::FxBus>(static_cast<int>(kMaxTrackLanes));
    fx->set_channel_layout(bus_configs_[bus_index].layout);
    try {
      for (const auto& insert : bus.inserts) {
        auto processor = mastering::api::make_insert(
            insert.processor_name, insert.params_json, nullptr,
            resource::kDefaultProjectImportResourceLimits, sample_rate_);
        if (!processor) return false;
        const bool spo = mastering::api::channel_policy(insert.processor_name) ==
                         mastering::api::ChannelPolicy::StereoPairOnly;
        fx->add_insert(std::move(processor), spo);
      }
    } catch (const std::bad_alloc&) {
      return refuse_out_of_memory();
    } catch (...) {
      return false;
    }
    if (max_block_size_ > 0) {
      fx->prepare(sample_rate_, max_block_size_);
    }
    if (admit != nullptr && !admit(admit_context, bus_id, *fx)) return refuse_from_state();
  }
  mixing::api::Bus next_spec;
  try {
    next_spec = bus;
  } catch (const std::bad_alloc&) {
    return refuse_out_of_memory();
  } catch (...) {
    return false;
  }
  // A new chain's latency and its surviving keys are planned before anything is applied.
  SidechainTable next_sidechains{};
  PreparedPdc prepared;
  bool has_pdc = false;
  if (rebuild) {
    next_sidechains =
        without_inserts_from(sidechains_, SidechainTargetKind::Bus, bus_id, fx->num_inserts());
    if (const std::vector<TrackLaneConfig>* lanes = lanes_.control_current().get()) {
      BusGraphView view = current_bus_graph_view();
      view.bus[bus_index] = fx.get();
      view.latency_q8[bus_index] = fx->latency_samples_q8();
      build_routes(*view.buses, view.skip_binding, &view.routes, &next_sidechains);
      PdcPlan plan;
      if (!plan_pdc(*lanes, view, &plan, nullptr, nullptr, false, &next_sidechains)) {
        return refuse_from_state();
      }
      if (!prepare_identity_pdc(plan, &prepared)) return refuse_pdc(prepared);
      has_pdc = true;
    }
  }
  state->input_trim_gain.set_target(db_to_linear(bus.input_trim_db));
  state->width.set_width(bus.width);
  state->polarity_left.store(bus.polarity_invert_left ? -1.0f : 1.0f, std::memory_order_relaxed);
  state->polarity_right.store(bus.polarity_invert_right ? -1.0f : 1.0f, std::memory_order_relaxed);
  apply_bus_pan(*state, bus);
  mixing::apply_eq(state->eq, state->eq_enabled, bus.eq, &state->spec.eq);
  refresh_bus_eq_active(*state);
  state->spec = std::move(next_spec);
  if (!rebuild) return true;
  for (InsertAutoSlot& slot : insert_auto_slots_) {
    if (slot.assigned && slot.is_bus && slot.bus_id == bus_id) {
      slot.active = false;
      slot.assigned = false;
    }
  }
  state->bus = std::move(fx);
  // Keys stay on (bus, insert index); an index the new chain lacks is dropped.
  sidechains_ = next_sidechains;
  publish_sidechains();
  refresh_bus_graph();
  // The chain's latency joins the mixer's end-to-end PDC, staged above.
  if (has_pdc) commit_pdc_updates(prepared);
  return true;
}

bool TrackMixerRuntime::bus_config_valid(const std::vector<TrackBusConfig>& buses) const noexcept {
  if (buses.size() > kMaxBusLanes) return false;
  for (size_t i = 0; i < buses.size(); ++i) {
    if (buses[i].bus_id == 0 || !gain_db_in_range(buses[i].gain_db)) {
      return false;
    }
    for (size_t j = i + 1; j < buses.size(); ++j) {
      if (buses[i].bus_id == buses[j].bus_id) return false;
    }
    // Same value rules as a lane send; targets are checked by validate_bus_graph.
    const std::vector<TrackLaneConfig::Send>& sends = buses[i].sends;
    if (sends.size() > mixing::ChannelStrip::kMaxSends) return false;
    for (size_t send_index = 0; send_index < sends.size(); ++send_index) {
      const TrackLaneConfig::Send& send = sends[send_index];
      if (send.bus_id == 0 || !gain_db_in_range(send.level_db)) {
        return false;
      }
      for (size_t other = send_index + 1; other < sends.size(); ++other) {
        if (send.bus_id == sends[other].bus_id) return false;
      }
    }
  }
  return true;
}

bool TrackMixerRuntime::bus_insert_processor_name(size_t bus_index, unsigned int insert_index,
                                                  std::string* out_name) const noexcept {
  if (out_name == nullptr || bus_index >= bus_states_.size() ||
      bus_states_[bus_index].bus == nullptr) {
    return false;
  }
  if (insert_index >= bus_states_[bus_index].spec.inserts.size()) return false;
  *out_name = bus_states_[bus_index].spec.inserts[insert_index].processor_name;
  return true;
}

bool TrackMixerRuntime::bus_insert_processor_name_by_id(uint32_t bus_id, unsigned int insert_index,
                                                        std::string* out_name) const noexcept {
  const int bus_index = configured_bus_index(bus_id);
  return bus_index >= 0 &&
         bus_insert_processor_name(static_cast<size_t>(bus_index), insert_index, out_name);
}

size_t TrackMixerRuntime::bus_insert_count(uint32_t bus_id) const noexcept {
  const int index = configured_bus_index(bus_id);
  if (index < 0 || bus_states_[static_cast<size_t>(index)].bus == nullptr) return 0;
  return bus_states_[static_cast<size_t>(index)].bus->num_inserts();
}

const rt::ProcessorBase* TrackMixerRuntime::bus_insert_processor(
    uint32_t bus_id, unsigned int insert_index) const noexcept {
  if (insert_index >= bus_insert_count(bus_id)) return nullptr;
  return bus_states_[static_cast<size_t>(configured_bus_index(bus_id))].bus->insert_processor(
      insert_index);
}

bool TrackMixerRuntime::bus_insert_holds(uint32_t bus_id, unsigned int insert_index,
                                         const InsertProcessorLayout& processor) const noexcept {
  const int index = configured_bus_index(bus_id);
  if (index < 0) return false;
  const mixing::FxBus* bus = bus_states_[static_cast<size_t>(index)].bus.get();
  return bus != nullptr &&
         insert_processor_layout(bus->insert_processor(insert_index)) == processor;
}

TrackMixerRuntime::BusState* TrackMixerRuntime::bus_state_for(uint32_t bus_id) noexcept {
  for (BusState& state : bus_states_) {
    if (state.bus_id == bus_id) return &state;
  }
  return nullptr;
}

const TrackMixerRuntime::BusState* TrackMixerRuntime::bus_state_for(
    uint32_t bus_id) const noexcept {
  for (const BusState& state : bus_states_) {
    if (state.bus_id == bus_id) return &state;
  }
  return nullptr;
}

int TrackMixerRuntime::configured_bus_index(uint32_t bus_id) const noexcept {
  if (bus_id == 0) return -1;
  for (size_t index = 0; index < bus_configs_.size(); ++index) {
    if (bus_configs_[index].bus_id == bus_id) return static_cast<int>(index);
  }
  return -1;
}

TrackMixerRuntime::BusState* TrackMixerRuntime::pannable_bus_state_for(uint32_t bus_id) noexcept {
  const int index = configured_bus_index(bus_id);
  if (index < 0 || layout_wider_than_stereo(bus_configs_[static_cast<size_t>(index)].layout)) {
    return nullptr;
  }
  return &bus_states_[static_cast<size_t>(index)];
}

}  // namespace sonare::engine
