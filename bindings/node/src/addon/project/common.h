#pragma once

#include <cstdint>

#include "sonare_wrap_options.h"
#include "sonare_wrap_project.h"
#include "sonare_wrap_utils.h"

namespace sonare_node::project {

// Derived from the canonical C macro (via sonare_c.h) so this addon can never
// drift from SONARE_PROJECT_ABI_VERSION. A runtime mismatch means the loaded
// native binary lays out the flat project PODs differently than this addon
// expects (or arrangement support was compiled out -> runtime version 0).
constexpr uint32_t kExpectedProjectAbiVersion = SONARE_PROJECT_ABI_VERSION;

// The positional-argument readers, the property readers and the required-field
// readers all come from sonare_wrap_options.h (namespace sonare_node);
// re-exported here so the project TUs that `using namespace sonare_node::project`
// keep finding them. Nothing in this header may define a reader of its own: the
// bail-out contract (return false without touching `out`, refuse to throw on a
// pending exception) is what keeps a rejected argument from reaching the C ABI
// as a dummy value, and a file-local copy is how that contract went missing.
using sonare_node::BoolProperty;
using sonare_node::DoubleProperty;
using sonare_node::FloatProperty;
using sonare_node::Int32Arg;
using sonare_node::Int64Property;
using sonare_node::IntProperty;
using sonare_node::NonNegativeSizeTArg;
using sonare_node::OptionalDoubleArg;
using sonare_node::OptionalFloatArg;
using sonare_node::OptionalIntArg;
using sonare_node::OptionalMidiByteArg;
using sonare_node::OptionalStringArg;
using sonare_node::OptionalUint32Arg;
using sonare_node::RequiredDoubleProperty;
using sonare_node::RequiredDoubleValue;
using sonare_node::RequiredFloatProperty;
using sonare_node::RequiredIntProperty;
using sonare_node::RequiredStringProperty;
using sonare_node::RequiredUint32Property;
using sonare_node::RequiredUint32Value;
using sonare_node::RequireNumberValue;
using sonare_node::ThrowIfError;
using sonare_node::Uint32Property;

}  // namespace sonare_node::project
