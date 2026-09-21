/// @file spectral_edit.cpp
/// @brief Embind bindings for region-based spectral editing.

#ifdef __EMSCRIPTEN__

#include <algorithm>

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
  throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                "spectralEdit: unknown mode: " + s);
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
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "spectralEdit: window must be a window name or a window ordinal");
  }
  std::string s = window.as<std::string>();
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (s == "hann") return WindowType::Hann;
  if (s == "hamming") return WindowType::Hamming;
  if (s == "blackman") return WindowType::Blackman;
  if (s == "rectangular" || s == "rect") return WindowType::Rectangular;
  throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                "spectralEdit: unknown window: " + s);
}

}  // namespace

// Region-based spectral editing (STFT -> per-op bin/frame masking -> iSTFT).
// Mirrors the core sonare::spectral_edit. @p ops is a JS array of region objects
// { startSample, endSample, lowHz, highHz, gainDb, mode } and @p options is an
// optional config bag { nFft, hopLength, window, healRadiusFrames }. Returns the
// edited audio (same length/sample rate as the input) as a Float32Array.
val js_spectral_edit(val samples, const val& sample_rate, val ops, val options) {
  Audio audio = loadValidatedAudio(samples, checkedIntFromVal(sample_rate, "sampleRate"));

  SpectralEditConfig config;
  if (!options.isUndefined() && !options.isNull()) {
    // Mirror the C-ABI oracle (sonare_spectral_edit): a value of 0 means "keep
    // the core default" rather than forcing an invalid 0 into the core, which
    // requires n_fft/hop_length >= 1 and heal_radius_frames >= 1. Previously the
    // WASM path passed 0 through verbatim and threw where Node/Python succeeded.
    if (hasProperty(options, "nFft")) {
      const int n_fft = checkedIntFromVal(options["nFft"], "nFft");
      if (n_fft != 0) config.n_fft = n_fft;
    }
    if (hasProperty(options, "hopLength")) {
      const int hop_length = checkedIntFromVal(options["hopLength"], "hopLength");
      if (hop_length != 0) config.hop_length = hop_length;
    }
    if (hasProperty(options, "window")) {
      config.window = parseSpectralEditWindow(options["window"]);
    }
    if (hasProperty(options, "healRadiusFrames")) {
      const int heal_radius = checkedIntFromVal(options["healRadiusFrames"], "healRadiusFrames");
      if (heal_radius != 0) config.heal_radius_frames = heal_radius;
    }
  }

  std::vector<SpectralRegionOp> region_ops;
  if (!ops.isUndefined() && !ops.isNull()) {
    // The op count comes from an untrusted JS `.length`: validate it through the
    // shared safe-integer + budget guard, and cap the pre-reserve so a
    // fabricated length cannot allocate storage the array does not back. A
    // longer genuine array still works — the vector grows as ops are read.
    const std::size_t n = wasmArrayLikeLength(ops, "spectral edit ops");
    region_ops.reserve(std::min(n, kMaxWasmObjectArrayReserve));
    for (std::size_t i = 0; i < n; ++i) {
      val op = ops[i];
      SpectralRegionOp region;
      // An omitted endSample defaults to the whole signal (matches the Node
      // facade, sonare_wrap_effects.cpp). The core defaults end_sample to 0,
      // which would otherwise make an omitted endSample a silent no-op here while
      // Node processes the full region.
      region.end_sample = static_cast<int64_t>(audio.size());
      // Sample positions arrive as plain JS numbers; read as double and cast to
      // int64 (mirrors project.cpp's totalFrames) so callers need not pass BigInt.
      if (hasProperty(op, "startSample")) {
        region.start_sample = static_cast<int64_t>(op["startSample"].as<double>());
      }
      if (hasProperty(op, "endSample")) {
        region.end_sample = static_cast<int64_t>(op["endSample"].as<double>());
      }
      if (hasProperty(op, "lowHz")) region.low_hz = checkedFloatFromVal(op["lowHz"], "lowHz");
      if (hasProperty(op, "highHz")) region.high_hz = checkedFloatFromVal(op["highHz"], "highHz");
      if (hasProperty(op, "gainDb")) region.gain_db = checkedFloatFromVal(op["gainDb"], "gainDb");
      region.mode =
          hasProperty(op, "mode") ? parseSpectralEditMode(op["mode"]) : SpectralEditMode::Gain;
      region_ops.push_back(region);
    }
  }

  Audio result = spectral_edit(audio, config, region_ops.data(), region_ops.size());
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out_vec);
}

void registerEffectsSpectralEditBindings() { function("spectralEdit", &js_spectral_edit); }

#endif  // __EMSCRIPTEN__
