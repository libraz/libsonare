#pragma once

/// @file insert_factory.h
/// @brief Heap factory that builds streaming rt::ProcessorBase inserts from a
///        processor name plus a JSON params object.
///
/// Unlike apply_named_processor() (which is offline and returns a fully
/// processed buffer), make_insert() returns a freshly heap-allocated processor
/// that the caller drives via prepare()/process(). It is the bridge the mixer
/// uses to instantiate channel-strip inserts from a scene.

#include <memory>
#include <string>
#include <vector>

#include "mastering/api/named_processor.h"
#include "rt/processor_base.h"

namespace sonare::mastering::api {

/// @brief Construct a streaming insert processor by name.
/// @param name Processor name, e.g. "dynamics.compressor", "eq.parametric",
///             "effects.reverb.plate".
/// @param json_params A JSON object string ("{...}"). Empty or "{}" means the
///             processor's defaults. Keys mirror the camelCase param names that
///             named_processor.cpp accepts (e.g. "thresholdDb", "ratio").
/// @param out_unknown_keys When non-null and @p name is a known insert, receives
///             the supplied param keys that the processor did not read (silently
///             ignored). Sorted; empty when every key took effect. Left
///             untouched for an unknown @p name (which returns nullptr).
/// @return A heap-allocated processor, or nullptr if @p name is not a known
///         block-processor insert.
/// @throws sonare::SonareException (InvalidParameter) only when @p json_params
///         is malformed. Unknown names return nullptr rather than throwing.
std::unique_ptr<sonare::rt::ProcessorBase> make_insert(
    const std::string& name, const std::string& json_params,
    std::vector<std::string>* out_unknown_keys = nullptr);

/// @brief Same as make_insert() but takes an already-parsed Param list instead
///        of a JSON string. Used by the offline named-processor path so it can
///        reach the streaming inserts (e.g. the creative effects.* reverbs and
///        modulation effects) without re-serializing to JSON.
/// @return A heap-allocated processor, or nullptr if @p name is not a known
///         block-processor insert.
std::unique_ptr<sonare::rt::ProcessorBase> make_insert_from_params(
    const std::string& name, const std::vector<Param>& param_list);

/// @brief Construct a streaming insert that needs an impulse response.
/// @param name Processor name. For "effects.reverb.convolution" the returned
///             ConvolutionReverb is loaded with @p impulse_response so the
///             effect actually convolves. For every other name this behaves
///             exactly like make_insert() and @p impulse_response is ignored.
/// @param json_params A JSON object string (see make_insert()).
/// @param impulse_response IR samples to load (may be empty, which leaves a
///             passthrough convolver until an IR is provided).
/// @param ir_num_samples Number of samples in @p impulse_response.
/// @return A heap-allocated processor, or nullptr if @p name is unknown (or if
///         @p name is the convolution insert but the build lacks FX support).
/// @throws sonare::SonareException (InvalidParameter) when @p json_params is
///         malformed or the IR pointer/size is inconsistent.
std::unique_ptr<sonare::rt::ProcessorBase> make_insert_with_ir(const std::string& name,
                                                               const std::string& json_params,
                                                               const float* impulse_response,
                                                               int ir_num_samples);

/// @brief Names that make_insert() can build (a stable, sorted list).
std::vector<std::string> insert_factory_names();

/// @brief Parameter names a given insert processor reads, for tooling/validation.
/// @param name Processor name (see make_insert()).
/// @return The camelCase param keys the processor consumes for a default
///         configuration, sorted. Band/sub-band processors additionally enumerate
///         their indexed `band{i}.<field>` keys. Returns an empty list for an
///         unknown @p name (or a name whose insert needs an unavailable build
///         feature, e.g. FX).
std::vector<std::string> insert_param_names(const std::string& name);

/// @brief Realtime-automatable parameter descriptors for an insert processor.
/// @param name Processor name (see make_insert()).
/// @return A JSON array string `[{"name","id","rtSafe"}, ...]` mapping each
///         realtime-automatable parameter's JSON key to the integer param_id
///         accepted by the engine's realtime insert-parameter setter, with
///         `rtSafe` reporting whether the param can be changed from the audio
///         thread. Returns `[]` for an unknown @p name or a processor that
///         exposes no automatable parameters. Unlike insert_param_names (which
///         lists every construction key), this lists only the keys reachable via
///         set_parameter, i.e. the realtime-controllable subset.
std::string insert_param_info_json(const std::string& name);

}  // namespace sonare::mastering::api
