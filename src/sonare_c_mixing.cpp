#include <algorithm>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "mixing/api/presets.h"
#include "mixing/api/scene.h"
#include "mixing/channel_strip.h"
#include "mixing/send.h"
#include "sonare_c.h"
#include "sonare_c_internal.h"

namespace {

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

char* copy_string(const std::string& value) {
  std::unique_ptr<char[]> out(new char[value.size() + 1]);
  std::memcpy(out.get(), value.c_str(), value.size() + 1);
  return out.release();
}

const char* join_names(const std::vector<std::string>& values, std::string& storage) {
  std::ostringstream stream;
  for (size_t index = 0; index < values.size(); ++index) {
    if (index > 0) {
      stream << '\n';
    }
    stream << values[index];
  }
  storage = stream.str();
  return storage.c_str();
}

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

struct SonareStrip {
  std::string id;
  sonare::mixing::api::Strip scene_strip;
  sonare::mixing::ChannelStrip strip;
};

struct SonareMixer {
  int sample_rate = 48000;
  int max_block_size = 0;
  std::vector<std::unique_ptr<SonareStrip>> strips;
  std::vector<float> scratch_left;
  std::vector<float> scratch_right;
};

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
    strip->strip.prepare(static_cast<double>(mixer->sample_rate), mixer->max_block_size);
    SonareStrip* raw = strip.get();
    mixer->strips.push_back(std::move(strip));
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

SonareError sonare_strip_set_pan(SonareStrip* strip, float pan, int pan_mode) {
  if (!strip) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  strip->strip.set_pan_mode(to_pan_mode(pan_mode));
  strip->strip.set_pan(pan);
  strip->scene_strip.pan = pan;
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
      strip->strip.set_fader_db(scene_strip.fader_db);
      strip->strip.set_pan(scene_strip.pan);
      strip->strip.set_width(scene_strip.width);
      strip->strip.set_muted(scene_strip.muted);
      strip->strip.set_soloed(scene_strip.soloed);
      strip->strip.set_solo_safe(scene_strip.solo_safe);
      for (const auto& send : scene_strip.sends) {
        sonare::mixing::SendConfig config;
        config.send_db = send.send_db;
        config.timing = to_send_timing(send.timing);
        strip->strip.add_send(config);
      }
    }
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
  scene.buses.push_back({"master", "master"});
  for (const auto& strip : mixer->strips) {
    sonare::mixing::api::Strip scene_strip = strip->scene_strip;
    scene_strip.id = strip->id;
    scene_strip.fader_db = strip->strip.fader_db();
    scene_strip.pan = strip->strip.pan();
    scene_strip.width = strip->strip.width();
    scene_strip.muted = strip->strip.muted();
    scene_strip.soloed = strip->strip.soloed();
    scene_strip.solo_safe = strip->strip.solo_safe();
    scene.strips.push_back(std::move(scene_strip));
    scene.connections.push_back({strip->id, "master"});
  }
  *json_out = copy_string(sonare::mixing::api::scene_to_json(scene));
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mixer_process_stereo(SonareMixer* mixer, const float* const* input_left,
                                        const float* const* input_right, size_t input_count,
                                        float* output_left, float* output_right,
                                        size_t num_samples) {
  if (!mixer || !output_left || !output_right || (!input_left && input_count > 0) ||
      (!input_right && input_count > 0) ||
      num_samples > static_cast<size_t>(mixer->max_block_size)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (num_samples == 0) {
    return SONARE_OK;
  }

  SONARE_C_TRY
  std::fill(output_left, output_left + num_samples, 0.0f);
  std::fill(output_right, output_right + num_samples, 0.0f);
  const size_t count = std::min(input_count, mixer->strips.size());
  for (size_t index = 0; index < count; ++index) {
    if (!input_left[index] || !input_right[index]) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    std::copy(input_left[index], input_left[index] + num_samples, mixer->scratch_left.begin());
    std::copy(input_right[index], input_right[index] + num_samples, mixer->scratch_right.begin());
    float* channels[] = {mixer->scratch_left.data(), mixer->scratch_right.data()};
    mixer->strips[index]->strip.process(channels, 2, static_cast<int>(num_samples));
    for (size_t i = 0; i < num_samples; ++i) {
      output_left[i] += mixer->scratch_left[i];
      output_right[i] += mixer->scratch_right[i];
    }
  }
  return SONARE_OK;
  SONARE_C_CATCH
}

const char* sonare_mixing_scene_preset_names(void) {
  static std::string storage;
  return join_names(sonare::mixing::api::scene_preset_names(), storage);
}

SonareError sonare_mixing_scene_preset_json(const char* preset_name, char** json_out) {
  if (!preset_name || !json_out) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  *json_out = nullptr;
  const auto preset = sonare::mixing::api::scene_preset_from_string(preset_name);
  const auto scene = sonare::mixing::api::scene_preset(preset);
  *json_out = copy_string(sonare::mixing::api::scene_to_json(scene));
  return SONARE_OK;
  SONARE_C_CATCH
}

void sonare_mixer_destroy(SonareMixer* mixer) { delete mixer; }
