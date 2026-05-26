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

#include "rt/processor_base.h"

namespace sonare::mastering::api {

/// @brief Construct a streaming insert processor by name.
/// @param name Processor name, e.g. "dynamics.compressor", "eq.parametric",
///             "effects.reverb.plate".
/// @param json_params A JSON object string ("{...}"). Empty or "{}" means the
///             processor's defaults. Keys mirror the camelCase param names that
///             named_processor.cpp accepts (e.g. "thresholdDb", "ratio").
/// @return A heap-allocated processor, or nullptr if @p name is not a known
///         block-processor insert.
/// @throws sonare::SonareException (InvalidParameter) only when @p json_params
///         is malformed. Unknown names return nullptr rather than throwing.
std::unique_ptr<sonare::rt::ProcessorBase> make_insert(const std::string& name,
                                                       const std::string& json_params);

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

}  // namespace sonare::mastering::api
