/// @file spectral_edit.cpp
/// @brief Embind bindings for region-based spectral editing.

#ifdef __EMSCRIPTEN__

#include <algorithm>
#include <string>

#include "c_api/effects_config_domains.h"
#include "core/spectrum.h"
#include "util/parameter_checks.h"
#include "wasm/bindings/common/common.h"

namespace {

// Map a spectral-edit mode string ('gain'|'attenuate'|'mute'|'heal') to the
// SpectralEditMode enum. Defaults to Gain when the value is absent.
SpectralEditMode parseSpectralEditMode(val mode) {
  if (mode.isUndefined() || mode.isNull()) return SpectralEditMode::Gain;
  std::string s = mode.as<std::string>();
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (s == "gain") return SpectralEditMode::Gain;
  if (s == "attenuate") return SpectralEditMode::Attenuate;
  if (s == "mute") return SpectralEditMode::Mute;
  if (s == "heal") return SpectralEditMode::Heal;
  throw WasmRangeError("unknown mode: " + s);
}

// Map a window string ('hann'|'hamming'|'blackman'|'rectangular') or a
// SonareWindowType ordinal to WindowType. Both spellings are accepted on every
// surface and both refuse an unmapped value with InvalidParameter; reading the
// field as a string unconditionally used to send a number out as a raw embind
// conversion error carrying no error code at all.
WindowType parseSpectralEditWindow(val window) {
  if (window.isNumber()) {
    const int ordinal = checkedIntFromVal(window, "window");
    requireOrdinalInRange(ordinal, SONARE_WINDOW_HANN, SONARE_WINDOW_RECTANGULAR, "window");
    switch (static_cast<SonareWindowType>(ordinal)) {
      case SONARE_WINDOW_HANN:
        return WindowType::Hann;
      case SONARE_WINDOW_HAMMING:
        return WindowType::Hamming;
      case SONARE_WINDOW_BLACKMAN:
        return WindowType::Blackman;
      case SONARE_WINDOW_RECTANGULAR:
        return WindowType::Rectangular;
    }
  }
  if (!window.isString()) {
    throw WasmRangeError("window must be a window name or a window ordinal");
  }
  std::string s = window.as<std::string>();
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (s == "hann") return WindowType::Hann;
  if (s == "hamming") return WindowType::Hamming;
  if (s == "blackman") return WindowType::Blackman;
  if (s == "rectangular" || s == "rect") return WindowType::Rectangular;
  throw WasmRangeError("unknown window: " + s);
}

// Reads float member @p key of op @p index, refusing it under its path "ops[i].key".
std::string opPath(std::size_t index, const char* key) {
  return "ops[" + std::to_string(index) + "]." + key;
}

}  // namespace

// Region-based spectral editing (STFT -> per-op bin/frame masking -> iSTFT).
// Mirrors the core sonare::spectral_edit. @p ops is a JS array of region objects
// { startSample, endSample, lowHz, highHz, gainDb, mode } and @p options is an
// optional config bag { nFft, hopLength, window, healRadiusFrames }. Returns the
// edited audio (same length/sample rate as the input) as a Float32Array.
val js_spectral_edit(val samples, const val& sample_rate, val ops, val options) {
  Audio audio =
      loadValidatedAudio("samples", samples, checkedIntFromVal(sample_rate, "sampleRate"));

  SpectralEditConfig config;
  if (!options.isUndefined() && !options.isNull()) {
    // An absent key keeps the core default (an absent hop derives it from nFft);
    // a given value is read literally and checked as sonare_spectral_edit checks it.
    if (hasProperty(options, "nFft")) {
      config.n_fft = checkedIntFromVal(options["nFft"], "nFft");
    }
    if (hasProperty(options, "hopLength")) {
      config.hop_length =
          domainNumberFromVal<int>(options["hopLength"], "hopLength", sonare::kStftHopDomain);
    }
    if (hasProperty(options, "window")) {
      config.window = parseSpectralEditWindow(options["window"]);
    }
    if (hasProperty(options, "healRadiusFrames")) {
      config.heal_radius_frames =
          domainNumberFromVal<int>(options["healRadiusFrames"], "healRadiusFrames",
                                   sonare_c_detail::kSpectralEditHealRadiusDomain);
    }
  }

  std::vector<SpectralRegionOp> region_ops;
  if (!ops.isUndefined() && !ops.isNull()) {
    // The op count comes from an untrusted JS `.length`: validate it through the
    // shared safe-integer + budget guard, and cap the pre-reserve so a
    // fabricated length cannot allocate storage the array does not back. A
    // longer genuine array still works — the vector grows as ops are read.
    const std::size_t n = wasmArrayLength(ops, "ops");
    region_ops.reserve(std::min(n, kMaxWasmObjectArrayReserve));
    for (std::size_t i = 0; i < n; ++i) {
      val op = ops[i];
      SpectralRegionOp region;
      // An omitted endSample defaults to the whole signal (matches the Node
      // addon, bindings/node/src/addon/effects/spectral_edit.cpp). The core
      // defaults end_sample to 0, which would otherwise make an omitted endSample a
      // silent no-op here while Node processes the full region.
      region.end_sample = static_cast<int64_t>(audio.size());
      // Sample positions arrive as plain JS numbers; requireInt64Property
      // refuses a fraction or an out-of-int64-range value by name instead of
      // truncating or casting into undefined behavior. Callers need not pass
      // BigInt -- a plain number is required.
      if (hasProperty(op, "startSample")) {
        region.start_sample = requireInt64Property(op, "startSample", "region");
      }
      if (hasProperty(op, "endSample")) {
        region.end_sample = requireInt64Property(op, "endSample", "region");
      }
      if (hasProperty(op, "lowHz"))
        region.low_hz = checkedFloatFromVal(op["lowHz"], opPath(i, "lowHz").c_str());
      // An absent high edge is nyquist (the core's 0); a given one must be positive.
      if (hasProperty(op, "highHz")) {
        region.high_hz = checkedFloatFromVal(op["highHz"], opPath(i, "highHz").c_str());
        sonare::util::check::greater_than(region.high_hz, 0.0, opPath(i, "highHz").c_str());
      }
      if (hasProperty(op, "gainDb"))
        region.gain_db = checkedFloatFromVal(op["gainDb"], opPath(i, "gainDb").c_str());
      region.mode =
          hasProperty(op, "mode") ? parseSpectralEditMode(op["mode"]) : SpectralEditMode::Gain;
      region_ops.push_back(region);
    }
  }

  Audio result = spectral_edit(audio, config, region_ops.data(), region_ops.size());
  return vectorToFloat32Array(result.data(), result.size());
}

void registerEffectsSpectralEditBindings() { function("spectralEdit", &js_spectral_edit); }

#endif  // __EMSCRIPTEN__
