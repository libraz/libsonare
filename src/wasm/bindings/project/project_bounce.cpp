/// @file project_bounce.cpp
/// @brief Embind project facade: compile + offline bounce family, the SoundFont
/// surface, the sample bank, and the NativeSynth preset / enum free functions.

#ifdef __EMSCRIPTEN__

#include <unordered_map>

#include "project_wasm.h"

#if defined(SONARE_WITH_ARRANGEMENT)

namespace {

/// Live sample banks by id. A bounce binding names a bank by the id its handle
/// carries rather than by a raw pointer, so a released or fabricated id is an
/// InvalidParameter instead of a use-after-free. Control thread only, which
/// single-threaded WASM guarantees.
std::unordered_map<uint32_t, SonareSampleBank*>& sampleBankRegistry() {
  static std::unordered_map<uint32_t, SonareSampleBank*> registry;
  return registry;
}

/// Ids start at 1 so zero stays available as "no bank" in a binding.
uint32_t nextSampleBankId() {
  static uint32_t next = 1;
  return next++;
}

/// Optional numeric field: absent keeps @p fallback, present-but-not-a-finite-
/// number throws rather than coercing.
double numberField(val object, const char* key, const char* subject, double fallback) {
  if (!hasProperty(object, key)) return fallback;
  return requireNumberProperty(object, key, subject);
}

/// Optional unsigned-integer field, rejected before it is narrowed — a bare
/// cast would wrap 128 to 0 and 4294967296 to nothing at all.
double integerField(val object, const char* key, const char* subject, double max) {
  const double value = numberField(object, key, subject, 0.0);
  if (value < 0.0 || value > max || std::floor(value) != value) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  std::string(subject) + "." + key + " must be an integer in [0, " +
                                      std::to_string(static_cast<long long>(max)) + "]");
  }
  return value;
}

/// SonareSampleDesc.loop_mode carries SoundFont sampleModes, so a NUMBER passes
/// through as the raw SF2 value and SF2-derived data needs no translation. The
/// names are SynthPatch.sampleLoop's spellings mapped onto that scale, matching
/// the Node reader.
int sampleDescLoopMode(val desc) {
  if (!hasProperty(desc, "loopMode")) return 0;
  const val value = desc["loopMode"];
  if (value.typeOf().as<std::string>() == "string") {
    const std::string name = value.as<std::string>();
    if (name == "none") return 0;
    if (name == "continuous") return 1;
    if (name == "key-down") return 3;
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        "Unknown sample loop mode name: '" + name + "' (expected none, continuous or key-down)");
  }
  return static_cast<int>(integerField(desc, "loopMode", "sample descriptor", 3.0));
}

}  // namespace

SampleBankWasm::SampleBankWasm() : bank_(sonare_sample_bank_create()), id_(nextSampleBankId()) {
  if (bank_ == nullptr) {
    throw sonare::SonareException(sonare::ErrorCode::OutOfMemory, "failed to create sample bank");
  }
  sampleBankRegistry().emplace(id_, bank_);
}

SampleBankWasm::~SampleBankWasm() {
  sampleBankRegistry().erase(id_);
  sonare_sample_bank_destroy(bank_);
}

SonareSampleBank* SampleBankWasm::lookup(uint32_t id) {
  const auto& registry = sampleBankRegistry();
  const auto it = registry.find(id);
  return it != registry.end() ? it->second : nullptr;
}

uint32_t SampleBankWasm::addSample(val data, val desc) {
  if (wasmFloat32ArrayLength(data, "sample data") == 0) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "sample data must not be empty");
  }
  const std::vector<float> frames = float32ArrayToVector(data);

  SonareSampleDesc c{};
  c.root_key = static_cast<uint8_t>(integerField(desc, "rootKey", "sample descriptor", 127.0));
  c.fine_tune_cents =
      static_cast<float>(numberField(desc, "fineTuneCents", "sample descriptor", 0.0));
  c.source_rate = numberField(desc, "sourceRate", "sample descriptor", 0.0);
  c.loop_start =
      static_cast<uint32_t>(integerField(desc, "loopStart", "sample descriptor", 4294967295.0));
  c.loop_end =
      static_cast<uint32_t>(integerField(desc, "loopEnd", "sample descriptor", 4294967295.0));
  c.loop_mode = sampleDescLoopMode(desc);

  uint32_t index = 0;
  const SonareError err =
      sonare_sample_bank_add_sample(bank_, frames.data(), frames.size(), &c, &index);
  if (err != SONARE_OK) throwCError(err, "failed to add a sample to the bank");
  return index;
}

void SampleBankWasm::addZone(const val& set_index_val, val zone) {
  // Taken as a val rather than a double so the shared uint32 reader can serve:
  // wasmCountArg cannot, because on wasm32 a size_t is 32 bits and bounding its
  // result at 2^32-1 is a tautology -Werror rejects.
  const uint32_t set_index = checkedUintFromVal(set_index_val, "setIndex");
  // An absent bag leaves the C struct zero-initialized, which the C ABI
  // documents as the neutral zone; a present one that is not an object is a
  // caller mistake rather than a default.
  if (!zone.isUndefined() && !zone.isNull() && zone.typeOf().as<std::string>() != "object") {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "addZone zone must be an object");
  }
  SonareSampleZoneDesc c{};
  c.sample_index =
      static_cast<uint32_t>(integerField(zone, "sampleIndex", "sample zone", 4294967295.0));
  // An absent bound stays zero, which the C ABI defaults per bound: 127 for an
  // upper edge, 1 for vel_lo, the lowest key for key_lo.
  c.key_lo = static_cast<uint8_t>(integerField(zone, "keyLo", "sample zone", 127.0));
  c.key_hi = static_cast<uint8_t>(integerField(zone, "keyHi", "sample zone", 127.0));
  c.vel_lo = static_cast<uint8_t>(integerField(zone, "velLo", "sample zone", 127.0));
  c.vel_hi = static_cast<uint8_t>(integerField(zone, "velHi", "sample zone", 127.0));
  c.tune_cents = static_cast<float>(numberField(zone, "tuneCents", "sample zone", 0.0));
  c.gain = static_cast<float>(numberField(zone, "gain", "sample zone", 0.0));
  c.pan_units = static_cast<float>(numberField(zone, "panUnits", "sample zone", 0.0));

  const SonareError err = sonare_sample_bank_add_zone(bank_, set_index, &c);
  if (err != SONARE_OK) throwCError(err, "failed to add a zone to the sample bank");
}

double SampleBankWasm::sampleCount() const {
  std::size_t count = 0;
  const SonareError err = sonare_sample_bank_sample_count(bank_, &count);
  if (err != SONARE_OK) throwCError(err, "failed to read the bank sample count");
  return static_cast<double>(count);
}

double SampleBankWasm::setCount() const {
  std::size_t count = 0;
  const SonareError err = sonare_sample_bank_set_count(bank_, &count);
  if (err != SONARE_OK) throwCError(err, "failed to read the bank keymap set count");
  return static_cast<double>(count);
}

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
      opts.block_size = checkedIntFromVal(options["blockSize"], "blockSize");
    }
    if (hasProperty(options, "numChannels")) {
      opts.num_channels = checkedIntFromVal(options["numChannels"], "numChannels");
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
      opts.sample_rate = checkedIntFromVal(options["sampleRate"], "sampleRate");
    }
    if (hasProperty(options, "instrumentLatencySamples")) {
      opts.instrument_latency_samples =
          checkedIntFromVal(options["instrumentLatencySamples"], "instrumentLatencySamples");
    }
  }
  return opts;
}

SonareBuiltinInstrumentBinding ProjectWasm::builtinBindingFromVal(val desc) {
  SonareBuiltinInstrumentBinding binding{};
  if (desc.isUndefined() || desc.isNull()) {
    return binding;
  }
  binding.destination_id = uintProperty(desc, "destinationId", binding.destination_id);
  if (hasProperty(desc, "waveform")) {
    binding.config.waveform = builtinWaveformFromVal(desc["waveform"]);
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
    binding.config.polyphony = checkedIntFromVal(desc["polyphony"], "polyphony");
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
      if (desc.typeOf().as<std::string>() == "object") {
        binding.destination_id = uintProperty(desc, "destinationId", binding.destination_id);
        if (hasProperty(desc, "useGmPrograms")) {
          binding.use_gm_programs =
              requireProperty<bool>(desc, "useGmPrograms", "synth instrument") ? 1 : 0;
        }
        binding.sample_bank = SampleBankWasm::fromDescriptor(desc);
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
  binding.destination_id = uintProperty(desc, "destinationId", binding.destination_id);
  if (hasProperty(desc, "gain")) {
    binding.config.gain = desc["gain"].as<float>();
  }
  if (hasProperty(desc, "polyphony")) {
    binding.config.polyphony = checkedIntFromVal(desc["polyphony"], "polyphony");
  }
  if (hasProperty(desc, "preferModelForModeledFamilies")) {
    binding.config.struct_version = 2;
    binding.config.prefer_model_for_modeled_families =
        desc["preferModelForModeledFamilies"].as<bool>() ? 1 : 0;
  }
  // Version 3 reads version 2's field as well, so raising it here covers both
  // whichever of the two the caller passed.
  if (hasProperty(desc, "clearBankRig")) {
    binding.config.struct_version = 3;
    binding.config.clear_bank_rig = desc["clearBankRig"].as<bool>() ? 1 : 0;
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

void registerSampleBank() {
  class_<SampleBankWasm>("SampleBank")
      .constructor<>()
      .property("id", &SampleBankWasm::id)
      .function("addSample", &SampleBankWasm::addSample)
      .function("addZone", &SampleBankWasm::addZone)
      .function("sampleCount", &SampleBankWasm::sampleCount)
      .function("setCount", &SampleBankWasm::setCount);
}

#endif  // SONARE_WITH_ARRANGEMENT

#endif  // __EMSCRIPTEN__
