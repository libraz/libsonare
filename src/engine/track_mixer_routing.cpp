#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include "engine/track_mixer.h"

namespace sonare::engine {

void TrackMixerRuntime::flush_pdc_delays() noexcept {
  for (mixing::AlignmentDelay& delay : lane_pdc_delays_) {
    delay.reset();
  }
  for (mixing::AlignmentDelay& delay : lane_pre_send_pdc_delays_) {
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
  master_pdc_delay_.reset();
}

uint64_t TrackMixerRuntime::pdc_storage_generation() const noexcept {
  uint64_t total = master_pdc_delay_.storage_generation();
  for (const mixing::AlignmentDelay& delay : lane_pdc_delays_) {
    total += delay.storage_generation();
  }
  for (const mixing::AlignmentDelay& delay : lane_pre_send_pdc_delays_) {
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
                                     std::array<BusRoute, kMaxBusLanes>* routes) const noexcept {
  const auto index_of = [&buses](uint32_t bus_id) -> int {
    if (bus_id == 0) return -1;
    for (size_t i = 0; i < buses.size(); ++i) {
      if (buses[i].bus_id == bus_id) return static_cast<int>(i);
    }
    return -1;
  };
  const size_t binding_count = sidechains_.count;
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
      const SidechainBinding& binding = sidechains_.bindings[i];
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
    view.latency_q8[bus_index] = bus != nullptr ? bus->latency_samples_q8() : 0;
  }
  return view;
}

bool TrackMixerRuntime::plan_pdc(const std::vector<TrackLaneConfig>& lanes,
                                 const BusGraphView& view, PdcPlan* plan) const noexcept {
  // Lane stage: the widest strip latency. Every lane leaves process_lane_strip
  // at this offset, and every path the lane's audio then takes is tapped from
  // there -- the direct master sum, the output-bus routing and the post-fader
  // sends all read the aligned lane buffer, while a pre-fader send reads the
  // strip's earlier tap through lane_pre_send_pdc_delays_ below. One lane
  // timebase, three exits.
  int max_strip_q8 = 0;
  for (size_t lane_index = 0; lane_index < lanes.size(); ++lane_index) {
    const mixing::ChannelStrip* strip = lane_states_[lane_index].strip;
    if (strip != nullptr) {
      max_strip_q8 = std::max(max_strip_q8, strip->latency_samples_q8());
    }
  }
  *plan = PdcPlan{};
  for (size_t lane_index = 0; lane_index < lanes.size(); ++lane_index) {
    const mixing::ChannelStrip* strip = lane_states_[lane_index].strip;
    plan->lane_q8[lane_index] = max_strip_q8 - (strip != nullptr ? strip->latency_samples_q8() : 0);
    // Pre-fader send stage: the same target offset, measured from the earlier
    // tap. A strip's pre-fader latency is what it has accrued by the time the
    // pre tap is taken (channel delay + pre inserts), so this bank carries the
    // rest of the widest strip latency -- including the strip's own
    // post-insert chain, which the lane's output passes through and the pre
    // tap does not.
    plan->lane_pre_q8[lane_index] =
        max_strip_q8 - (strip != nullptr ? strip->pre_fader_latency_samples_q8() : 0);
  }

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
  const size_t binding_count = sidechains_.count;
  const auto live_bus_key = [&](size_t i, uint32_t source_bus) {
    const SidechainBinding& binding = sidechains_.bindings[i];
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
      const SidechainBinding& binding = sidechains_.bindings[i];
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
  // Key edges: in(target) - L from a track, in(target) - out(source) from a bus.
  for (size_t i = 0; i < binding_count; ++i) {
    if (view.skip_binding[i]) continue;
    const SidechainBinding& binding = sidechains_.bindings[i];
    const auto target_kind = binding.target_kind;
    if (target_kind == static_cast<uint8_t>(SidechainTargetKind::Lane)) continue;
    int target = -1;
    if (target_kind == static_cast<uint8_t>(SidechainTargetKind::Bus)) {
      target = index_of(binding.target_id);
      if (target < 0) continue;
    }
    int source_out = max_strip_q8;
    if (binding.source_kind == static_cast<uint8_t>(SidechainSourceKind::Bus)) {
      const int source = index_of(binding.source_id);
      if (source < 0) continue;
      source_out = bus_out[static_cast<size_t>(source)];
    }
    plan->key_q8[binding.key_slot] = input_of(target) - source_out;
  }
  // What the engine advertises to the host: the master input's arrival.
  plan->latency_q8 = master_in;

  // A delay line past its ceiling would clamp silently and misalign the mix,
  // so such a configuration is refused instead.
  constexpr int kCapQ8 = mixing::kMaxAlignmentDelaySamples << 8;
  const auto within = [](const auto& values) {
    return std::all_of(values.begin(), values.end(), [](int v) { return v <= kCapQ8; });
  };
  return within(plan->lane_q8) && within(plan->lane_pre_q8) && within(plan->bus_in_q8) &&
         within(plan->edge_q8) && within(plan->key_q8) && plan->master_q8 <= kCapQ8;
}

bool TrackMixerRuntime::apply_pdc(const PdcPlan& plan) noexcept {
  bool ok = true;
  for (size_t i = 0; i < lane_pdc_delays_.size(); ++i) {
    ok = lane_pdc_delays_[i].try_set_delay_samples_q8(plan.lane_q8[i]) && ok;
    ok = lane_pre_send_pdc_delays_[i].try_set_delay_samples_q8(plan.lane_pre_q8[i]) && ok;
  }
  for (size_t i = 0; i < bus_pdc_delays_.size(); ++i) {
    ok = bus_pdc_delays_[i].try_set_delay_samples_q8(plan.bus_in_q8[i]) && ok;
  }
  for (size_t i = 0; i < bus_edge_delays_.size(); ++i) {
    ok = bus_edge_delays_[i].try_set_delay_samples_q8(plan.edge_q8[i]) && ok;
  }
  for (size_t i = 0; i < key_edge_delays_.size(); ++i) {
    ok = key_edge_delays_[i].try_set_delay_samples_q8(plan.key_q8[i]) && ok;
  }
  ok = master_pdc_delay_.try_set_delay_samples_q8(plan.master_q8) && ok;
  latency_samples_q8_ = plan.latency_q8;
  return ok;
}

}  // namespace sonare::engine
