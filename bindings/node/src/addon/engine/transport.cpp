#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

#include "c_api/engine_config_domains.h"
#include "engine/common.h"
#include "engine/realtime_engine.h"
#include "midi/live_push_args.h"
#include "sonare_wrap_engine.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_sample_bank.h"
#include "sonare_wrap_synth_patch.h"
#include "sonare_wrap_utils.h"

using namespace sonare_node::engine;

Napi::Value RealtimeEngineWrap::Prepare(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  // The sample rate is required: an absent one reads as undefined and is
  // rejected by the reader below, so it reaches the caller as a TypeError rather
  // than as a silent no-op.
  int sample_rate = 0;
  if (!sonare_node::ReadWholeHertzSampleRate(env, info[0], &sample_rate)) return env.Undefined();
  int max_block_size = 0;
  int64_t command_capacity = 0;
  int64_t telemetry_capacity = 0;
  int max_channels = 0;
  if (!OptionalDomainArg(env, info, 1, "maxBlockSize", 0,
                         sonare_c_detail::kEngineMaxBlockSizeDomain, &max_block_size) ||
      !OptionalInt64Arg(env, info, 2, "commandCapacity", 0, &command_capacity) ||
      !OptionalInt64Arg(env, info, 3, "telemetryCapacity", 0, &telemetry_capacity) ||
      !OptionalDomainArg(env, info, 4, "maxChannels", 0, sonare_c_detail::kEngineMaxChannelsDomain,
                         &max_channels)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_prepare(engine_, sample_rate, max_block_size,
                                          static_cast<size_t>(command_capacity),
                                          static_cast<size_t>(telemetry_capacity), max_channels));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::Play(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  int64_t deadline = -1;
  if (!OptionalInt64Arg(env, info, 0, "renderFrame", -1, &deadline)) return env.Undefined();
  ThrowIfError(env, sonare_engine_play(engine_, deadline));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::Stop(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  int64_t deadline = -1;
  if (!OptionalInt64Arg(env, info, 0, "renderFrame", -1, &deadline)) return env.Undefined();
  ThrowIfError(env, sonare_engine_stop(engine_, deadline));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SeekSample(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  int64_t sample = 0;
  int64_t deadline = -1;
  if (!OptionalInt64Arg(env, info, 0, "timelineSample", 0, &sample) ||
      !OptionalInt64Arg(env, info, 1, "renderFrame", -1, &deadline)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_seek_sample(engine_, sample, deadline));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SettleParameters(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  ThrowIfError(env, sonare_engine_settle_parameters(engine_));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::FlushControlCommands(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  ThrowIfError(env, sonare_engine_flush_control_commands(engine_));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SettleInsertParameters(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  ThrowIfError(env, sonare_engine_settle_insert_parameters(engine_));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::ApplyCommandsDueNowPreservingFuture(
    const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  ThrowIfError(env, sonare_engine_apply_commands_due_now_preserving_future(engine_));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SeekPpq(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  double ppq = 0.0;
  int64_t deadline = -1;
  if (!RequiredDomainDoubleValue(env, info[0], "ppq", &ppq) ||
      !OptionalInt64Arg(env, info, 1, "renderFrame", -1, &deadline)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_seek_ppq(engine_, ppq, deadline));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetTempo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  double bpm = 0.0;
  if (!RequiredDoubleArg(env, info, 0, "bpm", &bpm)) return env.Undefined();
  ThrowIfError(env, sonare_engine_set_tempo(engine_, bpm));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetTimeSignature(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  int numerator = 0;
  int denominator = 0;
  if (!RequiredIntArg(env, info, 0, "numerator", &numerator) ||
      !RequiredIntArg(env, info, 1, "denominator", &denominator)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_set_time_signature(engine_, numerator, denominator));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::ResetMasterLoudnessMeter(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  int64_t render_frame = -1;
  if (!OptionalInt64Arg(env, info, 0, "renderFrame", -1, &render_frame)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_reset_master_loudness_meter(engine_, render_frame));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::PrimeOfflineParameters(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  int num_channels = 0;
  int block_size = 0;  // C value 0 = library default offline block
  if (!RequiredIntArg(env, info, 0, "numChannels", &num_channels) ||
      !OptionalDomainArg(env, info, 1, "blockSize", 0,
                         sonare_c_detail::kEngineRenderBlockSizeDomain, &block_size)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_prime_offline_parameters(engine_, num_channels, block_size));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::ResetProcessorState(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  int64_t render_frame = -1;
  if (!OptionalInt64Arg(env, info, 0, "renderFrame", -1, &render_frame)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_reset_processor_state(engine_, render_frame));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::TailSamples(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  int tail = 0;
  ThrowIfError(env, sonare_engine_tail_samples(engine_, &tail));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(tail));
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::MaxBlockSize(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  int max_block = 0;
  ThrowIfError(env, sonare_engine_max_block_size(engine_, &max_block));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, max_block);
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::GraphLatencySamplesQ8(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  int latency_q8 = 0;
  ThrowIfError(env, sonare_engine_graph_latency_samples_q8(engine_, &latency_q8));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(latency_q8));
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetTempoSegments(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  Napi::Array input;
  if (!RequiredArrayValue(env, info[0], "segments", &input)) return env.Undefined();
  std::vector<SonareProjectTempoSegment> segments;
  const uint32_t input_count = sonare_node::node_list_length(input, "segments");
  segments.reserve(input_count);
  for (uint32_t i = 0; i < input_count; ++i) {
    Napi::Object obj;
    std::string at;
    SonareProjectTempoSegment segment{};
    // Presence and type here, each domain in the C entry's text under the same index.
    if (!RequiredElementObject(env, input, i, "segments", &obj, &at) ||
        !RequiredDomainDoubleValue(env, obj.Get("startPpq"), at + ".startPpq",
                                   &segment.start_ppq) ||
        !RequiredDomainDoubleValue(env, obj.Get("bpm"), at + ".bpm", &segment.bpm)) {
      return env.Undefined();
    }
    segment.end_bpm = DomainDoubleProperty(obj, "endBpm", segment.end_bpm, at);
    segments.push_back(segment);
  }
  ThrowIfError(env, sonare_engine_set_tempo_segments(engine_, sonare_node::ElementData(segments),
                                                     segments.size()));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetTimeSignatureSegments(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  Napi::Array input;
  if (!RequiredArrayValue(env, info[0], "segments", &input)) return env.Undefined();
  std::vector<SonareProjectTimeSignatureSegment> segments;
  const uint32_t input_count = sonare_node::node_list_length(input, "segments");
  segments.reserve(input_count);
  for (uint32_t i = 0; i < input_count; ++i) {
    Napi::Object obj;
    std::string at;
    SonareProjectTimeSignatureSegment segment{};
    if (!RequiredElementObject(env, input, i, "segments", &obj, &at) ||
        !RequiredDomainDoubleValue(env, obj.Get("startPpq"), at + ".startPpq",
                                   &segment.start_ppq) ||
        !RequiredDomainValue(env, obj.Get("numerator"), (at + ".numerator").c_str(),
                             sonare_c_detail::kEngineTimeSignatureNumeratorDomain,
                             &segment.numerator) ||
        !RequiredDomainValue(env, obj.Get("denominator"), (at + ".denominator").c_str(),
                             sonare_c_detail::kEngineTimeSignatureDenominatorDomain,
                             &segment.denominator)) {
      return env.Undefined();
    }
    segments.push_back(segment);
  }
  ThrowIfError(env, sonare_engine_set_time_signature_segments(
                        engine_, sonare_node::ElementData(segments), segments.size()));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SampleAtPpq(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  double ppq = 0.0;
  if (!RequiredDomainDoubleValue(env, info[0], "ppq", &ppq)) return env.Undefined();
  int64_t sample = 0;
  ThrowIfError(env, sonare_engine_sample_at_ppq(engine_, ppq, &sample));
  return Napi::Number::New(env, static_cast<double>(sample));
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetLoop(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  double start_ppq = 0.0;
  double end_ppq = 0.0;
  bool enabled = true;
  if (!RequiredDoubleArg(env, info, 0, "startPpq", &start_ppq) ||
      !RequiredDoubleArg(env, info, 1, "endPpq", &end_ppq) ||
      !OptionalBoolArg(env, info, 2, "enabled", true, &enabled)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_set_loop(engine_, start_ppq, end_ppq, enabled ? 1 : 0));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}
