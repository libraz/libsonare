#include "c_api/mixing_internal.h"
#include "mixing/solo_mute.h"

namespace sonare_c_mixing_detail {

// Graph wrapper that exposes a ChannelStrip's main path and its aux send taps
// as separate output ports. Ports 0,1 carry the processed main L/R signal;
// ports (2 + 2*s, 3 + 2*s) carry send index s's L/R tap. The strip is owned and
// prepared externally (by SonareStrip), so prepare()/reset() only forward reset.
class StripNode final : public sonare::rt::ProcessorBase {
 public:
  struct SidechainInput {
    unsigned int insert_index = 0;
    int left_port = 0;
    int right_port = 0;
  };

  StripNode(sonare::mixing::ChannelStrip* strip, int num_sends,
            std::vector<SidechainInput> sidechain_inputs = {})
      : strip_(strip), num_sends_(num_sends), sidechain_inputs_(std::move(sidechain_inputs)) {}

  void prepare(double, int) override {}  // inner strip prepared via add_strip()

  void process(float* const* channels, int num_channels, int num_samples) override {
    (void)num_channels;  // Node passes num_ports; main path always uses L/R.
    strip_->clear_insert_sidechains();
    for (const auto& input : sidechain_inputs_) {
      const float* key[2] = {channels[input.left_port], channels[input.right_port]};
      strip_->set_insert_sidechain(input.insert_index, key, 2, num_samples);
    }
    strip_->process_at(channels, 2, num_samples, sample_pos_);
    for (int s = 0; s < num_sends_; ++s) {
      float* dst[2] = {channels[2 + 2 * s], channels[3 + 2 * s]};
      std::fill(dst[0], dst[0] + num_samples, 0.0f);
      std::fill(dst[1], dst[1] + num_samples, 0.0f);
      strip_->mix_send_at(static_cast<size_t>(s), dst, 2, num_samples, sample_pos_);  // additive
    }
    sample_pos_ += num_samples;
  }

  void reset() override {
    strip_->reset();
    sample_pos_ = 0;
  }
  int latency_samples() const noexcept override { return strip_->latency_samples(); }
  int latency_samples_q8() const noexcept override { return strip_->latency_samples_q8(); }
  int output_latency_samples_q8(int output_port) const noexcept override {
    if (output_port >= 2) {
      const int send_index = (output_port - 2) / 2;
      return strip_->send_latency_samples_q8(static_cast<size_t>(send_index));
    }
    return strip_->post_fader_latency_samples_q8();
  }

 private:
  sonare::mixing::ChannelStrip* strip_;  // borrowed; owned by SonareStrip
  int num_sends_;
  std::vector<SidechainInput> sidechain_inputs_;
  int64_t sample_pos_ = 0;
};

class BusNode final : public sonare::rt::ProcessorBase {
 public:
  struct SidechainInput {
    unsigned int insert_index = 0;
    int left_port = 0;
    int right_port = 0;
  };

  BusNode(std::unique_ptr<sonare::mixing::FxBus> bus,
          std::vector<SidechainInput> sidechain_inputs = {})
      : bus_(std::move(bus)), sidechain_inputs_(std::move(sidechain_inputs)) {}

  void prepare(double sample_rate, int max_block_size) override {
    bus_->prepare(sample_rate, max_block_size);
  }

  void process(float* const* channels, int, int num_samples) override {
    bus_->clear_insert_sidechains();
    for (const auto& input : sidechain_inputs_) {
      const float* key[2] = {channels[input.left_port], channels[input.right_port]};
      bus_->set_insert_sidechain(input.insert_index, key, 2, num_samples);
    }
    bus_->process(channels, 2, num_samples);
  }

  void reset() override { bus_->reset(); }
  int latency_samples() const noexcept override { return bus_->latency_samples(); }
  int latency_samples_q8() const noexcept override { return bus_->latency_samples_q8(); }

 private:
  std::unique_ptr<sonare::mixing::FxBus> bus_;
  std::vector<SidechainInput> sidechain_inputs_;
};

void apply_solo_mutes(SonareMixer* mixer) {
  bool any_solo = false;
  for (const auto& strip : mixer->strips) {
    any_solo = any_solo || strip->strip.soloed();
  }
  for (const auto& strip : mixer->strips) {
    strip->strip.set_implied_mute(sonare::mixing::solo_implies_mute(any_solo, strip->strip.soloed(),
                                                                    strip->strip.solo_safe()));
  }
}

// Rebuilds the routing graph from the mixer's stored strips/buses/connections,
// wiring main edges, send taps, and default master routing, then compiles and
// prepares it. Throws sonare::SonareException on invalid topology.
void build_and_compile(SonareMixer* mixer) {
  using sonare::ErrorCode;
  using sonare::SonareException;

  sonare::graph::Graph graph;
  apply_solo_mutes(mixer);
  int tail_samples = 0;
  auto checked_connect = [&](sonare::graph::Connection connection) {
    if (!graph.connect(std::move(connection))) {
      throw SonareException(ErrorCode::InvalidParameter, "invalid or duplicate mixer connection");
    }
  };

  // Work on a local bus list so manually-built mixers (no scene) still get a
  // master, and any send destination that isn't an explicit bus becomes an
  // implicit aux bus. mixer->buses is left untouched.
  std::vector<sonare::mixing::api::Bus> buses = mixer->buses;

  // Resolve the master bus id: prefer role == "master", else id == "master";
  // synthesize one if neither exists (e.g. the manual create/add_strip path).
  std::string master_id;
  for (const auto& bus : buses) {
    if (bus.role == "master") {
      master_id = bus.id;
      break;
    }
  }
  if (master_id.empty()) {
    for (const auto& bus : buses) {
      if (bus.id == "master") {
        master_id = bus.id;
        break;
      }
    }
  }
  if (master_id.empty()) {
    buses.push_back({"master", "master"});
    master_id = "master";
  }

  // Any send destination that isn't already a bus becomes an implicit aux bus
  // (it default-routes to master below, so manual sends are still audible).
  std::unordered_map<std::string, bool> is_bus;
  std::unordered_map<std::string, bool> is_implicit_bus;
  for (const auto& bus : buses) {
    is_bus[bus.id] = true;
  }
  for (const auto& strip : mixer->strips) {
    for (const auto& send : strip->scene_strip.sends) {
      if (!is_bus.count(send.destination_bus_id)) {
        buses.push_back({send.destination_bus_id, "aux"});
        is_bus[send.destination_bus_id] = true;
        is_implicit_bus[send.destination_bus_id] = true;
      }
    }
  }

  // Bus nodes: post-sum insert chains live inside FxBus/BusProcessor.
  std::unordered_map<std::string, std::vector<BusNode::SidechainInput>> bus_sidechain_inputs_by_id;
  std::unordered_map<std::string, std::vector<std::string>> bus_sidechain_keys_by_id;
  for (const auto& bus : buses) {
    auto fx_bus = std::make_unique<sonare::mixing::FxBus>();
    for (const auto& insert : bus.inserts) {
      auto processor =
          sonare::mastering::api::make_insert(insert.processor_name, insert.params_json);
      if (!processor) {
        throw SonareException(
            ErrorCode::InvalidParameter,
            "unknown bus insert processor: " + insert.processor_name + " (bus " + bus.id + ")");
      }
      fx_bus->add_insert(std::move(processor));
    }
    int next_sidechain_port = 2;
    std::vector<BusNode::SidechainInput> sidechain_inputs;
    std::vector<std::string> sidechain_keys;
    for (size_t insert_index = 0; insert_index < bus.inserts.size(); ++insert_index) {
      const auto& insert = bus.inserts[insert_index];
      if (insert.sidechain_key.empty()) {
        continue;
      }
      sidechain_inputs.push_back(
          {static_cast<unsigned int>(insert_index), next_sidechain_port, next_sidechain_port + 1});
      sidechain_keys.push_back(insert.sidechain_key);
      next_sidechain_port += 2;
    }
    bus_sidechain_inputs_by_id[bus.id] = sidechain_inputs;
    bus_sidechain_keys_by_id[bus.id] = sidechain_keys;
    tail_samples = std::max(tail_samples, fx_bus->tail_samples());
    auto node = std::make_unique<BusNode>(std::move(fx_bus), std::move(sidechain_inputs));
    if (!graph.add_node(bus.id, std::move(node), next_sidechain_port)) {
      throw SonareException(ErrorCode::InvalidParameter, "duplicate or invalid bus id: " + bus.id);
    }
  }

  // Strip nodes: 2 main ports + 2 ports per send tap.
  std::unordered_map<std::string, SonareStrip*> strip_by_id;
  std::unordered_map<std::string, std::vector<StripNode::SidechainInput>> sidechain_inputs_by_id;
  std::unordered_map<std::string, std::vector<std::string>> sidechain_keys_by_id;
  for (const auto& strip : mixer->strips) {
    const int num_sends = static_cast<int>(strip->strip.num_sends());
    int next_sidechain_port = 2 + 2 * num_sends;
    std::vector<StripNode::SidechainInput> sidechain_inputs;
    std::vector<std::string> sidechain_keys;
    const size_t pre_insert_count =
        std::count_if(strip->scene_strip.inserts.begin(), strip->scene_strip.inserts.end(),
                      [](const sonare::mixing::api::Insert& insert) {
                        return insert.slot == sonare::mixing::api::InsertSlot::PreFader;
                      });
    size_t pre_index = 0;
    size_t post_index = 0;
    for (size_t insert_index = 0; insert_index < strip->scene_strip.inserts.size();
         ++insert_index) {
      const auto& insert = strip->scene_strip.inserts[insert_index];
      const size_t combined_insert_index = insert.slot == sonare::mixing::api::InsertSlot::PreFader
                                               ? pre_index++
                                               : pre_insert_count + post_index++;
      if (insert.sidechain_key.empty()) {
        continue;
      }
      sidechain_inputs.push_back({static_cast<unsigned int>(combined_insert_index),
                                  next_sidechain_port, next_sidechain_port + 1});
      sidechain_keys.push_back(insert.sidechain_key);
      next_sidechain_port += 2;
    }
    const int num_ports = next_sidechain_port;
    sidechain_inputs_by_id[strip->id] = sidechain_inputs;
    sidechain_keys_by_id[strip->id] = sidechain_keys;
    auto node = std::make_unique<StripNode>(&strip->strip, num_sends, std::move(sidechain_inputs));
    if (!graph.add_node(strip->id, std::move(node), num_ports)) {
      throw SonareException(ErrorCode::InvalidParameter,
                            "duplicate or invalid strip id: " + strip->id);
    }
    tail_samples = std::max(tail_samples, strip->strip.tail_samples());
    strip_by_id[strip->id] = strip.get();
  }

  // Main connections. Track which strips have an explicit outgoing main edge so
  // the rest can default-route to master.
  std::unordered_map<std::string, bool> has_main_out;
  for (const auto& conn : mixer->connections) {
    if (graph.node(conn.source) == nullptr || graph.node(conn.destination) == nullptr) {
      throw SonareException(
          ErrorCode::InvalidParameter,
          "connection references unknown node: " + conn.source + " -> " + conn.destination);
    }
    checked_connect({conn.source, 0, conn.destination, 0, sonare::graph::Connection::Mix::Add});
    checked_connect({conn.source, 1, conn.destination, 1, sonare::graph::Connection::Mix::Add});
    has_main_out[conn.source] = true;
  }

  // Default-route strips with no outgoing main connection to the master bus.
  for (const auto& strip : mixer->strips) {
    if (!has_main_out[strip->id] && strip->id != master_id) {
      checked_connect({strip->id, 0, master_id, 0, sonare::graph::Connection::Mix::Add});
      checked_connect({strip->id, 1, master_id, 1, sonare::graph::Connection::Mix::Add});
    }
  }

  // Default-route only implicit buses created by the manual send API. Explicit
  // scene buses keep their authored topology, including intentionally unpatched
  // aux/submix buses.
  for (const auto& bus : buses) {
    if (is_implicit_bus[bus.id] && !has_main_out[bus.id] && bus.id != master_id) {
      checked_connect({bus.id, 0, master_id, 0, sonare::graph::Connection::Mix::Add});
      checked_connect({bus.id, 1, master_id, 1, sonare::graph::Connection::Mix::Add});
    }
  }

  // Explicit scene buses are allowed to remain unpatched. Do not report that as
  // last_error on a successful compile: last_error is reserved for failing C API
  // calls, and stale warning text after SONARE_OK breaks callers that check it
  // only on error.

  // Send taps: strip send output ports -> destination bus input ports.
  for (const auto& strip : mixer->strips) {
    const auto& sends = strip->scene_strip.sends;
    for (size_t s = 0; s < sends.size(); ++s) {
      const std::string& dest = sends[s].destination_bus_id;
      if (!is_bus.count(dest)) {
        throw SonareException(ErrorCode::InvalidParameter, "send destination is not a bus: " +
                                                               dest + " (strip " + strip->id + ")");
      }
      const int src_l = 2 + 2 * static_cast<int>(s);
      const int src_r = 3 + 2 * static_cast<int>(s);
      checked_connect({strip->id, src_l, dest, 0, sonare::graph::Connection::Mix::Add});
      checked_connect({strip->id, src_r, dest, 1, sonare::graph::Connection::Mix::Add});
    }
  }

  // Insert sidechain keys: source main output -> destination strip key input ports.
  for (const auto& strip : mixer->strips) {
    const auto inputs_it = sidechain_inputs_by_id.find(strip->id);
    const auto keys_it = sidechain_keys_by_id.find(strip->id);
    if (inputs_it == sidechain_inputs_by_id.end() || keys_it == sidechain_keys_by_id.end()) {
      continue;
    }
    const auto& inputs = inputs_it->second;
    const auto& keys = keys_it->second;
    for (size_t index = 0; index < inputs.size(); ++index) {
      const std::string& key_source = keys[index];
      if (graph.node(key_source) == nullptr) {
        throw SonareException(
            ErrorCode::InvalidParameter,
            "sidechain key references unknown node: " + key_source + " (strip " + strip->id + ")");
      }
      checked_connect(
          {key_source, 0, strip->id, inputs[index].left_port, sonare::graph::Connection::Mix::Add});
      checked_connect({key_source, 1, strip->id, inputs[index].right_port,
                       sonare::graph::Connection::Mix::Add});
    }
  }

  for (const auto& bus : buses) {
    const auto inputs_it = bus_sidechain_inputs_by_id.find(bus.id);
    const auto keys_it = bus_sidechain_keys_by_id.find(bus.id);
    if (inputs_it == bus_sidechain_inputs_by_id.end() ||
        keys_it == bus_sidechain_keys_by_id.end()) {
      continue;
    }
    const auto& inputs = inputs_it->second;
    const auto& keys = keys_it->second;
    for (size_t index = 0; index < inputs.size(); ++index) {
      const std::string& key_source = keys[index];
      if (graph.node(key_source) == nullptr) {
        throw SonareException(
            ErrorCode::InvalidParameter,
            "bus sidechain key references unknown node: " + key_source + " (bus " + bus.id + ")");
      }
      checked_connect(
          {key_source, 0, bus.id, inputs[index].left_port, sonare::graph::Connection::Mix::Add});
      checked_connect(
          {key_source, 1, bus.id, inputs[index].right_port, sonare::graph::Connection::Mix::Add});
    }
  }

  // VCA group offsets are applied to live ChannelStrips at scene load (see
  // sonare_mixer_from_scene_json); they persist across graph rebuilds, so no
  // node or edge is needed here.

  if (!graph.compile()) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "mixer routing graph failed to compile (cycle or invalid topology)");
  }
  graph.prepare(static_cast<double>(mixer->sample_rate), mixer->max_block_size);

  mixer->graph = std::move(graph);
  mixer->master_id = std::move(master_id);
  mixer->latency_samples = mixer->graph.node_latency_samples(mixer->master_id);
  mixer->tail_samples = tail_samples;
  mixer->compiled_dirty = false;
}

}  // namespace sonare_c_mixing_detail
