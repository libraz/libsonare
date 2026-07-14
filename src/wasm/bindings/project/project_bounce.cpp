/// @file project_bounce.cpp
/// @brief Embind project facade: compile + offline bounce family, the SoundFont
/// surface, and the NativeSynth preset / enum free functions.

#ifdef __EMSCRIPTEN__

#include "project_wasm.h"

#if defined(SONARE_WITH_ARRANGEMENT)

val ProjectWasm::compile() {
  SonareProjectCompileResult result{};
  const SonareError err = sonare_project_compile(project_.get(), &result);
  if (err != SONARE_OK) {
    sonare_project_free_compile_result(&result);
    throwCError(err, "failed to compile project");
  }
  val out = projectCompileResultToVal(result);
  sonare_project_free_compile_result(&result);
  return out;
}

SonareProjectBounceOptions ProjectWasm::bounceOptionsFromVal(val options) {
  SonareProjectBounceOptions opts{};
  if (!options.isUndefined() && !options.isNull()) {
    if (hasProperty(options, "totalFrames")) {
      opts.total_frames = static_cast<int64_t>(options["totalFrames"].as<double>());
    }
    if (hasProperty(options, "blockSize")) {
      opts.block_size = options["blockSize"].as<int>();
    }
    if (hasProperty(options, "numChannels")) {
      opts.num_channels = options["numChannels"].as<int>();
      // The project bounce only produces a mono downmix or the stereo pair;
      // wider counts would surface a generic InvalidState from the C ABI later.
      // Reject them here so WASM matches the C-ABI oracle up front. A
      // non-positive count defers to the C-ABI default (stereo).
      if (opts.num_channels > 2) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                      "unsupported bounce channel count");
      }
    }
    if (hasProperty(options, "sampleRate")) {
      opts.sample_rate = options["sampleRate"].as<int>();
    }
    if (hasProperty(options, "instrumentLatencySamples")) {
      opts.instrument_latency_samples = options["instrumentLatencySamples"].as<int>();
    }
  }
  return opts;
}

int ProjectWasm::waveformFromName(const std::string& name) {
  if (name == "sine") return SONARE_SYNTH_WAVEFORM_SINE;
  if (name == "saw" || name == "sawtooth") return SONARE_SYNTH_WAVEFORM_SAW;
  if (name == "square") return SONARE_SYNTH_WAVEFORM_SQUARE;
  if (name == "triangle") return SONARE_SYNTH_WAVEFORM_TRIANGLE;
  return -1;
}

SonareBuiltinInstrumentBinding ProjectWasm::builtinBindingFromVal(val desc) {
  SonareBuiltinInstrumentBinding binding{};
  if (desc.isUndefined() || desc.isNull()) {
    return binding;
  }
  if (hasProperty(desc, "destinationId")) {
    binding.destination_id = desc["destinationId"].as<uint32_t>();
  }
  if (hasProperty(desc, "waveform")) {
    val wf = desc["waveform"];
    if (wf.typeOf().as<std::string>() == "string") {
      const std::string s = wf.as<std::string>();
      const int mapped = waveformFromName(s);
      if (mapped < 0) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                      "Unknown synth waveform name: '" + s +
                                          "' (expected sine, saw, sawtooth, square, or triangle)");
      }
      binding.config.waveform = mapped;
    } else {
      binding.config.waveform = wf.as<int>();
    }
  }
  if (hasProperty(desc, "gain")) {
    binding.config.gain = desc["gain"].as<float>();
  }
  if (hasProperty(desc, "attackMs")) {
    binding.config.attack_ms = desc["attackMs"].as<float>();
  }
  if (hasProperty(desc, "decayMs")) {
    binding.config.decay_ms = desc["decayMs"].as<float>();
  }
  if (hasProperty(desc, "sustain")) {
    binding.config.sustain = desc["sustain"].as<float>();
  }
  if (hasProperty(desc, "releaseMs")) {
    binding.config.release_ms = desc["releaseMs"].as<float>();
  }
  if (hasProperty(desc, "polyphony")) {
    binding.config.polyphony = desc["polyphony"].as<int>();
  }
  return binding;
}

std::vector<SonareBuiltinInstrumentBinding> ProjectWasm::builtinBindingsFromVal(val bindings) {
  std::vector<SonareBuiltinInstrumentBinding> out;
  if (bindings.isUndefined() || bindings.isNull()) {
    return out;
  }
  if (val::global("Array").call<bool>("isArray", bindings)) {
    const size_t count = bindings["length"].as<size_t>();
    out.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      out.push_back(builtinBindingFromVal(bindings[i]));
    }
    return out;
  }
  out.push_back(builtinBindingFromVal(bindings));
  return out;
}

val ProjectWasm::bounce(val options) {
  SonareProjectBounceOptions opts = bounceOptionsFromVal(options);
  float* interleaved = nullptr;
  size_t len = 0;
  const SonareError err = sonare_project_bounce(project_.get(), &opts, &interleaved, &len);
  if (err != SONARE_OK) {
    sonare_free_floats(interleaved);
    throwCError(err, "failed to bounce project");
  }
  std::vector<float> samples(interleaved, interleaved + len);
  sonare_free_floats(interleaved);
  return vectorToFloat32Array(samples);
}

val ProjectWasm::bounceWithBuiltinInstrument(val bindings, val options) {
  std::vector<SonareBuiltinInstrumentBinding> synths = builtinBindingsFromVal(bindings);
  SonareProjectBounceOptions opts = bounceOptionsFromVal(options);
  float* interleaved = nullptr;
  size_t len = 0;
  const SonareError err = sonare_project_bounce_with_builtin_instruments(
      project_.get(), &opts, synths.empty() ? nullptr : synths.data(), synths.size(), &interleaved,
      &len);
  if (err != SONARE_OK) {
    sonare_free_floats(interleaved);
    throwCError(err, "failed to bounce project with built-in instrument");
  }
  std::vector<float> samples(interleaved, interleaved + len);
  sonare_free_floats(interleaved);
  return vectorToFloat32Array(samples);
}

val ProjectWasm::bounceWithSynthInstrument(val bindings, val options) {
  std::vector<SonareSynthInstrumentBinding> synths;
  if (!bindings.isUndefined() && !bindings.isNull()) {
    auto bindingFromVal = [](val desc) {
      SonareSynthInstrumentBinding binding{};
      if (desc.typeOf().as<std::string>() == "object" && hasProperty(desc, "destinationId")) {
        binding.destination_id = desc["destinationId"].as<uint32_t>();
      }
      binding.patch = sonare_wasm_synth::synthPatchFromVal(desc);
      return binding;
    };
    if (val::global("Array").call<bool>("isArray", bindings)) {
      const size_t count = bindings["length"].as<size_t>();
      synths.reserve(count);
      for (size_t i = 0; i < count; ++i) synths.push_back(bindingFromVal(bindings[i]));
    } else {
      synths.push_back(bindingFromVal(bindings));
    }
  }
  SonareProjectBounceOptions opts = bounceOptionsFromVal(options);
  float* interleaved = nullptr;
  size_t len = 0;
  const SonareError err = sonare_project_bounce_with_synth_instruments(
      project_.get(), &opts, synths.empty() ? nullptr : synths.data(), synths.size(), &interleaved,
      &len);
  if (err != SONARE_OK) {
    sonare_free_floats(interleaved);
    throwCError(err, "failed to bounce project with synth instrument");
  }
  std::vector<float> samples(interleaved, interleaved + len);
  sonare_free_floats(interleaved);
  return vectorToFloat32Array(samples);
}

void ProjectWasm::loadSoundFont(val data) {
  std::vector<uint8_t> bytes = uint8ArrayToVector(data);
  const SonareError err = sonare_project_load_soundfont(
      project_.get(), bytes.empty() ? nullptr : bytes.data(), bytes.size());
  if (err != SONARE_OK) {
    throwCError(err, "failed to load SoundFont");
  }
}

void ProjectWasm::clearSoundFont() {
  const SonareError err = sonare_project_clear_soundfont(project_.get());
  if (err != SONARE_OK) {
    throwCError(err, "failed to clear SoundFont");
  }
}

size_t ProjectWasm::soundFontPresetCount() {
  size_t count = 0;
  const SonareError err = sonare_project_soundfont_preset_count(project_.get(), &count);
  if (err != SONARE_OK) {
    throwCError(err, "failed to query SoundFont preset count");
  }
  return count;
}

val ProjectWasm::soundFontManifest() {
  size_t total = 0;
  SonareError err = sonare_project_soundfont_manifest(project_.get(), nullptr, 0, &total);
  if (err != SONARE_OK) {
    throwCError(err, "failed to build SoundFont manifest");
  }
  std::vector<SonareSf2ProgramStatus> entries(total);
  if (total > 0) {
    err = sonare_project_soundfont_manifest(project_.get(), entries.data(), total, &total);
    if (err != SONARE_OK) {
      throwCError(err, "failed to build SoundFont manifest");
    }
  }
  val out = val::array();
  for (size_t i = 0; i < entries.size(); ++i) {
    val entry = val::object();
    entry.set("channel", entries[i].channel);
    entry.set("bank", entries[i].bank);
    entry.set("program", entries[i].program);
    entry.set("backend",
              std::string(entries[i].backend == SONARE_SOURCE_BACKEND_SF2 ? "sf2" : "synth"));
    entry.set("presetName", std::string(entries[i].preset_name));
    out.set(i, entry);
  }
  return out;
}

SonareSf2InstrumentBinding ProjectWasm::sf2BindingFromVal(val desc) {
  SonareSf2InstrumentBinding binding{};
  if (desc.isUndefined() || desc.isNull()) {
    return binding;
  }
  if (hasProperty(desc, "destinationId")) {
    binding.destination_id = desc["destinationId"].as<uint32_t>();
  }
  if (hasProperty(desc, "gain")) {
    binding.config.gain = desc["gain"].as<float>();
  }
  if (hasProperty(desc, "polyphony")) {
    binding.config.polyphony = desc["polyphony"].as<int>();
  }
  return binding;
}

val ProjectWasm::bounceWithSf2Instrument(val bindings, val options) {
  std::vector<SonareSf2InstrumentBinding> players;
  if (!bindings.isUndefined() && !bindings.isNull()) {
    if (val::global("Array").call<bool>("isArray", bindings)) {
      const size_t count = bindings["length"].as<size_t>();
      players.reserve(count);
      for (size_t i = 0; i < count; ++i) {
        players.push_back(sf2BindingFromVal(bindings[i]));
      }
    } else {
      players.push_back(sf2BindingFromVal(bindings));
    }
  }
  SonareProjectBounceOptions opts = bounceOptionsFromVal(options);
  float* interleaved = nullptr;
  size_t len = 0;
  const SonareError err = sonare_project_bounce_with_sf2_instruments(
      project_.get(), &opts, players.empty() ? nullptr : players.data(), players.size(),
      &interleaved, &len);
  if (err != SONARE_OK) {
    sonare_free_floats(interleaved);
    throwCError(err, "failed to bounce project with SF2 instrument");
  }
  std::vector<float> samples(interleaved, interleaved + len);
  sonare_free_floats(interleaved);
  return vectorToFloat32Array(samples);
}

// NativeSynth preset catalog ('\n'-joined program-lifetime string from the C
// ABI) split into a JS string[].
val js_synth_preset_names() {
  val out = val::array();
  const char* joined = sonare_synth_preset_names();
  if (joined == nullptr || joined[0] == '\0') return out;
  std::string names(joined);
  size_t start = 0;
  while (start <= names.size()) {
    const size_t end = names.find('\n', start);
    if (end == std::string::npos) {
      out.call<void>("push", names.substr(start));
      break;
    }
    out.call<void>("push", names.substr(start, end - start));
    start = end + 1;
  }
  return out;
}

// Fetches a named catalog preset as a SynthPatch object (the preset name plus
// its wrapper-section values). A "va:" routing prefix is accepted; unknown
// names throw.
val js_synth_preset_patch(const std::string& name) {
  const std::string bare = name.rfind("va:", 0) == 0 ? name.substr(3) : name;
  SonareSynthPatch patch{};
  if (sonare_synth_preset_patch(bare.c_str(), &patch) != SONARE_OK) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "unknown synth preset name: '" + name + "'");
  }
  return sonare_wasm_synth::synthPatchToVal(patch);
}

val js_synth_enum_tables() { return sonare_wasm_synth::synthEnumTablesToVal(); }

val js_synth_patch_round_trip(val desc) {
  return sonare_wasm_synth::synthPatchToVal(sonare_wasm_synth::synthPatchFromVal(desc));
}

void registerProjectBounce(class_<ProjectWasm>& cls) {
  cls.function("compile", &ProjectWasm::compile)
      .function("bounce", &ProjectWasm::bounce)
      .function("bounceWithBuiltinInstrument", &ProjectWasm::bounceWithBuiltinInstrument)
      .function("bounceWithSynthInstrument", &ProjectWasm::bounceWithSynthInstrument)
      .function("loadSoundFont", &ProjectWasm::loadSoundFont)
      .function("clearSoundFont", &ProjectWasm::clearSoundFont)
      .function("soundFontPresetCount", &ProjectWasm::soundFontPresetCount)
      .function("soundFontManifest", &ProjectWasm::soundFontManifest)
      .function("bounceWithSf2Instrument", &ProjectWasm::bounceWithSf2Instrument);
}

#endif  // SONARE_WITH_ARRANGEMENT

#endif  // __EMSCRIPTEN__
