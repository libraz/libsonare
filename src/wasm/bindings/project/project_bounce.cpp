/// @file project_bounce.cpp
/// @brief Embind project facade: compile + offline bounce family, the SoundFont
/// surface, the sample bank, and the NativeSynth preset / enum free functions.

#ifdef __EMSCRIPTEN__

#include <emscripten/emscripten.h>
#include <emscripten/val.h>

#include <algorithm>
#include <climits>
#include <string>
#include <unordered_map>

#include "c_api/daw_config_domains.h"
#include "project_wasm.h"

#if defined(SONARE_WITH_ARRANGEMENT)

#include "c_api/metronome_c.h"
#include "midi/controller_profile.h"

// Runs one instrument callback behind a JS try/catch, because C++ cannot catch a
// JS throw escaping emscripten::val::operator(). A thenable result is a failure
// too: an async instrument cannot finish inside the synchronous render.
// clang-format off
EM_JS(EM_VAL, sonare_project_invoke_instrument,
      (EM_VAL callback, EM_VAL self, EM_VAL args, const char* name), {
  try {
    const result = Emval.toValue(callback).apply(Emval.toValue(self), Emval.toValue(args));
    if (result !== null && (typeof result === 'object' || typeof result === 'function') &&
        typeof result.then === 'function') {
      return Emval.toHandle({
        failed: true,
        error: new TypeError('instrument ' + UTF8ToString(name) +
                             ' must not return a Promise; instruments are synchronous'),
      });
    }
    return Emval.toHandle({ failed: false });
  } catch (error) {
    // The caller's own throw: marked so no public entry renames it on the way out.
    if (error !== null && (typeof error === 'object' || typeof error === 'function')) {
      try {
        Object.defineProperty(error, Symbol.for('libsonare.foreignError'), { value: true, configurable: true });
      } catch (_) {}
    }
    return Emval.toHandle({ failed: true, error });
  }
});
// clang-format on

// Whether a JS scratch array still holds `frames` samples (a transferred buffer
// leaves it empty).
// clang-format off
EM_JS(bool, sonare_project_scratch_ready, (EM_VAL scratch, int frames), {
  return Emval.toValue(scratch).length >= frames;
});
// clang-format on

// Adds the first `frames` samples of a JS scratch array into the engine's planar
// buffer at linear-memory address `dst`; a scratch shorter than that (its buffer
// was transferred away) adds nothing.
// clang-format off
EM_JS(void, sonare_project_add_scratch, (EM_VAL scratch, float* dst, int frames), {
  const src = Emval.toValue(scratch);
  if (src.length < frames) return;
  const base = dst >> 2;
  for (let i = 0; i < frames; ++i) HEAPF32[base + i] += src[i];
});
// clang-format on

namespace {

/// Live sample banks by id. A bounce binding names a bank by the id its handle
/// carries rather than by a raw pointer, so a released or fabricated id is a
/// refusal instead of a use-after-free. Control thread only, which
/// single-threaded WASM guarantees.
std::unordered_map<uint32_t, SonareSampleBank*>& sampleBankRegistry() {
  static std::unordered_map<uint32_t, SonareSampleBank*> registry;
  return registry;
}

/// Live compiled timelines by id, looked up by RealtimeEngineWasm::applyProjectTimeline.
/// Control thread only, which single-threaded WASM guarantees.
std::unordered_map<uint32_t, SonareProjectTimeline*>& projectTimelineRegistry() {
  static std::unordered_map<uint32_t, SonareProjectTimeline*> registry;
  return registry;
}

/// Ids start at 1 so zero stays available as "no timeline". Monotonic, so an id
/// below the counter was issued once and is released if no longer registered.
uint32_t g_next_project_timeline_id = 1;
uint32_t nextProjectTimelineId() { return g_next_project_timeline_id++; }

/// Ids start at 1 so zero stays available as "no bank" in a binding.
uint32_t g_next_sample_bank_id = 1;
uint32_t nextSampleBankId() { return g_next_sample_bank_id++; }

/// Optional numeric field: absent keeps @p fallback, present-but-not-a-finite-
/// number throws rather than coercing.
double numberField(val object, const char* key, const char* subject, double fallback,
                   bool integer = false) {
  if (!hasProperty(object, key)) return fallback;
  return requireNumberProperty(object, key, subject, integer);
}

/// Optional unsigned-integer field, rejected before it is narrowed — a bare
/// cast would wrap 128 to 0 and 4294967296 to nothing at all.
double integerField(val object, const char* key, const char* subject, double max) {
  const double value = numberField(object, key, subject, 0.0, true);
  if (value < 0.0 || value > max || std::floor(value) != value) {
    throw WasmRangeError(std::string(subject) + "." + key + " must be an integer in [0, " +
                         std::to_string(static_cast<long long>(max)) + "]");
  }
  return value;
}

/// SonareSampleDesc.loop_mode carries SoundFont sampleModes, so a NUMBER passes
/// through as the raw SF2 value and SF2-derived data needs no translation. The
/// names are the C ABI's own table mapped onto that scale, matching the
/// Node reader.
int sampleDescLoopMode(val desc) {
  if (!hasProperty(desc, "loopMode")) return 0;
  const val value = desc["loopMode"];
  if (value.typeOf().as<std::string>() == "string") {
    const std::string name = value.as<std::string>();
    const int mapped = sonare::c_api::synth_names::sample_desc_loop_value(name);
    if (mapped >= 0) return mapped;
    throw WasmRangeError("Unknown sample loop mode name: '" + name +
                         "' (expected none, continuous or keyDown)");
  }
  return static_cast<int>(integerField(desc, "loopMode", "sample descriptor", 3.0));
}

/// The engine hosts every callback instrument as a stereo source, whatever the
/// bounce's output channel count.
constexpr int kInstrumentChannels = 2;

/// The first failure of any instrument callback; later callbacks are skipped.
struct InstrumentFailure {
  bool failed = false;
  val error;
};

/// One JS instrument for the duration of a synchronous bounce; the C callbacks
/// hold a raw pointer to it.
struct JsInstrumentSlot {
  val self;
  val prepare;
  val onEvent;
  val render;
  bool hasPrepare = false;
  bool hasOnEvent = false;
  InstrumentFailure* failure = nullptr;
  int maxBlock = 1;
  /// JS-owned scratch the instrument renders into, one array per channel.
  std::vector<val> scratch;
};

void invokeInstrument(JsInstrumentSlot* slot, const val& callback, const char* name,
                      const val& args) {
  const val outcome = val::take_ownership(sonare_project_invoke_instrument(
      callback.as_handle(), slot->self.as_handle(), args.as_handle(), name));
  if (boolProperty(outcome, "failed", false)) {
    slot->failure->failed = true;
    slot->failure->error = outcome["error"];
  }
}

void jsInstrumentPrepare(void* userData, double sampleRate, int maxBlockSize) {
  auto* slot = static_cast<JsInstrumentSlot*>(userData);
  slot->maxBlock = std::max(maxBlockSize, 1);
  if (slot->failure->failed || !slot->hasPrepare) return;
  val args = val::array();
  args.call<void>("push", sampleRate);
  args.call<void>("push", maxBlockSize);
  args.call<void>("push", kInstrumentChannels);
  invokeInstrument(slot, slot->prepare, "prepare", args);
}

void jsInstrumentOnEvent(void* userData, uint32_t destinationId, const uint32_t* umpWords,
                         int wordCount, int64_t renderFrame) {
  auto* slot = static_cast<JsInstrumentSlot*>(userData);
  if (slot->failure->failed || !slot->hasOnEvent) return;
  val words = val::array();
  for (int i = 0; i < wordCount; ++i) words.call<void>("push", umpWords[i]);
  val event = val::object();
  event.set("destinationId", destinationId);
  event.set("words", words);
  event.set("renderFrame", static_cast<double>(renderFrame));
  val args = val::array();
  args.call<void>("push", event);
  invokeInstrument(slot, slot->onEvent, "onEvent", args);
}

void jsInstrumentRender(void* userData, float* const* channels, int numChannels, int numFrames) {
  auto* slot = static_cast<JsInstrumentSlot*>(userData);
  if (slot->failure->failed || numChannels <= 0 || numFrames <= 0) return;
  const size_t frames = static_cast<size_t>(numFrames);
  // Zeroed views over JS-owned scratch; the instrument's output is added into
  // the engine's buffers only after the call returns normally.
  if (slot->scratch.size() < static_cast<size_t>(numChannels)) {
    slot->scratch.resize(static_cast<size_t>(numChannels));
  }
  val outputs = val::array();
  for (int ch = 0; ch < numChannels; ++ch) {
    val& base = slot->scratch[static_cast<size_t>(ch)];
    if (base.isUndefined() || !sonare_project_scratch_ready(base.as_handle(), numFrames)) {
      base =
          val::global("Float32Array").new_(std::max(frames, static_cast<size_t>(slot->maxBlock)));
    }
    val view = base.call<val>("subarray", 0, numFrames);
    view.call<void>("fill", 0);
    outputs.call<void>("push", view);
  }
  val args = val::array();
  args.call<void>("push", outputs);
  args.call<void>("push", numFrames);
  invokeInstrument(slot, slot->render, "render", args);
  if (slot->failure->failed) return;
  for (int ch = 0; ch < numChannels; ++ch) {
    sonare_project_add_scratch(slot->scratch[static_cast<size_t>(ch)].as_handle(), channels[ch],
                               numFrames);
  }
}

JsInstrumentSlot instrumentSlotFromVal(const val& desc, InstrumentFailure* failure,
                                       SonareInstrumentBinding* binding) {
  if (desc.typeOf().as<std::string>() != "object" || desc.isNull()) {
    throw WasmTypeError("instrument bindings must be objects");
  }
  JsInstrumentSlot slot;
  slot.self = desc;
  slot.render = desc["render"];
  slot.prepare = desc["prepare"];
  slot.onEvent = desc["onEvent"];
  slot.hasPrepare = slot.prepare.typeOf().as<std::string>() == "function";
  slot.hasOnEvent = slot.onEvent.typeOf().as<std::string>() == "function";
  slot.failure = failure;
  *binding = SonareInstrumentBinding{};
  binding->destination_id = uintProperty(desc, "destinationId", binding->destination_id);
  binding->callbacks.latency_samples =
      static_cast<int>(integerField(desc, "latencySamples", "instrument", INT_MAX));
  binding->callbacks.tail_samples =
      static_cast<int>(integerField(desc, "tailSamples", "instrument", INT_MAX));
  binding->callbacks.prepare = &jsInstrumentPrepare;
  binding->callbacks.on_event = &jsInstrumentOnEvent;
  binding->callbacks.render = &jsInstrumentRender;
  return slot;
}

// A project bounce result: the interleaved render and the width it was mixed at.
val bounceResultToVal(const std::vector<float>& samples, int num_channels) {
  val out = val::object();
  out.set("interleaved", vectorToFloat32Array(samples));
  out.set("numChannels", num_channels);
  return out;
}

}  // namespace

SampleBankWasm::SampleBankWasm() : bank_(nullptr), id_(nextSampleBankId()) {
  checkCodedCError(sonare_sample_bank_create(&bank_));
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

bool SampleBankWasm::issued(uint32_t id) { return id != 0 && id < g_next_sample_bank_id; }

namespace {

uint8_t bankMidiByte(const val& object, const char* key, const char* subject,
                     const sonare::util::check::NumericDomain& domain) {
  return optionalDomainValue<uint8_t>(objectProperty(object, key), subject, domain).value_or(0);
}

}  // namespace

uint32_t SampleBankWasm::addSample(val data, val desc) {
  if (wasmFloat32ArrayLength(data, "data") == 0) {
    throw WasmRangeError("sample data must not be empty");
  }
  const std::vector<float> frames = float32ArrayToVector(data, "data");

  SonareSampleDesc c{};
  c.root_key =
      bankMidiByte(desc, "rootKey", "rootKey", sonare_c_detail::kSampleBankDefaultedMidiByteDomain);
  c.fine_tune_cents =
      static_cast<float>(numberField(desc, "fineTuneCents", "sample descriptor", 0.0));
  c.source_rate = numberField(desc, "sourceRate", "sample descriptor", 0.0);
  c.loop_start =
      static_cast<uint32_t>(integerField(desc, "loopStart", "sample descriptor", 4294967295.0));
  c.loop_end =
      static_cast<uint32_t>(integerField(desc, "loopEnd", "sample descriptor", 4294967295.0));
  c.loop_mode = sampleDescLoopMode(desc);

  uint32_t index = 0;
  checkCodedCErrorFlattening(
      sonare_sample_bank_add_sample(bank_, elementData(frames), frames.size(), &c, &index), "desc");
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
    throw WasmTypeError("zone must be an object");
  }
  SonareSampleZoneDesc c{};
  c.sample_index =
      static_cast<uint32_t>(integerField(zone, "sampleIndex", "sample zone", 4294967295.0));
  // An absent bound stays zero, which the C ABI defaults per bound: 127 for an
  // upper edge, 1 for vel_lo, the lowest key for key_lo.
  const auto& defaulted = sonare_c_detail::kSampleBankDefaultedMidiByteDomain;
  c.key_lo = bankMidiByte(zone, "keyLo", "zone.keyLo", sonare_c_detail::kMidiDataByteDomain);
  c.key_hi = bankMidiByte(zone, "keyHi", "zone.keyHi", defaulted);
  c.vel_lo = bankMidiByte(zone, "velLo", "zone.velLo", defaulted);
  c.vel_hi = bankMidiByte(zone, "velHi", "zone.velHi", defaulted);
  c.tune_cents = static_cast<float>(numberField(zone, "tuneCents", "sample zone", 0.0));
  c.gain = static_cast<float>(numberField(zone, "gain", "sample zone", 0.0));
  c.pan_units = static_cast<float>(numberField(zone, "panUnits", "sample zone", 0.0));

  const SonareError err = sonare_sample_bank_add_zone(bank_, set_index, &c);
  if (err != SONARE_OK) throwCodedCError(err);
}

double SampleBankWasm::sampleCount() const {
  std::size_t count = 0;
  const SonareError err = sonare_sample_bank_sample_count(bank_, &count);
  if (err != SONARE_OK) throwCodedCError(err);
  return static_cast<double>(count);
}

double SampleBankWasm::setCount() const {
  std::size_t count = 0;
  const SonareError err = sonare_sample_bank_set_count(bank_, &count);
  if (err != SONARE_OK) throwCodedCError(err);
  return static_cast<double>(count);
}

val ProjectWasm::compile(const val& with_timeline_val) {
  const bool with_timeline = optionalBool(with_timeline_val, "withTimeline").value_or(false);
  SonareProjectCompileResult result{};
  SonareProjectTimeline* timeline = nullptr;
  const SonareError err =
      sonare_project_compile(project_.get(), &result, with_timeline ? &timeline : nullptr);
  if (err != SONARE_OK) {
    sonare_free_project_compile_result(&result);
    sonare_project_timeline_destroy(timeline);
    throwCodedCError(err);
  }
  val out = val::undefined();
  try {
    out = projectCompileResultToVal(result);
  } catch (...) {
    sonare_free_project_compile_result(&result);
    sonare_project_timeline_destroy(timeline);
    throw;
  }
  sonare_free_project_compile_result(&result);
  if (with_timeline) {
    if (timeline != nullptr) {
      out.set("timeline", ProjectTimelineWasm(timeline));
    } else {
      out.set("timeline", val::null());
    }
  }
  return out;
}

SonareProjectBounceOptions ProjectWasm::bounceOptionsFromVal(val options) {
  SonareProjectBounceOptions opts{};
  sonare_project_bounce_options_init(&opts);
  if (!options.isUndefined() && !options.isNull()) {
    // A field the caller writes is read literally: only absence selects a default, so a written
    // sentinel is passed on as 0, which the core refuses in the field's own domain text.
    const auto writtenInt = [](val value, const char* name, int sentinel) {
      const int read = checkedIntFromVal(value, name);
      return read == sentinel ? 0 : read;
    };
    // requireInt64Property, not a raw static_cast<int64_t> of a double:
    // refuses a fraction and an out-of-int64-range value by name instead of
    // truncating the one and casting the other into undefined behavior.
    if (hasProperty(options, "totalFrames")) {
      const int64_t read = requireInt64Property(options, "totalFrames", "");
      opts.total_frames = read == SONARE_PROJECT_BOUNCE_LENGTH_FROM_ARRANGEMENT ? 0 : read;
    }
    if (hasProperty(options, "blockSize")) {
      opts.block_size = checkedIntFromVal(options["blockSize"], "blockSize");
    }
    if (hasProperty(options, "numChannels")) {
      opts.num_channels = writtenInt(options["numChannels"], "numChannels",
                                     SONARE_PROJECT_BOUNCE_CHANNELS_FROM_MASTER);
    }
    if (hasProperty(options, "instrumentLatencySamples")) {
      opts.instrument_latency_samples =
          checkedIntFromVal(options["instrumentLatencySamples"], "instrumentLatencySamples");
    }
    if (hasProperty(options, "countInBars")) {
      opts.count_in_bars = checkedIntFromVal(options["countInBars"], "countInBars");
    }
  }
  return opts;
}

SonareBuiltinInstrumentBinding ProjectWasm::builtinBindingFromVal(val desc) {
  SonareBuiltinInstrumentBinding binding{};
  sonare_builtin_synth_config_init(&binding.config);
  if (desc.isUndefined() || desc.isNull()) {
    return binding;
  }
  binding.destination_id = uintProperty(desc, "destinationId", binding.destination_id);
  if (hasProperty(desc, "waveform")) {
    binding.config.waveform = builtinWaveformFromVal(desc["waveform"]);
  }
  if (hasProperty(desc, "gain")) {
    binding.config.gain = checkedFloatFromVal(desc["gain"], "gain");
  }
  if (hasProperty(desc, "attackMs")) {
    binding.config.attack_ms = checkedFloatFromVal(desc["attackMs"], "attackMs");
  }
  if (hasProperty(desc, "decayMs")) {
    binding.config.decay_ms = checkedFloatFromVal(desc["decayMs"], "decayMs");
  }
  if (hasProperty(desc, "sustain")) {
    binding.config.sustain = checkedFloatFromVal(desc["sustain"], "sustain");
  }
  if (hasProperty(desc, "releaseMs")) {
    binding.config.release_ms = checkedFloatFromVal(desc["releaseMs"], "releaseMs");
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
    const size_t count = wasmArrayLength(bindings, "bindings");
    out.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      out.push_back(builtinBindingFromVal(bindings[i]));
    }
    return out;
  }
  out.push_back(builtinBindingFromVal(bindings));
  return out;
}

val ProjectWasm::bounce(val options, val builtin_bindings, val instrument_bindings,
                        val synth_bindings, val sf2_bindings) {
  // A callback may delete() the JS project wrapper mid-bounce; the local copy
  // keeps the C handle alive until the bounce and the result conversion finish.
  const auto project_keepalive = project_;
  std::vector<SonareBuiltinInstrumentBinding> builtins = builtinBindingsFromVal(builtin_bindings);

  InstrumentFailure failure;
  std::vector<JsInstrumentSlot> slots;
  std::vector<SonareInstrumentBinding> instruments;
  if (!instrument_bindings.isUndefined() && !instrument_bindings.isNull()) {
    const bool isList = val::global("Array").call<bool>("isArray", instrument_bindings);
    const size_t count = isList ? wasmArrayLength(instrument_bindings, "bindings") : 1;
    slots.reserve(count);
    instruments.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      SonareInstrumentBinding binding{};
      slots.push_back(instrumentSlotFromVal(isList ? instrument_bindings[i] : instrument_bindings,
                                            &failure, &binding));
      instruments.push_back(binding);
    }
    // The slots vector no longer grows, so these addresses stay valid.
    for (size_t i = 0; i < slots.size(); ++i) instruments[i].callbacks.user_data = &slots[i];
  }

  std::vector<SonareSynthInstrumentBinding> synths;
  // Owns the keys behind each binding's engine_params; alive across the C call.
  std::vector<sonare_wasm_synth::SynthPatchStorage> patch_storage;
  if (!synth_bindings.isUndefined() && !synth_bindings.isNull()) {
    auto bindingFromVal = [&patch_storage](val desc, size_t index) {
      SonareSynthInstrumentBinding binding{};
      if (desc.typeOf().as<std::string>() == "object") {
        binding.destination_id = uintProperty(desc, "destinationId", binding.destination_id);
        if (hasProperty(desc, "useGmPrograms")) {
          binding.use_gm_programs =
              requireProperty<bool>(desc, "useGmPrograms", "synth instrument") ? 1 : 0;
        }
        binding.sample_bank = SampleBankWasm::fromDescriptor(desc);
      }
      patch_storage.push_back(sonare_wasm_synth::synthPatchFromVal(
          desc, "synthInstruments[" + std::to_string(index) + "].instrument"));
      binding.patch = patch_storage.back().patch;
      return binding;
    };
    if (val::global("Array").call<bool>("isArray", synth_bindings)) {
      const size_t count = wasmArrayLength(synth_bindings, "bindings");
      synths.reserve(count);
      for (size_t i = 0; i < count; ++i) synths.push_back(bindingFromVal(synth_bindings[i], i));
    } else {
      synths.push_back(bindingFromVal(synth_bindings, 0));
    }
  }

  std::vector<SonareSf2InstrumentBinding> players;
  std::vector<std::string> sf2_uri_storage;
  if (!sf2_bindings.isUndefined() && !sf2_bindings.isNull()) {
    if (val::global("Array").call<bool>("isArray", sf2_bindings)) {
      const size_t count = wasmArrayLength(sf2_bindings, "bindings");
      players.reserve(count);
      sf2_uri_storage.reserve(count);
      for (size_t i = 0; i < count; ++i) {
        players.push_back(sf2BindingFromVal(sf2_bindings[i], &sf2_uri_storage));
      }
    } else {
      sf2_uri_storage.reserve(1);
      players.push_back(sf2BindingFromVal(sf2_bindings, &sf2_uri_storage));
    }
  }

  SonareProjectBounceOptions opts = bounceOptionsFromVal(options);

  SonareEngineMetronomeConfig metronome{};
  std::vector<SonareProjectControllerProfileBinding> controller_profiles;
  std::vector<std::string> controller_profile_names;
  std::vector<SonareProjectArticulationBinding> articulations;
  if (!options.isUndefined() && !options.isNull()) {
    if (hasProperty(options, "metronome")) {
      val config = options["metronome"];
      if (!config.isUndefined() && !config.isNull()) {
        metronome = sonare_c_detail::metronome_to_c(sonare::engine::MetronomeConfig{});
        metronome.enabled = boolProperty(config, "enabled", metronome.enabled != 0) ? 1 : 0;
        metronome.beat_gain = floatProperty(config, "beatGain", metronome.beat_gain);
        metronome.accent_gain = floatProperty(config, "accentGain", metronome.accent_gain);
        if (hasProperty(config, "clickSamples")) {
          metronome.click_samples = checkedIntFromVal(config["clickSamples"], "clickSamples");
        }
        if (hasProperty(config, "clickSec")) {
          metronome.click_sec = checkedDoubleFromVal(config["clickSec"], "clickSec");
        }
        opts.metronome = &metronome;
      }
    }
    if (hasProperty(options, "controllerProfiles")) {
      val descriptors = options["controllerProfiles"];
      if (!descriptors.isUndefined() && !descriptors.isNull()) {
        const size_t count = wasmArrayLength(descriptors, "controllerProfiles");
        controller_profiles.reserve(count);
        controller_profile_names.reserve(count);
        for (size_t i = 0; i < count; ++i) {
          val descriptor = descriptors[i];
          SonareProjectControllerProfileBinding binding{};
          binding.destination_id = uintProperty(descriptor, "destinationId", 0);
          controller_profile_names.push_back(stringProperty(descriptor, "presetName", ""));
          binding.preset_name = wasmCString(controller_profile_names.back(), "presetName");
          controller_profiles.push_back(binding);
        }
      }
    }
    if (hasProperty(options, "articulations")) {
      val descriptors = options["articulations"];
      if (!descriptors.isUndefined() && !descriptors.isNull()) {
        const size_t count = wasmArrayLength(descriptors, "articulations");
        articulations.reserve(count);
        for (size_t i = 0; i < count; ++i) {
          val descriptor = descriptors[i];
          SonareProjectArticulationBinding binding{};
          binding.destination_id = uintProperty(descriptor, "destinationId", 0);
          binding.channel = uintProperty(descriptor, "channel", 0);
          binding.articulation = static_cast<SonareArticulation>(sonare_wasm_synth::enumFromVal(
              descriptor["articulation"], sonare_wasm_synth::kArticulations,
              SONARE_ARTICULATION_COUNT, "articulation"));
          articulations.push_back(binding);
        }
      }
    }
  }
  opts.controller_profiles = controller_profiles.empty() ? nullptr : controller_profiles.data();
  opts.controller_profile_count = controller_profiles.size();
  opts.articulations = articulations.empty() ? nullptr : articulations.data();
  opts.articulation_count = articulations.size();

  opts.instruments = instruments.empty() ? nullptr : instruments.data();
  opts.instrument_count = instruments.size();
  opts.builtin_instruments = builtins.empty() ? nullptr : builtins.data();
  opts.builtin_instrument_count = builtins.size();
  opts.synth_instruments = synths.empty() ? nullptr : synths.data();
  opts.synth_instrument_count = synths.size();
  opts.sf2_instruments = players.empty() ? nullptr : players.data();
  opts.sf2_instrument_count = players.size();
  float* interleaved = nullptr;
  size_t frames = 0;
  int num_channels = 0;
  const SonareError err =
      sonare_project_bounce(project_keepalive.get(), &opts, &interleaved, &frames, &num_channels);
  // A JS throw from here would skip every destructor in this frame, so a
  // throwing callback is returned as data and the TypeScript facade rethrows it.
  // The native failure is reported ahead of a callback's, as in the Python binding.
  if (err != SONARE_OK) {
    sonare_free_floats(interleaved);
    throwCodedCError(err, CErrorReasonMode::kCurrentEntry);
  }
  if (failure.failed) {
    sonare_free_floats(interleaved);
    val result = val::object();
    result.set("instrumentFailure", failure.error);
    return result;
  }
  std::vector<float> samples(interleaved, interleaved + frames * static_cast<size_t>(num_channels));
  sonare_free_floats(interleaved);
  return bounceResultToVal(samples, num_channels);
}

void ProjectWasm::loadSoundFont(val data, const val& uri_val) {
  std::string uri_storage;
  const char* uri = nullptr;
  if (!uri_val.isUndefined() && !uri_val.isNull()) {
    uri_storage = stringFromVal(uri_val, "uri");
    uri = wasmCString(uri_storage, "uri");
  }
  std::vector<uint8_t> bytes = uint8ArrayToVector(data, "data");
  // An empty buffer reaches the C entry as one, which refuses it as empty rather than as NULL.
  const SonareError err =
      sonare_project_load_sound_font(project_.get(), uri, nonNullBytes(bytes), bytes.size());
  if (err != SONARE_OK) {
    throwCodedCError(err);
  }
}

void ProjectWasm::clearSoundFont(const val& uri_val) {
  std::string uri_storage;
  const char* uri = nullptr;
  if (!uri_val.isUndefined() && !uri_val.isNull()) {
    uri_storage = stringFromVal(uri_val, "uri");
    uri = wasmCString(uri_storage, "uri");
  }
  const SonareError err = sonare_project_clear_sound_font(project_.get(), uri);
  if (err != SONARE_OK) {
    throwCodedCError(err);
  }
}

size_t ProjectWasm::soundFontPresetCount(const val& uri_val) {
  std::string uri_storage;
  const char* uri = nullptr;
  if (!uri_val.isUndefined() && !uri_val.isNull()) {
    uri_storage = stringFromVal(uri_val, "uri");
    uri = wasmCString(uri_storage, "uri");
  }
  size_t count = 0;
  const SonareError err = sonare_project_sound_font_preset_count(project_.get(), uri, &count);
  if (err != SONARE_OK) {
    throwCodedCError(err);
  }
  return count;
}

val ProjectWasm::soundFontManifest(const val& uri_val) {
  std::string uri_storage;
  const char* uri = nullptr;
  if (!uri_val.isUndefined() && !uri_val.isNull()) {
    uri_storage = stringFromVal(uri_val, "uri");
    uri = wasmCString(uri_storage, "uri");
  }
  size_t total = 0;
  SonareError err = sonare_project_sound_font_manifest(project_.get(), uri, nullptr, 0, &total);
  if (err != SONARE_OK) {
    throwCodedCError(err);
  }
  std::vector<SonareSf2ProgramStatus> entries(total);
  if (total > 0) {
    err = sonare_project_sound_font_manifest(project_.get(), uri, elementData(entries), total,
                                             &total);
    if (err != SONARE_OK) {
      throwCodedCError(err);
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

SonareSf2InstrumentBinding ProjectWasm::sf2BindingFromVal(val desc,
                                                          std::vector<std::string>* uri_storage) {
  SonareSf2InstrumentBinding binding{};
  sonare_sf2_instrument_config_init(&binding.config);
  if (desc.isUndefined() || desc.isNull()) {
    return binding;
  }
  binding.destination_id = uintProperty(desc, "destinationId", binding.destination_id);
  if (hasProperty(desc, "gain")) {
    binding.config.gain = checkedFloatFromVal(desc["gain"], "gain");
  }
  if (hasProperty(desc, "polyphony")) {
    binding.config.polyphony = checkedIntFromVal(desc["polyphony"], "polyphony");
  }
  if (hasProperty(desc, "preferModelForModeledFamilies")) {
    binding.config.prefer_model_for_modeled_families =
        boolProperty(desc, "preferModelForModeledFamilies", false) ? 1 : 0;
  }
  if (hasProperty(desc, "clearBankRig")) {
    binding.config.clear_bank_rig = boolProperty(desc, "clearBankRig", false) ? 1 : 0;
  }
  if (hasProperty(desc, "gsEfxRealization")) {
    binding.config.gs_efx_realization = gsEfxRealizationProperty(desc, "gsEfxRealization");
  }
  if (uri_storage != nullptr) {
    const val uri_val = desc["soundFontUri"];
    if (!uri_val.isUndefined() && !uri_val.isNull()) {
      uri_storage->push_back(stringFromVal(uri_val, "soundFontUri"));
      binding.sound_font_uri = wasmCString(uri_storage->back(), "soundFontUri");
    }
  }
  return binding;
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

val js_synth_gs_drum_kit_name(const val& program) {
  return js_nullable_string(sonare_synth_gs_drum_kit_name(checkedIntFromVal(program, "program")));
}

// 1 / 0 / -1 as the C ABI gives them; the facade turns them into boolean|null.
int js_synth_gs_drum_kit_is_voiced_apart(const val& program) {
  return sonare_synth_gs_drum_kit_is_voiced_apart(checkedIntFromVal(program, "program"));
}

int js_synth_gs_variation_is_voiced_apart(const val& bank, const val& program) {
  return sonare_synth_gs_variation_is_voiced_apart(checkedIntFromVal(bank, "bank"),
                                                   checkedIntFromVal(program, "program"));
}

// Fetches a named catalog preset as a SynthPatch object (the preset name plus
// its wrapper-section values). A "va:" routing prefix is accepted; unknown
// names throw.
val js_synth_preset_patch(const std::string& name) {
  const std::string bare = name.rfind("va:", 0) == 0 ? name.substr(3) : name;
  SonareSynthPatch patch{};
  if (sonare_synth_preset_patch(wasmCString(bare, "preset"), &patch) != SONARE_OK) {
    throw WasmRangeError("unknown synth preset name: '" + name + "'");
  }
  return sonare_wasm_synth::synthPatchToVal(patch);
}

// Controller-profile preset catalog as a JS string[]. Read from the core rather
// than from sonare_controller_profile_names: the controller C-ABI translation
// unit is not linked into this module, so the profile class is the source here
// exactly as it is for the realtime-engine controller entries.
val js_controller_profile_names() {
  val out = val::array();
  for (size_t i = 0; i < sonare::midi::ControllerProfile::preset_count(); ++i) {
    const char* name = sonare::midi::ControllerProfile::preset_name_at(i);
    if (name == nullptr) break;
    out.call<void>("push", std::string(name));
  }
  return out;
}

val js_synth_enum_tables() { return sonare_wasm_synth::synthEnumTablesToVal(); }

val js_synth_patch_round_trip(val desc) {
  return sonare_wasm_synth::synthPatchToVal(sonare_wasm_synth::synthPatchFromVal(desc).patch);
}

void registerProjectBounce(class_<ProjectWasm>& cls) {
  cls.function("compile", &ProjectWasm::compile)
      .function("bounce", &ProjectWasm::bounce)
      .function("loadSoundFont", &ProjectWasm::loadSoundFont)
      .function("clearSoundFont", &ProjectWasm::clearSoundFont)
      .function("soundFontPresetCount", &ProjectWasm::soundFontPresetCount)
      .function("soundFontManifest", &ProjectWasm::soundFontManifest);
}

ProjectTimelineWasm::ProjectTimelineWasm(SonareProjectTimeline* adopted) {
  id_ = nextProjectTimelineId();
  const uint32_t id = id_;
  timeline_ = std::shared_ptr<SonareProjectTimeline>(adopted, [id](SonareProjectTimeline* handle) {
    projectTimelineRegistry().erase(id);
    sonare_project_timeline_destroy(handle);
  });
  projectTimelineRegistry().emplace(id_, adopted);
}

SonareProjectTimeline* ProjectTimelineWasm::lookup(uint32_t id) {
  const auto& registry = projectTimelineRegistry();
  const auto it = registry.find(id);
  return it != registry.end() ? it->second : nullptr;
}

bool ProjectTimelineWasm::issued(uint32_t id) { return id != 0 && id < g_next_project_timeline_id; }

void registerProjectTimeline() {
  class_<ProjectTimelineWasm>("ProjectTimeline").property("id", &ProjectTimelineWasm::id);
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
