/// @file mixing.cpp
/// @brief Embind bindings for scene-based and one-shot mixing APIs.

#ifdef __EMSCRIPTEN__

#include "common.h"

val js_mixing_scene_preset_names() {
  val out = val::array();
  auto names = mixing::api::scene_preset_names();
  for (size_t index = 0; index < names.size(); ++index) {
    out.call<void>("push", names[index]);
  }
  return out;
}

std::string js_mixing_scene_preset_json(std::string preset_name) {
  return mixing::api::scene_to_json(
      mixing::api::scene_preset(mixing::api::scene_preset_from_string(preset_name)));
}

// ---------------------------------------------------------------------------
// MixerWasm: persistent scene-based mixer wrapper around the C mixer API
// (sonare_mixer_*). Owns a SonareMixer* built from a scene JSON string, routes
// strips through the compiled routing graph, and sums to a stereo master.
//
// processStereo takes planar inputs: leftChannels[i] / rightChannels[i] are the
// L/R Float32Array for strip i (matching mixStereo's input layout). It returns
// { left, right, sampleRate } with the mixed stereo master. Call delete() (or
// use try/finally) to release the underlying WASM object.
// ---------------------------------------------------------------------------
#if defined(SONARE_WITH_MIXING) && defined(SONARE_WITH_GRAPH)
class MixerWasm {
 public:
  MixerWasm(SonareMixer* mixer, int sample_rate, int block_size)
      : mixer_(mixer), sample_rate_(sample_rate), block_size_(block_size) {
    if (block_size_ <= 0) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "mixer block size must be positive");
    }
    const size_t strip_count = sonare_mixer_strip_count(mixer_);
    left_scratch_.resize(strip_count);
    right_scratch_.resize(strip_count);
    left_ptrs_.resize(strip_count);
    right_ptrs_.resize(strip_count);
    for (size_t index = 0; index < strip_count; ++index) {
      left_scratch_[index].resize(static_cast<size_t>(block_size_));
      right_scratch_[index].resize(static_cast<size_t>(block_size_));
      left_ptrs_[index] = left_scratch_[index].data();
      right_ptrs_[index] = right_scratch_[index].data();
    }
    out_scratch_left_.resize(static_cast<size_t>(block_size_));
    out_scratch_right_.resize(static_cast<size_t>(block_size_));
  }

  ~MixerWasm() {
    if (mixer_ != nullptr) {
      sonare_mixer_destroy(mixer_);
      mixer_ = nullptr;
    }
  }

  MixerWasm(const MixerWasm&) = delete;
  MixerWasm& operator=(const MixerWasm&) = delete;

  static MixerWasm* fromSceneJson(std::string json, int sample_rate, int block_size) {
    SonareMixer* mixer = sonare_mixer_from_scene_json(json.c_str(), sample_rate, block_size);
    if (mixer == nullptr) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidState,
          std::string("failed to build mixer from scene JSON: ") + sonare_last_error_message());
    }
    // Capture any non-fatal load warning (e.g. insert params no processor read)
    // before any later C-ABI call can overwrite the thread-local message.
    std::string warning = sonare_last_warning_message();
    auto* wrapped = new MixerWasm(mixer, sample_rate, block_size);
    wrapped->scene_warning_ = std::move(warning);
    return wrapped;
  }

  // Non-fatal warnings captured when this mixer was built from scene JSON, one
  // entry per insert handed param keys it does not read; empty when all consumed.
  val sceneWarnings() const {
    val out = val::array();
    if (scene_warning_.empty()) {
      return out;
    }
    size_t start = 0;
    while (start <= scene_warning_.size()) {
      size_t end = scene_warning_.find('\n', start);
      if (end == std::string::npos) {
        out.call<void>("push", scene_warning_.substr(start));
        break;
      }
      out.call<void>("push", scene_warning_.substr(start, end - start));
      start = end + 1;
    }
    return out;
  }

  static std::string presetJson(std::string name) {
    char* json = nullptr;
    SonareError err = sonare_mixing_scene_preset_json(name.c_str(), &json);
    if (err != SONARE_OK || json == nullptr) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidState,
          std::string("failed to get mixing scene preset JSON: ") + sonare_error_message(err));
    }
    std::string out(json);
    sonare_free_string(json);
    return out;
  }

  void compile() {
    SonareError err = sonare_mixer_compile(mixer_);
    if (err != SONARE_OK) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidState,
          std::string("failed to compile mixer graph: ") + sonare_error_message(err));
    }
  }

  size_t stripCount() const { return sonare_mixer_strip_count(mixer_); }

  // Schedules sample-accurate insert-parameter automation on the strip at
  // strip_index. insert_index addresses the strip's combined insert sequence
  // [pre-inserts... post-inserts...]. param_id is processor-specific. sample_pos
  // is in absolute samples from the start of processing. curve: 0 = Linear,
  // 1 = Exponential.
  void scheduleInsertAutomation(unsigned int strip_index, unsigned int insert_index,
                                unsigned int param_id, double sample_pos, float value, int curve) {
    SonareStrip* strip = sonare_mixer_strip_at(mixer_, static_cast<size_t>(strip_index));
    if (strip == nullptr) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                    "mixer strip index out of range");
    }
    SonareError err = sonare_strip_schedule_insert_automation(
        strip, insert_index, param_id, static_cast<int64_t>(sample_pos), value, curve);
    if (err != SONARE_OK) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidState,
          std::string("failed to schedule insert automation: ") + sonare_error_message(err));
    }
  }

  // Borrowed strip handle by index in [0, stripCount()). Throws if out of range.
  // The handle is owned by the mixer; do not free it.
  SonareStrip* stripAt(unsigned int strip_index) {
    SonareStrip* strip = sonare_mixer_strip_at(mixer_, static_cast<size_t>(strip_index));
    if (strip == nullptr) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                    "mixer strip index out of range");
    }
    return strip;
  }

  // Sets the strip's input trim in dB.
  void setInputTrimDb(unsigned int strip_index, float db) {
    checkStripError(sonare_strip_set_input_trim_db(stripAt(strip_index), db),
                    "failed to set input trim");
  }

  // Sets the strip's fader level in dB.
  void setFaderDb(unsigned int strip_index, float db) {
    checkStripError(sonare_strip_set_fader_db(stripAt(strip_index), db), "failed to set fader");
  }

  // Sets the strip's pan position. pan_mode is the SONARE_PAN_MODE_* ordinal;
  // pass SONARE_PAN_MODE_KEEP (-1) to keep the strip's current pan mode (e.g. a
  // scene-defined mode) on a plain pan nudge.
  void setPan(unsigned int strip_index, float pan, int pan_mode) {
    checkStripError(sonare_strip_set_pan(stripAt(strip_index), pan, pan_mode), "failed to set pan");
  }

  // Sets the strip's stereo width.
  void setWidth(unsigned int strip_index, float width) {
    checkStripError(sonare_strip_set_width(stripAt(strip_index), width), "failed to set width");
  }

  // Sets the strip's mute state.
  void setMuted(unsigned int strip_index, bool muted) {
    checkStripError(sonare_strip_set_muted(stripAt(strip_index), muted ? 1 : 0),
                    "failed to set muted");
  }

  // Sets the strip's solo state. Takes effect on the next process without a
  // graph recompile.
  void setSoloed(unsigned int strip_index, bool soloed) {
    checkStripError(sonare_strip_set_soloed(stripAt(strip_index), soloed ? 1 : 0),
                    "failed to set soloed");
  }

  // Marks a strip as solo-safe so it is never implied-muted by another strip's
  // solo. Takes effect on the next process without a graph recompile.
  void setSoloSafe(unsigned int strip_index, bool solo_safe) {
    checkStripError(sonare_strip_set_solo_safe(stripAt(strip_index), solo_safe ? 1 : 0),
                    "failed to set solo-safe");
  }

  // Inverts the polarity of the left and/or right channel.
  void setPolarityInvert(unsigned int strip_index, bool invert_left, bool invert_right) {
    checkStripError(sonare_strip_set_polarity_invert(stripAt(strip_index), invert_left ? 1 : 0,
                                                     invert_right ? 1 : 0),
                    "failed to set polarity invert");
  }

  // Sets the strip's pan law. pan_law: 0 = -3 dB, 1 = -4.5 dB, 2 = -6 dB,
  // 3 = linear (0 dB).
  void setPanLaw(unsigned int strip_index, int pan_law) {
    checkStripError(sonare_strip_set_pan_law(stripAt(strip_index), pan_law),
                    "failed to set pan law");
  }

  // Sets a per-strip channel delay in samples. This changes the strip's reported
  // latency; recompile to re-run latency compensation.
  void setChannelDelaySamples(unsigned int strip_index, int delay_samples) {
    checkStripError(sonare_strip_set_channel_delay_samples(stripAt(strip_index), delay_samples),
                    "failed to set channel delay samples");
  }

  // Sets the strip's live VCA gain offset in dB (not persisted to the scene).
  void setVcaOffsetDb(unsigned int strip_index, float offset_db) {
    checkStripError(sonare_strip_set_vca_offset_db(stripAt(strip_index), offset_db),
                    "failed to set VCA offset");
  }

  // Sets independent left/right pan positions (dual-pan mode).
  void setDualPan(unsigned int strip_index, float left_pan, float right_pan) {
    checkStripError(sonare_strip_set_dual_pan(stripAt(strip_index), left_pan, right_pan),
                    "failed to set dual pan");
  }

  // Sets the strip's surround pan from a JS object {azimuth, elevation,
  // divergence, lfe, distance}; absent/non-numeric fields fall back to the
  // centered point-source default.
  void setSurroundPan(unsigned int strip_index, val pan) {
    const auto field = [&](const char* key, float fallback) {
      val value = pan[key];
      return (!value.isUndefined() && !value.isNull() &&
              value.typeOf().as<std::string>() == "number")
                 ? value.as<float>()
                 : fallback;
    };
    SonareSurroundPan sp{};
    sp.azimuth = field("azimuth", 0.0f);
    sp.elevation = field("elevation", 0.0f);
    sp.divergence = field("divergence", 0.0f);
    sp.lfe = field("lfe", 0.0f);
    sp.distance = field("distance", 1.0f);
    checkStripError(sonare_strip_set_surround_pan(stripAt(strip_index), &sp),
                    "failed to set surround pan");
  }

  // Adds a post-construction send to the strip. timing: 0 = pre-fader,
  // 1 = post-fader. Returns the new send's index.
  size_t addSend(unsigned int strip_index, std::string id, std::string destination_bus_id,
                 float send_db, int timing) {
    size_t index = 0;
    checkStripError(sonare_strip_add_send(stripAt(strip_index), id.c_str(),
                                          destination_bus_id.c_str(), send_db, timing, &index),
                    "failed to add send");
    return index;
  }

  // Sets the send level (in dB) for an existing send by index.
  void setSendDb(unsigned int strip_index, size_t send_index, float send_db) {
    checkStripError(sonare_strip_set_send_db(stripAt(strip_index), send_index, send_db),
                    "failed to set send level");
  }

  // Removes the send at send_index (in add order) from the strip. Higher send
  // indices shift down by one after removal; recompile before processing.
  void removeSend(unsigned int strip_index, size_t send_index) {
    checkStripError(
        sonare_strip_remove_send(stripAt(strip_index), static_cast<unsigned int>(send_index)),
        "failed to remove send");
  }

  // Reads a meter snapshot at the given tap point. tap: 0 = pre-fader,
  // 1 = post-fader (see SonareMeterTap). Returns the full snapshot.
  val meterTap(unsigned int strip_index, int tap) {
    SonareMixMeterSnapshot snapshot{};
    checkStripError(sonare_strip_meter_tap(stripAt(strip_index), tap, &snapshot),
                    "failed to read meter tap");
    return mixMeterSnapshotToVal(snapshot);
  }

  // Reads the strip's current (post-fader) meter snapshot. Tap-less, mirroring
  // the Node/Python stripMeter contract which calls sonare_strip_meter; the
  // tap-selectable variant is meterTap.
  val stripMeter(unsigned int strip_index) {
    SonareMixMeterSnapshot snapshot{};
    checkStripError(sonare_strip_meter(stripAt(strip_index), &snapshot),
                    "failed to read strip meter");
    return mixMeterSnapshotToVal(snapshot);
  }

  // Schedules sample-accurate fader automation on a strip. sample_pos uses the
  // absolute-sample timeline; curve: 0 = Linear, 1 = Exponential.
  void scheduleFaderAutomation(unsigned int strip_index, double sample_pos, float fader_db,
                               int curve) {
    checkStripError(sonare_strip_schedule_fader_automation(
                        stripAt(strip_index), static_cast<int64_t>(sample_pos), fader_db, curve),
                    "failed to schedule fader automation");
  }

  void schedulePanAutomation(unsigned int strip_index, double sample_pos, float pan, int curve) {
    checkStripError(sonare_strip_schedule_pan_automation(
                        stripAt(strip_index), static_cast<int64_t>(sample_pos), pan, curve),
                    "failed to schedule pan automation");
  }

  void scheduleWidthAutomation(unsigned int strip_index, double sample_pos, float width,
                               int curve) {
    checkStripError(sonare_strip_schedule_width_automation(
                        stripAt(strip_index), static_cast<int64_t>(sample_pos), width, curve),
                    "failed to schedule width automation");
  }

  // Schedules sample-accurate send-level automation on a strip's send.
  void scheduleSendAutomation(unsigned int strip_index, size_t send_index, double sample_pos,
                              float db, int curve) {
    checkStripError(
        sonare_strip_schedule_send_automation(stripAt(strip_index), send_index,
                                              static_cast<int64_t>(sample_pos), db, curve),
        "failed to schedule send automation");
  }

  // Reads up to max_points of the strip's most recent goniometer samples.
  // Returns an array of { left, right } points (oldest to newest).
  val readGoniometerLatest(unsigned int strip_index, size_t max_points) {
    SonareStrip* strip = stripAt(strip_index);
    val out = val::array();
    if (max_points == 0) {
      return out;
    }
    std::vector<SonareMixGoniometerPoint> points(max_points);
    const size_t count = sonare_strip_read_goniometer_latest(strip, points.data(), max_points);
    for (size_t index = 0; index < count; ++index) {
      val point = val::object();
      point.set("left", points[index].left);
      point.set("right", points[index].right);
      out.call<void>("push", point);
    }
    return out;
  }

  // Resolves a strip's index from its id. Returns -1 when the id is not found;
  // the TS wrapper maps -1 to null for cross-binding consistency (Node returns
  // number | null).
  int stripById(std::string id) {
    const size_t count = sonare_mixer_strip_count(mixer_);
    SonareStrip* target = sonare_mixer_strip_by_id(mixer_, id.c_str());
    if (target == nullptr) {
      return -1;
    }
    for (size_t index = 0; index < count; ++index) {
      if (sonare_mixer_strip_at(mixer_, index) == target) {
        return static_cast<int>(index);
      }
    }
    return -1;
  }

  // Adds a bus to the mixer topology. role is one of "master", "aux", "submix"
  // (empty defaults to "aux"). Marks the routing graph dirty; call compile (or
  // process) to rebuild.
  void addBus(std::string id, std::string role) {
    SonareError err =
        sonare_mixer_add_bus(mixer_, id.c_str(), role.empty() ? nullptr : role.c_str());
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                    std::string("failed to add bus: ") + sonare_error_message(err));
    }
  }

  void removeBus(std::string id) {
    SonareError err = sonare_mixer_remove_bus(mixer_, id.c_str());
    if (err != SONARE_OK) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidState,
          std::string("failed to remove bus: ") + sonare_error_message(err));
    }
  }

  size_t busCount() const {
    size_t count = 0;
    SonareError err = sonare_mixer_bus_count(mixer_, &count);
    if (err != SONARE_OK) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidState,
          std::string("failed to read bus count: ") + sonare_error_message(err));
    }
    return count;
  }

  // Adds a VCA group with the given gain offset. members is an array of strip-id
  // strings (may be empty).
  void addVcaGroup(std::string id, float gain_db, val members) {
    std::vector<std::string> member_storage;
    std::vector<const char*> member_ptrs;
    if (!members.isUndefined() && !members.isNull()) {
      const int count = members["length"].as<int>();
      member_storage.reserve(static_cast<size_t>(count));
      member_ptrs.reserve(static_cast<size_t>(count));
      for (int i = 0; i < count; ++i) {
        member_storage.push_back(members[i].as<std::string>());
      }
      for (const auto& member : member_storage) {
        member_ptrs.push_back(member.c_str());
      }
    }
    SonareError err = sonare_mixer_add_vca_group(mixer_, id.c_str(), gain_db,
                                                 member_ptrs.empty() ? nullptr : member_ptrs.data(),
                                                 member_ptrs.size());
    if (err != SONARE_OK) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidState,
          std::string("failed to add VCA group: ") + sonare_error_message(err));
    }
  }

  void removeVcaGroup(std::string id) {
    SonareError err = sonare_mixer_remove_vca_group(mixer_, id.c_str());
    if (err != SONARE_OK) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidState,
          std::string("failed to remove VCA group: ") + sonare_error_message(err));
    }
  }

  void setVcaGroupGainDb(std::string id, float gain_db) {
    SonareError err = sonare_mixer_set_vca_group_gain_db(mixer_, id.c_str(), gain_db);
    if (err != SONARE_OK) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidState,
          std::string("failed to set VCA group gain: ") + sonare_error_message(err));
    }
  }

  size_t vcaGroupCount() const {
    size_t count = 0;
    SonareError err = sonare_mixer_vca_group_count(mixer_, &count);
    if (err != SONARE_OK) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidState,
          std::string("failed to read VCA group count: ") + sonare_error_message(err));
    }
    return count;
  }

  std::string toSceneJson() const {
    char* json = nullptr;
    SonareError err = sonare_mixer_to_scene_json(mixer_, &json);
    if (err != SONARE_OK || json == nullptr) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidState,
          std::string("failed to serialize mixer scene: ") + sonare_error_message(err));
    }
    std::string out(json);
    sonare_free_string(json);
    return out;
  }

  val processStereo(val left_channels, val right_channels) {
    const int count = left_channels["length"].as<int>();
    // Reject empty input to match the free js_mix_stereo contract: a zero-strip
    // call would derive a zero-length block and produce an empty master, which
    // is never a useful result. (There is no master-only path here.)
    if (count <= 0 || right_channels["length"].as<int>() != count) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidParameter,
          "leftChannels and rightChannels must have the same non-zero length");
    }

    std::vector<std::vector<float>> left_inputs;
    std::vector<std::vector<float>> right_inputs;
    left_inputs.reserve(static_cast<size_t>(count));
    right_inputs.reserve(static_cast<size_t>(count));

    size_t length = 0;
    for (int index = 0; index < count; ++index) {
      left_inputs.push_back(float32ArrayToVector(left_channels[index]));
      right_inputs.push_back(float32ArrayToVector(right_channels[index]));
      if (left_inputs.back().size() != right_inputs.back().size()) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                      "left and right channel lengths must match");
      }
      if (index == 0) {
        length = left_inputs.back().size();
      } else if (left_inputs.back().size() != length) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                      "all strips must have the same length");
      }
    }
    if (length > static_cast<size_t>(block_size_)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "block length exceeds the mixer's configured block size");
    }

    std::vector<const float*> left_ptrs(static_cast<size_t>(count));
    std::vector<const float*> right_ptrs(static_cast<size_t>(count));
    for (int index = 0; index < count; ++index) {
      left_ptrs[static_cast<size_t>(index)] = left_inputs[static_cast<size_t>(index)].data();
      right_ptrs[static_cast<size_t>(index)] = right_inputs[static_cast<size_t>(index)].data();
    }

    std::vector<float> out_left(length, 0.0f);
    std::vector<float> out_right(length, 0.0f);
    SonareError err = sonare_mixer_process_stereo(
        mixer_, count > 0 ? left_ptrs.data() : nullptr, count > 0 ? right_ptrs.data() : nullptr,
        static_cast<size_t>(count), out_left.data(), out_right.data(), length);
    if (err != SONARE_OK) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidState,
          std::string("mixer process failed: ") + sonare_error_message(err));
    }

    val out = val::object();
    out.set("left", vectorToFloat32Array(out_left));
    out.set("right", vectorToFloat32Array(out_right));
    out.set("sampleRate", sample_rate_);
    return out;
  }

  void processStereoInto(val left_channels, val right_channels, val out_left, val out_right) {
    const int count = left_channels["length"].as<int>();
    if (count < 0 || right_channels["length"].as<int>() != count) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "leftChannels and rightChannels must have the same length");
    }
    if (static_cast<size_t>(count) != left_scratch_.size()) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "input channel count must match the mixer's strip count");
    }

    const int length_i = out_left["length"].as<int>();
    if (length_i <= 0 || out_right["length"].as<int>() != length_i) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "output channels must have the same non-zero length");
    }
    const size_t length = static_cast<size_t>(length_i);
    if (length > static_cast<size_t>(block_size_)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "block length exceeds the mixer's configured block size");
    }

    for (int index = 0; index < count; ++index) {
      val left = left_channels[index];
      val right = right_channels[index];
      if (left["length"].as<int>() != length_i || right["length"].as<int>() != length_i) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                      "all input and output channels must have the same length");
      }
      auto& left_dest = left_scratch_[static_cast<size_t>(index)];
      auto& right_dest = right_scratch_[static_cast<size_t>(index)];
      for (size_t sample = 0; sample < length; ++sample) {
        left_dest[sample] = left[sample].as<float>();
        right_dest[sample] = right[sample].as<float>();
      }
    }

    SonareError err = sonare_mixer_process_stereo(
        mixer_, count > 0 ? left_ptrs_.data() : nullptr, count > 0 ? right_ptrs_.data() : nullptr,
        static_cast<size_t>(count), out_scratch_left_.data(), out_scratch_right_.data(), length);
    if (err != SONARE_OK) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidState,
          std::string("mixer process failed: ") + sonare_error_message(err));
    }
    for (size_t sample = 0; sample < length; ++sample) {
      out_left.set(sample, out_scratch_left_[sample]);
      out_right.set(sample, out_scratch_right_[sample]);
    }
  }

  val inputLeftView(size_t index) {
    if (index >= left_scratch_.size()) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "mixer input index out of range");
    }
    return val(typed_memory_view(static_cast<size_t>(block_size_), left_scratch_[index].data()));
  }

  val inputRightView(size_t index) {
    if (index >= right_scratch_.size()) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "mixer input index out of range");
    }
    return val(typed_memory_view(static_cast<size_t>(block_size_), right_scratch_[index].data()));
  }

  val outputLeftView() {
    return val(typed_memory_view(static_cast<size_t>(block_size_), out_scratch_left_.data()));
  }

  val outputRightView() {
    return val(typed_memory_view(static_cast<size_t>(block_size_), out_scratch_right_.data()));
  }

  void processPreparedStereo(size_t num_samples) {
    if (num_samples == 0 || num_samples > static_cast<size_t>(block_size_)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "invalid prepared mixer block length");
    }
    const size_t count = left_scratch_.size();
    SonareError err = sonare_mixer_process_stereo(
        mixer_, count > 0 ? left_ptrs_.data() : nullptr, count > 0 ? right_ptrs_.data() : nullptr,
        count, out_scratch_left_.data(), out_scratch_right_.data(), num_samples);
    if (err != SONARE_OK) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidState,
          std::string("mixer process failed: ") + sonare_error_message(err));
    }
  }

  // Reports the maximum processor tail length in the compiled mixer graph
  // (samples). Lazily compiles if the topology is dirty.
  int tailSamples() {
    int out = 0;
    SonareError err = sonare_mixer_tail_samples(mixer_, &out);
    if (err != SONARE_OK) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidState,
          std::string("failed to read mixer tail samples: ") + sonare_error_message(err));
    }
    return out;
  }

  // Drains delayed/tail audio by processing a zero-input block of num_samples
  // frames. Returns { left, right, sampleRate } mirroring processStereo.
  val drainTailStereo(size_t num_samples) {
    std::vector<float> out_left(num_samples, 0.0f);
    std::vector<float> out_right(num_samples, 0.0f);
    SonareError err =
        sonare_mixer_drain_tail_stereo(mixer_, out_left.data(), out_right.data(), num_samples);
    if (err != SONARE_OK) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidState,
          std::string("mixer drain tail failed: ") + sonare_error_message(err));
    }
    val out = val::object();
    out.set("left", vectorToFloat32Array(out_left));
    out.set("right", vectorToFloat32Array(out_right));
    out.set("sampleRate", sample_rate_);
    return out;
  }

 private:
  static void checkStripError(SonareError err, const char* what) {
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                    std::string(what) + ": " + sonare_error_message(err));
    }
  }

  static val mixMeterSnapshotToVal(const SonareMixMeterSnapshot& snapshot) {
    val out = val::object();
    out.set("peakDbL", snapshot.peak_db_l);
    out.set("peakDbR", snapshot.peak_db_r);
    out.set("rmsDbL", snapshot.rms_db_l);
    out.set("rmsDbR", snapshot.rms_db_r);
    out.set("correlation", snapshot.correlation);
    out.set("monoCompatWidth", snapshot.mono_compat_width);
    out.set("monoCompatPeak", snapshot.mono_compat_peak);
    out.set("monoCompatSideRms", snapshot.mono_compat_side_rms);
    out.set("likelyMonoCompatible", snapshot.likely_mono_compatible != 0);
    out.set("momentaryLufs", snapshot.momentary_lufs);
    out.set("shortTermLufs", snapshot.short_term_lufs);
    out.set("integratedLufs", snapshot.integrated_lufs);
    out.set("gainReductionDb", snapshot.gain_reduction_db);
    out.set("truePeakDbL", snapshot.true_peak_db_l);
    out.set("truePeakDbR", snapshot.true_peak_db_r);
    out.set("maxTruePeakDb", snapshot.max_true_peak_db);
    out.set("seq", static_cast<double>(snapshot.seq));
    return out;
  }

  SonareMixer* mixer_ = nullptr;
  int sample_rate_ = 48000;
  int block_size_ = 0;
  // Non-fatal warning captured at scene load (newline-joined; empty if none).
  std::string scene_warning_;
  std::vector<std::vector<float>> left_scratch_;
  std::vector<std::vector<float>> right_scratch_;
  std::vector<const float*> left_ptrs_;
  std::vector<const float*> right_ptrs_;
  std::vector<float> out_scratch_left_;
  std::vector<float> out_scratch_right_;
};

MixerWasm* createMixerFromSceneJson(std::string json, int sample_rate, int block_size) {
  return MixerWasm::fromSceneJson(std::move(json), sample_rate, block_size);
}
#endif  // SONARE_WITH_MIXING && SONARE_WITH_GRAPH

namespace {

val optionAt(val options, const char* key, int index) {
  if (!hasProperty(options, key)) {
    return val::undefined();
  }
  val value = options[key];
  if (val::global("Array").call<bool>("isArray", value)) {
    return value[index];
  }
  return value;
}

mixing::PanMode panModeFromVal(val value) {
  if (value.isUndefined() || value.isNull()) {
    return mixing::PanMode::Balance;
  }
  if (value.typeOf().as<std::string>() == "number") {
    const int mode = value.as<int>();
    if (mode == 1) return mixing::PanMode::StereoPan;
    if (mode == 2) return mixing::PanMode::DualPan;
    return mixing::PanMode::Balance;
  }
  if (value.typeOf().as<std::string>() != "string") {
    return mixing::PanMode::Balance;
  }
  std::string mode = value.as<std::string>();
  for (char& ch : mode) {
    if (ch == '_') ch = '-';
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  if (mode == "stereo-pan" || mode == "stereopan" || mode == "pan") {
    return mixing::PanMode::StereoPan;
  }
  if (mode == "dual-pan" || mode == "dualpan") {
    return mixing::PanMode::DualPan;
  }
  return mixing::PanMode::Balance;
}

val meterSnapshotToVal(const mixing::MeterSnapshot& snapshot) {
  val out = val::object();
  out.set("peakDbL", snapshot.peak_db[0]);
  out.set("peakDbR", snapshot.peak_db[1]);
  out.set("rmsDbL", snapshot.rms_db[0]);
  out.set("rmsDbR", snapshot.rms_db[1]);
  out.set("correlation", snapshot.correlation);
  out.set("monoCompatWidth", snapshot.mono_compat_width);
  out.set("monoCompatPeak", snapshot.mono_compat_peak);
  out.set("monoCompatSideRms", snapshot.mono_compat_side_rms);
  out.set("likelyMonoCompatible", snapshot.likely_mono_compatible);
  out.set("momentaryLufs", snapshot.momentary_lufs);
  out.set("shortTermLufs", snapshot.short_term_lufs);
  out.set("integratedLufs", snapshot.integrated_lufs);
  out.set("gainReductionDb", snapshot.gain_reduction_db);
  out.set("truePeakDbL", snapshot.true_peak_db[0]);
  out.set("truePeakDbR", snapshot.true_peak_db[1]);
  out.set("maxTruePeakDb", snapshot.max_true_peak_db);
  out.set("seq", static_cast<double>(snapshot.seq));
  return out;
}

#if defined(SONARE_WITH_MIXING) && defined(SONARE_WITH_GRAPH)
// Resolves a JS pan-mode value (number / string) to the SONARE_PAN_MODE_*
// ordinal accepted by sonare_strip_set_pan. Mirrors Node's PanModeValue so the
// real-graph mixStereo path behaves identically. Omitted / null defaults to
// Balance (mixStereo builds fresh strips, so there is no prior mode to keep).
int panModeOrdinalFromVal(val value) {
  if (value.isUndefined() || value.isNull()) {
    return SONARE_PAN_MODE_BALANCE;
  }
  if (value.typeOf().as<std::string>() == "number") {
    return value.as<int>();
  }
  if (value.typeOf().as<std::string>() != "string") {
    return SONARE_PAN_MODE_BALANCE;
  }
  std::string mode = value.as<std::string>();
  for (char& ch : mode) {
    if (ch == '_') ch = '-';
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  if (mode == "stereo-pan" || mode == "stereopan" || mode == "pan") {
    return SONARE_PAN_MODE_STEREO_PAN;
  }
  if (mode == "dual-pan" || mode == "dualpan") {
    return SONARE_PAN_MODE_DUAL_PAN;
  }
  return SONARE_PAN_MODE_BALANCE;
}

// Converts a C-ABI mix-meter snapshot to the JS meter object (same shape as
// meterSnapshotToVal / Node's MixMeterToObject).
val mixMeterSnapshotToValFree(const SonareMixMeterSnapshot& snapshot) {
  val out = val::object();
  out.set("peakDbL", snapshot.peak_db_l);
  out.set("peakDbR", snapshot.peak_db_r);
  out.set("rmsDbL", snapshot.rms_db_l);
  out.set("rmsDbR", snapshot.rms_db_r);
  out.set("correlation", snapshot.correlation);
  out.set("monoCompatWidth", snapshot.mono_compat_width);
  out.set("monoCompatPeak", snapshot.mono_compat_peak);
  out.set("monoCompatSideRms", snapshot.mono_compat_side_rms);
  out.set("likelyMonoCompatible", snapshot.likely_mono_compatible != 0);
  out.set("momentaryLufs", snapshot.momentary_lufs);
  out.set("shortTermLufs", snapshot.short_term_lufs);
  out.set("integratedLufs", snapshot.integrated_lufs);
  out.set("gainReductionDb", snapshot.gain_reduction_db);
  out.set("truePeakDbL", snapshot.true_peak_db_l);
  out.set("truePeakDbR", snapshot.true_peak_db_r);
  out.set("maxTruePeakDb", snapshot.max_true_peak_db);
  out.set("seq", static_cast<double>(snapshot.seq));
  return out;
}
#endif  // SONARE_WITH_MIXING && SONARE_WITH_GRAPH

}  // namespace

val js_mix_stereo(val left_channels, val right_channels, int sample_rate, val options) {
  const int count = left_channels["length"].as<int>();
  if (count <= 0 || right_channels["length"].as<int>() != count) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        "leftChannels and rightChannels must have the same non-zero length");
  }

  std::vector<std::vector<float>> left_inputs;
  std::vector<std::vector<float>> right_inputs;
  left_inputs.reserve(static_cast<size_t>(count));
  right_inputs.reserve(static_cast<size_t>(count));

  size_t length = 0;
  for (int index = 0; index < count; ++index) {
    left_inputs.push_back(float32ArrayToVector(left_channels[index]));
    right_inputs.push_back(float32ArrayToVector(right_channels[index]));
    if (left_inputs.back().size() != right_inputs.back().size()) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "left and right channel lengths must match");
    }
    if (index == 0) {
      length = left_inputs.back().size();
    } else if (left_inputs.back().size() != length) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "all strips must have the same length");
    }
  }

  std::vector<float> out_left(length, 0.0f);
  std::vector<float> out_right(length, 0.0f);
  val meters = val::array();

#if defined(SONARE_WITH_MIXING) && defined(SONARE_WITH_GRAPH)
  // Build a real mixer and route every input through the compiled routing graph
  // + master bus via sonare_mixer_process_stereo, matching the Node/Python
  // mixStereo path exactly (instead of summing bare ChannelStrip outputs).
  SonareMixer* mixer =
      sonare_mixer_create(sample_rate, static_cast<int>(std::max<size_t>(1, length)));
  if (mixer == nullptr) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidState,
        std::string("failed to create mixer: ") + sonare_last_error_message());
  }
  std::vector<SonareStrip*> strips;
  std::vector<const float*> left_ptrs(static_cast<size_t>(count));
  std::vector<const float*> right_ptrs(static_cast<size_t>(count));
  try {
    strips.reserve(static_cast<size_t>(count));
    for (int index = 0; index < count; ++index) {
      SonareStrip* strip = sonare_mixer_add_strip(mixer, ("strip" + std::to_string(index)).c_str());
      if (strip == nullptr) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidState, "failed to add mixer strip");
      }
      strips.push_back(strip);

      val inputTrim = optionAt(options, "inputTrimDb", index);
      if (!inputTrim.isUndefined() && !inputTrim.isNull() &&
          inputTrim.typeOf().as<std::string>() == "number") {
        sonare_strip_set_input_trim_db(strip, inputTrim.as<float>());
      }
      val fader = optionAt(options, "faderDb", index);
      if (!fader.isUndefined() && !fader.isNull() && fader.typeOf().as<std::string>() == "number") {
        sonare_strip_set_fader_db(strip, fader.as<float>());
      }
      val pan = optionAt(options, "pan", index);
      if (!pan.isUndefined() && !pan.isNull() && pan.typeOf().as<std::string>() == "number") {
        sonare_strip_set_pan(strip, pan.as<float>(),
                             panModeOrdinalFromVal(optionAt(options, "panMode", index)));
      }
      val width = optionAt(options, "width", index);
      if (!width.isUndefined() && !width.isNull() && width.typeOf().as<std::string>() == "number") {
        sonare_strip_set_width(strip, width.as<float>());
      }
      val muted = optionAt(options, "muted", index);
      if (!muted.isUndefined() && !muted.isNull() &&
          muted.typeOf().as<std::string>() == "boolean") {
        sonare_strip_set_muted(strip, muted.as<bool>() ? 1 : 0);
      }

      left_ptrs[static_cast<size_t>(index)] = left_inputs[static_cast<size_t>(index)].data();
      right_ptrs[static_cast<size_t>(index)] = right_inputs[static_cast<size_t>(index)].data();
    }

    SonareError err = sonare_mixer_process_stereo(mixer, left_ptrs.data(), right_ptrs.data(),
                                                  static_cast<size_t>(count), out_left.data(),
                                                  out_right.data(), length);
    if (err != SONARE_OK) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidState,
          std::string("mixer process failed: ") + sonare_error_message(err));
    }
    // The per-strip meter snapshots reflect only this single one-shot block.
    // The integrating-meter fields (momentaryLufs/shortTermLufs/integratedLufs
    // and the true-peak fields) require sustained streaming to populate; on a
    // short one-shot mix they read the -120 dB floor sentinel. Use the
    // streaming Mixer path if you need meaningful loudness/true-peak readings.
    for (size_t index = 0; index < strips.size(); ++index) {
      SonareMixMeterSnapshot snapshot{};
      sonare_strip_meter(strips[index], &snapshot);
      meters.call<void>("push", mixMeterSnapshotToValFree(snapshot));
    }
  } catch (...) {
    sonare_mixer_destroy(mixer);
    throw;
  }
  sonare_mixer_destroy(mixer);
#else
  // Fallback for builds without the mixing routing graph: bare per-strip
  // ChannelStrip processing + manual sum. Functionally equivalent for the simple
  // (no-routing) case the graph collapses to.
  for (int index = 0; index < count; ++index) {
    mixing::ChannelStrip strip;
    strip.prepare(sample_rate, static_cast<int>(std::max<size_t>(1, length)));

    val inputTrim = optionAt(options, "inputTrimDb", index);
    if (!inputTrim.isUndefined() && !inputTrim.isNull() &&
        inputTrim.typeOf().as<std::string>() == "number") {
      strip.set_input_trim_db(inputTrim.as<float>());
    }
    val fader = optionAt(options, "faderDb", index);
    if (!fader.isUndefined() && !fader.isNull() && fader.typeOf().as<std::string>() == "number") {
      strip.set_fader_db(fader.as<float>());
    }
    val pan = optionAt(options, "pan", index);
    if (!pan.isUndefined() && !pan.isNull() && pan.typeOf().as<std::string>() == "number") {
      strip.set_pan_mode(panModeFromVal(optionAt(options, "panMode", index)));
      strip.set_pan(pan.as<float>());
    }
    val width = optionAt(options, "width", index);
    if (!width.isUndefined() && !width.isNull() && width.typeOf().as<std::string>() == "number") {
      strip.set_width(width.as<float>());
    }
    val muted = optionAt(options, "muted", index);
    if (!muted.isUndefined() && !muted.isNull() && muted.typeOf().as<std::string>() == "boolean") {
      strip.set_muted(muted.as<bool>());
    }

    float* channels[] = {left_inputs[static_cast<size_t>(index)].data(),
                         right_inputs[static_cast<size_t>(index)].data()};
    strip.process(channels, 2, static_cast<int>(length));
    for (size_t sample = 0; sample < length; ++sample) {
      out_left[sample] += left_inputs[static_cast<size_t>(index)][sample];
      out_right[sample] += right_inputs[static_cast<size_t>(index)][sample];
    }
    meters.call<void>("push", meterSnapshotToVal(strip.meter_snapshot()));
  }
#endif  // SONARE_WITH_MIXING && SONARE_WITH_GRAPH

  val out = val::object();
  out.set("left", vectorToFloat32Array(out_left));
  out.set("right", vectorToFloat32Array(out_right));
  out.set("sampleRate", sample_rate);
  out.set("meters", meters);
  return out;
}

void registerMixingBindings() {
  function("mixingScenePresetNames", &js_mixing_scene_preset_names);
  function("mixingScenePresetJson", &js_mixing_scene_preset_json);
  function("mixStereo", &js_mix_stereo);
#if defined(SONARE_WITH_MIXING) && defined(SONARE_WITH_GRAPH)
  class_<MixerWasm>("Mixer")
      .function("compile", &MixerWasm::compile)
      .function("processStereo", &MixerWasm::processStereo)
      .function("processStereoInto", &MixerWasm::processStereoInto)
      .function("inputLeftView", &MixerWasm::inputLeftView)
      .function("inputRightView", &MixerWasm::inputRightView)
      .function("outputLeftView", &MixerWasm::outputLeftView)
      .function("outputRightView", &MixerWasm::outputRightView)
      .function("processPreparedStereo", &MixerWasm::processPreparedStereo)
      .function("stripCount", &MixerWasm::stripCount)
      .function("sceneWarnings", &MixerWasm::sceneWarnings)
      .function("scheduleInsertAutomation", &MixerWasm::scheduleInsertAutomation)
      .function("stripById", &MixerWasm::stripById)
      .function("setInputTrimDb", &MixerWasm::setInputTrimDb)
      .function("setFaderDb", &MixerWasm::setFaderDb)
      .function("setPan", &MixerWasm::setPan)
      .function("setWidth", &MixerWasm::setWidth)
      .function("setMuted", &MixerWasm::setMuted)
      .function("setSoloed", &MixerWasm::setSoloed)
      .function("setSoloSafe", &MixerWasm::setSoloSafe)
      .function("setPolarityInvert", &MixerWasm::setPolarityInvert)
      .function("setPanLaw", &MixerWasm::setPanLaw)
      .function("setChannelDelaySamples", &MixerWasm::setChannelDelaySamples)
      .function("setVcaOffsetDb", &MixerWasm::setVcaOffsetDb)
      .function("setDualPan", &MixerWasm::setDualPan)
      .function("setSurroundPan", &MixerWasm::setSurroundPan)
      .function("addSend", &MixerWasm::addSend)
      .function("setSendDb", &MixerWasm::setSendDb)
      .function("removeSend", &MixerWasm::removeSend)
      .function("meterTap", &MixerWasm::meterTap)
      .function("stripMeter", &MixerWasm::stripMeter)
      .function("scheduleFaderAutomation", &MixerWasm::scheduleFaderAutomation)
      .function("schedulePanAutomation", &MixerWasm::schedulePanAutomation)
      .function("scheduleWidthAutomation", &MixerWasm::scheduleWidthAutomation)
      .function("scheduleSendAutomation", &MixerWasm::scheduleSendAutomation)
      .function("readGoniometerLatest", &MixerWasm::readGoniometerLatest)
      .function("addBus", &MixerWasm::addBus)
      .function("removeBus", &MixerWasm::removeBus)
      .function("busCount", &MixerWasm::busCount)
      .function("addVcaGroup", &MixerWasm::addVcaGroup)
      .function("setVcaGroupGainDb", &MixerWasm::setVcaGroupGainDb)
      .function("removeVcaGroup", &MixerWasm::removeVcaGroup)
      .function("vcaGroupCount", &MixerWasm::vcaGroupCount)
      .function("toSceneJson", &MixerWasm::toSceneJson)
      .function("tailSamples", &MixerWasm::tailSamples)
      .function("drainTailStereo", &MixerWasm::drainTailStereo);
  function("createMixerFromSceneJson", &createMixerFromSceneJson, allow_raw_pointers());
#endif
}

#endif  // __EMSCRIPTEN__
