// Node engine: MIDI instrument and clip loading methods.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

#include "core/audio.h"
#include "engine/common.h"
#include "engine/realtime_engine.h"
#include "midi/live_push_args.h"
#include "sonare_wrap_engine.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_sample_bank.h"
#include "sonare_wrap_synth_patch.h"
#include "sonare_wrap_utils.h"

using namespace sonare_node::engine;

namespace {

bool ReadEngineBuiltinSynthConfig(Napi::Env env, const Napi::Object& obj,
                                  SonareEngineBuiltinSynthConfig* config) {
  if (!ReadBuiltinWaveform(env, obj.Get("waveform"), &config->waveform)) {
    return false;
  }
  config->gain = FloatProperty(obj, "gain", config->gain);
  config->attack_ms = FloatProperty(obj, "attackMs", config->attack_ms);
  config->decay_ms = FloatProperty(obj, "decayMs", config->decay_ms);
  config->sustain = FloatProperty(obj, "sustain", config->sustain);
  config->release_ms = FloatProperty(obj, "releaseMs", config->release_ms);
  config->polyphony = IntProperty(obj, "polyphony", kZeroIsSentinel);
  // A wrong-typed field left a pending JS exception; stop before the caller
  // reaches the C ABI with it still set.
  return !env.IsExceptionPending();
}

std::vector<SonareEngineMidiEvent> ReadEngineMidiEvents(Napi::Env env, Napi::Value value) {
  if (!value.IsArray()) {
    Napi::TypeError::New(env, "MIDI clip events must be an array").ThrowAsJavaScriptException();
    return {};
  }
  Napi::Array input = value.As<Napi::Array>();
  std::vector<SonareEngineMidiEvent> events(input.Length());
  for (uint32_t i = 0; i < input.Length(); ++i) {
    Napi::Value item = input.Get(i);
    if (!item.IsObject()) {
      Napi::TypeError::New(env, "MIDI clip events must be objects").ThrowAsJavaScriptException();
      return {};
    }
    Napi::Object obj = item.As<Napi::Object>();
    SonareEngineMidiEvent event{};
    event.render_frame = Int64Property(obj, "renderFrame", event.render_frame);
    event.word0 = WordProperty(obj, "word0", event.word0);
    event.word1 = WordProperty(obj, "word1", event.word1);
    event.word2 = WordProperty(obj, "word2", event.word2);
    event.word3 = WordProperty(obj, "word3", event.word3);
    // wordCount is documented as tolerant: anything outside [1,4] lets the C
    // bridge infer the word form. Narrow out of range rather than through the
    // uint8_t cast, which would land 257 on a spurious 1 instead of inferring.
    const uint32_t word_count = Uint32Property(obj, "wordCount", 0);
    event.word_count = word_count >= 1 && word_count <= 4 ? static_cast<uint8_t>(word_count) : 0;
    event.group = MidiByteProperty(env, obj, "group", event.group);
    event.sysex_handle = Uint32Property(obj, "sysexHandle", event.sysex_handle);
    // Bail on the first wrong-typed field so the next iteration's explicit
    // TypeError is never raised on top of an already-pending exception.
    if (env.IsExceptionPending()) return {};
    events[i] = event;
  }
  return events;
}

}  // namespace

Napi::Value RealtimeEngineWrap::SetMidiClips(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  if (info.Length() == 0 || !info[0].IsArray()) {
    Napi::TypeError::New(env, "clips must be an array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Array input = info[0].As<Napi::Array>();
  std::vector<std::vector<SonareEngineMidiEvent>> event_storage(input.Length());
  std::vector<SonareEngineMidiClipSchedule> clips(input.Length());
  for (uint32_t i = 0; i < input.Length(); ++i) {
    Napi::Value item = input.Get(i);
    if (!item.IsObject()) {
      Napi::TypeError::New(env, "MIDI clips must be objects").ThrowAsJavaScriptException();
      return env.Undefined();
    }
    Napi::Object obj = item.As<Napi::Object>();
    event_storage[i] =
        ReadEngineMidiEvents(env, obj.Has("events") ? obj.Get("events") : env.Null());
    if (env.IsExceptionPending()) return env.Undefined();
    SonareEngineMidiClipSchedule clip{};
    sonare_engine_midi_clip_schedule_init(&clip);
    clip.id = Uint32Property(obj, "id", clip.id);
    clip.track_id = Uint32Property(obj, "trackId", clip.track_id);
    clip.start_sample = Int64Property(obj, "startSample", clip.start_sample);
    clip.start_ppq = DoubleProperty(obj, "startPpq", clip.start_ppq);
    clip.length_samples =
        Int64Property(obj, "lengthSamples", AbsentIsSentinel<int64_t>{clip.length_samples});
    clip.loop = BoolProperty(obj, "loop", clip.loop != 0) ? 1 : 0;
    clip.loop_length_samples = Int64Property(obj, "loopLengthSamples", clip.loop_length_samples);
    clip.destination_id = Uint32Property(obj, "destinationId", Uint32Property(obj, "trackId", 0));
    clip.gain = FloatProperty(obj, "gain", clip.gain);
    clip.fade_in_samples = Int64Property(obj, "fadeInSamples", clip.fade_in_samples);
    clip.fade_out_samples = Int64Property(obj, "fadeOutSamples", clip.fade_out_samples);
    if (env.IsExceptionPending()) return env.Undefined();
    clip.events = event_storage[i].data();
    clip.event_count = event_storage[i].size();
    clips[i] = clip;
  }
  ThrowIfError(env, sonare_engine_set_midi_clips(engine_, clips.data(), clips.size()));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetBuiltinInstrument(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  uint32_t destination_id = 0;
  if (!OptionalUint32Arg(env, info, 0, "destinationId", 0, &destination_id)) return env.Undefined();
  SonareEngineBuiltinSynthConfig config{};
  if (info.Length() > 1 && info[1].IsObject()) {
    Napi::Object obj = info[1].As<Napi::Object>();
    if (!ReadEngineBuiltinSynthConfig(env, obj, &config)) return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_set_builtin_instrument(engine_, destination_id, &config));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

// Binds the patch-driven NativeSynth to a realtime MIDI destination:
//   setSynthInstrument(destinationId, patch)
// where `patch` is a SynthPatch object or a preset-name string ("sawLead" /
// "va:sawLead"), resolving exactly like Project.bounceWithSynthInstruments. A
// sample patch carries its bank as the descriptor's `sampleBank`, the same key
// the bounce reads; the engine takes a share of it. `useGmPrograms` follows
// incoming GM bank/program changes and routes channel 10 through the GM drum
// map, exactly like the bounce binding of the same name.
Napi::Value RealtimeEngineWrap::SetSynthInstrument(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  SonareSynthInstrumentBinding binding{};
  // Owns the engine-param keys the binding's patch borrows; alive across the C call.
  sonare_node::SynthPatchStorage patch_storage;
  if (!RequiredUint32Arg(env, info, 0, "destinationId", &binding.destination_id)) {
    return env.Undefined();
  }
  if (info.Length() > 1) {
    if (!sonare_node::ReadSynthPatch(env, info[1], &patch_storage)) {
      return env.Undefined();  // exception already pending
    }
    binding.patch = patch_storage.patch;
    if (info[1].IsObject() && !info[1].IsArray()) {
      Napi::Object obj = info[1].As<Napi::Object>();
      binding.use_gm_programs =
          BoolProperty(obj, "useGmPrograms", binding.use_gm_programs != 0) ? 1 : 0;
      if (env.IsExceptionPending()) return env.Undefined();
      if (!SampleBankWrap::ReadHandle(env, obj.Get("sampleBank"), &binding.sample_bank)) {
        return env.Undefined();  // exception already pending
      }
    }
  } else {
    binding.patch.struct_version = 3;
  }
  const SonareError bind_err = sonare_engine_set_synth_instrument_binding(engine_, &binding);
  if (bind_err != SONARE_OK && !env.IsExceptionPending()) {
    sonare_node::ThrowLastSonareError(env, "", bind_err);
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::LoadSoundFont(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  const uint8_t* bytes = nullptr;
  size_t len = 0;
  if (info.Length() > 0 && info[0].IsBuffer()) {
    Napi::Buffer<uint8_t> buf = info[0].As<Napi::Buffer<uint8_t>>();
    bytes = buf.Data();
    len = buf.Length();
  } else if (info.Length() > 0 && sonare_node::IsUint8Array(info[0])) {
    Napi::Uint8Array arr = info[0].As<Napi::Uint8Array>();
    bytes = arr.Data();
    len = arr.ByteLength();
  } else {
    Napi::TypeError::New(env, "data must be a Buffer or Uint8Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_load_soundfont(engine_, bytes, len));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetSf2Instrument(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  uint32_t destination_id = 0;
  if (!OptionalUint32Arg(env, info, 0, "destinationId", 0, &destination_id)) return env.Undefined();
  SonareEngineSf2InstrumentConfig config{};
  if (info.Length() > 1 && info[1].IsObject()) {
    Napi::Object obj = info[1].As<Napi::Object>();
    config.gain = FloatProperty(obj, "gain", config.gain);
    config.polyphony = IntProperty(obj, "polyphony", kZeroIsSentinel);
    const Napi::Value prefer_model = obj.Get("preferModelForModeledFamilies");
    if (!prefer_model.IsUndefined() && !prefer_model.IsNull()) {
      config.struct_version = 2;
      config.prefer_model_for_modeled_families =
          BoolProperty(obj, "preferModelForModeledFamilies",
                       config.prefer_model_for_modeled_families != 0)
              ? 1
              : 0;
    }
    // Version 3 reads version 2's field as well, so raising it here covers both
    // whichever of the two the caller passed.
    const Napi::Value clear_rig = obj.Get("clearBankRig");
    if (!clear_rig.IsUndefined() && !clear_rig.IsNull()) {
      config.struct_version = 3;
      config.clear_bank_rig = BoolProperty(obj, "clearBankRig", config.clear_bank_rig != 0) ? 1 : 0;
    }
    config.struct_version = 4;
    config.gs_efx_realization = sonare_node::GsEfxRealizationProperty(obj, "gsEfxRealization");
    if (env.IsExceptionPending()) return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_set_sf2_instrument(engine_, destination_id, &config));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::ClearMidiInstrument(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  uint32_t destination_id = 0;
  if (!OptionalUint32Arg(env, info, 0, "destinationId", 0, &destination_id)) return env.Undefined();
  ThrowIfError(env, sonare_engine_clear_midi_instrument(engine_, destination_id));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::MidiInstrumentCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  size_t count = 0;
  ThrowIfError(env, sonare_engine_midi_instrument_count(engine_, &count));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(count));
  SONARE_NODE_CATCH(env)
}
