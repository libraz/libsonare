#include <algorithm>

#include "c_api/mixing_internal.h"
#include "mixing/downmix.h"
#include "mixing/gain.h"
#include "mixing/solo_mute.h"
#include "mixing/stereo_width.h"
#include "mixing/tail_utils.h"

namespace sonare_c_mixing_detail {

// True if at least one of the EQ's 24 slots is enabled. A BusNode skips the EQ
// stage otherwise, keeping the default (no bands) bus bit-identical to input.
bool eq_has_active_band(const sonare::mastering::eq::ParametricEq& eq) {
  for (size_t index = 0; index < sonare::mastering::eq::ParametricEq::kMaxBands; ++index) {
    if (eq.band(index).enabled) {
      return true;
    }
  }
  return false;
}

// Graph wrapper that exposes a ChannelStrip's main path and its aux send taps
// as separate output ports. Ports 0,1 carry the processed main L/R signal (and
// are where a sidechain key taps it); when the main destination is wider than
// two planes, ports [2, 2 + W) carry that signal scattered by the surround
// panner; the next 2 * S ports carry send index s's L/R tap; sidechain inputs
// come last. The strip is owned and prepared externally (by SonareStrip).
class StripNode final : public sonare::rt::ProcessorBase {
 public:
  struct SidechainInput {
    unsigned int insert_index = 0;
    int left_port = 0;
    int right_port = 0;
  };

  StripNode(SonareStrip* owner, int scatter_planes, int num_sends, int64_t sample_pos,
            std::vector<SidechainInput> sidechain_inputs = {})
      : strip_(&owner->strip),
        surround_(&owner->surround),
        surround_prepared_(&owner->surround_prepared),
        scatter_planes_(scatter_planes),
        num_sends_(num_sends),
        sidechain_inputs_(std::move(sidechain_inputs)),
        sample_pos_(sample_pos) {
    if (scatter_planes_ > 0) {
      surround_->set_layout(sonare::layout_from_channel_count(scatter_planes_));
    }
  }

  // The inner strip is prepared via add_strip(). The scatter panner is prepared
  // once, on the first compile that needs it, so a rebuild does not restart it.
  void prepare(double sample_rate, int max_block_size) override {
    if (scatter_planes_ > 0 && !*surround_prepared_) {
      surround_->prepare(sample_rate, max_block_size);
      *surround_prepared_ = true;
    }
  }

  void process(float* const* channels, int num_channels, int num_samples) override {
    (void)num_channels;  // Node passes num_ports; main path always uses L/R.
    strip_->clear_insert_sidechains();
    for (const auto& input : sidechain_inputs_) {
      const float* key[2] = {channels[input.left_port], channels[input.right_port]};
      strip_->set_insert_sidechain(input.insert_index, key, 2, num_samples);
    }
    strip_->process_at(channels, 2, num_samples, sample_pos_);
    if (scatter_planes_ > 0) {
      float* const* scatter = channels + 2;
      for (int p = 0; p < scatter_planes_; ++p) {
        std::fill(scatter[p], scatter[p] + num_samples, 0.0f);
      }
      surround_->set_params(strip_->surround_pan_params());
      const float* main[2] = {channels[0], channels[1]};
      surround_->process_add(main, 2, scatter, scatter_planes_, num_samples);
    }
    const int send_base = 2 + scatter_planes_;
    for (int s = 0; s < num_sends_; ++s) {
      float* dst[2] = {channels[send_base + 2 * s], channels[send_base + 1 + 2 * s]};
      std::fill(dst[0], dst[0] + num_samples, 0.0f);
      std::fill(dst[1], dst[1] + num_samples, 0.0f);
      strip_->mix_send_at(static_cast<size_t>(s), dst, 2, num_samples, sample_pos_);  // additive
    }
    sample_pos_ += num_samples;
  }

  void reset() override {
    strip_->reset();
    if (scatter_planes_ > 0) surround_->reset();
    sample_pos_ = 0;
  }
  int latency_samples() const noexcept override { return strip_->latency_samples(); }
  int latency_samples_q8() const noexcept override { return strip_->latency_samples_q8(); }
  int tail_samples() const noexcept override { return strip_->tail_samples(); }
  int output_latency_samples_q8(int output_port) const noexcept override {
    const int send_base = 2 + scatter_planes_;
    if (output_port >= send_base) {
      const int send_index = (output_port - send_base) / 2;
      return strip_->send_latency_samples_q8(static_cast<size_t>(send_index));
    }
    return strip_->post_fader_latency_samples_q8();
  }

 private:
  sonare::mixing::ChannelStrip* strip_;                // borrowed; owned by SonareStrip
  sonare::mixing::SurroundPannerProcessor* surround_;  // borrowed; owned by SonareStrip
  bool* surround_prepared_;                            // borrowed; owned by SonareStrip
  int scatter_planes_;
  int num_sends_;
  std::vector<SidechainInput> sidechain_inputs_;
  int64_t sample_pos_ = 0;
};

// Folds its first W_from ports into the first W_to with mixing::downmix. One is
// inserted per (source, destination width) wherever an edge meets a narrower
// destination: a Connection carries no gain, so the coefficients live here.
class DownmixNode final : public sonare::rt::ProcessorBase {
 public:
  DownmixNode(sonare::ChannelLayout from, sonare::ChannelLayout to) : from_(from), to_(to) {}

  void prepare(double, int max_block_size) override {
    block_ = std::max(0, max_block_size);
    scratch_.assign(static_cast<size_t>(sonare::channel_count(from_)) * static_cast<size_t>(block_),
                    0.0f);
  }

  void process(float* const* channels, int, int num_samples) override {
    const int from_planes = sonare::channel_count(from_);
    const int to_planes = sonare::channel_count(to_);
    std::array<const float*, sonare::mixing::kMaxSurroundPlanes> in{};
    for (int p = 0; p < from_planes; ++p) {
      float* plane = scratch_.data() + static_cast<size_t>(p) * static_cast<size_t>(block_);
      std::copy(channels[p], channels[p] + num_samples, plane);
      in[static_cast<size_t>(p)] = plane;
    }
    sonare::mixing::downmix(from_, to_, in.data(), channels, static_cast<size_t>(num_samples));
    for (int p = to_planes; p < from_planes; ++p) {
      std::fill(channels[p], channels[p] + num_samples, 0.0f);
    }
  }

  void reset() override {}

 private:
  sonare::ChannelLayout from_;
  sonare::ChannelLayout to_;
  int block_ = 0;
  std::vector<float> scratch_;
};

class BusNode final : public sonare::rt::ProcessorBase {
 public:
  struct SidechainInput {
    unsigned int insert_index = 0;
    int left_port = 0;
    int right_port = 0;
  };

  // @p bus, @p panner and @p eq are borrowed, not owned: they live in
  // SonareMixer::bus_dsp and outlive every graph rebuild, exactly as StripNode
  // borrows its ChannelStrip. The trim/width/polarity members below are
  // per-compile like StripNode's own, and carry no state the invariant covers.
  BusNode(sonare::mixing::FxBus* bus, sonare::mixing::PannerProcessor* panner,
          sonare::mastering::eq::ParametricEq* eq, std::atomic<bool>* eq_enabled,
          float input_trim_db, float width, bool polarity_invert_left, bool polarity_invert_right,
          int planes, std::vector<SidechainInput> sidechain_inputs = {})
      : bus_(bus),
        panner_(panner),
        eq_(eq),
        eq_enabled_(eq_enabled),
        input_trim_({input_trim_db, 5.0f}),
        width_(width, 5.0f),
        polarity_left_(polarity_invert_left ? -1.0f : 1.0f),
        polarity_right_(polarity_invert_right ? -1.0f : 1.0f),
        planes_(planes),
        sidechain_inputs_(std::move(sidechain_inputs)) {}

  // The borrowed FxBus, panner and EQ are deliberately NOT prepared here,
  // mirroring StripNode::prepare: BusProcessor::prepare re-prepares every
  // insert, which clears the delay lines and filter state this node exists to
  // preserve. They are prepared once where their record is created, before the
  // bus's inserts are added, exactly as a strip is prepared in
  // sonare_mixer_add_strip_ex. Only the per-compile members below are prepared.
  void prepare(double sample_rate, int max_block_size) override {
    input_trim_.prepare(sample_rate, max_block_size);
    width_.prepare(sample_rate, max_block_size);
  }

  void process(float* const* channels, int, int num_samples) override {
    // Match TrackMixerRuntime's scene-bus signal order exactly:
    // trim -> front-pair polarity -> EQ -> inserts -> pan -> front-pair stereo
    // width, over the bus's planes (pan on a stereo bus only).
    input_trim_.process(channels, planes_, num_samples);
    if (channels[0] != nullptr && polarity_left_ < 0.0f) {
      for (int i = 0; i < num_samples; ++i) channels[0][i] *= polarity_left_;
    }
    if (channels[1] != nullptr && polarity_right_ < 0.0f) {
      for (int i = 0; i < num_samples; ++i) channels[1][i] *= polarity_right_;
    }
    if (eq_enabled_->load(std::memory_order_relaxed) && eq_has_active_band(*eq_)) {
      eq_->process(channels, planes_, num_samples);
    }
    bus_->clear_insert_sidechains();
    for (const auto& input : sidechain_inputs_) {
      const float* key[2] = {channels[input.left_port], channels[input.right_port]};
      bus_->set_insert_sidechain(input.insert_index, key, 2, num_samples);
    }
    bus_->process(channels, planes_, num_samples);
    if (planes_ == 2 && !panner_->at_rest_identity()) {
      panner_->process(channels, 2, num_samples);
    }
    if (width_.width() != 1.0f || width_.current_width() != 1.0f) {
      width_.process(channels, 2, num_samples);
    }
  }

  void reset() override {
    input_trim_.reset();
    bus_->reset();
    panner_->reset();
    eq_->reset();
    width_.reset();
  }
  int latency_samples() const noexcept override { return bus_->latency_samples(); }
  int latency_samples_q8() const noexcept override { return bus_->latency_samples_q8(); }
  int tail_samples() const noexcept override { return bus_->tail_samples(); }
  sonare::mixing::MeterSnapshot meter_snapshot() const noexcept {
    return bus_->bus().meter_snapshot();
  }

 private:
  sonare::mixing::FxBus* bus_;               // borrowed; owned by SonareMixer::bus_dsp
  sonare::mixing::PannerProcessor* panner_;  // borrowed; owned by SonareMixer::bus_dsp
  sonare::mastering::eq::ParametricEq* eq_;  // borrowed; owned by SonareMixer::bus_dsp
  std::atomic<bool>* eq_enabled_;            // borrowed; owned by SonareMixer::bus_dsp
  sonare::mixing::GainProcessor input_trim_;
  sonare::mixing::StereoWidthProcessor width_;
  float polarity_left_;
  float polarity_right_;
  int planes_;
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

SonareError sonare_mixer_bus_non_finite_discard_count(const SonareMixer* mixer, const char* bus_id,
                                                      uint32_t* out_count) {
  // A single relaxed atomic load once the bus is found, so it is safe to poll
  // from the audio thread alongside the meters; it touches no diagnostic string.
  SONARE_C_RT_API_ENTRY;
  if (!mixer || !bus_id || bus_id[0] == '\0' || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
  // Read from the bus record rather than the compiled node: the record outlives
  // every graph rebuild, so the count survives a recompile triggered by an
  // unrelated strip edit, and an uncompiled mixer can still be asked.
  for (const auto& bus : mixer->bus_dsp) {
    if (bus && bus->id == bus_id) {
      *out_count = bus->fx.non_finite_discard_count();
      return SONARE_OK;
    }
  }
  return SONARE_ERROR_INVALID_PARAMETER;
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

// The persistent DSP for @p bus_id, created empty (default pan, no EQ bands) on
// first use. A declared bus already has its record with its inserts, pan and EQ
// built by sonare_mixer_from_scene_json; this creates one for the implicit
// master and for an aux bus a send destination names, neither of which can
// carry a scene spec.
SonareBusDsp& bus_dsp_for(SonareMixer* mixer, const std::string& bus_id) {
  for (const auto& entry : mixer->bus_dsp) {
    if (entry->id == bus_id) return *entry;
  }
  auto entry = std::make_unique<SonareBusDsp>();
  entry->id = bus_id;
  // Prepared once, here, because BusNode::prepare deliberately leaves them alone.
  entry->fx.prepare(static_cast<double>(mixer->sample_rate), mixer->max_block_size);
  entry->panner.prepare(static_cast<double>(mixer->sample_rate), mixer->max_block_size);
  entry->eq.prepare(static_cast<double>(mixer->sample_rate), mixer->max_block_size);
  entry->eq.prepare_channels(sonare::channel_count(sonare::ChannelLayout::SevenPointOne));
  SonareBusDsp& dsp = *entry;
  mixer->bus_dsp.push_back(std::move(entry));
  return dsp;
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

  // The master's layout is the widest output it may be built at.
  for (const auto& bus : buses) {
    if (bus.id == master_id &&
        mixer->output_channels > std::max(2, sonare::channel_count(bus.layout))) {
      throw SonareException(ErrorCode::InvalidParameter,
                            "mixer output width exceeds the master layout: " + master_id);
    }
  }

  // Plane widths. The master runs at the render's output width; a surround bus
  // at its layout's; every other bus (mono included) and every strip input at 2.
  std::unordered_map<std::string, int> input_planes;
  for (const auto& bus : buses) {
    const int layout_planes = sonare::channel_count(bus.layout);
    input_planes[bus.id] = bus.id == master_id ? mixer->output_channels
                           : sonare::is_surround_channel_count(layout_planes) ? layout_planes
                                                                              : 2;
  }
  for (const auto& strip : mixer->strips) {
    input_planes[strip->id] = 2;
  }
  // A strip scatters when its widest main destination has more than two planes.
  std::unordered_map<std::string, int> scatter_planes_by_id;
  for (const auto& strip : mixer->strips) {
    int widest = 0;
    bool routed = false;
    for (const auto& conn : mixer->connections) {
      if (conn.source != strip->id) continue;
      routed = true;
      const auto it = input_planes.find(conn.destination);
      if (it != input_planes.end()) widest = std::max(widest, it->second);
    }
    if (!routed && strip->id != master_id) widest = input_planes[master_id];
    scatter_planes_by_id[strip->id] =
        strip->surround_scatter && sonare::is_surround_channel_count(widest) ? widest : 0;
  }

  // Bus nodes: post-sum insert chains live inside FxBus/BusProcessor.
  std::unordered_map<std::string, std::vector<BusNode::SidechainInput>> bus_sidechain_inputs_by_id;
  std::unordered_map<std::string, std::vector<std::string>> bus_sidechain_keys_by_id;
  for (const auto& bus : buses) {
    // The DSP is looked up, never rebuilt: its inserts were constructed once at
    // scene-apply time (sonare_mixer_from_scene_json), and an implicit bus -- the
    // synthesized master, or an aux a send destination created -- gets an empty
    // record here. Constructing it in this loop is what used to throw away every
    // bus insert's state on an unrelated strip edit.
    SonareBusDsp& dsp = bus_dsp_for(mixer, bus.id);
    sonare::mixing::FxBus& fx_bus = dsp.fx;
    const int planes = input_planes[bus.id];
    fx_bus.set_channel_layout(sonare::layout_from_channel_count(planes));
    int next_sidechain_port = planes;
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
    auto node = std::make_unique<BusNode>(
        &fx_bus, &dsp.panner, &dsp.eq, &dsp.eq_enabled, bus.input_trim_db, bus.width,
        bus.polarity_invert_left, bus.polarity_invert_right, planes, std::move(sidechain_inputs));
    if (!graph.add_node(bus.id, std::move(node), next_sidechain_port)) {
      throw SonareException(ErrorCode::InvalidParameter, "duplicate or invalid bus id: " + bus.id);
    }
  }

  // Strip nodes: 2 main ports, the scatter planes, 2 ports per send tap, keys.
  std::unordered_map<std::string, SonareStrip*> strip_by_id;
  std::unordered_map<std::string, std::vector<StripNode::SidechainInput>> sidechain_inputs_by_id;
  std::unordered_map<std::string, std::vector<std::string>> sidechain_keys_by_id;
  for (const auto& strip : mixer->strips) {
    const int num_sends = static_cast<int>(strip->strip.num_sends());
    const int scatter_planes = scatter_planes_by_id[strip->id];
    // A stereo build records its width the way the engine's stereo block does, so
    // a later surround build opens at placement instead of gliding from old gains.
    if (scatter_planes == 0) strip->surround.reset();
    int next_sidechain_port = 2 + scatter_planes + 2 * num_sends;
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
    auto node =
        std::make_unique<StripNode>(strip.get(), scatter_planes, num_sends,
                                    mixer->timeline_sample_pos, std::move(sidechain_inputs));
    if (!graph.add_node(strip->id, std::move(node), num_ports)) {
      throw SonareException(ErrorCode::InvalidParameter,
                            "duplicate or invalid strip id: " + strip->id);
    }
    strip_by_id[strip->id] = strip.get();
  }

  // An output span: the node, its first port and how many planes it carries.
  struct Span {
    std::string node;
    int first_port = 0;
    int planes = 2;
  };
  auto main_output = [&](const std::string& node_id) -> Span {
    const auto scatter = scatter_planes_by_id.find(node_id);
    if (scatter != scatter_planes_by_id.end()) {
      return scatter->second > 0 ? Span{node_id, 2, scatter->second} : Span{node_id, 0, 2};
    }
    const auto planes = input_planes.find(node_id);
    return {node_id, 0, planes == input_planes.end() ? 2 : planes->second};
  };
  // Narrows @p source to @p planes through a DownmixNode shared by every edge
  // that asks for the same fold of the same span.
  std::unordered_map<std::string, Span> downmix_by_key;
  auto narrowed = [&](const Span& source, int planes) -> Span {
    if (source.planes <= planes) return source;
    const std::string key =
        source.node + "#" + std::to_string(source.first_port) + ">" + std::to_string(planes);
    const auto found = downmix_by_key.find(key);
    if (found != downmix_by_key.end()) return found->second;
    const std::string id = "__sonare_downmix__/" + key;
    auto node = std::make_unique<DownmixNode>(sonare::layout_from_channel_count(source.planes),
                                              sonare::layout_from_channel_count(planes));
    if (!graph.add_node(id, std::move(node), source.planes)) {
      throw SonareException(ErrorCode::InvalidParameter, "duplicate downmix node: " + id);
    }
    for (int p = 0; p < source.planes; ++p) {
      checked_connect(
          {source.node, source.first_port + p, id, p, sonare::graph::Connection::Mix::Add});
    }
    audio_inputs_by_id[id].push_back(source.node);
    const Span folded{id, 0, planes};
    downmix_by_key.emplace(key, folded);
    return folded;
  };
  // Audio edge: same width plane for plane, a wider destination takes the
  // matching planes only, a narrower one receives the downmix.
  auto connect_audio = [&](const Span& source, const std::string& destination) {
    const Span from = narrowed(source, input_planes[destination]);
    for (int p = 0; p < from.planes; ++p) {
      checked_connect(
          {from.node, from.first_port + p, destination, p, sonare::graph::Connection::Mix::Add});
    }
    audio_inputs_by_id[destination].push_back(from.node);
  };
  // Sidechain edge: always two planes, folded down when the source is wider.
  auto connect_key = [&](const std::string& key_source, const std::string& destination,
                         int left_port, int right_port) {
    const bool is_strip = strip_by_id.count(key_source) != 0;
    const Span from = narrowed(is_strip ? Span{key_source, 0, 2} : main_output(key_source), 2);
    checked_connect(
        {from.node, from.first_port, destination, left_port, sonare::graph::Connection::Mix::Add});
    checked_connect({from.node, from.first_port + 1, destination, right_port,
                     sonare::graph::Connection::Mix::Add});
  };

  // Main connections. Track which strips have an explicit outgoing main edge so
  // the rest can default-route to master.
  std::unordered_map<std::string, bool> has_main_out;
  for (const auto& conn : mixer->connections) {
    if (graph.node(conn.source) == nullptr || graph.node(conn.destination) == nullptr) {
      throw SonareException(
          ErrorCode::InvalidParameter,
          "connection references unknown node: " + conn.source + " -> " + conn.destination);
    }
    connect_audio(main_output(conn.source), conn.destination);
    has_main_out[conn.source] = true;
  }

  // Default-route strips with no outgoing main connection to the master bus.
  for (const auto& strip : mixer->strips) {
    if (!has_main_out[strip->id] && strip->id != master_id) {
      connect_audio(main_output(strip->id), master_id);
    }
  }

  // Default-route only implicit buses created by the manual send API. Explicit
  // scene buses keep their authored topology, including intentionally unpatched
  // aux/submix buses.
  for (const auto& bus : buses) {
    if (is_implicit_bus[bus.id] && !has_main_out[bus.id] && bus.id != master_id) {
      connect_audio(main_output(bus.id), master_id);
    }
  }

  // Explicit scene buses are allowed to remain unpatched. Do not report that as
  // last_error on a successful compile: last_error is reserved for failing C API
  // calls, and stale warning text after SONARE_OK breaks callers that check it
  // only on error.

  // Send taps: strip send output ports -> destination bus input ports.
  for (const auto& strip : mixer->strips) {
    const auto& sends = strip->scene_strip.sends;
    const int send_base = 2 + scatter_planes_by_id[strip->id];
    for (size_t s = 0; s < sends.size(); ++s) {
      const std::string& dest = sends[s].destination_bus_id;
      if (!is_bus.count(dest)) {
        throw SonareException(ErrorCode::InvalidParameter, "send destination is not a bus: " +
                                                               dest + " (strip " + strip->id + ")");
      }
      connect_audio({strip->id, send_base + 2 * static_cast<int>(s), 2}, dest);
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
      connect_key(key_source, strip->id, inputs[index].left_port, inputs[index].right_port);
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
      connect_key(key_source, bus.id, inputs[index].left_port, inputs[index].right_port);
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
