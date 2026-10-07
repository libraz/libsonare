/// @file project_bounce.cpp
/// @brief Embind project facade: compile + offline bounce family, the SoundFont
/// surface, the sample bank, and the NativeSynth preset / enum free functions.

#ifdef __EMSCRIPTEN__

#include <emscripten/emscripten.h>
#include <emscripten/val.h>

#include <algorithm>
#include <climits>
#include <unordered_map>

#include "project_wasm.h"

#if defined(SONARE_WITH_ARRANGEMENT)

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
        error: new TypeError('bounceWithInstruments: instrument ' + UTF8ToString(name) +
                             ' must not return a Promise; instruments are synchronous'),
      });
    }
    return Emval.toHandle({ failed: false });
  } catch (error) {
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
/// carries rather than by a raw pointer, so a released or fabricated id is an
/// InvalidParameter instead of a use-after-free. Control thread only, which
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

/// Ids start at 1 so zero stays available as "no timeline".
uint32_t nextProjectTimelineId() {
  static uint32_t next = 1;
  return next++;
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
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "instrument bindings must be objects");
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
  binding->destination_id = uintProperty(desc, "destinationId", 0u);
  binding->callbacks.latency_samples =
      static_cast<int>(integerField(desc, "latencySamples", "instrument", INT_MAX));
  binding->callbacks.tail_samples =
      static_cast<int>(integerField(desc, "tailSamples", "instrument", INT_MAX));
  binding->callbacks.prepare = &jsInstrumentPrepare;
  binding->callbacks.on_event = &jsInstrumentOnEvent;
  binding->callbacks.render = &jsInstrumentRender;
  return slot;
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

val ProjectWasm::compileTimeline() {
  SonareProjectCompileResult result{};
  SonareProjectTimeline* timeline = nullptr;
  const SonareError err = sonare_project_compile_timeline(project_.get(), &result, &timeline);
  if (err != SONARE_OK) {
    sonare_project_free_compile_result(&result);
    sonare_project_timeline_destroy(timeline);
    throwCError(err, "failed to compile project timeline");
  }
  val out = projectCompileResultToVal(result);
  sonare_project_free_compile_result(&result);
  if (timeline != nullptr) {
    out.set("timeline", ProjectTimelineWasm(timeline));
  } else {
    out.set("timeline", val::null());
  }
  return out;
}

SonareProjectBounceOptions ProjectWasm::bounceOptionsFromVal(val options) {
  SonareProjectBounceOptions opts{};
  if (!options.isUndefined() && !options.isNull()) {
    // requireInt64Property, not a raw static_cast<int64_t> of a double:
    // refuses a fraction and an out-of-int64-range value by name instead of
    // truncating the one and casting the other into undefined behavior.
    if (hasProperty(options, "totalFrames")) {
      opts.total_frames = requireInt64Property(options, "totalFrames", "Project.bounce options");
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
    const size_t count = wasmArrayLikeLength(bindings, "bindings");
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

val ProjectWasm::bounceWithInstruments(val bindings, val options) {
  // A callback may delete() the JS project wrapper mid-bounce; the local copy
  // keeps the C handle alive until the bounce and the result conversion finish.
  const auto project_keepalive = project_;
  InstrumentFailure failure;
  std::vector<JsInstrumentSlot> slots;
  std::vector<SonareInstrumentBinding> instruments;
  if (!bindings.isUndefined() && !bindings.isNull()) {
    const bool isList = val::global("Array").call<bool>("isArray", bindings);
    const size_t count = isList ? wasmArrayLikeLength(bindings, "bindings") : 1;
    slots.reserve(count);
    instruments.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      SonareInstrumentBinding binding{};
      slots.push_back(instrumentSlotFromVal(isList ? bindings[i] : bindings, &failure, &binding));
      instruments.push_back(binding);
    }
    // The slots vector no longer grows, so these addresses stay valid.
    for (size_t i = 0; i < slots.size(); ++i) instruments[i].callbacks.user_data = &slots[i];
  }
  SonareProjectBounceOptions opts = bounceOptionsFromVal(options);
  float* interleaved = nullptr;
  size_t len = 0;
  const SonareError err = sonare_project_bounce_with_instruments(
      project_keepalive.get(), &opts, instruments.empty() ? nullptr : instruments.data(),
      instruments.size(), &interleaved, &len);
  // A JS throw from here would skip every destructor in this frame, so a
  // throwing callback is returned as data and the TypeScript facade rethrows it.
  // The native failure is reported ahead of a callback's, as in the Python binding.
  if (err != SONARE_OK) {
    sonare_free_floats(interleaved);
    throwCError(err, "failed to bounce project with callback instrument");
  }
  if (failure.failed) {
    sonare_free_floats(interleaved);
    val result = val::object();
    result.set("instrumentFailure", failure.error);
    return result;
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
      const size_t count = wasmArrayLikeLength(bindings, "bindings");
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
    binding.config.gain = checkedFloatFromVal(desc["gain"], "gain");
  }
  if (hasProperty(desc, "polyphony")) {
    binding.config.polyphony = checkedIntFromVal(desc["polyphony"], "polyphony");
  }
  if (hasProperty(desc, "preferModelForModeledFamilies")) {
    binding.config.struct_version = 2;
    binding.config.prefer_model_for_modeled_families =
        boolProperty(desc, "preferModelForModeledFamilies", false) ? 1 : 0;
  }
  // Version 3 reads version 2's field as well, so raising it here covers both
  // whichever of the two the caller passed.
  if (hasProperty(desc, "clearBankRig")) {
    binding.config.struct_version = 3;
    binding.config.clear_bank_rig = boolProperty(desc, "clearBankRig", false) ? 1 : 0;
  }
  // Version 4 reads every earlier field too.
  if (hasProperty(desc, "gsEfxRealization")) {
    binding.config.struct_version = 4;
    binding.config.gs_efx_realization = gsEfxRealizationProperty(desc, "gsEfxRealization");
  }
  return binding;
}

val ProjectWasm::bounceWithSf2Instrument(val bindings, val options) {
  std::vector<SonareSf2InstrumentBinding> players;
  if (!bindings.isUndefined() && !bindings.isNull()) {
    if (val::global("Array").call<bool>("isArray", bindings)) {
      const size_t count = wasmArrayLikeLength(bindings, "bindings");
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
  if (sonare_synth_preset_patch(bare.c_str(), &patch) != SONARE_OK) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "unknown synth preset name: '" + name + "'");
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
  return sonare_wasm_synth::synthPatchToVal(sonare_wasm_synth::synthPatchFromVal(desc));
}

void registerProjectBounce(class_<ProjectWasm>& cls) {
  cls.function("compile", &ProjectWasm::compile)
      .function("compileTimeline", &ProjectWasm::compileTimeline)
      .function("bounce", &ProjectWasm::bounce)
      .function("bounceWithBuiltinInstrument", &ProjectWasm::bounceWithBuiltinInstrument)
      .function("bounceWithInstruments", &ProjectWasm::bounceWithInstruments)
      .function("bounceWithSynthInstrument", &ProjectWasm::bounceWithSynthInstrument)
      .function("loadSoundFont", &ProjectWasm::loadSoundFont)
      .function("clearSoundFont", &ProjectWasm::clearSoundFont)
      .function("soundFontPresetCount", &ProjectWasm::soundFontPresetCount)
      .function("soundFontManifest", &ProjectWasm::soundFontManifest)
      .function("bounceWithSf2Instrument", &ProjectWasm::bounceWithSf2Instrument);
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
