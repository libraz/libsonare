/// @file mixing_strip.cpp
/// @brief Embind scene-based mixer facade: per-strip control setters.

#ifdef __EMSCRIPTEN__

#include "mixing_wasm.h"

#if defined(SONARE_WITH_MIXING) && defined(SONARE_WITH_GRAPH)

// Sets the strip's input trim in dB.
void MixerWasm::setInputTrimDb(const val& strip_index_val, const val& db_val) {
  const unsigned int strip_index = checkedUintFromVal(strip_index_val, "stripIndex");
  const float db = checkedFloatFromVal(db_val, "db");
  checkStripError(sonare_strip_set_input_trim_db(stripAt(strip_index), db),
                  "failed to set input trim");
}

// Sets the strip's fader level in dB.
void MixerWasm::setFaderDb(const val& strip_index_val, const val& db_val) {
  const unsigned int strip_index = checkedUintFromVal(strip_index_val, "stripIndex");
  const float db = checkedFloatFromVal(db_val, "db");
  checkStripError(sonare_strip_set_fader_db(stripAt(strip_index), db), "failed to set fader");
}

// Sets the strip's pan position. pan_mode is the SONARE_PAN_MODE_* ordinal;
// pass SONARE_PAN_MODE_KEEP (-1) to keep the strip's current pan mode (e.g. a
// scene-defined mode) on a plain pan nudge.
void MixerWasm::setPan(const val& strip_index_val, const val& pan_val, const val& pan_mode_val) {
  const unsigned int strip_index = checkedUintFromVal(strip_index_val, "stripIndex");
  const float pan = checkedFloatFromVal(pan_val, "pan");
  const int pan_mode = checkedIntFromVal(pan_mode_val, "panMode");
  checkStripError(sonare_strip_set_pan(stripAt(strip_index), pan, pan_mode), "failed to set pan");
}

// Sets the strip's stereo width.
void MixerWasm::setWidth(const val& strip_index_val, const val& width_val) {
  const unsigned int strip_index = checkedUintFromVal(strip_index_val, "stripIndex");
  const float width = checkedFloatFromVal(width_val, "width");
  checkStripError(sonare_strip_set_width(stripAt(strip_index), width), "failed to set width");
}

// Snaps the strip's input-trim, fader, pan and width smoothers to the values
// already set on it, so the next processed block opens at those values instead
// of gliding to them. Clears nothing.
void MixerWasm::settle(const val& strip_index_val) {
  const unsigned int strip_index = checkedUintFromVal(strip_index_val, "stripIndex");
  checkStripError(sonare_strip_settle(stripAt(strip_index)), "failed to settle strip smoothers");
}

// Sets the strip's mute state.
void MixerWasm::setMuted(const val& strip_index_val, bool muted) {
  const unsigned int strip_index = checkedUintFromVal(strip_index_val, "stripIndex");
  checkStripError(sonare_strip_set_muted(stripAt(strip_index), muted ? 1 : 0),
                  "failed to set muted");
}

// Sets the strip's solo state. Takes effect on the next process without a
// graph recompile.
void MixerWasm::setSoloed(const val& strip_index_val, bool soloed) {
  const unsigned int strip_index = checkedUintFromVal(strip_index_val, "stripIndex");
  checkStripError(sonare_strip_set_soloed(stripAt(strip_index), soloed ? 1 : 0),
                  "failed to set soloed");
}

// Marks a strip as solo-safe so it is never implied-muted by another strip's
// solo. Takes effect on the next process without a graph recompile.
void MixerWasm::setSoloSafe(const val& strip_index_val, bool solo_safe) {
  const unsigned int strip_index = checkedUintFromVal(strip_index_val, "stripIndex");
  checkStripError(sonare_strip_set_solo_safe(stripAt(strip_index), solo_safe ? 1 : 0),
                  "failed to set solo-safe");
}

// Inverts the polarity of the left and/or right channel.
void MixerWasm::setPolarityInvert(const val& strip_index_val, bool invert_left, bool invert_right) {
  const unsigned int strip_index = checkedUintFromVal(strip_index_val, "stripIndex");
  checkStripError(sonare_strip_set_polarity_invert(stripAt(strip_index), invert_left ? 1 : 0,
                                                   invert_right ? 1 : 0),
                  "failed to set polarity invert");
}

// Sets the strip's pan law. pan_law: 0 = -3 dB, 1 = -4.5 dB, 2 = -6 dB,
// 3 = linear (0 dB).
void MixerWasm::setPanLaw(const val& strip_index_val, const val& pan_law_val) {
  const unsigned int strip_index = checkedUintFromVal(strip_index_val, "stripIndex");
  const int pan_law = checkedIntFromVal(pan_law_val, "panLaw");
  checkStripError(sonare_strip_set_pan_law(stripAt(strip_index), pan_law), "failed to set pan law");
}

// Sets a per-strip channel delay in samples. This changes the strip's reported
// latency; recompile to re-run latency compensation.
void MixerWasm::setChannelDelaySamples(const val& strip_index_val, const val& delay_samples_val) {
  const unsigned int strip_index = checkedUintFromVal(strip_index_val, "stripIndex");
  const int delay_samples = checkedIntFromVal(delay_samples_val, "delaySamples");
  checkStripError(sonare_strip_set_channel_delay_samples(stripAt(strip_index), delay_samples),
                  "failed to set channel delay samples");
}

// Sets the strip's live VCA gain offset in dB (not persisted to the scene).
void MixerWasm::setVcaOffsetDb(const val& strip_index_val, const val& offset_db_val) {
  const unsigned int strip_index = checkedUintFromVal(strip_index_val, "stripIndex");
  const float offset_db = checkedFloatFromVal(offset_db_val, "offsetDb");
  checkStripError(sonare_strip_set_vca_offset_db(stripAt(strip_index), offset_db),
                  "failed to set VCA offset");
}

// Sets independent left/right pan positions (dual-pan mode).
void MixerWasm::setDualPan(const val& strip_index_val, const val& left_pan_val,
                           const val& right_pan_val) {
  const unsigned int strip_index = checkedUintFromVal(strip_index_val, "stripIndex");
  const float left_pan = checkedFloatFromVal(left_pan_val, "leftPan");
  const float right_pan = checkedFloatFromVal(right_pan_val, "rightPan");
  checkStripError(sonare_strip_set_dual_pan(stripAt(strip_index), left_pan, right_pan),
                  "failed to set dual pan");
}

// Sets the strip's surround pan from a JS object {azimuth, elevation,
// divergence, lfe, distance}; absent/non-numeric fields fall back to the
// centered point-source default.
void MixerWasm::setSurroundPan(const val& strip_index_val, val pan) {
  const unsigned int strip_index = checkedUintFromVal(strip_index_val, "stripIndex");
  const auto field = [&](const char* key, float fallback) {
    return optionalNumber(pan[key], key).value_or(fallback);
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
size_t MixerWasm::addSend(const val& strip_index_val, std::string id,
                          std::string destination_bus_id, const val& send_db_val,
                          const val& timing_val) {
  const unsigned int strip_index = checkedUintFromVal(strip_index_val, "stripIndex");
  const float send_db = checkedFloatFromVal(send_db_val, "sendDb");
  const int timing = checkedIntFromVal(timing_val, "timing");
  size_t index = 0;
  checkStripError(sonare_strip_add_send(stripAt(strip_index), id.c_str(),
                                        destination_bus_id.c_str(), send_db, timing, &index),
                  "failed to add send");
  return index;
}

// Sets the send level (in dB) for an existing send by index.
void MixerWasm::setSendDb(const val& strip_index_val, const val& send_index_val,
                          const val& send_db_val) {
  const unsigned int strip_index = checkedUintFromVal(strip_index_val, "stripIndex");
  const size_t send_index = static_cast<size_t>(checkedUintFromVal(send_index_val, "sendIndex"));
  const float send_db = checkedFloatFromVal(send_db_val, "sendDb");
  checkStripError(sonare_strip_set_send_db(stripAt(strip_index), send_index, send_db),
                  "failed to set send level");
}

// Removes the send at send_index (in add order) from the strip. Higher send
// indices shift down by one after removal; recompile before processing.
void MixerWasm::removeSend(const val& strip_index_val, const val& send_index_val) {
  const unsigned int strip_index = checkedUintFromVal(strip_index_val, "stripIndex");
  const size_t send_index = static_cast<size_t>(checkedUintFromVal(send_index_val, "sendIndex"));
  checkStripError(
      sonare_strip_remove_send(stripAt(strip_index), static_cast<unsigned int>(send_index)),
      "failed to remove send");
}

void registerMixerStripControls(class_<MixerWasm>& cls) {
  cls.function("setInputTrimDb", &MixerWasm::setInputTrimDb)
      .function("setFaderDb", &MixerWasm::setFaderDb)
      .function("setPan", &MixerWasm::setPan)
      .function("setWidth", &MixerWasm::setWidth)
      .function("settle", &MixerWasm::settle)
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
