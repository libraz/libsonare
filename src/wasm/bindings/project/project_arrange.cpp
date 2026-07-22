/// @file project_arrange.cpp
/// @brief Embind project facade: track + clip arrangement structure and warp.

#ifdef __EMSCRIPTEN__

#include "project_wasm.h"

#if defined(SONARE_WITH_ARRANGEMENT)

uint32_t ProjectWasm::addTrack(val desc) {
  SonareProjectTrackDesc d{};
  std::string name;
  if (!desc.isUndefined() && !desc.isNull()) {
    if (hasProperty(desc, "kind")) {
      val kind = desc["kind"];
      if (kind.typeOf().as<std::string>() == "string") {
        const std::string k = kind.as<std::string>();
        d.kind =
            k == "midi" ? SONARE_TRACK_MIDI : (k == "aux" ? SONARE_TRACK_AUX : SONARE_TRACK_AUDIO);
      } else {
        d.kind = kind.as<int>();
      }
    }
    if (hasProperty(desc, "name")) {
      name = desc["name"].as<std::string>();
      d.name = name.c_str();
    }
  }
  uint32_t out = 0;
  const SonareError err = sonare_project_add_track(project_.get(), &d, &out);
  if (err != SONARE_OK) {
    throwCError(err, "failed to add track");
  }
  return out;
}

uint32_t ProjectWasm::addClip(val desc) {
  if (desc.isUndefined() || desc.isNull()) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "addClip expects a descriptor object");
  }
  SonareProjectClipDesc d{};
  std::vector<float> audio;
  std::string source_uri;
  d.track_id = hasProperty(desc, "trackId") ? desc["trackId"].as<uint32_t>() : 0;
  d.is_midi = hasProperty(desc, "isMidi") && desc["isMidi"].as<bool>() ? 1 : 0;
  d.start_ppq = hasProperty(desc, "startPpq") ? desc["startPpq"].as<double>() : 0.0;
  d.length_ppq = hasProperty(desc, "lengthPpq") ? desc["lengthPpq"].as<double>() : 0.0;
  d.source_offset_ppq =
      hasProperty(desc, "sourceOffsetPpq") ? desc["sourceOffsetPpq"].as<double>() : 0.0;
  d.gain = hasProperty(desc, "gain") ? desc["gain"].as<float>() : 1.0f;
  d.audio_channels = hasProperty(desc, "audioChannels") ? desc["audioChannels"].as<int>() : 1;
  d.audio_sample_rate =
      hasProperty(desc, "audioSampleRate") ? desc["audioSampleRate"].as<int>() : 0;
  if (hasProperty(desc, "audio")) {
    audio = float32ArrayToVector(desc["audio"]);
    if (d.audio_channels <= 0 || audio.size() % static_cast<size_t>(d.audio_channels) != 0) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "audio length must be a multiple of audioChannels");
    }
    d.audio_interleaved = audio.data();
    d.audio_frames = static_cast<int64_t>(audio.size()) / d.audio_channels;
  }
  if (hasProperty(desc, "sourceUri")) {
    source_uri = desc["sourceUri"].as<std::string>();
    d.source_uri = source_uri.c_str();
  }
  uint32_t out = 0;
  const SonareError err = sonare_project_add_clip(project_.get(), &d, &out);
  if (err != SONARE_OK) {
    throwCError(err, "failed to add clip");
  }
  return out;
}

val ProjectWasm::addLoopRecordingTakes(val desc) {
  if (desc.isUndefined() || desc.isNull()) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "addLoopRecordingTakes expects a descriptor object");
  }
  SonareProjectLoopRecordingDesc d{};
  std::vector<float> audio;
  d.track_id = hasProperty(desc, "trackId") ? desc["trackId"].as<uint32_t>() : 0;
  d.start_ppq = hasProperty(desc, "startPpq") ? desc["startPpq"].as<double>() : 0.0;
  d.loop_length_ppq = hasProperty(desc, "loopLengthPpq") ? desc["loopLengthPpq"].as<double>() : 0.0;
  d.audio_channels = hasProperty(desc, "audioChannels") ? desc["audioChannels"].as<int>() : 1;
  d.audio_sample_rate =
      hasProperty(desc, "audioSampleRate") ? desc["audioSampleRate"].as<int>() : 48000;
  if (hasProperty(desc, "audio")) {
    audio = float32ArrayToVector(desc["audio"]);
    if (d.audio_channels <= 0 || audio.size() % static_cast<size_t>(d.audio_channels) != 0) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "audio length must be a multiple of audioChannels");
    }
    d.audio_interleaved = audio.data();
    d.audio_frames = static_cast<int64_t>(audio.size()) / d.audio_channels;
  }
  uint32_t clip_id = 0;
  size_t take_count = 0;
  const SonareError err =
      sonare_project_add_loop_recording_takes(project_.get(), &d, &clip_id, &take_count);
  if (err != SONARE_OK) {
    throwCError(err, "failed to add loop recording takes");
  }
  val out = val::object();
  out.set("clipId", clip_id);
  out.set("takeCount", static_cast<double>(take_count));
  return out;
}

val ProjectWasm::addMidiClip(double start_ppq, double length_ppq) {
  uint32_t track = 0;
  uint32_t clip = 0;
  const SonareError err =
      sonare_project_add_midi_clip(project_.get(), start_ppq, length_ppq, &track, &clip);
  if (err != SONARE_OK) {
    throwCError(err, "failed to add MIDI clip");
  }
  val out = val::object();
  out.set("trackId", track);
  out.set("clipId", clip);
  return out;
}

uint32_t ProjectWasm::splitClip(uint32_t clip_id, double split_ppq) {
  uint32_t out = 0;
  const SonareError err = sonare_project_split_clip(project_.get(), clip_id, split_ppq, &out);
  if (err != SONARE_OK) {
    throwCError(err, "failed to split clip");
  }
  return out;
}

void ProjectWasm::trimClip(uint32_t clip_id, double start_ppq, double length_ppq) {
  const SonareError err = sonare_project_trim_clip(project_.get(), clip_id, start_ppq, length_ppq);
  if (err != SONARE_OK) {
    throwCError(err, "failed to trim clip");
  }
}

void ProjectWasm::moveClip(uint32_t clip_id, double start_ppq, uint32_t track_id) {
  const SonareError err = sonare_project_move_clip(project_.get(), clip_id, start_ppq, track_id);
  if (err != SONARE_OK) {
    throwCError(err, "failed to move clip");
  }
}

void ProjectWasm::setTrackKind(uint32_t track_id, uint32_t kind) {
  const SonareError err = sonare_project_set_track_kind(project_.get(), track_id, kind);
  if (err != SONARE_OK) {
    throwCError(err, "failed to set track kind");
  }
}

void ProjectWasm::setClipWarpRef(uint32_t clip_id, uint32_t warp_ref_id) {
  const SonareError err = sonare_project_set_clip_warp_ref(project_.get(), clip_id, warp_ref_id);
  if (err != SONARE_OK) {
    throwCError(err, "failed to set clip warp reference");
  }
}

void ProjectWasm::setClipWarpMode(uint32_t clip_id, val mode_val) {
  SonareProjectWarpMode mode = SONARE_PROJECT_WARP_MODE_OFF;
  if (mode_val.typeOf().as<std::string>() == "string") {
    const std::string mode_string = mode_val.as<std::string>();
    if (mode_string == "off") {
      mode = SONARE_PROJECT_WARP_MODE_OFF;
    } else if (mode_string == "repitch") {
      mode = SONARE_PROJECT_WARP_MODE_REPITCH;
    } else if (mode_string == "tempo-sync") {
      mode = SONARE_PROJECT_WARP_MODE_TEMPO_SYNC;
    } else {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "unknown warp mode");
    }
  } else {
    const int mode_int = mode_val.as<int>();
    if (mode_int < SONARE_PROJECT_WARP_MODE_OFF || mode_int > SONARE_PROJECT_WARP_MODE_TEMPO_SYNC) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "unknown warp mode");
    }
    mode = static_cast<SonareProjectWarpMode>(mode_int);
  }
  const SonareError err = sonare_project_set_clip_warp_mode(project_.get(), clip_id, mode);
  if (err != SONARE_OK) {
    throwCError(err, "failed to set clip warp mode");
  }
}

void ProjectWasm::setWarpMap(val desc) {
  std::vector<SonareProjectWarpAnchor> anchors;
  const size_t count = hasProperty(desc, "anchors") ? desc["anchors"]["length"].as<size_t>() : 0;
  anchors.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    val anchor = desc["anchors"][static_cast<unsigned>(i)];
    SonareProjectWarpAnchor out{};
    out.warp_sample = hasProperty(anchor, "warpSample") ? anchor["warpSample"].as<double>() : 0.0;
    out.source_sample =
        hasProperty(anchor, "sourceSample") ? anchor["sourceSample"].as<double>() : 0.0;
    anchors.push_back(out);
  }
  std::string name = hasProperty(desc, "name") ? desc["name"].as<std::string>() : "";
  SonareProjectWarpMapDesc cdesc{};
  cdesc.id = hasProperty(desc, "id") ? desc["id"].as<uint32_t>() : 0u;
  cdesc.name = name.empty() ? nullptr : name.c_str();
  cdesc.anchors = anchors.empty() ? nullptr : anchors.data();
  cdesc.anchor_count = anchors.size();
  const SonareError err = sonare_project_set_warp_map(project_.get(), &cdesc);
  if (err != SONARE_OK) {
    throwCError(err, "failed to set warp map");
  }
}

void ProjectWasm::removeWarpMap(uint32_t warp_ref_id) {
  const SonareError err = sonare_project_remove_warp_map(project_.get(), warp_ref_id);
  if (err != SONARE_OK) {
    throwCError(err, "failed to remove warp map");
  }
}

void ProjectWasm::setTrackMidiDestination(uint32_t track_id, uint32_t destination_id) {
  const SonareError err =
      sonare_project_set_track_midi_destination(project_.get(), track_id, destination_id);
  if (err != SONARE_OK) {
    throwCError(err, "failed to set track MIDI destination");
  }
}

void ProjectWasm::setTrackGain(uint32_t track_id, float gain) {
  const SonareError err = sonare_project_set_track_gain(project_.get(), track_id, gain);
  if (err != SONARE_OK) {
    throwCError(err, "failed to set track gain");
  }
}

void ProjectWasm::setTrackMute(uint32_t track_id, bool mute) {
  const SonareError err = sonare_project_set_track_mute(project_.get(), track_id, mute ? 1 : 0);
  if (err != SONARE_OK) {
    throwCError(err, "failed to set track mute");
  }
}

void ProjectWasm::setTrackSolo(uint32_t track_id, bool solo) {
  const SonareError err = sonare_project_set_track_solo(project_.get(), track_id, solo ? 1 : 0);
  if (err != SONARE_OK) {
    throwCError(err, "failed to set track solo");
  }
}

void ProjectWasm::setTrackPan(uint32_t track_id, float pan) {
  const SonareError err = sonare_project_set_track_pan(project_.get(), track_id, pan);
  if (err != SONARE_OK) {
    throwCError(err, "failed to set track pan");
  }
}

void ProjectWasm::undo() {
  const SonareError err = sonare_project_undo(project_.get());
  if (err != SONARE_OK) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState, "nothing to undo");
  }
}

void ProjectWasm::redo() {
  const SonareError err = sonare_project_redo(project_.get());
  if (err != SONARE_OK) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState, "nothing to redo");
  }
}

void registerProjectArrange(class_<ProjectWasm>& cls) {
  cls.function("addTrack", &ProjectWasm::addTrack)
      .function("addClip", &ProjectWasm::addClip)
      .function("addLoopRecordingTakes", &ProjectWasm::addLoopRecordingTakes)
      .function("addMidiClip", &ProjectWasm::addMidiClip)
      .function("splitClip", &ProjectWasm::splitClip)
      .function("trimClip", &ProjectWasm::trimClip)
      .function("moveClip", &ProjectWasm::moveClip)
      .function("setTrackKind", &ProjectWasm::setTrackKind)
      .function("setClipWarpRef", &ProjectWasm::setClipWarpRef)
      .function("setClipWarpMode", &ProjectWasm::setClipWarpMode)
      .function("setWarpMap", &ProjectWasm::setWarpMap)
      .function("removeWarpMap", &ProjectWasm::removeWarpMap)
      .function("setTrackMidiDestination", &ProjectWasm::setTrackMidiDestination)
      .function("setTrackGain", &ProjectWasm::setTrackGain)
      .function("setTrackMute", &ProjectWasm::setTrackMute)
      .function("setTrackSolo", &ProjectWasm::setTrackSolo)
      .function("setTrackPan", &ProjectWasm::setTrackPan)
      .function("undo", &ProjectWasm::undo)
      .function("redo", &ProjectWasm::redo);
}

#endif  // SONARE_WITH_ARRANGEMENT

#endif  // __EMSCRIPTEN__
