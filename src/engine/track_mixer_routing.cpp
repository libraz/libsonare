#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <vector>

#include "engine/track_mixer.h"
#include "mixing/tail_planner.h"
#include "rt/tail_budget.h"

namespace sonare::engine {

void TrackMixerRuntime::flush_pdc_delays() noexcept {
  for (mixing::AlignmentDelay& delay : lane_pdc_delays_) {
    delay.reset();
  }
  for (mixing::AlignmentDelay& delay : lane_pre_send_pdc_delays_) {
    delay.reset();
  }
  for (mixing::AlignmentDelay& delay : lane_in_pdc_delays_) {
    delay.reset();
  }
  for (mixing::AlignmentDelay& delay : bus_pdc_delays_) {
    delay.reset();
  }
  for (mixing::AlignmentDelay& delay : bus_edge_delays_) {
    delay.reset();
  }
  for (mixing::AlignmentDelay& delay : key_edge_delays_) {
    delay.reset();
  }
  direct_pdc_delay_.reset();
  master_pdc_delay_.reset();
}

bool TrackMixerRuntime::prepare_master_strip_update(const mixing::ChannelStrip* candidate,
                                                    size_t next_insert_count,
                                                    PreparedMasterStripUpdate* out) const noexcept {
  if (out == nullptr) return false;
  *out = PreparedMasterStripUpdate{};
  const size_t actual_insert_count =
      candidate == nullptr ? 0 : candidate->num_pre_inserts() + candidate->num_post_inserts();
  out->candidate = candidate;
  out->next_insert_count = std::min(next_insert_count, actual_insert_count);
  out->next_sidechains =
      without_inserts_from(sidechains_, SidechainTargetKind::Master, 0, out->next_insert_count);

  const std::vector<TrackLaneConfig>* lanes = lanes_.control_current().get();
  if (lanes == nullptr) return true;

  BusGraphView view = current_bus_graph_view();
  view.skip_binding.fill(false);
  build_routes(*view.buses, view.skip_binding, &view.routes, &out->next_sidechains);
  PdcPlan plan;
  if (!plan_pdc(*lanes, view, &plan, nullptr, candidate, true, &out->next_sidechains)) {
    return false;
  }
  std::array<int, kMaxTrackLanes> sources{};
  std::array<bool, kMaxTrackLanes> reset{};
  for (size_t i = 0; i < kMaxTrackLanes; ++i) sources[i] = static_cast<int>(i);
  if (!prepare_pdc_updates(plan, sources, reset, &out->pdc)) return false;
  out->has_pdc = true;
  return true;
}

void TrackMixerRuntime::commit_master_strip_update(const mixing::ChannelStrip* durable,
                                                   PreparedMasterStripUpdate& update) noexcept {
  if (update.has_pdc) commit_pdc_updates(update.pdc);
  master_strip_ = durable;
  master_insert_count_ = update.next_insert_count;
  sidechains_ = update.next_sidechains;
  publish_sidechains();
  refresh_bus_graph();
  update.candidate = nullptr;
  update.has_pdc = false;
}

uint64_t TrackMixerRuntime::pdc_storage_generation() const noexcept {
  uint64_t total = master_pdc_delay_.storage_generation();
  for (const LaneState& lane : lane_states_) {
    total += lane.clip_pdc_delay.storage_generation();
  }
  for (const mixing::AlignmentDelay& delay : lane_pdc_delays_) {
    total += delay.storage_generation();
  }
  for (const mixing::AlignmentDelay& delay : lane_pre_send_pdc_delays_) {
    total += delay.storage_generation();
  }
  for (const mixing::AlignmentDelay& delay : lane_in_pdc_delays_) {
    total += delay.storage_generation();
  }
  for (const mixing::AlignmentDelay& delay : bus_pdc_delays_) {
    total += delay.storage_generation();
  }
  for (const mixing::AlignmentDelay& delay : bus_edge_delays_) {
    total += delay.storage_generation();
  }
  for (const mixing::AlignmentDelay& delay : key_edge_delays_) {
    total += delay.storage_generation();
  }
  total += direct_pdc_delay_.storage_generation();
  return total;
}

bool TrackMixerRuntime::validate_bus_graph(const std::vector<TrackBusConfig>& buses,
                                           const KeyEdge* key_edges, size_t key_edge_count,
                                           std::array<size_t, kMaxBusLanes>* order) noexcept {
  const size_t count = buses.size();
  if (count > kMaxBusLanes || order == nullptr) return false;
  const auto index_of = [&buses](uint32_t bus_id) -> int {
    for (size_t i = 0; i < buses.size(); ++i) {
      if (buses[i].bus_id == bus_id) return static_cast<int>(i);
    }
    return -1;
  };
  std::array<std::array<int, kMaxBusLanes>, kMaxBusLanes> edges{};
  std::array<int, kMaxBusLanes> indegree{};
  const auto add_edge = [&](int from, int to) {
    if (from < 0 || to < 0 || from == to) return false;
    ++edges[static_cast<size_t>(from)][static_cast<size_t>(to)];
    ++indegree[static_cast<size_t>(to)];
    return true;
  };
  for (size_t i = 0; i < count; ++i) {
    const int from = static_cast<int>(i);
    if (buses[i].output_bus_id != 0 && !add_edge(from, index_of(buses[i].output_bus_id))) {
      return false;
    }
    for (const TrackLaneConfig::Send& send : buses[i].sends) {
      if (!add_edge(from, index_of(send.bus_id))) return false;
    }
  }
  for (size_t i = 0; i < key_edge_count; ++i) {
    if (!add_edge(index_of(key_edges[i].source_bus), index_of(key_edges[i].target_bus))) {
      return false;
    }
  }
  std::array<bool, kMaxBusLanes> placed{};
  for (size_t position = 0; position < count; ++position) {
    size_t next = count;
    for (size_t i = 0; i < count && next == count; ++i) {
      if (!placed[i] && indegree[i] == 0) next = i;
    }
    if (next == count) return false;
    placed[next] = true;
    (*order)[position] = next;
    for (size_t j = 0; j < count; ++j) indegree[j] -= edges[next][j];
  }
  return true;
}

size_t TrackMixerRuntime::collect_key_edges(std::array<KeyEdge, kMaxSidechainBindings>& out,
                                            size_t skip) const noexcept {
  size_t edges = 0;
  const size_t count = sidechains_.count;
  for (size_t i = 0; i < count; ++i) {
    const SidechainBinding& binding = sidechains_.bindings[i];
    if (i == skip || binding.target_kind != static_cast<uint8_t>(SidechainTargetKind::Bus) ||
        binding.source_kind != static_cast<uint8_t>(SidechainSourceKind::Bus)) {
      continue;
    }
    out[edges++] = KeyEdge{binding.source_id, binding.target_id};
  }
  return edges;
}

void TrackMixerRuntime::build_routes(const std::vector<TrackBusConfig>& buses,
                                     const std::array<bool, kMaxSidechainBindings>& skip,
                                     std::array<BusRoute, kMaxBusLanes>* routes,
                                     const SidechainTable* sidechains) const noexcept {
  const auto index_of = [&buses](uint32_t bus_id) -> int {
    if (bus_id == 0) return -1;
    for (size_t i = 0; i < buses.size(); ++i) {
      if (buses[i].bus_id == bus_id) return static_cast<int>(i);
    }
    return -1;
  };
  const SidechainTable& table = sidechains != nullptr ? *sidechains : sidechains_;
  const size_t binding_count = table.count;
  *routes = {};
  for (size_t bus_index = 0; bus_index < buses.size(); ++bus_index) {
    const TrackBusConfig& config = buses[bus_index];
    BusRoute& route = (*routes)[bus_index];
    route.output_index = index_of(config.output_bus_id);
    route.send_count = config.sends.size();
    for (size_t send_index = 0; send_index < route.send_count; ++send_index) {
      route.send_index[send_index] = index_of(config.sends[send_index].bus_id);
      route.any_pre_send =
          route.any_pre_send || config.sends[send_index].timing == mixing::SendTiming::PreFader;
    }
    for (size_t i = 0; i < binding_count; ++i) {
      const SidechainBinding& binding = table.bindings[i];
      route.key_source =
          route.key_source ||
          (!skip[i] && binding.source_kind == static_cast<uint8_t>(SidechainSourceKind::Bus) &&
           binding.source_id == config.bus_id);
    }
  }
}

void TrackMixerRuntime::refresh_bus_graph() noexcept {
  std::array<KeyEdge, kMaxSidechainBindings> edges{};
  const size_t edge_count = collect_key_edges(edges, kMaxSidechainBindings);
  std::array<size_t, kMaxBusLanes> order{};
  // Every writer validated this state first, so the order always resolves.
  if (!validate_bus_graph(bus_configs_, edges.data(), edge_count, &order)) return;
  bus_order_ = order;
  build_routes(bus_configs_, {}, &bus_routes_);
}

bool TrackMixerRuntime::recompute_lane_pdc(const std::vector<TrackLaneConfig>& lanes) noexcept {
  PdcPlan plan;
  if (!plan_pdc(lanes, current_bus_graph_view(), &plan)) return false;
  return apply_pdc(plan);
}

TrackMixerRuntime::BusGraphView TrackMixerRuntime::current_bus_graph_view() const noexcept {
  BusGraphView view;
  view.buses = &bus_configs_;
  view.order = bus_order_;
  view.routes = bus_routes_;
  for (size_t bus_index = 0; bus_index < bus_configs_.size(); ++bus_index) {
    const mixing::FxBus* bus = bus_states_[bus_index].bus.get();
    view.bus[bus_index] = bus;
    view.latency_q8[bus_index] = bus != nullptr ? bus->latency_samples_q8() : 0;
  }
  return view;
}

bool TrackMixerRuntime::plan_pdc(
    const std::vector<TrackLaneConfig>& lanes, const BusGraphView& view, PdcPlan* plan,
    const std::array<mixing::ChannelStrip*, kMaxTrackLanes>* candidate_strips,
    const mixing::ChannelStrip* candidate_master_strip, bool use_candidate_master_strip,
    const SidechainTable* candidate_sidechains) const noexcept {
  const auto strip_at = [this, candidate_strips](size_t lane_index) {
    return candidate_strips != nullptr ? (*candidate_strips)[lane_index]
                                       : lane_states_[lane_index].strip;
  };
  const mixing::ChannelStrip* master_strip =
      use_candidate_master_strip ? candidate_master_strip : master_strip_;
  const SidechainTable& sidechains =
      candidate_sidechains != nullptr ? *candidate_sidechains : sidechains_;
  const auto checked_add = [](int base, int offset, int* result) noexcept {
    const int64_t sum = static_cast<int64_t>(base) + static_cast<int64_t>(offset);
    if (sum < std::numeric_limits<int>::min() || sum > std::numeric_limits<int>::max()) {
      return false;
    }
    *result = static_cast<int>(sum);
    return true;
  };
  *plan = PdcPlan{};
  const size_t lane_count = lanes.size();
  std::array<int, kMaxTrackLanes> strip_q8{};
  for (size_t lane_index = 0; lane_index < lane_count; ++lane_index) {
    const mixing::ChannelStrip* strip = strip_at(lane_index);
    strip_q8[lane_index] = strip != nullptr ? strip->latency_samples_q8() : 0;
  }
  const auto lane_of = [&lanes](uint32_t track_id) -> int {
    for (size_t i = 0; i < lanes.size(); ++i) {
      if (lanes[i].track_id == track_id) return static_cast<int>(i);
    }
    return -1;
  };
  const size_t binding_count = sidechains.count;
  // Lane-key edges (source lane -> destination lane), indexed like the bindings.
  std::array<int, kMaxSidechainBindings> edge_source{};
  std::array<int, kMaxSidechainBindings> edge_target{};
  edge_source.fill(-1);
  edge_target.fill(-1);
  std::array<int, kMaxTrackLanes> indegree{};
  for (size_t i = 0; i < binding_count; ++i) {
    const SidechainBinding& binding = sidechains.bindings[i];
    if (view.skip_binding[i] ||
        binding.target_kind != static_cast<uint8_t>(SidechainTargetKind::Lane) ||
        binding.source_kind != static_cast<uint8_t>(SidechainSourceKind::Track)) {
      continue;
    }
    const int source = lane_of(binding.source_id);
    const int target = lane_of(binding.target_id);
    if (source < 0 || target < 0 || source == target) continue;
    edge_source[i] = source;
    edge_target[i] = target;
    ++indegree[static_cast<size_t>(target)];
  }
  // Stable Kahn sort (lowest index first), so an edge-free list keeps its order;
  // p(D) = max(0, max over edges (p(S) + s(S))) follows along it.
  std::array<int, kMaxTrackLanes> pre_q8{};
  std::array<bool, kMaxTrackLanes> placed{};
  for (size_t position = 0; position < lane_count; ++position) {
    size_t next = lane_count;
    for (size_t i = 0; i < lane_count && next == lane_count; ++i) {
      if (!placed[i] && indegree[i] == 0) next = i;
    }
    if (next == lane_count) return false;
    placed[next] = true;
    plan->lane_order[position] = static_cast<uint8_t>(next);
    for (size_t i = 0; i < binding_count; ++i) {
      if (edge_source[i] != static_cast<int>(next)) continue;
      const size_t target = static_cast<size_t>(edge_target[i]);
      pre_q8[target] = std::max(pre_q8[target], pre_q8[next] + strip_q8[next]);
      --indegree[target];
    }
  }
  plan->lane_order_count = lane_count;
  // Lane stage: the latest strip output. Every lane leaves process_lane_strip
  // at this offset, and every path the lane's audio then takes is tapped from
  // there -- the direct master sum, the output-bus routing and the post-fader
  // sends all read the aligned lane buffer, while a pre-fader send reads the
  // strip's earlier tap through lane_pre_send_pdc_delays_ below. One lane
  // timebase, three exits.
  int max_strip_q8 = 0;
  for (size_t lane_index = 0; lane_index < lane_count; ++lane_index) {
    max_strip_q8 = std::max(max_strip_q8, pre_q8[lane_index] + strip_q8[lane_index]);
  }
  for (size_t lane_index = 0; lane_index < lane_count; ++lane_index) {
    const mixing::ChannelStrip* strip = strip_at(lane_index);
    plan->lane_in_q8[lane_index] = pre_q8[lane_index];
    plan->lane_q8[lane_index] = max_strip_q8 - pre_q8[lane_index] - strip_q8[lane_index];
    // Pre-fader send stage: the same target offset, measured from the earlier
    // tap. A strip's pre-fader latency is what it has accrued by the time the
    // pre tap is taken (its pre inserts), so this bank carries the
    // rest of the widest strip latency -- including the strip's own
    // post-insert chain, which the lane's output passes through and the pre
    // tap does not.
    plan->lane_pre_q8[lane_index] = max_strip_q8 - pre_q8[lane_index] -
                                    (strip != nullptr ? strip->pre_fader_latency_samples_q8() : 0);
  }
  // Direct clips and unmatched sources join the lane-stage timebase of lane_pdc_delays_.
  plan->direct_q8 = max_strip_q8;

  // Bus stage along the order: in(b) is the latest arrival over the lane stage
  // and every incoming edge (output, send, bus-sourced key), out(b) adds the
  // bus's own chain latency, and every edge carries the difference.
  const std::vector<TrackBusConfig>& buses = *view.buses;
  const auto index_of = [&buses](uint32_t bus_id) -> int {
    for (size_t i = 0; i < buses.size(); ++i) {
      if (buses[i].bus_id == bus_id) return static_cast<int>(i);
    }
    return -1;
  };
  const size_t bus_count = buses.size();
  std::array<int, kMaxBusLanes> bus_in{};
  std::array<int, kMaxBusLanes> bus_out{};
  bus_in.fill(max_strip_q8);
  int master_in = max_strip_q8;
  const auto live_bus_key = [&](size_t i, uint32_t source_bus) {
    const SidechainBinding& binding = sidechains.bindings[i];
    return !view.skip_binding[i] &&
           binding.source_kind == static_cast<uint8_t>(SidechainSourceKind::Bus) &&
           binding.source_id == source_bus;
  };
  for (size_t order_index = 0; order_index < bus_count; ++order_index) {
    const size_t bus_index = view.order[order_index];
    bus_out[bus_index] = bus_in[bus_index] + view.latency_q8[bus_index];
    const int out = bus_out[bus_index];
    const BusRoute& route = view.routes[bus_index];
    int& output_in =
        route.output_index < 0 ? master_in : bus_in[static_cast<size_t>(route.output_index)];
    output_in = std::max(output_in, out);
    for (size_t send_index = 0; send_index < route.send_count; ++send_index) {
      if (route.send_index[send_index] < 0) continue;
      int& in = bus_in[static_cast<size_t>(route.send_index[send_index])];
      in = std::max(in, out);
    }
    for (size_t i = 0; i < binding_count; ++i) {
      if (!live_bus_key(i, buses[bus_index].bus_id)) continue;
      const SidechainBinding& binding = sidechains.bindings[i];
      const auto target_kind = binding.target_kind;
      if (target_kind == static_cast<uint8_t>(SidechainTargetKind::Master)) {
        master_in = std::max(master_in, out);
      } else if (target_kind == static_cast<uint8_t>(SidechainTargetKind::Bus)) {
        const int target = index_of(binding.target_id);
        if (target >= 0) {
          int& in = bus_in[static_cast<size_t>(target)];
          in = std::max(in, out);
        }
      }
    }
  }
  const auto input_of = [&](int index) {
    return index < 0 ? master_in : bus_in[static_cast<size_t>(index)];
  };
  for (size_t bus_index = 0; bus_index < bus_count; ++bus_index) {
    plan->bus_in_q8[bus_index] = bus_in[bus_index] - max_strip_q8;
    const BusRoute& route = view.routes[bus_index];
    plan->edge_q8[bus_index * kBusEdgesPerBus] = input_of(route.output_index) - bus_out[bus_index];
    for (size_t send_index = 0; send_index < route.send_count; ++send_index) {
      if (route.send_index[send_index] < 0) continue;
      plan->edge_q8[bus_index * kBusEdgesPerBus + 1 + send_index] =
          input_of(route.send_index[send_index]) - bus_out[bus_index];
    }
  }
  plan->master_q8 = master_in - max_strip_q8;
  // Key edges: in(target) - out(source); a track key leaves at p(S) + s(S). The target input
  // includes the latency of earlier target inserts; manual channel delay is excluded.
  for (size_t i = 0; i < binding_count; ++i) {
    if (view.skip_binding[i]) continue;
    const SidechainBinding& binding = sidechains.bindings[i];
    const auto target_kind = binding.target_kind;
    int source_out = 0;
    if (binding.source_kind == static_cast<uint8_t>(SidechainSourceKind::Bus)) {
      const int source = index_of(binding.source_id);
      if (source < 0) continue;
      source_out = bus_out[static_cast<size_t>(source)];
    } else {
      const int source = lane_of(binding.source_id);
      if (source < 0) continue;
      source_out = pre_q8[static_cast<size_t>(source)] + strip_q8[static_cast<size_t>(source)];
    }
    int target_in = 0;
    if (target_kind == static_cast<uint8_t>(SidechainTargetKind::Lane)) {
      if (edge_target[i] < 0) continue;
      const size_t target = static_cast<size_t>(edge_target[i]);
      target_in = pre_q8[target];
      const mixing::ChannelStrip* strip = strip_at(target);
      // A key on an insert the new strip lacks is pruned by the caller; it times nothing.
      if (strip != nullptr &&
          binding.insert_index >= strip->num_pre_inserts() + strip->num_post_inserts()) {
        continue;
      }
      if (strip != nullptr) {
        const auto prefix = strip->insert_input_latency_samples_q8(binding.insert_index);
        if (!prefix.has_value() || !checked_add(target_in, *prefix, &target_in)) return false;
      }
    } else if (target_kind == static_cast<uint8_t>(SidechainTargetKind::Bus)) {
      const int target = index_of(binding.target_id);
      if (target < 0) continue;
      target_in = input_of(target);
      const mixing::FxBus* bus = view.bus[static_cast<size_t>(target)];
      if (bus != nullptr) {
        const auto prefix = bus->insert_input_latency_samples_q8(binding.insert_index);
        if (!prefix.has_value() || !checked_add(target_in, *prefix, &target_in)) return false;
      }
    } else {
      target_in = master_in;
      if (master_strip != nullptr) {
        const auto prefix = master_strip->insert_input_latency_samples_q8(binding.insert_index);
        if (!prefix.has_value() || !checked_add(target_in, *prefix, &target_in)) return false;
      }
    }
    const int64_t key = static_cast<int64_t>(target_in) - static_cast<int64_t>(source_out);
    if (key < std::numeric_limits<int>::min() || key > std::numeric_limits<int>::max()) {
      return false;
    }
    plan->key_q8[binding.key_slot] = static_cast<int>(key);
  }
  // What the engine advertises to the host: the master input's arrival.
  plan->latency_q8 = master_in;

  // A delay line past its ceiling would clamp silently and misalign the mix,
  // so such a configuration is refused instead.
  constexpr int kCapQ8 = mixing::kMaxAlignmentDelaySamples << 8;
  const auto within = [](const auto& values) {
    return std::all_of(values.begin(), values.end(), [](int v) { return v >= 0 && v <= kCapQ8; });
  };
  return within(plan->lane_q8) && within(plan->lane_pre_q8) && within(plan->lane_in_q8) &&
         within(plan->bus_in_q8) && within(plan->edge_q8) && within(plan->key_q8) &&
         plan->direct_q8 >= 0 && plan->direct_q8 <= kCapQ8 && plan->master_q8 >= 0 &&
         plan->master_q8 <= kCapQ8 && plan->latency_q8 >= 0 && plan->latency_q8 <= kCapQ8;
}

void TrackMixerRuntime::make_lane_pdc_sources(
    const std::vector<TrackLaneConfig>& lanes, std::array<int, kMaxTrackLanes>* sources,
    std::array<bool, kMaxTrackLanes>* reset) const noexcept {
  sources->fill(-1);
  reset->fill(true);
  std::array<bool, kMaxTrackLanes> used{};
  const size_t previous_active_count =
      std::min(applied_lane_count_, static_cast<size_t>(kMaxTrackLanes));

  // Reserve every matching identity first.  The second pass must never let a
  // newly inserted lane steal a positional bank that a later surviving lane
  // still owns.
  for (size_t lane_index = 0; lane_index < lanes.size(); ++lane_index) {
    for (size_t previous = 0; previous < lane_states_.size(); ++previous) {
      if (!used[previous] && lane_states_[previous].track_id == lanes[lane_index].track_id) {
        (*sources)[lane_index] = static_cast<int>(previous);
        (*reset)[lane_index] = previous >= previous_active_count;
        used[previous] = true;
        break;
      }
    }
  }
  for (size_t lane_index = 0; lane_index < lanes.size(); ++lane_index) {
    if ((*sources)[lane_index] >= 0) continue;
    for (size_t previous = 0; previous < lane_states_.size(); ++previous) {
      if (!used[previous]) {
        (*sources)[lane_index] = static_cast<int>(previous);
        used[previous] = true;
        break;
      }
    }
  }
  for (size_t lane_index = lanes.size(); lane_index < kMaxTrackLanes; ++lane_index) {
    for (size_t previous = 0; previous < lane_states_.size(); ++previous) {
      if (!used[previous]) {
        (*sources)[lane_index] = static_cast<int>(previous);
        used[previous] = true;
        break;
      }
    }
  }
}

bool TrackMixerRuntime::prepare_pdc_updates(
    const PdcPlan& plan, const std::array<int, kMaxTrackLanes>& sources,
    const std::array<bool, kMaxTrackLanes>& reset, PreparedPdc* prepared,
    const std::array<int, kMaxBusLanes>* bus_sources) const noexcept {
  if (prepared == nullptr) return false;
  prepared->plan = plan;
  prepared->lane_source = sources;
  prepared->lane_reset = reset;
  try {
    for (size_t i = 0; i < kMaxTrackLanes; ++i) {
      const size_t source = sources[i] >= 0 ? static_cast<size_t>(sources[i]) : i;
      if (!lane_pdc_delays_[source].prepare_update(
              plan.lane_q8[i], mixing::FractionalDelayMode::Lagrange3, prepared->lane_updates[i]) ||
          !lane_pre_send_pdc_delays_[source].prepare_update(plan.lane_pre_q8[i],
                                                            mixing::FractionalDelayMode::Lagrange3,
                                                            prepared->lane_pre_send_updates[i]) ||
          !lane_in_pdc_delays_[source].prepare_update(plan.lane_in_q8[i],
                                                      mixing::FractionalDelayMode::Lagrange3,
                                                      prepared->lane_in_updates[i])) {
        return false;
      }
    }
    for (size_t i = 0; i < kMaxBusLanes; ++i) {
      const size_t source = bus_sources != nullptr && (*bus_sources)[i] >= 0
                                ? static_cast<size_t>((*bus_sources)[i])
                                : i;
      if (source >= kMaxBusLanes || !bus_pdc_delays_[source].prepare_update(
                                        plan.bus_in_q8[i], mixing::FractionalDelayMode::Lagrange3,
                                        prepared->bus_updates[i])) {
        return false;
      }
    }
    for (size_t i = 0; i < bus_edge_delays_.size(); ++i) {
      const size_t bus = i / kBusEdgesPerBus;
      const size_t edge = i % kBusEdgesPerBus;
      const size_t source_bus = bus_sources != nullptr && (*bus_sources)[bus] >= 0
                                    ? static_cast<size_t>((*bus_sources)[bus])
                                    : bus;
      const size_t source = source_bus * kBusEdgesPerBus + edge;
      if (source >= bus_edge_delays_.size() ||
          !bus_edge_delays_[source].prepare_update(
              plan.edge_q8[i], mixing::FractionalDelayMode::Lagrange3, prepared->edge_updates[i])) {
        return false;
      }
    }
    for (size_t i = 0; i < key_edge_delays_.size(); ++i) {
      if (!key_edge_delays_[i].prepare_update(
              plan.key_q8[i], mixing::FractionalDelayMode::Lagrange3, prepared->key_updates[i])) {
        return false;
      }
    }
    if (!direct_pdc_delay_.prepare_update(plan.direct_q8, mixing::FractionalDelayMode::Lagrange3,
                                          prepared->direct_update)) {
      return false;
    }
    return master_pdc_delay_.prepare_update(plan.master_q8, mixing::FractionalDelayMode::Lagrange3,
                                            prepared->master_update);
  } catch (...) {
    return false;
  }
}

void TrackMixerRuntime::commit_pdc_updates(PreparedPdc& prepared) noexcept {
  std::array<mixing::AlignmentDelay, kMaxTrackLanes> old_lane = std::move(lane_pdc_delays_);
  std::array<mixing::AlignmentDelay, kMaxTrackLanes> next_lane;
  std::array<mixing::AlignmentDelay, kMaxTrackLanes> old_pre = std::move(lane_pre_send_pdc_delays_);
  std::array<mixing::AlignmentDelay, kMaxTrackLanes> next_pre;
  std::array<mixing::AlignmentDelay, kMaxTrackLanes> old_in = std::move(lane_in_pdc_delays_);
  std::array<mixing::AlignmentDelay, kMaxTrackLanes> next_in;
  std::array<bool, kMaxTrackLanes> moved{};
  for (size_t destination = 0; destination < kMaxTrackLanes; ++destination) {
    size_t source = prepared.lane_source[destination] >= 0
                        ? static_cast<size_t>(prepared.lane_source[destination])
                        : destination;
    if (source >= kMaxTrackLanes || moved[source]) source = destination;
    moved[source] = true;
    next_lane[destination] = std::move(old_lane[source]);
    next_pre[destination] = std::move(old_pre[source]);
    next_in[destination] = std::move(old_in[source]);
    if (prepared.lane_reset[destination]) {
      next_lane[destination].reset();
      next_pre[destination].reset();
      next_in[destination].reset();
    }
  }
  lane_pdc_delays_ = std::move(next_lane);
  lane_pre_send_pdc_delays_ = std::move(next_pre);
  lane_in_pdc_delays_ = std::move(next_in);
  for (size_t i = 0; i < kMaxTrackLanes; ++i) {
    lane_pdc_delays_[i].commit_update(prepared.lane_updates[i]);
    lane_pre_send_pdc_delays_[i].commit_update(prepared.lane_pre_send_updates[i]);
    lane_in_pdc_delays_[i].commit_update(prepared.lane_in_updates[i]);
  }
  lane_order_ = prepared.plan.lane_order;
  lane_order_count_ = prepared.plan.lane_order_count;
  for (size_t i = 0; i < kMaxBusLanes; ++i) {
    bus_pdc_delays_[i].commit_update(prepared.bus_updates[i]);
  }
  for (size_t i = 0; i < bus_edge_delays_.size(); ++i) {
    bus_edge_delays_[i].commit_update(prepared.edge_updates[i]);
  }
  for (size_t i = 0; i < key_edge_delays_.size(); ++i) {
    key_edge_delays_[i].commit_update(prepared.key_updates[i]);
  }
  direct_pdc_delay_.commit_update(prepared.direct_update);
  master_pdc_delay_.commit_update(prepared.master_update);
  latency_samples_q8_ = prepared.plan.latency_q8;
}

bool TrackMixerRuntime::prepare_identity_pdc(const PdcPlan& plan,
                                             PreparedPdc* prepared) const noexcept {
  std::array<int, kMaxTrackLanes> sources{};
  std::array<bool, kMaxTrackLanes> reset{};
  for (size_t i = 0; i < kMaxTrackLanes; ++i) sources[i] = static_cast<int>(i);
  return prepare_pdc_updates(plan, sources, reset, prepared);
}

bool TrackMixerRuntime::apply_pdc(const PdcPlan& plan) noexcept {
  PreparedPdc prepared;
  if (!prepare_identity_pdc(plan, &prepared)) return false;
  commit_pdc_updates(prepared);
  return true;
}

TrackMixerRuntime::SidechainTable TrackMixerRuntime::without_inserts_from(
    const SidechainTable& source, SidechainTargetKind target_kind, uint32_t target_id,
    size_t insert_count) noexcept {
  SidechainTable filtered = source;
  for (size_t i = filtered.count; i > 0; --i) {
    const SidechainBinding& binding = filtered.bindings[i - 1];
    if (binding.target_kind != static_cast<uint8_t>(target_kind) ||
        binding.target_id != target_id || binding.insert_index < insert_count) {
      continue;
    }
    filtered.bindings[i - 1] = filtered.bindings[filtered.count - 1];
    filtered.bindings[filtered.count - 1] = SidechainBinding{};
    --filtered.count;
  }
  return filtered;
}

int TrackMixerRuntime::tail_samples() const noexcept {
  const std::vector<TrackLaneConfig>* lanes = lanes_.control_current().get();
  if (lanes == nullptr) return 0;
  const BusGraphView view = current_bus_graph_view();
  const size_t lane_count = lanes->size();
  const size_t bus_count = bus_configs_.size();
  // Nodes: lanes, then buses, then the master.
  const size_t master = lane_count + bus_count;
  const auto bus_node = [&](int bus_index) {
    return bus_index < 0 ? master : lane_count + static_cast<size_t>(bus_index);
  };
  const auto bus_index_of = [this, bus_count](uint32_t bus_id) -> int {
    if (bus_id == 0) return -1;
    for (size_t i = 0; i < bus_count; ++i) {
      if (bus_configs_[i].bus_id == bus_id) return static_cast<int>(i);
    }
    return -1;
  };
  const auto lane_of = [lanes](uint32_t track_id) -> int {
    for (size_t i = 0; i < lanes->size(); ++i) {
      if ((*lanes)[i].track_id == track_id) return static_cast<int>(i);
    }
    return -1;
  };
  const auto bus_tail = [&view](size_t bus_index) {
    const mixing::FxBus* bus = view.bus[bus_index];
    return bus != nullptr ? bus->tail_samples() : 0;
  };
  try {
    mixing::MixerTailPlanner planner(master + 1);
    planner.reserve(lane_count * 2 + bus_count * 2 + sidechains_.count);
    for (size_t lane_index = 0; lane_index < lane_count; ++lane_index) {
      const TrackLaneConfig& lane = (*lanes)[lane_index];
      const mixing::ChannelStrip* strip = bound_strip_for(lane.track_id);
      planner.add_path(lane_index, bus_node(bus_index_of(lane.output_bus_id)),
                       strip != nullptr ? strip->tail_samples() : 0);
      for (const TrackLaneConfig::Send& send : lane.sends) {
        const bool pre = send.timing == mixing::SendTiming::PreFader;
        const int tap = strip == nullptr ? 0
                        : pre            ? strip->pre_fader_tail_samples()
                                         : strip->tail_samples();
        planner.add_path(lane_index, bus_node(bus_index_of(send.bus_id)), tap);
      }
    }
    for (size_t bus_index = 0; bus_index < bus_count; ++bus_index) {
      const BusRoute& route = view.routes[bus_index];
      const size_t node = bus_node(static_cast<int>(bus_index));
      planner.add_path(node, bus_node(route.output_index), bus_tail(bus_index));
      for (size_t i = 0; i < route.send_count; ++i) {
        planner.add_path(node, bus_node(route.send_index[i]), bus_tail(bus_index));
      }
    }
    // A key reaches the output only while its insert monitors it.
    for (size_t i = 0; i < sidechains_.count; ++i) {
      if (view.skip_binding[i]) continue;
      const SidechainBinding& binding = sidechains_.bindings[i];
      const rt::ProcessorBase* insert = nullptr;
      size_t target = master;
      if (binding.target_kind == static_cast<uint8_t>(SidechainTargetKind::Lane)) {
        const int lane = lane_of(binding.target_id);
        const mixing::ChannelStrip* strip = bound_strip_for(binding.target_id);
        if (lane < 0 || strip == nullptr) continue;
        target = static_cast<size_t>(lane);
        insert = strip->insert_processor(binding.insert_index);
      } else if (binding.target_kind == static_cast<uint8_t>(SidechainTargetKind::Bus)) {
        const int bus = bus_index_of(binding.target_id);
        if (bus < 0 || view.bus[static_cast<size_t>(bus)] == nullptr) continue;
        target = bus_node(bus);
        insert = view.bus[static_cast<size_t>(bus)]->insert_processor(binding.insert_index);
      } else if (master_strip_ != nullptr) {
        insert = master_strip_->insert_processor(binding.insert_index);
      }
      if (insert == nullptr || !insert->sidechain_audible()) continue;
      if (binding.source_kind == static_cast<uint8_t>(SidechainSourceKind::Bus)) {
        const int bus = bus_index_of(binding.source_id);
        if (bus < 0) continue;
        planner.add_path(bus_node(bus), target, bus_tail(static_cast<size_t>(bus)));
      } else {
        const int lane = lane_of(binding.source_id);
        const mixing::ChannelStrip* strip = bound_strip_for(binding.source_id);
        if (lane < 0) continue;
        planner.add_path(static_cast<size_t>(lane), target,
                         strip != nullptr ? strip->tail_samples() : 0);
      }
    }
    // The master contributes 0 here; the engine adds the master strip itself.
    return rt::TailBudget::reported(planner.arriving(master)).samples();
  } catch (...) {
    // Out of memory planning the query: unbounded keeps it an upper bound.
    return rt::TailBudget::kUnbounded;
  }
}

}  // namespace sonare::engine
