#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "graph/connection.h"
#include "graph/graph.h"
#include "mastering/api/insert_factory.h"
#include "mixing/api/presets.h"
#include "mixing/api/scene.h"
#include "mixing/channel_strip.h"
#include "mixing/fx_bus.h"
#include "mixing/send.h"
#include "rt/processor_base.h"
#include "sonare_c.h"
#include "sonare_c_internal.h"

namespace {

// Pin the sample-accurate mixing automation curve ordinal mapping used by
// sonare_strip_schedule_*_automation. As of the AutomationCurve unification
// this canonical scheme (0=Linear, 1=Exponential, 2=Hold, 3=SCurve) is shared
// with the engine PPQ-domain SonareAutomationPoint.curve_to_next path and with
// the Node / Python / WASM AutomationCurve enums. The canonical enum lives at
// sonare::AutomationCurve in src/util/automation_curve.h.
static_assert(static_cast<int>(sonare::mixing::AutomationCurveType::Linear) == 0,
              "sonare::mixing::AutomationCurveType::Linear must be ordinal 0 to keep "
              "sonare_strip_schedule_*_automation curve ABI stable");
static_assert(static_cast<int>(sonare::mixing::AutomationCurveType::Exponential) == 1,
              "sonare::mixing::AutomationCurveType::Exponential must be ordinal 1");
static_assert(static_cast<int>(sonare::mixing::AutomationCurveType::Hold) == 2,
              "sonare::mixing::AutomationCurveType::Hold must be ordinal 2");
static_assert(static_cast<int>(sonare::mixing::AutomationCurveType::SCurve) == 3,
              "sonare::mixing::AutomationCurveType::SCurve must be ordinal 3");

bool parse_automation_curve(int curve, sonare::mixing::AutomationCurveType* out) noexcept {
  if (!out) {
    return false;
  }
  if (curve < 0 || curve > 3) {
    return false;
  }
  *out = static_cast<sonare::mixing::AutomationCurveType>(curve);
  return true;
}

sonare::mixing::PanMode to_pan_mode(int mode) {
  switch (mode) {
    case SONARE_PAN_MODE_BALANCE:
      return sonare::mixing::PanMode::Balance;
    case SONARE_PAN_MODE_STEREO_PAN:
      return sonare::mixing::PanMode::StereoPan;
    case SONARE_PAN_MODE_DUAL_PAN:
      return sonare::mixing::PanMode::DualPan;
    default:
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "unknown mixing pan mode");
  }
}

sonare::mixing::PanLaw to_pan_law(int law) {
  switch (law) {
    case 0:
      return sonare::mixing::PanLaw::Const3dB;
    case 1:
      return sonare::mixing::PanLaw::Const4p5dB;
    case 2:
      return sonare::mixing::PanLaw::Const6dB;
    case 3:
      return sonare::mixing::PanLaw::Linear0dB;
    default:
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "unknown mixing pan law");
  }
}

int from_pan_law(sonare::mixing::PanLaw law) {
  switch (law) {
    case sonare::mixing::PanLaw::Const3dB:
      return 0;
    case sonare::mixing::PanLaw::Const4p5dB:
      return 1;
    case sonare::mixing::PanLaw::Const6dB:
      return 2;
    case sonare::mixing::PanLaw::Linear0dB:
      return 3;
  }
  return 0;
}

int from_pan_mode(sonare::mixing::PanMode mode) {
  switch (mode) {
    case sonare::mixing::PanMode::Balance:
      return SONARE_PAN_MODE_BALANCE;
    case sonare::mixing::PanMode::StereoPan:
      return SONARE_PAN_MODE_STEREO_PAN;
    case sonare::mixing::PanMode::DualPan:
      return SONARE_PAN_MODE_DUAL_PAN;
  }
  return SONARE_PAN_MODE_BALANCE;
}

sonare::mixing::SendTiming to_send_timing(int timing) {
  switch (timing) {
    case SONARE_SEND_TIMING_PRE_FADER:
      return sonare::mixing::SendTiming::PreFader;
    case SONARE_SEND_TIMING_POST_FADER:
      return sonare::mixing::SendTiming::PostFader;
    default:
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "unknown mixing send timing");
  }
}

sonare::mixing::SendTiming to_send_timing(sonare::mixing::api::SendTiming timing) {
  return timing == sonare::mixing::api::SendTiming::PreFader
             ? sonare::mixing::SendTiming::PreFader
             : sonare::mixing::SendTiming::PostFader;
}

sonare::mixing::api::SendTiming to_api_send_timing(int timing) {
  switch (timing) {
    case SONARE_SEND_TIMING_PRE_FADER:
      return sonare::mixing::api::SendTiming::PreFader;
    case SONARE_SEND_TIMING_POST_FADER:
      return sonare::mixing::api::SendTiming::PostFader;
    default:
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "unknown mixing send timing");
  }
}

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

void copy_meter_snapshot(const sonare::mixing::MeterSnapshot& snapshot,
                         SonareMixMeterSnapshot* out) {
  out->peak_db_l = snapshot.peak_db[0];
  out->peak_db_r = snapshot.peak_db[1];
  out->rms_db_l = snapshot.rms_db[0];
  out->rms_db_r = snapshot.rms_db[1];
  out->correlation = snapshot.correlation;
  out->mono_compat_width = snapshot.mono_compat_width;
  out->mono_compat_peak = snapshot.mono_compat_peak;
  out->mono_compat_side_rms = snapshot.mono_compat_side_rms;
  out->likely_mono_compatible = snapshot.likely_mono_compatible ? 1 : 0;
  out->momentary_lufs = snapshot.momentary_lufs;
  out->short_term_lufs = snapshot.short_term_lufs;
  out->integrated_lufs = snapshot.integrated_lufs;
  out->gain_reduction_db = snapshot.gain_reduction_db;
  out->true_peak_db_l = snapshot.true_peak_db[0];
  out->true_peak_db_r = snapshot.true_peak_db[1];
  out->max_true_peak_db = snapshot.max_true_peak_db;
  out->seq = snapshot.seq;
}

}  // namespace

struct SonareMixer;

struct SonareStrip {
  std::string id;
  sonare::mixing::api::Strip scene_strip;
  sonare::mixing::ChannelStrip strip;
  SonareMixer* owner = nullptr;  // borrowed back-pointer for dirty propagation
};

struct SonareMixer {
  int sample_rate = 48000;
  int max_block_size = 0;
  std::vector<std::unique_ptr<SonareStrip>> strips;
  std::vector<sonare::mixing::api::Bus> buses;
  std::vector<sonare::mixing::api::VcaGroup> vca_groups;
  std::vector<sonare::mixing::api::Connection> connections;
  std::string master_id;
  sonare::graph::Graph graph;
  bool compiled_dirty = true;
  std::vector<float> scratch_left;
  std::vector<float> scratch_right;
};

namespace {

void apply_solo_mutes(SonareMixer* mixer) {
  bool any_solo = false;
  for (const auto& strip : mixer->strips) {
    any_solo = any_solo || strip->strip.soloed();
  }
  for (const auto& strip : mixer->strips) {
    strip->strip.set_implied_mute(any_solo && !strip->strip.soloed() && !strip->strip.solo_safe());
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
  mixer->compiled_dirty = false;
}

}  // namespace

SonareMixer* sonare_mixer_create(int sample_rate, int max_block_size) {
  if (sample_rate <= 0 || max_block_size <= 0) {
    return nullptr;
  }
  try {
    auto* mixer = new SonareMixer;
    mixer->sample_rate = sample_rate;
    mixer->max_block_size = max_block_size;
    mixer->scratch_left.assign(static_cast<size_t>(max_block_size), 0.0f);
    mixer->scratch_right.assign(static_cast<size_t>(max_block_size), 0.0f);
    return mixer;
  } catch (const std::exception& e) {
    sonare_c_detail::set_last_error(e.what());
    return nullptr;
  } catch (...) {
    sonare_c_detail::set_last_error("Unknown C++ exception (non-std::exception type)");
    return nullptr;
  }
}

SonareStrip* sonare_mixer_add_strip(SonareMixer* mixer, const char* id) {
  if (!mixer) {
    return nullptr;
  }
  try {
    auto strip = std::make_unique<SonareStrip>();
    strip->id = id != nullptr ? id : "";
    strip->scene_strip.id = strip->id;
    strip->owner = mixer;
    strip->strip.prepare(static_cast<double>(mixer->sample_rate), mixer->max_block_size);
    SonareStrip* raw = strip.get();
    mixer->strips.push_back(std::move(strip));
    mixer->compiled_dirty = true;
    return raw;
  } catch (const std::exception& e) {
    sonare_c_detail::set_last_error(e.what());
    return nullptr;
  } catch (...) {
    sonare_c_detail::set_last_error("Unknown C++ exception (non-std::exception type)");
    return nullptr;
  }
}

SonareError sonare_strip_set_fader_db(SonareStrip* strip, float db) {
  if (!strip) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  strip->strip.set_fader_db(db);
  strip->scene_strip.fader_db = db;
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_strip_set_input_trim_db(SonareStrip* strip, float db) {
  if (!strip) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  strip->strip.set_input_trim_db(db);
  strip->scene_strip.input_trim_db = db;
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_strip_set_pan(SonareStrip* strip, float pan, int pan_mode) {
  if (!strip) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  // to_pan_mode throws SONARE_ERROR_INVALID_PARAMETER for unknown modes, so by
  // the time we mirror it into the scene strip the value is already validated.
  strip->strip.set_pan_mode(to_pan_mode(pan_mode));
  strip->strip.set_pan(pan);
  strip->scene_strip.pan = pan;
  strip->scene_strip.pan_mode = pan_mode;
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_strip_set_dual_pan(SonareStrip* strip, float left_pan, float right_pan) {
  if (!strip) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  strip->strip.set_pan_mode(sonare::mixing::PanMode::DualPan);
  strip->strip.set_dual_pan(left_pan, right_pan);
  strip->scene_strip.pan = 0.5f * (left_pan + right_pan);
  strip->scene_strip.pan_mode = SONARE_PAN_MODE_DUAL_PAN;
  strip->scene_strip.dual_pan_left = std::clamp(left_pan, -1.0f, 1.0f);
  strip->scene_strip.dual_pan_right = std::clamp(right_pan, -1.0f, 1.0f);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_strip_set_width(SonareStrip* strip, float width) {
  if (!strip) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  strip->strip.set_width(width);
  strip->scene_strip.width = width;
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_strip_set_muted(SonareStrip* strip, int muted) {
  if (!strip) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  strip->strip.set_muted(muted != 0);
  strip->scene_strip.muted = muted != 0;
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_strip_set_soloed(SonareStrip* strip, int soloed) {
  if (!strip) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  strip->strip.set_soloed(soloed != 0);
  strip->scene_strip.soloed = soloed != 0;
  // Solo state is consumed live via implied_mute in process(); recompute the
  // implied mutes across the mixer so the change takes effect on the next
  // process without a full graph recompile.
  if (strip->owner != nullptr) {
    apply_solo_mutes(strip->owner);
  }
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_strip_set_solo_safe(SonareStrip* strip, int solo_safe) {
  if (!strip) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  strip->strip.set_solo_safe(solo_safe != 0);
  strip->scene_strip.solo_safe = solo_safe != 0;
  // Solo-safe strips are excluded from implied mutes; recompute live.
  if (strip->owner != nullptr) {
    apply_solo_mutes(strip->owner);
  }
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_strip_set_polarity_invert(SonareStrip* strip, int invert_left,
                                             int invert_right) {
  if (!strip) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  strip->strip.set_polarity_invert(invert_left != 0, invert_right != 0);
  strip->scene_strip.polarity_invert_left = invert_left != 0;
  strip->scene_strip.polarity_invert_right = invert_right != 0;
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_strip_set_pan_law(SonareStrip* strip, int pan_law) {
  if (!strip) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  strip->strip.set_pan_law(to_pan_law(pan_law));  // validates pan_law (throws on unknown)
  strip->scene_strip.pan_law = pan_law;
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_strip_set_channel_delay_samples(SonareStrip* strip, int delay_samples) {
  if (!strip) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  strip->strip.set_channel_delay_samples(delay_samples);
  strip->scene_strip.channel_delay_samples = delay_samples;
  // Channel delay changes the strip's reported latency; mark the graph dirty so
  // latency compensation re-runs at the next compile.
  if (strip->owner != nullptr) {
    strip->owner->compiled_dirty = true;
  }
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_strip_set_vca_offset_db(SonareStrip* strip, float offset_db) {
  if (!strip) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  // VCA offset is a group concept with no per-strip scene field; only the live
  // value is updated (not cached to scene_strip).
  strip->strip.set_vca_offset_db(offset_db);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_strip_add_send(SonareStrip* strip, const char* id,
                                  const char* destination_bus_id, float send_db, int timing,
                                  size_t* index_out) {
  if (!strip || !destination_bus_id) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  sonare::mixing::SendConfig config;
  config.send_db = send_db;
  config.timing = to_send_timing(timing);
  const size_t index = strip->strip.add_send(config);

  sonare::mixing::api::Send send;
  send.id = id != nullptr ? id : "";
  send.destination_bus_id = destination_bus_id;
  send.send_db = send_db;
  send.timing = to_api_send_timing(timing);
  strip->scene_strip.sends.push_back(std::move(send));
  if (strip->owner != nullptr) {
    strip->owner->compiled_dirty = true;  // send count changes node port layout
  }

  if (index_out != nullptr) {
    *index_out = index;
  }
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_strip_set_send_db(SonareStrip* strip, size_t index, float send_db) {
  if (!strip) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  strip->strip.set_send_db(index, send_db);
  if (index < strip->scene_strip.sends.size()) {
    strip->scene_strip.sends[index].send_db = send_db;
  }
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_strip_meter(const SonareStrip* strip, SonareMixMeterSnapshot* out) {
  if (!strip || !out) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  copy_meter_snapshot(strip->strip.meter_snapshot(), out);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_strip_meter_tap(const SonareStrip* strip, int tap, SonareMixMeterSnapshot* out) {
  if (!strip || !out) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  sonare::mixing::TapPoint tap_point;
  switch (tap) {
    case SONARE_METER_TAP_PRE_FADER:
      tap_point = sonare::mixing::TapPoint::PreFader;
      break;
    case SONARE_METER_TAP_POST_FADER:
      tap_point = sonare::mixing::TapPoint::PostFader;
      break;
    default:
      return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  copy_meter_snapshot(strip->strip.meter_snapshot(tap_point), out);
  return SONARE_OK;
  SONARE_C_CATCH
}

size_t sonare_strip_read_goniometer_latest(const SonareStrip* strip, SonareMixGoniometerPoint* out,
                                           size_t max_points) {
  if (!strip || !out || max_points == 0) {
    return 0;
  }
  // SonareMixGoniometerPoint and sonare::mixing::GoniometerPoint are both
  // standard-layout {float left; float right;} pairs, so we can write the
  // result directly into the caller's buffer instead of allocating a temporary
  // vector on every (potentially per-frame, UI-rate) call. The static_asserts
  // pin the layout so this stays valid if either struct changes.
  using NativePoint = sonare::mixing::GoniometerPoint;
  static_assert(sizeof(SonareMixGoniometerPoint) == sizeof(NativePoint),
                "GoniometerPoint size must match the C ABI struct");
  static_assert(offsetof(SonareMixGoniometerPoint, left) == offsetof(NativePoint, left),
                "GoniometerPoint 'left' offset must match the C ABI struct");
  static_assert(offsetof(SonareMixGoniometerPoint, right) == offsetof(NativePoint, right),
                "GoniometerPoint 'right' offset must match the C ABI struct");
  // read_goniometer_latest is noexcept, so no try/catch is required here.
  return strip->strip.read_goniometer_latest(reinterpret_cast<NativePoint*>(out), max_points);
}

size_t sonare_mixer_strip_count(const SonareMixer* mixer) {
  if (!mixer) {
    return 0;
  }
  return mixer->strips.size();
}

SonareError sonare_mixer_get_strip_count(const SonareMixer* mixer, size_t* out_count) {
  if (!mixer || !out_count) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  *out_count = mixer->strips.size();
  return SONARE_OK;
}

SonareError sonare_mixer_add_bus(SonareMixer* mixer, const char* id, const char* role) {
  if (!mixer || !id) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  const std::string bus_id = id;
  for (const auto& bus : mixer->buses) {
    if (bus.id == bus_id) {
      return SONARE_ERROR_INVALID_PARAMETER;  // duplicate bus id
    }
  }
  mixer->buses.emplace_back(bus_id, role != nullptr ? std::string(role) : std::string("aux"));
  mixer->compiled_dirty = true;
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mixer_remove_bus(SonareMixer* mixer, const char* id) {
  if (!mixer || !id) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  const std::string bus_id = id;
  const auto before = mixer->buses.size();
  mixer->buses.erase(
      std::remove_if(mixer->buses.begin(), mixer->buses.end(),
                     [&](const sonare::mixing::api::Bus& bus) { return bus.id == bus_id; }),
      mixer->buses.end());
  if (mixer->buses.size() == before) {
    return SONARE_ERROR_INVALID_PARAMETER;  // no such bus
  }
  mixer->compiled_dirty = true;
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mixer_bus_count(const SonareMixer* mixer, size_t* out_count) {
  if (!mixer || !out_count) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  *out_count = mixer->buses.size();
  return SONARE_OK;
}

SonareError sonare_mixer_add_vca_group(SonareMixer* mixer, const char* id, float gain_db,
                                       const char* const* members, size_t member_count) {
  if (!mixer || !id || (member_count > 0 && !members)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  const std::string group_id = id;
  for (const auto& group : mixer->vca_groups) {
    if (group.id == group_id) {
      return SONARE_ERROR_INVALID_PARAMETER;  // duplicate group id
    }
  }
  sonare::mixing::api::VcaGroup group;
  group.id = group_id;
  group.gain_db = gain_db;
  group.members.reserve(member_count);
  for (size_t i = 0; i < member_count; ++i) {
    if (!members[i]) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    group.members.emplace_back(members[i]);
  }
  // Apply the group's gain offset to the live ChannelStrip of each member, the
  // same control-only path scene load uses (last group touching a strip wins).
  for (const auto& member : group.members) {
    for (const auto& strip : mixer->strips) {
      if (strip->id == member) {
        strip->strip.set_vca_offset_db(gain_db);
        break;
      }
    }
  }
  mixer->vca_groups.push_back(std::move(group));
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mixer_remove_vca_group(SonareMixer* mixer, const char* id) {
  if (!mixer || !id) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  const std::string group_id = id;
  const auto it =
      std::find_if(mixer->vca_groups.begin(), mixer->vca_groups.end(),
                   [&](const sonare::mixing::api::VcaGroup& g) { return g.id == group_id; });
  if (it == mixer->vca_groups.end()) {
    return SONARE_ERROR_INVALID_PARAMETER;  // no such group
  }
  // Clear the live gain offset on the removed group's members.
  for (const auto& member : it->members) {
    for (const auto& strip : mixer->strips) {
      if (strip->id == member) {
        strip->strip.set_vca_offset_db(0.0f);
        break;
      }
    }
  }
  mixer->vca_groups.erase(it);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mixer_vca_group_count(const SonareMixer* mixer, size_t* out_count) {
  if (!mixer || !out_count) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  *out_count = mixer->vca_groups.size();
  return SONARE_OK;
}

SonareStrip* sonare_mixer_strip_at(SonareMixer* mixer, size_t index) {
  if (!mixer || index >= mixer->strips.size()) {
    return nullptr;
  }
  return mixer->strips[index].get();
}

SonareStrip* sonare_mixer_strip_by_id(SonareMixer* mixer, const char* id) {
  if (!mixer || !id) {
    return nullptr;
  }
  for (const auto& strip : mixer->strips) {
    if (strip->id == id) {
      return strip.get();
    }
  }
  return nullptr;
}

SonareError sonare_strip_schedule_insert_automation(SonareStrip* strip, unsigned int insert_index,
                                                    unsigned int param_id, int64_t sample_pos,
                                                    float value, int curve) {
  if (!strip) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  sonare::mixing::AutomationCurveType curve_enum;
  if (!parse_automation_curve(curve, &curve_enum)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  // ChannelStrip::schedule_insert_automation silently drops out-of-range indices
  // at apply time (it only returns false when its event lane is full), so bound
  // insert_index against the combined [pre... post...] insert count here to give
  // callers an explicit error.
  const size_t insert_count = strip->strip.num_pre_inserts() + strip->strip.num_post_inserts();
  if (insert_index >= insert_count) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  // Mirror ChannelStrip::schedule_insert_automation's gross param-id bound
  // (kMaxReasonableParamId) so an out-of-range param id is reported as a bad
  // argument rather than being conflated with the full-lane capacity condition
  // below (the underlying call returns a bare bool for both).
  constexpr unsigned int kMaxReasonableParamId = 65535u;
  if (param_id > kMaxReasonableParamId) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  // After the index/param-id bounds above, a false return is dominated by the
  // capacity condition (the event lane is full): map it to OUT_OF_MEMORY so
  // callers can distinguish "lane full" from a bad argument.
  if (!strip->strip.schedule_insert_automation(insert_index, param_id, sample_pos, value,
                                               curve_enum)) {
    return SONARE_ERROR_OUT_OF_MEMORY;
  }
  return SONARE_OK;
}

SonareError sonare_strip_schedule_fader_automation(SonareStrip* strip, int64_t sample_pos,
                                                   float fader_db, int curve) {
  if (!strip) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  sonare::mixing::AutomationCurveType curve_enum;
  if (!parse_automation_curve(curve, &curve_enum)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  // A false return is a full-lane capacity condition, not a bad argument.
  if (!strip->strip.schedule_fader_automation(sample_pos, fader_db, curve_enum)) {
    return SONARE_ERROR_OUT_OF_MEMORY;
  }
  return SONARE_OK;
}

SonareError sonare_strip_schedule_pan_automation(SonareStrip* strip, int64_t sample_pos, float pan,
                                                 int curve) {
  if (!strip) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  sonare::mixing::AutomationCurveType curve_enum;
  if (!parse_automation_curve(curve, &curve_enum)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  // A false return is a full-lane capacity condition, not a bad argument.
  if (!strip->strip.schedule_pan_automation(sample_pos, pan, curve_enum)) {
    return SONARE_ERROR_OUT_OF_MEMORY;
  }
  return SONARE_OK;
}

SonareError sonare_strip_schedule_width_automation(SonareStrip* strip, int64_t sample_pos,
                                                   float width, int curve) {
  if (!strip) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  sonare::mixing::AutomationCurveType curve_enum;
  if (!parse_automation_curve(curve, &curve_enum)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  // A false return is a full-lane capacity condition, not a bad argument.
  if (!strip->strip.schedule_width_automation(sample_pos, width, curve_enum)) {
    return SONARE_ERROR_OUT_OF_MEMORY;
  }
  return SONARE_OK;
}

SonareError sonare_strip_schedule_send_automation(SonareStrip* strip, size_t send_index,
                                                  int64_t sample_pos, float db, int curve) {
  if (!strip) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  sonare::mixing::AutomationCurveType curve_enum;
  if (!parse_automation_curve(curve, &curve_enum)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  // Validate the send index up front so a bad argument is reported distinctly
  // from a capacity condition (mirrors the insert-automation convention above).
  if (send_index >= strip->strip.num_sends()) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  // After the index bound above, a false return is dominated by the capacity
  // condition (the send lane is full): map it to OUT_OF_MEMORY so callers can
  // distinguish "lane full" from a bad argument, matching the fader/pan/width
  // automation functions.
  if (!strip->strip.schedule_send_automation(send_index, sample_pos, db, curve_enum)) {
    return SONARE_ERROR_OUT_OF_MEMORY;
  }
  return SONARE_OK;
}

SonareMixer* sonare_mixer_from_scene_json(const char* json, int sample_rate, int max_block_size) {
  if (!json) {
    return nullptr;
  }
  try {
    const auto scene = sonare::mixing::api::scene_from_json(json);
    std::unique_ptr<SonareMixer> mixer(sonare_mixer_create(sample_rate, max_block_size));
    if (!mixer) {
      return nullptr;
    }
    for (const auto& scene_strip : scene.strips) {
      SonareStrip* strip = sonare_mixer_add_strip(mixer.get(), scene_strip.id.c_str());
      if (!strip) {
        return nullptr;
      }
      strip->scene_strip = scene_strip;
      strip->strip.set_input_trim_db(scene_strip.input_trim_db);
      strip->strip.set_fader_db(scene_strip.fader_db);
      strip->strip.set_pan(scene_strip.pan);
      strip->strip.set_width(scene_strip.width);
      strip->strip.set_muted(scene_strip.muted);
      strip->strip.set_soloed(scene_strip.soloed);
      strip->strip.set_solo_safe(scene_strip.solo_safe);
      strip->strip.set_pan_mode(to_pan_mode(scene_strip.pan_mode));
      strip->strip.set_dual_pan(scene_strip.dual_pan_left, scene_strip.dual_pan_right);
      strip->strip.set_pan_law(to_pan_law(scene_strip.pan_law));
      strip->strip.set_polarity_invert(scene_strip.polarity_invert_left,
                                       scene_strip.polarity_invert_right);
      strip->strip.set_channel_delay_samples(scene_strip.channel_delay_samples);
      for (const auto& insert : scene_strip.inserts) {
        auto processor =
            sonare::mastering::api::make_insert(insert.processor_name, insert.params_json);
        if (!processor) {
          throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                        "unknown insert processor: " + insert.processor_name +
                                            " (strip " + scene_strip.id + ")");
        }
        if (insert.slot == sonare::mixing::api::InsertSlot::PreFader) {
          strip->strip.add_pre_insert(std::move(processor));
        } else {
          strip->strip.add_post_insert(std::move(processor));
        }
      }
      for (const auto& send : scene_strip.sends) {
        sonare::mixing::SendConfig config;
        config.send_db = send.send_db;
        config.timing = to_send_timing(send.timing);
        strip->strip.add_send(config);
      }
    }

    // Store routing topology. If the scene defines no master bus, synthesize one
    // so strips default-route to it.
    mixer->buses = scene.buses;
    mixer->vca_groups = scene.vca_groups;
    mixer->connections = scene.connections;
    bool has_master = false;
    for (const auto& bus : mixer->buses) {
      if (bus.role == "master" || bus.id == "master") {
        has_master = true;
        break;
      }
    }
    if (!has_master) {
      mixer->buses.push_back({"master", "master"});
    }

    // VCA group offsets (control-only; last group touching a strip wins).
    for (const auto& group : scene.vca_groups) {
      for (const auto& member : group.members) {
        for (const auto& strip : mixer->strips) {
          if (strip->id == member) {
            strip->strip.set_vca_offset_db(group.gain_db);
            break;
          }
        }
      }
    }

    apply_solo_mutes(mixer.get());
    build_and_compile(mixer.get());
    return mixer.release();
  } catch (const std::exception& e) {
    sonare_c_detail::set_last_error(e.what());
    return nullptr;
  } catch (...) {
    sonare_c_detail::set_last_error("Unknown C++ exception (non-std::exception type)");
    return nullptr;
  }
}

SonareError sonare_mixer_to_scene_json(const SonareMixer* mixer, char** json_out) {
  if (!mixer || !json_out) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  sonare::mixing::api::Scene scene;
  scene.buses = mixer->buses;
  scene.vca_groups = mixer->vca_groups;
  if (scene.buses.empty()) {
    scene.buses.push_back({"master", "master"});
  }
  scene.connections = mixer->connections;
  for (const auto& strip : mixer->strips) {
    sonare::mixing::api::Strip scene_strip = strip->scene_strip;
    scene_strip.id = strip->id;
    scene_strip.input_trim_db = strip->strip.input_trim_db();
    scene_strip.fader_db = strip->strip.fader_db();
    scene_strip.pan = strip->strip.pan();
    scene_strip.width = strip->strip.width();
    scene_strip.muted = strip->strip.muted();
    scene_strip.soloed = strip->strip.soloed();
    scene_strip.solo_safe = strip->strip.solo_safe();
    scene_strip.pan_mode = from_pan_mode(strip->strip.pan_mode());
    scene_strip.pan_law = from_pan_law(strip->strip.pan_law());
    // ChannelStrip exposes no dual-pan getters; reuse the cached scene values.
    scene_strip.dual_pan_left = strip->scene_strip.dual_pan_left;
    scene_strip.dual_pan_right = strip->scene_strip.dual_pan_right;
    scene_strip.polarity_invert_left = strip->strip.polarity_invert_left();
    scene_strip.polarity_invert_right = strip->strip.polarity_invert_right();
    scene_strip.channel_delay_samples = strip->strip.channel_delay_samples();
    scene.strips.push_back(std::move(scene_strip));
  }
  *json_out = sonare_c_detail::copy_string(sonare::mixing::api::scene_to_json(scene));
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mixer_compile(SonareMixer* mixer) {
  if (!mixer) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  build_and_compile(mixer);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mixer_process_stereo(SonareMixer* mixer, const float* const* input_left,
                                        const float* const* input_right, size_t input_count,
                                        float* output_left, float* output_right,
                                        size_t num_samples) {
  if (!mixer || !output_left || !output_right || (!input_left && input_count > 0) ||
      (!input_right && input_count > 0) || input_count > mixer->strips.size() ||
      num_samples > static_cast<size_t>(mixer->max_block_size)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (num_samples == 0) {
    return SONARE_OK;
  }

  SONARE_C_TRY
  std::fill(output_left, output_left + num_samples, 0.0f);
  std::fill(output_right, output_right + num_samples, 0.0f);

  // Lazy compile: rebuild the routing graph if topology changed since the last
  // process/compile. Acceptable to allocate here (offline/block convenience entry).
  if (mixer->compiled_dirty) {
    build_and_compile(mixer);
  }

  const int n = static_cast<int>(num_samples);
  mixer->graph.clear_inputs(n);
  const size_t count = std::min(input_count, mixer->strips.size());
  for (size_t index = 0; index < count; ++index) {
    if (!input_left[index] || !input_right[index]) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    const std::string& id = mixer->strips[index]->id;
    mixer->graph.set_input(id, 0, input_left[index], n);
    mixer->graph.set_input(id, 1, input_right[index], n);
  }

  mixer->graph.process_block(n);

  const float* master_l = mixer->graph.output(mixer->master_id, 0);
  const float* master_r = mixer->graph.output(mixer->master_id, 1);
  if (master_l != nullptr && master_r != nullptr) {
    std::copy(master_l, master_l + num_samples, output_left);
    std::copy(master_r, master_r + num_samples, output_right);
  }
  return SONARE_OK;
  SONARE_C_CATCH
}

const char* sonare_mixing_scene_preset_names(void) {
  // thread_local (not plain static): each call reassigns the storage and returns
  // a borrowed pointer into it. A plain static would let a concurrent caller
  // reassign the string and invalidate another thread's returned pointer. Per
  // the C-ABI contract this pointer is borrowed (callers must not free it) and
  // is valid until the next call ON THE SAME THREAD.
  static thread_local std::string storage;
  return sonare_c_detail::join_names(sonare::mixing::api::scene_preset_names(), storage);
}

SonareError sonare_mixing_scene_preset_json(const char* preset_name, char** json_out) {
  if (!preset_name || !json_out) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  *json_out = nullptr;
  const auto preset = sonare::mixing::api::scene_preset_from_string(preset_name);
  const auto scene = sonare::mixing::api::scene_preset(preset);
  *json_out = sonare_c_detail::copy_string(sonare::mixing::api::scene_to_json(scene));
  return SONARE_OK;
  SONARE_C_CATCH
}

void sonare_mixer_destroy(SonareMixer* mixer) { delete mixer; }
