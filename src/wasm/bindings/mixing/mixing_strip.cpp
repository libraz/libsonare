/// @file mixing_strip.cpp
/// @brief Embind scene-based mixer facade: per-strip control setters.

#ifdef __EMSCRIPTEN__

#include "mixing_wasm.h"

#if defined(SONARE_WITH_MIXING) && defined(SONARE_WITH_GRAPH)

// Sets the strip's input trim in dB.
void MixerWasm::setInputTrimDb(unsigned int strip_index, float db) {
  checkStripError(sonare_strip_set_input_trim_db(stripAt(strip_index), db),
                  "failed to set input trim");
}

// Sets the strip's fader level in dB.
void MixerWasm::setFaderDb(unsigned int strip_index, float db) {
  checkStripError(sonare_strip_set_fader_db(stripAt(strip_index), db), "failed to set fader");
}

// Sets the strip's pan position. pan_mode is the SONARE_PAN_MODE_* ordinal;
// pass SONARE_PAN_MODE_KEEP (-1) to keep the strip's current pan mode (e.g. a
// scene-defined mode) on a plain pan nudge.
void MixerWasm::setPan(unsigned int strip_index, float pan, int pan_mode) {
  checkStripError(sonare_strip_set_pan(stripAt(strip_index), pan, pan_mode), "failed to set pan");
}

// Sets the strip's stereo width.
void MixerWasm::setWidth(unsigned int strip_index, float width) {
  checkStripError(sonare_strip_set_width(stripAt(strip_index), width), "failed to set width");
}

// Sets the strip's mute state.
void MixerWasm::setMuted(unsigned int strip_index, bool muted) {
  checkStripError(sonare_strip_set_muted(stripAt(strip_index), muted ? 1 : 0),
                  "failed to set muted");
}

// Sets the strip's solo state. Takes effect on the next process without a
// graph recompile.
void MixerWasm::setSoloed(unsigned int strip_index, bool soloed) {
  checkStripError(sonare_strip_set_soloed(stripAt(strip_index), soloed ? 1 : 0),
                  "failed to set soloed");
}

// Marks a strip as solo-safe so it is never implied-muted by another strip's
// solo. Takes effect on the next process without a graph recompile.
void MixerWasm::setSoloSafe(unsigned int strip_index, bool solo_safe) {
  checkStripError(sonare_strip_set_solo_safe(stripAt(strip_index), solo_safe ? 1 : 0),
                  "failed to set solo-safe");
}

// Inverts the polarity of the left and/or right channel.
void MixerWasm::setPolarityInvert(unsigned int strip_index, bool invert_left, bool invert_right) {
  checkStripError(sonare_strip_set_polarity_invert(stripAt(strip_index), invert_left ? 1 : 0,
                                                   invert_right ? 1 : 0),
                  "failed to set polarity invert");
}

// Sets the strip's pan law. pan_law: 0 = -3 dB, 1 = -4.5 dB, 2 = -6 dB,
// 3 = linear (0 dB).
void MixerWasm::setPanLaw(unsigned int strip_index, int pan_law) {
  checkStripError(sonare_strip_set_pan_law(stripAt(strip_index), pan_law), "failed to set pan law");
}

// Sets a per-strip channel delay in samples. This changes the strip's reported
// latency; recompile to re-run latency compensation.
void MixerWasm::setChannelDelaySamples(unsigned int strip_index, int delay_samples) {
  checkStripError(sonare_strip_set_channel_delay_samples(stripAt(strip_index), delay_samples),
                  "failed to set channel delay samples");
}

// Sets the strip's live VCA gain offset in dB (not persisted to the scene).
void MixerWasm::setVcaOffsetDb(unsigned int strip_index, float offset_db) {
  checkStripError(sonare_strip_set_vca_offset_db(stripAt(strip_index), offset_db),
                  "failed to set VCA offset");
}

// Sets independent left/right pan positions (dual-pan mode).
void MixerWasm::setDualPan(unsigned int strip_index, float left_pan, float right_pan) {
  checkStripError(sonare_strip_set_dual_pan(stripAt(strip_index), left_pan, right_pan),
                  "failed to set dual pan");
}

// Sets the strip's surround pan from a JS object {azimuth, elevation,
// divergence, lfe, distance}; absent/non-numeric fields fall back to the
// centered point-source default.
void MixerWasm::setSurroundPan(unsigned int strip_index, val pan) {
  const auto field = [&](const char* key, float fallback) {
    return optionalNumber(pan[key]).value_or(fallback);
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

// Adds a post-construction send to the strip. timing mirrors SonareSendTiming:
// 0 = post-fader, 1 = pre-fader. Returns the new send's index.
size_t MixerWasm::addSend(unsigned int strip_index, std::string id, std::string destination_bus_id,
                          float send_db, int timing) {
  size_t index = 0;
  checkStripError(sonare_strip_add_send(stripAt(strip_index), id.c_str(),
                                        destination_bus_id.c_str(), send_db, timing, &index),
                  "failed to add send");
  return index;
}

// Sets the send level (in dB) for an existing send by index.
void MixerWasm::setSendDb(unsigned int strip_index, size_t send_index, float send_db) {
  checkStripError(sonare_strip_set_send_db(stripAt(strip_index), send_index, send_db),
                  "failed to set send level");
}

// Removes the send at send_index (in add order) from the strip. Higher send
// indices shift down by one after removal; recompile before processing.
void MixerWasm::removeSend(unsigned int strip_index, size_t send_index) {
  checkStripError(
      sonare_strip_remove_send(stripAt(strip_index), static_cast<unsigned int>(send_index)),
      "failed to remove send");
}

void registerMixerStripControls(class_<MixerWasm>& cls) {
  cls.function("setInputTrimDb", &MixerWasm::setInputTrimDb)
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
      .function("removeSend", &MixerWasm::removeSend);
}

#endif  // SONARE_WITH_MIXING && SONARE_WITH_GRAPH

#endif  // __EMSCRIPTEN__
