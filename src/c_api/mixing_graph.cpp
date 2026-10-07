#include <algorithm>

#include "c_api/mixing_internal.h"
#include "mixing/downmix.h"
#include "mixing/gain.h"
#include "mixing/solo_mute.h"
#include "mixing/stereo_width.h"
#include "mixing/tail_planner.h"

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
// as separate output ports. Ports [0, I) carry the processed main signal at the
// strip's input width I (2, or a surround bus's width for the strip behind it);
// a stereo strip whose main destination is wider scatters it into the next W
// ports through the surround panner; then I ports per send tap; sidechain
// inputs come last. The strip is owned and prepared externally (by SonareStrip).
class StripNode final : public sonare::rt::ProcessorBase {
 public:
  struct SidechainInput {
    unsigned int insert_index = 0;
    int left_port = 0;
    int right_port = 0;
  };

  StripNode(SonareStrip* owner, int input_planes, int scatter_planes, int num_sends,
            int64_t sample_pos, std::vector<SidechainInput> sidechain_inputs = {})
      : strip_(&owner->strip),
        surround_(&owner->surround),
        surround_prepared_(&owner->surround_prepared),
        input_planes_(input_planes),
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
    (void)num_channels;  // Node passes num_ports; the main path spans the input planes.
    strip_->clear_insert_sidechains();
    for (const auto& input : sidechain_inputs_) {
      const float* key[2] = {channels[input.left_port], channels[input.right_port]};
      strip_->set_insert_sidechain(input.insert_index, key, 2, num_samples);
    }
    strip_->process_at(channels, input_planes_, num_samples, sample_pos_);
    if (scatter_planes_ > 0) {
      float* const* scatter = channels + input_planes_;
      for (int p = 0; p < scatter_planes_; ++p) {
        std::fill(scatter[p], scatter[p] + num_samples, 0.0f);
      }
      surround_->set_params(strip_->surround_pan_params());
      const float* main[2] = {channels[0], channels[1]};
      surround_->process_add(main, 2, scatter, scatter_planes_, num_samples);
    }
    const int send_base = input_planes_ + scatter_planes_;
    for (int s = 0; s < num_sends_; ++s) {
      float* const* dst = channels + send_base + input_planes_ * s;
      for (int p = 0; p < input_planes_; ++p) std::fill(dst[p], dst[p] + num_samples, 0.0f);
      strip_->mix_send_at(static_cast<size_t>(s), dst, input_planes_, num_samples,
                          sample_pos_);  // additive
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
  int input_tap_latency_samples_q8(int input_port) const noexcept override {
    for (const auto& input : sidechain_inputs_) {
      if (input.left_port == input_port || input.right_port == input_port) {
        return strip_->insert_input_latency_samples_q8(input.insert_index).value_or(0);
      }
    }
    return 0;
  }
  int output_tail_samples(int output_port) const noexcept override {
    const int send_base = input_planes_ + scatter_planes_;
    if (output_port >= send_base) {
      return strip_->send_tail_samples(
          static_cast<size_t>((output_port - send_base) / input_planes_));
    }
    return strip_->tail_samples();
  }
  bool input_port_audible(int input_port) const noexcept override {
    for (const auto& input : sidechain_inputs_) {
      if (input.left_port == input_port || input.right_port == input_port) {
        const sonare::rt::ProcessorBase* insert = strip_->insert_processor(input.insert_index);
        return insert != nullptr && insert->sidechain_audible();
      }
    }
    return true;
  }
  int output_latency_samples_q8(int output_port) const noexcept override {
    const int send_base = input_planes_ + scatter_planes_;
    if (output_port >= send_base) {
      const int send_index = (output_port - send_base) / input_planes_;
      return strip_->send_latency_samples_q8(static_cast<size_t>(send_index));
    }
    return strip_->post_fader_latency_samples_q8();
  }

 private:
  sonare::mixing::ChannelStrip* strip_;                // borrowed; owned by SonareStrip
  sonare::mixing::SurroundPannerProcessor* surround_;  // borrowed; owned by SonareStrip
  bool* surround_prepared_;                            // borrowed; owned by SonareStrip
  int input_planes_;
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

// Carries a strip's external block input into the graph, so the input reaches
// the strip through an edge the arrival plan compensates like any other.
class InputNode final : public sonare::rt::ProcessorBase {
 public:
  void prepare(double, int) override {}
  void process(float* const*, int, int) override {}
  void reset() override {}
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
  int input_tap_latency_samples_q8(int input_port) const noexcept override {
    for (const auto& input : sidechain_inputs_) {
      if (input.left_port == input_port || input.right_port == input_port) {
        return bus_->insert_input_latency_samples_q8(input.insert_index).value_or(0);
      }
    }
    return 0;
  }
  bool input_port_audible(int input_port) const noexcept override {
    for (const auto& input : sidechain_inputs_) {
      if (input.left_port == input_port || input.right_port == input_port) {
        const sonare::rt::ProcessorBase* insert = bus_->insert_processor(input.insert_index);
        return insert != nullptr && insert->sidechain_audible();
      }
    }
    return true;
  }
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
  // at its layout's; every other bus (mono included) at 2. A strip runs at the
  // widest source feeding it -- a surround bus's width for the strip behind it,
  // 2 otherwise -- resolved to a fixed point so a chain of strips carries it on.
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
  for (bool widened = true; widened;) {
    widened = false;
    for (const auto& strip : mixer->strips) {
      int widest = input_planes[strip->id];
      for (const auto& conn : mixer->connections) {
        if (conn.destination != strip->id) continue;
        const auto source = input_planes.find(conn.source);
        if (source != input_planes.end() && sonare::is_surround_channel_count(source->second)) {
          widest = std::max(widest, source->second);
        }
      }
      if (widest != input_planes[strip->id]) {
        input_planes[strip->id] = widest;
        widened = true;
      }
    }
  }
  // A stereo strip scatters when its widest main destination has more than two
  // planes; a strip already running at a surround width does not, and a wide
  // strip destination takes the stereo signal on its front pair instead.
  std::unordered_map<std::string, bool> is_strip;
  for (const auto& strip : mixer->strips) {
    is_strip[strip->id] = true;
  }
  std::unordered_map<std::string, int> scatter_planes_by_id;
  for (const auto& strip : mixer->strips) {
    if (input_planes[strip->id] > 2) {
      scatter_planes_by_id[strip->id] = 0;
      continue;
    }
    int widest = 0;
    bool routed = false;
    for (const auto& conn : mixer->connections) {
      if (conn.source != strip->id) continue;
      routed = true;
      if (is_strip.count(conn.destination) != 0 && input_planes[conn.destination] > 2) continue;
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

  // Strip nodes: the main ports, the scatter planes, one main width per send
  // tap, keys.
  std::unordered_map<std::string, std::vector<StripNode::SidechainInput>> sidechain_inputs_by_id;
  std::unordered_map<std::string, std::vector<std::string>> sidechain_keys_by_id;
  for (const auto& strip : mixer->strips) {
    const int num_sends = static_cast<int>(strip->strip.num_sends());
    const int main_planes = input_planes[strip->id];
    const int scatter_planes = scatter_planes_by_id[strip->id];
    // A stereo build records its width the way the engine's stereo block does, so
    // a later surround build opens at placement instead of gliding from old gains.
    if (scatter_planes == 0) strip->surround.reset();
    int next_sidechain_port = main_planes + scatter_planes + main_planes * num_sends;
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
        std::make_unique<StripNode>(strip.get(), main_planes, scatter_planes, num_sends,
                                    mixer->timeline_sample_pos, std::move(sidechain_inputs));
    if (!graph.add_node(strip->id, std::move(node), num_ports)) {
      throw SonareException(ErrorCode::InvalidParameter,
                            "duplicate or invalid strip id: " + strip->id);
    }
  }

  // An output span: the node, its first port and how many planes it carries.
  struct Span {
    std::string node;
    int first_port = 0;
    int planes = 2;
  };
  // The main signal before any scatter: where a key taps it, and what a strip
  // destination receives.
  auto unscattered = [&](const std::string& node_id) -> Span {
    const auto planes = input_planes.find(node_id);
    return {node_id, 0, planes == input_planes.end() ? 2 : planes->second};
  };
  auto main_output = [&](const std::string& node_id) -> Span {
    const Span main = unscattered(node_id);
    const auto scatter = scatter_planes_by_id.find(node_id);
    if (scatter != scatter_planes_by_id.end() && scatter->second > 0) {
      return {node_id, main.planes, scatter->second};
    }
    return main;
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
  };
  // Sidechain edge: always two planes, tapped before any scatter and folded
  // down when the source runs wider.
  auto connect_key = [&](const std::string& key_source, const std::string& destination,
                         int left_port, int right_port) {
    const Span from = narrowed(unscattered(key_source), 2);
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
    const bool wide_strip_destination =
        is_strip.count(conn.destination) != 0 && input_planes[conn.destination] > 2;
    const Span source =
        wide_strip_destination ? unscattered(conn.source) : main_output(conn.source);
    connect_audio(source, conn.destination);
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

  // A strip's external block input enters through its own node, so the arrival
  // plan delays it like any edge. A strip a main connection feeds takes that
  // edge's signal on its main ports instead and has no input node.
  std::unordered_map<std::string, bool> has_main_in;
  for (const auto& conn : mixer->connections) {
    has_main_in[conn.destination] = true;
  }
  std::vector<std::string> input_node_ids;
  input_node_ids.reserve(mixer->strips.size());
  for (const auto& strip : mixer->strips) {
    if (has_main_in[strip->id]) {
      input_node_ids.push_back(strip->id);
      continue;
    }
    const std::string id = "__sonare_input__/" + strip->id;
    if (!graph.add_node(id, std::make_unique<InputNode>(), 2)) {
      throw SonareException(ErrorCode::InvalidParameter, "duplicate input node: " + id);
    }
    for (int p = 0; p < 2; ++p) {
      checked_connect({id, p, strip->id, p, sonare::graph::Connection::Mix::Add});
    }
    input_node_ids.push_back(id);
  }

  // Explicit scene buses are allowed to remain unpatched. Do not report that as
  // last_error on a successful compile: last_error is reserved for failing C API
  // calls, and stale warning text after SONARE_OK breaks callers that check it
  // only on error.

  // Send taps: strip send output ports -> destination bus input ports.
  for (const auto& strip : mixer->strips) {
    const auto& sends = strip->scene_strip.sends;
    const int send_planes = input_planes[strip->id];
    const int send_base = send_planes + scatter_planes_by_id[strip->id];
    for (size_t s = 0; s < sends.size(); ++s) {
      const std::string& dest = sends[s].destination_bus_id;
      if (!is_bus.count(dest)) {
        throw SonareException(ErrorCode::InvalidParameter, "send destination is not a bus: " +
                                                               dest + " (strip " + strip->id + ")");
      }
      connect_audio({strip->id, send_base + send_planes * static_cast<int>(s), send_planes}, dest);
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

  const sonare::graph::Node* master_node = graph.node(master_id);
  if (master_node == nullptr) {
    throw SonareException(ErrorCode::InvalidState, "mixer master node missing after compile");
  }
  const int master_latency_q8 =
      graph.node_latency_samples_q8(master_id) + master_node->processor().latency_samples_q8();

  graph.adopt_connection_state(mixer->graph);
  mixer->graph = std::move(graph);
  mixer->input_node_ids = std::move(input_node_ids);
  mixer->master_id = std::move(master_id);
  mixer->latency_samples = std::max(0, master_latency_q8 >> 8);
  mixer->compiled_dirty = false;
}

int tail_samples(SonareMixer* mixer) {
  if (mixer->compiled_dirty) {
    build_and_compile(mixer);
  }
  const sonare::graph::Graph& graph = mixer->graph;
  const auto& ids = graph.topo_order_ids();
  std::unordered_map<std::string, size_t> index_of;
  for (size_t i = 0; i < ids.size(); ++i) index_of.emplace(ids[i], i);
  sonare::mixing::MixerTailPlanner planner(ids.size());
  planner.reserve(graph.connection_count());
  for (size_t c = 0; c < graph.connection_count(); ++c) {
    const sonare::graph::Connection& edge = graph.connection(c);
    if (!graph.node(edge.dest_node)->processor().input_port_audible(edge.dest_port)) continue;
    planner.add_path(
        index_of.at(edge.source_node), index_of.at(edge.dest_node),
        graph.node(edge.source_node)->processor().output_tail_samples(edge.source_port));
  }
  return planner.leaving(index_of.at(mixer->master_id),
                         graph.node(mixer->master_id)->processor().output_tail_samples(0));
}

}  // namespace sonare_c_mixing_detail
