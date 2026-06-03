/// @file streaming_retune.cpp
/// @brief Embind bindings for streaming retune APIs.

#ifdef __EMSCRIPTEN__

#include "common.h"

// ---------------------------------------------------------------------------
// StreamingRetune wrapper (block-by-block voice retune / pitch shift).
// Construct via createStreamingRetune(config) factory.
// ---------------------------------------------------------------------------

editing::voice_changer::StreamingRetuneConfig streamingRetuneConfigFromVal(val config) {
  editing::voice_changer::StreamingRetuneConfig result;
  if (config.isNull() || config.isUndefined()) {
    return result;
  }
  result.semitones = floatProperty(config, "semitones", result.semitones);
  result.mix = floatProperty(config, "mix", result.mix);
  result.grain_size = intProperty(config, "grainSize", result.grain_size);
  result.grain_size = intProperty(config, "grain_size", result.grain_size);
  return result;
}

val streamingRetuneConfigToVal(const editing::voice_changer::StreamingRetuneConfig& config) {
  val out = val::object();
  out.set("semitones", config.semitones);
  out.set("mix", config.mix);
  out.set("grainSize", config.grain_size);
  return out;
}

class StreamingRetuneWrapper {
 public:
  explicit StreamingRetuneWrapper(val config) : retune_(streamingRetuneConfigFromVal(config)) {}

  void prepare(double sample_rate, int max_block_size) {
    retune_.prepare(sample_rate, max_block_size);
  }

  void reset() { retune_.reset(); }

  void setConfig(val config) { retune_.set_config(streamingRetuneConfigFromVal(config)); }

  val config() const { return streamingRetuneConfigToVal(retune_.config()); }

  int grainSize() const { return retune_.grain_size(); }

  val processMono(val samples) {
    std::vector<float> block = float32ArrayToVector(samples);
    std::vector<float> out(block.size());
    retune_.process_block(block.data(), out.data(), static_cast<int>(block.size()));
    return vectorToFloat32Array(out);
  }

 private:
  editing::voice_changer::StreamingRetune retune_;
};

StreamingRetuneWrapper* createStreamingRetune(val config) {
  return new StreamingRetuneWrapper(config);
}

void registerStreamingRetuneBindings() {
  class_<StreamingRetuneWrapper>("StreamingRetune")
      .function("prepare", &StreamingRetuneWrapper::prepare)
      .function("reset", &StreamingRetuneWrapper::reset)
      .function("setConfig", &StreamingRetuneWrapper::setConfig)
      .function("config", &StreamingRetuneWrapper::config)
      .function("grainSize", &StreamingRetuneWrapper::grainSize)
      .function("processMono", &StreamingRetuneWrapper::processMono);
  function("createStreamingRetune", &createStreamingRetune, allow_raw_pointers());
}

#endif  // __EMSCRIPTEN__
