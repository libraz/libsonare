#include "c_api/mixing_internal.h"
#include "mixing/gain.h"
#include "mixing/solo_mute.h"
#include "mixing/stereo_width.h"
#include "mixing/tail_utils.h"

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

  StripNode(sonare::mixing::ChannelStrip* strip, int num_sends, int64_t sample_pos,
            std::vector<SidechainInput> sidechain_inputs = {})
      : strip_(strip),
        num_sends_(num_sends),
        sidechain_inputs_(std::move(sidechain_inputs)),
        sample_pos_(sample_pos) {}

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
  int tail_samples() const noexcept override { return strip_->tail_samples(); }
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

  BusNode(std::unique_ptr<sonare::mixing::FxBus> bus, float input_trim_db, float width,
          bool polarity_invert_left, bool polarity_invert_right,
          std::vector<SidechainInput> sidechain_inputs = {})
      : bus_(std::move(bus)),
        input_trim_({input_trim_db, 5.0f}),
        width_(width, 5.0f),
        polarity_left_(polarity_invert_left ? -1.0f : 1.0f),
        polarity_right_(polarity_invert_right ? -1.0f : 1.0f),
        sidechain_inputs_(std::move(sidechain_inputs)) {}

  void prepare(double sample_rate, int max_block_size) override {
    input_trim_.prepare(sample_rate, max_block_size);
    bus_->prepare(sample_rate, max_block_size);
    width_.prepare(sample_rate, max_block_size);
  }

  void process(float* const* channels, int, int num_samples) override {
    // Match TrackMixerRuntime's scene-bus signal order exactly:
    // trim -> front-pair polarity -> inserts -> front-pair stereo width.
    input_trim_.process(channels, 2, num_samples);
    if (channels[0] != nullptr && polarity_left_ < 0.0f) {
      for (int i = 0; i < num_samples; ++i) channels[0][i] *= polarity_left_;
    }
    if (channels[1] != nullptr && polarity_right_ < 0.0f) {
      for (int i = 0; i < num_samples; ++i) channels[1][i] *= polarity_right_;
    }
    bus_->clear_insert_sidechains();
    for (const auto& input : sidechain_inputs_) {
      const float* key[2] = {channels[input.left_port], channels[input.right_port]};
      bus_->set_insert_sidechain(input.insert_index, key, 2, num_samples);
    }
    bus_->process(channels, 2, num_samples);
    if (width_.width() != 1.0f || width_.current_width() != 1.0f) {
      width_.process(channels, 2, num_samples);
    }
  }

  void reset() override {
    input_trim_.reset();
    bus_->reset();
    width_.reset();
  }
  int latency_samples() const noexcept override { return bus_->latency_samples(); }
  int latency_samples_q8() const noexcept override { return bus_->latency_samples_q8(); }
  int tail_samples() const noexcept override { return bus_->tail_samples(); }
  sonare::mixing::MeterSnapshot meter_snapshot() const noexcept {
    return bus_->bus().meter_snapshot();
  }

 private:
  std::unique_ptr<sonare::mixing::FxBus> bus_;
  sonare::mixing::GainProcessor input_trim_;
  sonare::mixing::StereoWidthProcessor width_;
  float polarity_left_;
  float polarity_right_;
  std::vector<SidechainInput> sidechain_inputs_;
};

}  // namespace sonare_c_mixing_detail

SonareError sonare_mixer_bus_meter(SonareMixer* mixer, const char* bus_id,
                                   SonareMixMeterSnapshot* out) {
  SONARE_C_API_ENTRY;
  if (!mixer || !bus_id || bus_id[0] == '\0' || !out) return SONARE_ERROR_INVALID_PARAMETER;
  if (mixer->compiled_dirty) return SONARE_ERROR_INVALID_STATE;
  sonare::graph::Node* node = mixer->graph.node(bus_id);
  if (!node) return SONARE_ERROR_INVALID_PARAMETER;
  auto* bus = dynamic_cast<sonare_c_mixing_detail::BusNode*>(&node->processor());
  if (!bus) return SONARE_ERROR_INVALID_PARAMETER;
  sonare_c_mixing_detail::copy_meter_snapshot(bus->meter_snapshot(), out);
  return SONARE_OK;
}

namespace sonare_c_mixing_detail {

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
  std::unordered_map<std::string, int> local_tail_by_id;
  std::unordered_map<std::string, std::vector<std::string>> audio_inputs_by_id;
  auto checked_connect = [&](sonare::graph::Connection connection) {
    if (!graph.connect(std::move(connection))) {
      throw SonareException(ErrorCode::InvalidParameter, "invalid or duplicate mixer connection");
    }
  };
  auto checked_connect_audio = [&](const std::string& source, int source_left, int source_right,
                                   const std::string& destination) {
    checked_connect({source, source_left, destination, 0, sonare::graph::Connection::Mix::Add});
    checked_connect({source, source_right, destination, 1, sonare::graph::Connection::Mix::Add});
    audio_inputs_by_id[destination].push_back(source);
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
    auto node = std::make_unique<BusNode>(std::move(fx_bus), bus.input_trim_db, bus.width,
                                          bus.polarity_invert_left, bus.polarity_invert_right,
                                          std::move(sidechain_inputs));
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
    auto node = std::make_unique<StripNode>(&strip->strip, num_sends, mixer->timeline_sample_pos,
                                            std::move(sidechain_inputs));
    if (!graph.add_node(strip->id, std::move(node), num_ports)) {
      throw SonareException(ErrorCode::InvalidParameter,
                            "duplicate or invalid strip id: " + strip->id);
    }
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
    checked_connect_audio(conn.source, 0, 1, conn.destination);
    has_main_out[conn.source] = true;
  }

  // Default-route strips with no outgoing main connection to the master bus.
  for (const auto& strip : mixer->strips) {
    if (!has_main_out[strip->id] && strip->id != master_id) {
      checked_connect_audio(strip->id, 0, 1, master_id);
    }
  }

  // Default-route only implicit buses created by the manual send API. Explicit
  // scene buses keep their authored topology, including intentionally unpatched
  // aux/submix buses.
  for (const auto& bus : buses) {
    if (is_implicit_bus[bus.id] && !has_main_out[bus.id] && bus.id != master_id) {
      checked_connect_audio(bus.id, 0, 1, master_id);
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
      checked_connect_audio(strip->id, src_l, src_r, dest);
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

  graph.prepare(static_cast<double>(mixer->sample_rate), mixer->max_block_size);

  // Tail values are prepared-state capabilities just like latency. Query the
  // graph wrappers only after prepare() so config-dependent delay lengths are
  // the exact values used by processing, rather than constructor fallbacks.
  for (const std::string& node_id : graph.topo_order_ids()) {
    const sonare::graph::Node* node = graph.node(node_id);
    if (node == nullptr) {
      throw SonareException(ErrorCode::InvalidState, "mixer tail node missing after compile");
    }
    local_tail_by_id[node_id] = std::max(0, node->processor().tail_samples());
  }

  const sonare::graph::Node* master_node = graph.node(master_id);
  if (master_node == nullptr) {
    throw SonareException(ErrorCode::InvalidState, "mixer master node missing after compile");
  }
  const int master_latency_q8 =
      graph.node_latency_samples_q8(master_id) + master_node->processor().latency_samples_q8();

  // Tail propagation follows only audible main/send edges. Sidechain edges are
  // graph dependencies but do not feed their source audio into the keyed
  // processor's output, so including them would overstate the master tail.
  // Serial nodes add their local tails; merged main/send branches take max.
  std::unordered_map<std::string, int> accumulated_tail_by_id;
  for (const std::string& node_id : graph.topo_order_ids()) {
    int upstream_tail = 0;
    const auto inputs_it = audio_inputs_by_id.find(node_id);
    if (inputs_it != audio_inputs_by_id.end()) {
      for (const std::string& source_id : inputs_it->second) {
        const auto source_it = accumulated_tail_by_id.find(source_id);
        if (source_it == accumulated_tail_by_id.end()) {
          throw SonareException(ErrorCode::InvalidState,
                                "mixer tail topology is not in dependency order");
        }
        upstream_tail = sonare::mixing::combine_tail_samples(
            upstream_tail, source_it->second, sonare::mixing::TailTopology::kParallel);
      }
    }
    const auto local_it = local_tail_by_id.find(node_id);
    const int local_tail = local_it == local_tail_by_id.end() ? 0 : local_it->second;
    accumulated_tail_by_id[node_id] = sonare::mixing::combine_tail_samples(
        upstream_tail, local_tail, sonare::mixing::TailTopology::kSerial);
  }
  const auto master_tail_it = accumulated_tail_by_id.find(master_id);
  if (master_tail_it == accumulated_tail_by_id.end()) {
    throw SonareException(ErrorCode::InvalidState, "mixer master tail path missing after compile");
  }

  mixer->graph = std::move(graph);
  mixer->master_id = std::move(master_id);
  mixer->latency_samples = std::max(0, master_latency_q8 >> 8);
  mixer->tail_samples = master_tail_it->second;
  mixer->compiled_dirty = false;
}

}  // namespace sonare_c_mixing_detail
