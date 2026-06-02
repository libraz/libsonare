#ifndef SONARE_NODE_SONARE_WRAP_KEY_OPTIONS_H_
#define SONARE_NODE_SONARE_WRAP_KEY_OPTIONS_H_

#include <napi.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "sonare_c.h"

namespace sonare_node {

/// @brief Parse a single key mode from a JS value (numeric ordinal or name).
inline SonareMode node_mode_from_value(const Napi::Value& value) {
  if (value.IsNumber()) {
    const int mode = value.As<Napi::Number>().Int32Value();
    if (mode < SONARE_MODE_MAJOR || mode > SONARE_MODE_LOCRIAN) {
      throw Napi::Error::New(value.Env(), "invalid key mode");
    }
    return static_cast<SonareMode>(mode);
  }
  if (!value.IsString()) {
    throw Napi::Error::New(value.Env(), "key modes must be strings or numbers");
  }
  std::string key = value.As<Napi::String>().Utf8Value();
  std::transform(key.begin(), key.end(), key.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (key == "major" || key == "maj") return SONARE_MODE_MAJOR;
  if (key == "minor" || key == "min" || key == "m") return SONARE_MODE_MINOR;
  if (key == "dorian") return SONARE_MODE_DORIAN;
  if (key == "phrygian") return SONARE_MODE_PHRYGIAN;
  if (key == "lydian") return SONARE_MODE_LYDIAN;
  if (key == "mixolydian") return SONARE_MODE_MIXOLYDIAN;
  if (key == "locrian") return SONARE_MODE_LOCRIAN;
  throw Napi::Error::New(value.Env(), "invalid key mode: " + key);
}

/// @brief Parse the `modes` option: a mode-set string, single mode, or array.
inline std::vector<SonareMode> node_modes_option(const Napi::Object& object) {
  Napi::Value value = object.Get("modes");
  if (value.IsUndefined() || value.IsNull()) {
    return {};
  }
  if (value.IsString()) {
    std::string key = value.As<Napi::String>().Utf8Value();
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (key == "major-minor" || key == "majmin" || key == "diatonic") {
      return {SONARE_MODE_MAJOR, SONARE_MODE_MINOR};
    }
    if (key == "all" || key == "modal") {
      return {SONARE_MODE_MAJOR,  SONARE_MODE_MINOR,      SONARE_MODE_DORIAN, SONARE_MODE_PHRYGIAN,
              SONARE_MODE_LYDIAN, SONARE_MODE_MIXOLYDIAN, SONARE_MODE_LOCRIAN};
    }
    return {node_mode_from_value(value)};
  }
  if (!value.IsArray()) {
    throw Napi::Error::New(object.Env(), "modes must be an array or mode-set string");
  }
  Napi::Array arr = value.As<Napi::Array>();
  std::vector<SonareMode> modes;
  modes.reserve(arr.Length());
  for (uint32_t i = 0; i < arr.Length(); ++i) {
    modes.push_back(node_mode_from_value(arr.Get(i)));
  }
  return modes;
}

/// @brief Parse the `profile` option (numeric ordinal or profile name).
inline SonareKeyProfileType node_profile_from_value(const Napi::Value& value) {
  if (value.IsUndefined() || value.IsNull()) {
    return SONARE_KEY_PROFILE_KRUMHANSL_SCHMUCKLER;
  }
  if (value.IsNumber()) {
    const int profile = value.As<Napi::Number>().Int32Value();
    if (profile < SONARE_KEY_PROFILE_KRUMHANSL_SCHMUCKLER ||
        profile > SONARE_KEY_PROFILE_BELLMAN_BUDGE) {
      throw Napi::Error::New(value.Env(), "invalid key profile");
    }
    return static_cast<SonareKeyProfileType>(profile);
  }
  if (!value.IsString()) {
    throw Napi::Error::New(value.Env(), "key profile must be a string or number");
  }
  std::string key = value.As<Napi::String>().Utf8Value();
  std::transform(key.begin(), key.end(), key.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (key == "ks" || key == "krumhansl" || key == "krumhansl-schmuckler") {
    return SONARE_KEY_PROFILE_KRUMHANSL_SCHMUCKLER;
  }
  if (key == "temperley") return SONARE_KEY_PROFILE_TEMPERLEY;
  if (key == "shaath" || key == "keyfinder") return SONARE_KEY_PROFILE_SHAATH;
  if (key == "faraldo-edmt" || key == "edmt") return SONARE_KEY_PROFILE_FARALDO_EDMT;
  if (key == "faraldo-edma" || key == "edma") return SONARE_KEY_PROFILE_FARALDO_EDMA;
  if (key == "faraldo-edmm" || key == "edmm") return SONARE_KEY_PROFILE_FARALDO_EDMM;
  if (key == "bellman-budge" || key == "bellman") return SONARE_KEY_PROFILE_BELLMAN_BUDGE;
  throw Napi::Error::New(value.Env(), "invalid key profile: " + key);
}

/// @brief Read the optional `genreHint` string option ("" if absent).
inline std::string node_genre_hint_option(const Napi::Object& object) {
  Napi::Value value = object.Get("genreHint");
  return value.IsString() ? value.As<Napi::String>().Utf8Value() : std::string{};
}

}  // namespace sonare_node

#endif  // SONARE_NODE_SONARE_WRAP_KEY_OPTIONS_H_
