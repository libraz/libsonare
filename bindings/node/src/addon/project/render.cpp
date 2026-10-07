#include <uv.h>

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <exception>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "project/common.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_project.h"
#include "sonare_wrap_project_timeline.h"
#include "sonare_wrap_sample_bank.h"
#include "sonare_wrap_synth_patch.h"
#include "sonare_wrap_utils.h"

using namespace sonare_node::project;

namespace {

// Fills `options` from a JS bounce-options object (zero-initialized on entry).
void FillBounceOptions(const Napi::Object& obj, SonareProjectBounceOptions* options) {
  options->total_frames = Int64Property(obj, "totalFrames", kZeroIsSentinel);
  options->block_size = IntProperty(obj, "blockSize", 0);
  options->num_channels = IntProperty(obj, "numChannels", kZeroIsSentinel);
  options->sample_rate = IntProperty(obj, "sampleRate", 0);
  options->instrument_latency_samples = IntProperty(obj, "instrumentLatencySamples", 0);
}

// Parses a JS instrument descriptor into a built-in synth binding. Throws a
// JS exception (and returns false) on an unknown waveform name. A zero-init
// config is the native default sine patch, so only present fields are set.
bool ParseBuiltinInstrument(Napi::Env env, const Napi::Object& obj,
                            SonareBuiltinInstrumentBinding* binding) {
  binding->destination_id =
      obj.Get("destinationId").IsUndefined()
          ? 0u
          : node_narrow_uint32(obj.Env(), obj.Get("destinationId"), "destinationId");
  SonareBuiltinSynthConfig& config = binding->config;
  if (!ReadBuiltinWaveform(env, obj.Get("waveform"), &config.waveform)) {
    return false;
  }
  config.gain = FloatProperty(obj, "gain", 0.0f);
  config.attack_ms = FloatProperty(obj, "attackMs", 0.0f);
  config.decay_ms = FloatProperty(obj, "decayMs", 0.0f);
  config.sustain = FloatProperty(obj, "sustain", 0.0f);
  config.release_ms = FloatProperty(obj, "releaseMs", 0.0f);
  config.polyphony = IntProperty(obj, "polyphony", kZeroIsSentinel);
  return true;
}

// Marshals a heap-owned SonareProjectCompileResult into the JS compile-result
// object shape and frees its heap fields (consuming the struct).
Napi::Object CompileResultToObject(Napi::Env env, SonareProjectCompileResult* result) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("hasTimeline", Napi::Boolean::New(env, result->has_timeline != 0));
  const std::string messages = result->messages != nullptr ? result->messages : "";
  out.Set("messages", Napi::String::New(env, messages));
  std::vector<std::string> diagnostic_messages;
  std::stringstream message_stream(messages);
  std::string line;
  while (std::getline(message_stream, line)) {
    diagnostic_messages.push_back(line);
  }
  Napi::Array diagnostics = Napi::Array::New(env, result->diagnostic_count);
  for (size_t i = 0; i < result->diagnostic_count; ++i) {
    Napi::Object diag = Napi::Object::New(env);
    diag.Set("code", Napi::Number::New(env, result->diagnostics[i].code));
    diag.Set("severity", Napi::Number::New(env, result->diagnostics[i].severity));
    diag.Set("targetId", Napi::Number::New(env, result->diagnostics[i].target_id));
    diag.Set("message",
             Napi::String::New(env, i < diagnostic_messages.size() ? diagnostic_messages[i] : ""));
    diagnostics.Set(static_cast<uint32_t>(i), diag);
  }
  out.Set("diagnostics", diagnostics);
  sonare_project_free_compile_result(result);
  return out;
}

// The offline bounce keeps close to a megabyte of locals live while it renders,
// which is the whole of V8's stack budget, so a JS call made from inside it
// overflows at once. The bounce therefore runs on a thread with its own large
// stack, and every instrument callback is handed back here, to the JS thread,
// which is blocked in Serve() for the duration and so still the calling thread
// as far as the JS callbacks can tell.
constexpr size_t kBounceStackBytes = size_t{16} << 20;

// The engine hosts every callback instrument as a stereo source, whatever the
// bounce's output channel count.
constexpr int kInstrumentChannels = 2;

class JsThreadBridge {
 public:
  // Worker thread: runs `fn` on the JS thread and waits for it to finish. After
  // Abort() it returns at once without running `fn`.
  template <typename Fn>
  void Run(Fn&& fn) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (aborted_) return;
    job_ = [](void* arg) { (*static_cast<std::remove_reference_t<Fn>*>(arg))(); };
    job_arg_ = &fn;
    state_ = State::kRequested;
    cv_.notify_all();
    cv_.wait(lock, [this] { return state_ == State::kServed; });
    state_ = State::kIdle;
  }

  // Worker thread: no further Run() will follow.
  void Finish() {
    std::lock_guard<std::mutex> lock(mutex_);
    finished_ = true;
    cv_.notify_all();
  }

  // JS thread: stops serving, so the worker runs to its end without calling JS.
  void Abort() {
    std::lock_guard<std::mutex> lock(mutex_);
    aborted_ = true;
    if (state_ == State::kRequested) state_ = State::kServed;
    cv_.notify_all();
  }

  // JS thread: executes requested jobs until the worker finishes. A job that
  // throws aborts the bridge and keeps the exception for TakeError().
  void Serve() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
      cv_.wait(lock, [this] { return state_ == State::kRequested || finished_; });
      if (state_ != State::kRequested) return;
      void (*job)(void*) = job_;
      void* arg = job_arg_;
      lock.unlock();
      try {
        job(arg);
      } catch (...) {
        lock.lock();
        error_ = std::current_exception();
        aborted_ = true;
        state_ = State::kServed;
        cv_.notify_all();
        return;
      }
      lock.lock();
      state_ = State::kServed;
      cv_.notify_all();
    }
  }

  std::exception_ptr TakeError() {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::exchange(error_, nullptr);
  }

 private:
  enum class State { kIdle, kRequested, kServed };
  std::mutex mutex_;
  std::condition_variable cv_;
  State state_ = State::kIdle;
  bool finished_ = false;
  bool aborted_ = false;
  std::exception_ptr error_;
  void (*job_)(void*) = nullptr;
  void* job_arg_ = nullptr;
};

// Owns the bounce thread: whatever path leaves the calling frame, the worker is
// told to stop calling JS and is joined before the frame it reads from dies.
class BounceThread {
 public:
  explicit BounceThread(JsThreadBridge* bridge) : bridge_(bridge) {}
  BounceThread(const BounceThread&) = delete;
  BounceThread& operator=(const BounceThread&) = delete;
  ~BounceThread() {
    if (!started_) return;
    bridge_->Abort();
    uv_thread_join(&thread_);
  }

  bool Start(uv_thread_cb entry, void* arg) {
    uv_thread_options_t options{};
    options.flags = UV_THREAD_HAS_STACK_SIZE;
    options.stack_size = kBounceStackBytes;
    started_ = uv_thread_create_ex(&thread_, &options, entry, arg) == 0;
    return started_;
  }

  void Join() {
    if (!started_) return;
    uv_thread_join(&thread_);
    started_ = false;
  }

 private:
  JsThreadBridge* bridge_;
  uv_thread_t thread_{};
  bool started_ = false;
};

// One JS instrument for the duration of a synchronous callback bounce. It lives
// on the caller's stack, so the C callbacks can hold a raw pointer to it.
struct JsInstrumentSlot {
  Napi::Env env;
  Napi::Object self;
  std::optional<Napi::Function> prepare;
  std::optional<Napi::Function> on_event;
  Napi::Function render;
  JsThreadBridge* bridge;
  int max_block = 1;
  // JS-owned scratch the instrument renders into, one array per channel.
  std::vector<Napi::Reference<Napi::Float32Array>> scratch;
};

// Throws a TypeError and returns true when a callback result is a thenable: an
// async instrument cannot finish inside the synchronous render.
bool RefuseThenable(Napi::Env env, const Napi::Value& result, const char* callback) {
  if (!result.IsObject()) return false;
  const Napi::Value then = result.As<Napi::Object>().Get("then");
  if (env.IsExceptionPending()) return true;
  if (!then.IsFunction()) return false;
  Napi::TypeError::New(env, std::string("bounceWithInstruments: instrument ") + callback +
                                " must not return a Promise; instruments are synchronous")
      .ThrowAsJavaScriptException();
  return true;
}

// Reads an optional instrument callback, refusing any non-function by name.
bool ReadInstrumentCallback(Napi::Env env, const Napi::Value& value, const std::string& label,
                            std::optional<Napi::Function>* out) {
  if (value.IsUndefined() || value.IsNull()) return true;
  if (!value.IsFunction()) {
    Napi::TypeError::New(env, "bounceWithInstruments: " + label + " must be a function")
        .ThrowAsJavaScriptException();
    return false;
  }
  out->emplace(value.As<Napi::Function>());
  return true;
}

// A callback is skipped once any earlier one has thrown, so the throw is
// rethrown after the bounce as the first failure.
void PrepareOnJsThread(JsInstrumentSlot* slot, double sample_rate, int max_block_size) {
  slot->max_block = std::max(max_block_size, 1);
  if (slot->env.IsExceptionPending() || !slot->prepare) return;
  Napi::HandleScope scope(slot->env);
  const Napi::Value result =
      slot->prepare->Call(slot->self, {Napi::Number::New(slot->env, sample_rate),
                                       Napi::Number::New(slot->env, max_block_size),
                                       Napi::Number::New(slot->env, kInstrumentChannels)});
  if (slot->env.IsExceptionPending()) return;
  RefuseThenable(slot->env, result, "prepare");
}

void OnEventOnJsThread(JsInstrumentSlot* slot, uint32_t destination_id, const uint32_t* ump_words,
                       int word_count, int64_t render_frame) {
  if (slot->env.IsExceptionPending() || !slot->on_event) return;
  Napi::Env env = slot->env;
  Napi::HandleScope scope(env);
  const uint32_t count = static_cast<uint32_t>(std::max(word_count, 0));
  Napi::Array words = Napi::Array::New(env, count);
  for (uint32_t i = 0; i < count; ++i) {
    words.Set(i, Napi::Number::New(env, ump_words[i]));
  }
  Napi::Object event = Napi::Object::New(env);
  event.Set("destinationId", Napi::Number::New(env, destination_id));
  event.Set("words", words);
  event.Set("renderFrame", Napi::Number::New(env, static_cast<double>(render_frame)));
  const Napi::Value result = slot->on_event->Call(slot->self, {event});
  if (env.IsExceptionPending()) return;
  RefuseThenable(env, result, "onEvent");
}

// Makes scratch channel `ch` hold at least `frames` samples; a view whose buffer
// the instrument transferred away is replaced.
Napi::Float32Array ScratchChannel(JsInstrumentSlot* slot, size_t ch, size_t frames) {
  Napi::Env env = slot->env;
  const size_t capacity = std::max(frames, static_cast<size_t>(slot->max_block));
  if (ch >= slot->scratch.size()) slot->scratch.resize(ch + 1);
  Napi::Reference<Napi::Float32Array>& ref = slot->scratch[ch];
  if (ref.IsEmpty() || ref.Value().ElementLength() < frames ||
      ref.Value().ArrayBuffer().IsDetached()) {
    ref = Napi::Persistent(Napi::Float32Array::New(env, capacity));
  }
  return ref.Value();
}

void RenderOnJsThread(JsInstrumentSlot* slot, float* const* channels, int num_channels,
                      int num_frames) {
  Napi::Env env = slot->env;
  if (env.IsExceptionPending() || num_channels <= 0 || num_frames <= 0) return;
  Napi::HandleScope scope(env);
  const size_t frames = static_cast<size_t>(num_frames);
  // Zeroed scratch views; the instrument's output is added into the engine's
  // buffers only after the call returns normally.
  Napi::Array outputs = Napi::Array::New(env, static_cast<size_t>(num_channels));
  for (int ch = 0; ch < num_channels; ++ch) {
    Napi::Float32Array base = ScratchChannel(slot, static_cast<size_t>(ch), frames);
    std::memset(base.Data(), 0, frames * sizeof(float));
    outputs.Set(static_cast<uint32_t>(ch),
                Napi::Float32Array::New(env, frames, base.ArrayBuffer(), base.ByteOffset()));
  }
  const Napi::Value result =
      slot->render.Call(slot->self, {outputs, Napi::Number::New(env, num_frames)});
  if (env.IsExceptionPending() || RefuseThenable(env, result, "render")) return;
  for (int ch = 0; ch < num_channels; ++ch) {
    Napi::Float32Array base = ScratchChannel(slot, static_cast<size_t>(ch), frames);
    // A scratch the instrument transferred away was just replaced by zeros.
    const float* src = base.Data();
    float* dst = channels[ch];
    for (size_t i = 0; i < frames; ++i) dst[i] += src[i];
  }
}

// C callbacks, called on the bounce thread: each forwards to the JS thread.
void JsInstrumentPrepare(void* user_data, double sample_rate, int max_block_size) {
  auto* slot = static_cast<JsInstrumentSlot*>(user_data);
  slot->bridge->Run([&] { PrepareOnJsThread(slot, sample_rate, max_block_size); });
}

void JsInstrumentOnEvent(void* user_data, uint32_t destination_id, const uint32_t* ump_words,
                         int word_count, int64_t render_frame) {
  auto* slot = static_cast<JsInstrumentSlot*>(user_data);
  slot->bridge->Run(
      [&] { OnEventOnJsThread(slot, destination_id, ump_words, word_count, render_frame); });
}

void JsInstrumentRender(void* user_data, float* const* channels, int num_channels, int num_frames) {
  auto* slot = static_cast<JsInstrumentSlot*>(user_data);
  slot->bridge->Run([&] { RenderOnJsThread(slot, channels, num_channels, num_frames); });
}

// Everything the bounce thread reads and writes, so it touches no JS state.
struct BounceJob {
  JsThreadBridge* bridge;
  SonareProject* project;
  const SonareProjectBounceOptions* options;
  const SonareInstrumentBinding* bindings;
  size_t binding_count;
  float* interleaved = nullptr;
  size_t len = 0;
  SonareError error = SONARE_OK;
  std::string detail;

  BounceJob(JsThreadBridge* bridge_in, SonareProject* project_in,
            const SonareProjectBounceOptions* options_in,
            const SonareInstrumentBinding* bindings_in, size_t binding_count_in)
      : bridge(bridge_in),
        project(project_in),
        options(options_in),
        bindings(bindings_in),
        binding_count(binding_count_in) {}
  BounceJob(const BounceJob&) = delete;
  BounceJob& operator=(const BounceJob&) = delete;
  ~BounceJob() {
    if (interleaved != nullptr) sonare_free_floats(interleaved);
  }
};

void RunBounceJob(void* arg) {
  auto* job = static_cast<BounceJob*>(arg);
  job->error = sonare_project_bounce_with_instruments(
      job->project, job->options, job->bindings, job->binding_count, &job->interleaved, &job->len);
  // The detail slot is thread-local, so it is read here and not on the JS thread.
  const char* detail = sonare_last_error_message();
  if (job->error != SONARE_OK && detail != nullptr) job->detail = detail;
  job->bridge->Finish();
}
}  // namespace

Napi::Value ProjectWrap::Compile(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SonareProjectCompileResult result{};
  ThrowIfError(env, sonare_project_compile(project_, &result));
  if (env.IsExceptionPending()) return env.Undefined();
  return CompileResultToObject(env, &result);
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::CompileTimeline(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SonareProjectCompileResult result{};
  SonareProjectTimeline* timeline = nullptr;
  ThrowIfError(env, sonare_project_compile_timeline(project_, &result, &timeline));
  if (env.IsExceptionPending()) return env.Undefined();
  Napi::Object out = CompileResultToObject(env, &result);
  out.Set("timeline",
          timeline != nullptr ? Napi::Value(ProjectTimelineWrap::Wrap(env, timeline)) : env.Null());
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::LastBounceCompileResult(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SonareProjectCompileResult result{};
  ThrowIfError(env, sonare_project_last_bounce_compile_result(project_, &result));
  if (env.IsExceptionPending()) return env.Undefined();
  return CompileResultToObject(env, &result);
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::Bounce(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SonareProjectBounceOptions options{};
  if (info.Length() > 0 && info[0].IsObject()) {
    FillBounceOptions(info[0].As<Napi::Object>(), &options);
    if (env.IsExceptionPending()) return env.Undefined();
  }
  float* interleaved = nullptr;
  size_t len = 0;
  ThrowIfError(env, sonare_project_bounce(project_, &options, &interleaved, &len));
  if (env.IsExceptionPending()) return env.Undefined();
  Napi::Float32Array out = Napi::Float32Array::New(env, len);
  if (len > 0 && interleaved != nullptr) {
    std::memcpy(out.Data(), interleaved, len * sizeof(float));
  }
  if (interleaved != nullptr) sonare_free_floats(interleaved);
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::BounceWithBuiltinInstruments(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  // Argument order is instrument-first to match the WASM and Python bindings:
  //   bounceWithBuiltinInstruments(instruments, options?)
  SonareProjectBounceOptions options{};
  if (info.Length() > 1 && info[1].IsObject() && !info[1].IsArray()) {
    FillBounceOptions(info[1].As<Napi::Object>(), &options);
    if (env.IsExceptionPending()) return env.Undefined();
  }
  std::vector<SonareBuiltinInstrumentBinding> bindings;
  if (info.Length() > 0 && info[0].IsArray()) {
    Napi::Array arr = info[0].As<Napi::Array>();
    bindings.reserve(arr.Length());
    for (uint32_t i = 0; i < arr.Length(); ++i) {
      Napi::Value element = arr.Get(i);
      if (!element.IsObject()) {
        Napi::TypeError::New(env,
                             "bounceWithBuiltinInstruments: instrument bindings must be objects")
            .ThrowAsJavaScriptException();
        return env.Undefined();
      }
      SonareBuiltinInstrumentBinding binding{};
      if (!ParseBuiltinInstrument(env, element.As<Napi::Object>(), &binding)) {
        return env.Undefined();  // exception already pending
      }
      bindings.push_back(binding);
    }
  }
  float* interleaved = nullptr;
  size_t len = 0;
  ThrowIfError(env, sonare_project_bounce_with_builtin_instruments(
                        project_, &options, bindings.empty() ? nullptr : bindings.data(),
                        bindings.size(), &interleaved, &len));
  if (env.IsExceptionPending()) return env.Undefined();
  Napi::Float32Array out = Napi::Float32Array::New(env, len);
  if (len > 0 && interleaved != nullptr) {
    std::memcpy(out.Data(), interleaved, len * sizeof(float));
  }
  if (interleaved != nullptr) sonare_free_floats(interleaved);
  return out;
  SONARE_NODE_CATCH(env)
}

// Compiles + renders the project, driving MIDI tracks through JS instruments
// whose callbacks run synchronously on this thread for the whole bounce:
//   bounceWithInstruments(instruments, options?)
// A throw from any callback skips the remaining callbacks and surfaces after
// the bounce returns.
Napi::Value ProjectWrap::BounceWithInstruments(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SonareProjectBounceOptions options{};
  if (info.Length() > 1 && info[1].IsObject() && !info[1].IsArray()) {
    FillBounceOptions(info[1].As<Napi::Object>(), &options);
    if (env.IsExceptionPending()) return env.Undefined();
  }
  JsThreadBridge bridge;
  std::vector<JsInstrumentSlot> slots;
  std::vector<SonareInstrumentBinding> bindings;
  if (!info[0].IsUndefined() && !info[0].IsNull()) {
    Napi::Array arr;
    if (!RequiredArrayValue(env, info[0], "instruments", &arr)) return env.Undefined();
    slots.reserve(arr.Length());
    bindings.reserve(arr.Length());
    for (uint32_t i = 0; i < arr.Length(); ++i) {
      const Napi::Value element = arr.Get(i);
      if (!element.IsObject()) {
        Napi::TypeError::New(env, "bounceWithInstruments: instrument bindings must be objects")
            .ThrowAsJavaScriptException();
        return env.Undefined();
      }
      const Napi::Object obj = element.As<Napi::Object>();
      JsInstrumentSlot slot{env, obj, std::nullopt, std::nullopt, Napi::Function(), &bridge, 1, {}};
      const std::string label = "instruments[" + std::to_string(i) + "]";
      std::optional<Napi::Function> render;
      if (!ReadInstrumentCallback(env, obj.Get("prepare"), label + ".prepare", &slot.prepare) ||
          !ReadInstrumentCallback(env, obj.Get("onEvent"), label + ".onEvent", &slot.on_event) ||
          !ReadInstrumentCallback(env, obj.Get("render"), label + ".render", &render)) {
        return env.Undefined();
      }
      if (!render) {
        Napi::TypeError::New(env, "bounceWithInstruments: " + label + ".render must be a function")
            .ThrowAsJavaScriptException();
        return env.Undefined();
      }
      slot.render = *render;
      SonareInstrumentBinding binding{};
      binding.destination_id = Uint32Property(obj, "destinationId", 0u);
      binding.callbacks.latency_samples = IntProperty(obj, "latencySamples", 0);
      binding.callbacks.tail_samples = IntProperty(obj, "tailSamples", 0);
      if (env.IsExceptionPending()) return env.Undefined();
      binding.callbacks.prepare = &JsInstrumentPrepare;
      binding.callbacks.on_event = &JsInstrumentOnEvent;
      binding.callbacks.render = &JsInstrumentRender;
      slots.push_back(std::move(slot));
      bindings.push_back(binding);
    }
    // The slots vector no longer grows, so these addresses stay valid.
    for (size_t i = 0; i < slots.size(); ++i) bindings[i].callbacks.user_data = &slots[i];
  }
  auto busy_call = BeginBusyCall();
  BounceJob job(&bridge, project_, &options, bindings.empty() ? nullptr : bindings.data(),
                bindings.size());
  BounceThread worker(&bridge);
  if (!worker.Start(RunBounceJob, &job)) {
    sonare_node::ThrowSonareErrorMessage(env, SONARE_ERROR_UNKNOWN,
                                         "bounceWithInstruments: cannot start the bounce thread");
    return env.Undefined();
  }
  bridge.Serve();
  worker.Join();
  if (const std::exception_ptr failure = bridge.TakeError()) std::rethrow_exception(failure);
  // The native failure outranks a callback's error, as in the Python binding;
  // the callback's pending exception is dropped to report it.
  if (job.error != SONARE_OK) {
    if (env.IsExceptionPending()) (void)env.GetAndClearPendingException();
    if (!job.detail.empty()) {
      sonare_node::ThrowSonareErrorMessage(env, job.error, job.detail);
    } else {
      // The detail slot is thread-local and was read on the bounce thread.
      sonare_node::ThrowIfRealtimeError(env, job.error);
    }
    return env.Undefined();
  }
  if (env.IsExceptionPending()) return env.Undefined();
  const float* const interleaved = job.interleaved;
  const size_t len = job.len;
  Napi::Float32Array out = Napi::Float32Array::New(env, len);
  if (len > 0 && interleaved != nullptr) {
    std::memcpy(out.Data(), interleaved, len * sizeof(float));
  }
  return out;
  SONARE_NODE_CATCH(env)
}

// Compiles + renders the project, routing MIDI tracks through the patch-driven
// NativeSynth (the full synthesizer; see SonareSynthPatch). Argument order is
// instrument-first to match the WASM and Python bindings:
//   bounceWithSynthInstruments(instruments, options?)
// Each binding is { destinationId?, ...patch } where the patch is a SynthPatch
// object or a preset-name string ("saw-lead" / "va:saw-lead"). An unknown
// preset name throws (the C ABI rejects it).
Napi::Value ProjectWrap::BounceWithSynthInstruments(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SonareProjectBounceOptions options{};
  if (info.Length() > 1 && info[1].IsObject() && !info[1].IsArray()) {
    FillBounceOptions(info[1].As<Napi::Object>(), &options);
    if (env.IsExceptionPending()) return env.Undefined();
  }
  std::vector<SonareSynthInstrumentBinding> bindings;
  if (info.Length() > 0 && info[0].IsArray()) {
    Napi::Array arr = info[0].As<Napi::Array>();
    bindings.reserve(arr.Length());
    for (uint32_t i = 0; i < arr.Length(); ++i) {
      Napi::Value element = arr.Get(i);
      SonareSynthInstrumentBinding binding{};
      if (element.IsObject() && !element.IsArray()) {
        Napi::Object obj = element.As<Napi::Object>();
        binding.destination_id = sonare_node::Uint32Property(obj, "destinationId", 0u);
        if (env.IsExceptionPending()) return env.Undefined();
        binding.use_gm_programs = BoolProperty(obj, "useGmPrograms", false) ? 1 : 0;
        if (env.IsExceptionPending()) return env.Undefined();
        // Borrowed for the call: the JS bank object stays reachable through the
        // instruments array for the whole synchronous bounce.
        if (!SampleBankWrap::ReadHandle(env, obj.Get("sampleBank"), &binding.sample_bank)) {
          return env.Undefined();  // exception already pending
        }
      }
      if (!sonare_node::ReadSynthPatch(env, element, &binding.patch)) {
        return env.Undefined();  // exception already pending
      }
      bindings.push_back(binding);
    }
  }
  float* interleaved = nullptr;
  size_t len = 0;
  ThrowIfError(env, sonare_project_bounce_with_synth_instruments(
                        project_, &options, bindings.empty() ? nullptr : bindings.data(),
                        bindings.size(), &interleaved, &len));
  if (env.IsExceptionPending()) return env.Undefined();
  Napi::Float32Array out = Napi::Float32Array::New(env, len);
  if (len > 0 && interleaved != nullptr) {
    std::memcpy(out.Data(), interleaved, len * sizeof(float));
  }
  if (interleaved != nullptr) sonare_free_floats(interleaved);
  return out;
  SONARE_NODE_CATCH(env)
}

void ProjectWrap::LoadSoundFont(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
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
    Napi::TypeError::New(env, "loadSoundFont expects a Buffer or Uint8Array")
        .ThrowAsJavaScriptException();
    return;
  }
  ThrowIfError(env, sonare_project_load_soundfont(project_, bytes, len));
  SONARE_NODE_CATCH_VOID(env)
}

void ProjectWrap::ClearSoundFont(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  ThrowIfError(info.Env(), sonare_project_clear_soundfont(project_));
  SONARE_NODE_CATCH_VOID(env)
}

Napi::Value ProjectWrap::SoundFontPresetCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  size_t out = 0;
  ThrowIfError(env, sonare_project_soundfont_preset_count(project_, &out));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(out));
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::SoundFontManifest(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  size_t total = 0;
  ThrowIfError(env, sonare_project_soundfont_manifest(project_, nullptr, 0, &total));
  if (env.IsExceptionPending()) return env.Undefined();
  std::vector<SonareSf2ProgramStatus> entries(total);
  if (total > 0) {
    ThrowIfError(env, sonare_project_soundfont_manifest(project_, entries.data(), total, &total));
    if (env.IsExceptionPending()) return env.Undefined();
  }
  Napi::Array out = Napi::Array::New(env, entries.size());
  for (size_t i = 0; i < entries.size(); ++i) {
    Napi::Object entry = Napi::Object::New(env);
    entry.Set("channel", Napi::Number::New(env, entries[i].channel));
    entry.Set("bank", Napi::Number::New(env, entries[i].bank));
    entry.Set("program", Napi::Number::New(env, entries[i].program));
    entry.Set(
        "backend",
        Napi::String::New(env, entries[i].backend == SONARE_SOURCE_BACKEND_SF2 ? "sf2" : "synth"));
    entry.Set("presetName", Napi::String::New(env, entries[i].preset_name));
    out.Set(static_cast<uint32_t>(i), entry);
  }
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::BounceWithSf2Instruments(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  // Argument order is instrument-first to match bounceWithBuiltinInstruments:
  //   bounceWithSf2Instruments(instruments, options?)
  SonareProjectBounceOptions options{};
  if (info.Length() > 1 && info[1].IsObject() && !info[1].IsArray()) {
    FillBounceOptions(info[1].As<Napi::Object>(), &options);
    if (env.IsExceptionPending()) return env.Undefined();
  }
  std::vector<SonareSf2InstrumentBinding> bindings;
  if (!info[0].IsUndefined() && !info[0].IsNull()) {
    Napi::Array arr;
    if (!RequiredArrayValue(env, info[0], "instruments", &arr)) return env.Undefined();
    bindings.reserve(arr.Length());
    for (uint32_t i = 0; i < arr.Length(); ++i) {
      Napi::Value element = arr.Get(i);
      if (!element.IsObject()) {
        Napi::TypeError::New(env, "bounceWithSf2Instruments: instrument bindings must be objects")
            .ThrowAsJavaScriptException();
        return env.Undefined();
      }
      Napi::Object obj = element.As<Napi::Object>();
      SonareSf2InstrumentBinding binding{};
      binding.destination_id = Uint32Property(obj, "destinationId", 0);
      binding.config.gain = FloatProperty(obj, "gain", 0.0f);
      binding.config.polyphony = IntProperty(obj, "polyphony", kZeroIsSentinel);
      const Napi::Value prefer_model = obj.Get("preferModelForModeledFamilies");
      if (!prefer_model.IsUndefined() && !prefer_model.IsNull()) {
        binding.config.struct_version = 2;
        binding.config.prefer_model_for_modeled_families =
            BoolProperty(obj, "preferModelForModeledFamilies", false) ? 1 : 0;
      }
      // Version 3 reads version 2's field as well, so raising it here covers
      // both whichever of the two the caller passed.
      const Napi::Value clear_rig = obj.Get("clearBankRig");
      if (!clear_rig.IsUndefined() && !clear_rig.IsNull()) {
        binding.config.struct_version = 3;
        binding.config.clear_bank_rig = BoolProperty(obj, "clearBankRig", false) ? 1 : 0;
      }
      binding.config.struct_version = 4;
      binding.config.gs_efx_realization =
          sonare_node::GsEfxRealizationProperty(obj, "gsEfxRealization");
      bindings.push_back(binding);
    }
  }
  float* interleaved = nullptr;
  size_t len = 0;
  ThrowIfError(env, sonare_project_bounce_with_sf2_instruments(
                        project_, &options, bindings.empty() ? nullptr : bindings.data(),
                        bindings.size(), &interleaved, &len));
  if (env.IsExceptionPending()) return env.Undefined();
  Napi::Float32Array out = Napi::Float32Array::New(env, len);
  if (len > 0 && interleaved != nullptr) {
    std::memcpy(out.Data(), interleaved, len * sizeof(float));
  }
  if (interleaved != nullptr) sonare_free_floats(interleaved);
  return out;
  SONARE_NODE_CATCH(env)
}
