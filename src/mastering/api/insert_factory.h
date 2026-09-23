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
#include "util/resource_limits.h"

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
/// @param limits Budget @p json_params is parsed under, as a total for the whole
///             call rather than per reader. The default is the budget that
///             admitted the enclosing project document, since an insert's params
///             are a string lifted out of one. Overridable so the exact boundary
///             can be exercised without building a document of the production
///             size.
/// @return A heap-allocated processor, or nullptr if @p name is not a known
///         block-processor insert.
/// @throws sonare::SonareException (InvalidParameter) when @p json_params is
///         malformed or does not fit @p limits. Unknown names return nullptr
///         rather than throwing.
std::unique_ptr<sonare::rt::ProcessorBase> make_insert(
    const std::string& name, const std::string& json_params,
    std::vector<std::string>* out_unknown_keys = nullptr,
    const resource::ProjectImportResourceLimits& limits =
        resource::kDefaultProjectImportResourceLimits);

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
/// @param limits Parse budget for @p json_params (see make_insert()).
/// @return A heap-allocated processor, or nullptr if @p name is unknown (or if
///         @p name is the convolution insert but the build lacks FX support).
/// @throws sonare::SonareException (InvalidParameter) when @p json_params is
///         malformed, does not fit @p limits, or the IR pointer/size is
///         inconsistent.
std::unique_ptr<sonare::rt::ProcessorBase> make_insert_with_ir(
    const std::string& name, const std::string& json_params, const float* impulse_response,
    int ir_num_samples,
    const resource::ProjectImportResourceLimits& limits =
        resource::kDefaultProjectImportResourceLimits);

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

/// @brief Parameter descriptors for an insert processor.
/// @param name Processor name (see make_insert()).
/// @return A JSON array string: first each realtime-automatable parameter, in
///         descriptor order, with the integer param_id accepted by the engine's
///         realtime insert-parameter setter; then, sorted by name, every other
///         key construction reads, with a null id. Each carries its declared
///         type, measured bounds or choices, default, unit and the slot it
///         belongs to (see insert_slot_info_json()), or null. See
///         insert_param_info_schema_paths() for the exact field set; every
///         entry carries every field, with `null` where a value could not be
///         measured. Returns `[]` for an unknown @p name.
std::string insert_param_info_json(const std::string& name);

/// @brief The key groups of an insert that exist only under a condition.
/// @return A JSON array string, one entry per slot in the order construction
///         declares them: `name` (the key prefix, e.g. "midBand3" or
///         "band1.dyn2"), `parent` (the enclosing slot or null), `activation`
///         ("anyKey": exists once any of its keys is supplied; "always") and
///         `minCrossoverCutoffs` (cutoffs the crossover in effect needs for the
///         slot to exist; 0 when it needs none). `[]` for an unknown @p name or
///         an insert with no slots.
std::string insert_slot_info_json(const std::string& name);

/// Rate and block size an insert is prepared at when the catalog measures it.
inline constexpr double kInsertProbeSampleRate = 48000.0;
inline constexpr int kInsertProbeBlockSize = 512;

/// @brief The configuration that measures @p key of insert @p name at @p value.
/// @details `{key: value}`, which on its own makes a presence-gated slot
///          exist; plus, when @p key's slot is a crossover band past the default
///          split, as many ascending cutoffs as that band needs to exist.
std::vector<Param> insert_probe_params(const std::string& name, const std::string& key,
                                       double value);

/// Latency and tail an insert reports once prepared, in samples.
struct InsertTiming {
  int latency_samples = 0;
  int tail_samples = 0;
};

/// @brief Latency and tail of insert @p name built from @p json_params and
///        prepared at @p sample_rate with kInsertProbeBlockSize.
/// @details Both are clamped at zero. The block size does not change either.
/// @throws sonare::SonareException (InvalidParameter) for an unknown @p name
///         ("unknown insert processor: <name>"), a key the insert does not read
///         ("<name> does not read parameter(s): <k1>, <k2>"), or a value its
///         construction or prepare refuses.
InsertTiming insert_timing(const std::string& name, const std::string& json_params,
                           double sample_rate);

/// @brief Canonical field paths for one entry of the parameter info array.
/// @details The array is the root, so each path begins with the `[]` element
/// segment. Keep the JSON writer and both TypeScript result types in parity by
/// testing them against this list.
const std::vector<std::string>& insert_param_info_schema_paths();

/// @brief Canonical field paths for one entry of insert_slot_info_json().
const std::vector<std::string>& insert_slot_info_schema_paths();

}  // namespace sonare::mastering::api
