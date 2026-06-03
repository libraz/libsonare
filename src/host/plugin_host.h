#pragma once

/// @file plugin_host.h
/// @brief Host instrument / plugin provider seam: an abstract factory that
///        yields plugin INSTANCES expressed as core types
///        (rt::ProcessorBase for inserts/effects, midi::MidiInstrument for
///        MIDI-driven instruments) from a plain descriptor. Header-only.
///
/// Scope and invariants
/// --------------------
///  - A plugin instance is a core rt::ProcessorBase (an effect/insert) or a
///    midi::MidiInstrument (an instrument that IS-A rt::ProcessorBase + a MIDI
///    event sink). It is NEVER an SDK object (VST3 / AU / CLAP). The real SDK
///    bridge implements this factory out-of-tree / behind a build option and
///    wraps each SDK plugin in a core ProcessorBase adapter; core only sees the
///    abstract interfaces below. This header includes NO plugin SDK headers
///    (invariant 6).
///  - The instrument the factory returns plugs straight into the host-instrument wiring
///    point: RealtimeEngine::set_midi_instrument(midi::MidiInstrument*). The
///    engine sums its audio at the clip/source-merge stage and the sequencer
///    dispatches MIDI events to it.
///  - Header-only: abstract interfaces + a POD descriptor; no .cpp, no lib.
///
/// PDC / latency
/// -------------
/// Every provider surfaces the instance's reported latency in samples so the
/// arrangement compiler can fold it into the CompiledTimeline PDC / latency
/// summary. The instance itself already
/// reports latency via rt::ProcessorBase::latency_samples(); the provider seam
/// ALSO exposes latency_samples(descriptor) as a control-thread query so the
/// compiler can compute PDC WITHOUT instantiating (and preparing) the plugin —
/// the same number the engine returns from midi_instrument_latency_samples().

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "midi/instrument.h"
#include "rt/processor_base.h"

namespace sonare::host {

/// Which core interface a plugin instance is delivered as.
enum class PluginKind : uint8_t {
  /// An audio insert / effect: an rt::ProcessorBase. Returned by
  /// InstrumentProvider::create_effect / make_processor.
  kEffect = 0,
  /// A MIDI-driven instrument: a midi::MidiInstrument (IS-A rt::ProcessorBase +
  /// midi::MidiEventSink). Returned by InstrumentProvider::create_instrument.
  kInstrument = 1,
};

/// Display/automation unit for a plugin parameter. Plain enum: no SDK unit id.
enum class PluginParameterUnit : uint8_t {
  kGeneric = 0,
  kNormalized = 1,
  kDecibels = 2,
  kHertz = 3,
  kMilliseconds = 4,
  kPercent = 5,
  kBoolean = 6,
  kSemitones = 7,
};

/// One automatable/plugin parameter exposed by a provider. Control-thread value
/// data used for UI, automation binding and RT-safety filtering before an
/// instance is created. `id` is the value passed to ProcessorBase::set_parameter.
struct PluginParameterDescriptor {
  uint32_t id = 0;
  std::string name;
  PluginParameterUnit unit = PluginParameterUnit::kGeneric;
  float default_value = 0.0f;
  float min_value = 0.0f;
  float max_value = 1.0f;
  bool automatable = true;
  bool realtime_safe = true;

  bool operator==(const PluginParameterDescriptor& o) const noexcept {
    return id == o.id && name == o.name && unit == o.unit && default_value == o.default_value &&
           min_value == o.min_value && max_value == o.max_value && automatable == o.automatable &&
           realtime_safe == o.realtime_safe;
  }
};

/// Bus role exposed by a plugin. Plain metadata only; actual buffers remain
/// core float channel arrays at render time.
enum class PluginBusRole : uint8_t {
  kMainInput = 0,
  kMainOutput = 1,
  kSidechainInput = 2,
  kAuxOutput = 3,
};

/// One audio bus layout option/request for a descriptor. `index` is per-role
/// ordering (e.g. sidechain 0). Channel counts are inclusive; `default_channels`
/// is the provider's preferred host selection inside the range.
struct PluginBusDescriptor {
  PluginBusRole role = PluginBusRole::kMainOutput;
  uint32_t index = 0;
  std::string name;
  uint32_t min_channels = 0;
  uint32_t max_channels = 0;
  uint32_t default_channels = 0;
  bool required = true;

  bool operator==(const PluginBusDescriptor& o) const noexcept {
    return role == o.role && index == o.index && name == o.name && min_channels == o.min_channels &&
           max_channels == o.max_channels && default_channels == o.default_channels &&
           required == o.required;
  }
};

/// One factory/user preset a plugin descriptor exposes. Names/indices only — no
/// SDK preset object, no payload bytes (invariant 6). `index` is the provider's
/// stable preset ordinal a host passes back to select it via a program-change
/// or the plugin's parameter/state path; `name` is for display. The actual
/// preset DATA (when a host wants to persist it) travels as the opaque state
/// blob of rt::ProcessorBase::save_state / create_instrument_with_state — never
/// through this descriptor.
struct PluginPresetDescriptor {
  uint32_t index = 0;
  std::string name;

  bool operator==(const PluginPresetDescriptor& o) const noexcept {
    return index == o.index && name == o.name;
  }
};

/// A plugin/instrument identity the host hands the provider to instantiate one.
/// Plain value data — no SDK handle, no path resolution here. `id` is a
/// host-stable opaque identifier (e.g. "builtin.sine", a VST3 class id string);
/// `name` is for display; `format` names the bridge ("builtin" / "vst3" / "au"
/// / "clap" / ...) the out-of-tree provider understands; `kind` says which
/// interface to deliver.
struct PluginDescriptor {
  std::string id;
  std::string name;
  std::string format;
  PluginKind kind = PluginKind::kInstrument;

  bool operator==(const PluginDescriptor& o) const noexcept {
    return id == o.id && name == o.name && format == o.format && kind == o.kind;
  }
};

/// The provider seam: an abstract FACTORY a host implements (out-of-tree) to
/// turn a PluginDescriptor into a core instance. Lives on the CONTROL thread;
/// instantiation MAY allocate. Returned instances are core types the engine
/// already renders/feeds — never SDK objects.
class InstrumentProvider {
 public:
  virtual ~InstrumentProvider() = default;

  /// True if this provider can instantiate `descriptor` (right format / id).
  virtual bool can_create(const PluginDescriptor& descriptor) const noexcept = 0;

  /// CONTROL thread: instantiate a MIDI-driven instrument for `descriptor`.
  /// Returns nullptr if the descriptor is not an instrument this provider
  /// handles. The returned node plugs into
  /// RealtimeEngine::set_midi_instrument(...). May allocate.
  virtual std::unique_ptr<midi::MidiInstrument> create_instrument(
      const PluginDescriptor& descriptor) = 0;

  /// CONTROL thread: instantiate a MIDI-driven instrument for `descriptor` and
  /// rehydrate it from a previously-saved opaque state blob (see
  /// rt::ProcessorBase::save_state) so a hosted external instrument reloads with
  /// its session preset/parameters rather than at default state. A null/empty
  /// `state` span yields a default instance (identical to create_instrument).
  /// Distinct name (not an overload of create_instrument) so a provider that
  /// overrides only create_instrument does not hide this entry point. Default:
  /// delegate to create_instrument(descriptor) then load_state, which is correct
  /// for providers that do not need a specialized construction path.
  virtual std::unique_ptr<midi::MidiInstrument> create_instrument_with_state(
      const PluginDescriptor& descriptor, const uint8_t* state, size_t len) {
    std::unique_ptr<midi::MidiInstrument> instance = create_instrument(descriptor);
    if (instance && state != nullptr && len > 0) {
      instance->load_state(state, len);
    }
    return instance;
  }

  /// CONTROL thread: instantiate an audio effect / insert for `descriptor`.
  /// Returns nullptr if the descriptor is not an effect this provider handles.
  /// May allocate. Default: no effect support (instrument-only providers).
  virtual std::unique_ptr<rt::ProcessorBase> create_effect(const PluginDescriptor& descriptor) {
    (void)descriptor;
    return nullptr;
  }

  /// CONTROL thread: report the instance's latency in samples WITHOUT
  /// instantiating it, so the compiler can compute the CompiledTimeline PDC /
  /// latency summary up front. Must equal the latency the created instance
  /// reports from rt::ProcessorBase::latency_samples(). Default 0 (no latency).
  ///
  /// Best-effort contract: this is the provider's pre-instantiation ESTIMATE.
  /// A provider that only knows the true figure after instantiating/preparing
  /// the plugin returns its best estimate here; the authoritative value is the
  /// instance's rt::ProcessorBase::latency_samples() once created. A provider
  /// that cannot estimate returns 0 ("unknown / no latency"), in which case PDC
  /// is computed from the instantiated instance instead. The compiler treats a
  /// later mismatch as a re-latency event, not a contract violation.
  virtual int latency_samples(const PluginDescriptor& descriptor) const noexcept {
    (void)descriptor;
    return 0;
  }

  /// CONTROL thread: Q8 fixed-point latency (samples << 8) WITHOUT instantiating,
  /// matching the instance's rt::ProcessorBase::latency_samples_q8() so the
  /// engine's Q8 PDC (graph_latency_samples_q8, rt fractional delay) keeps
  /// fractional latency instead of rounding to whole samples. Default derives
  /// from latency_samples() (integer floor << 8); providers whose plugins report
  /// fractional latency override this to thread the exact Q8 value through PDC.
  virtual int latency_samples_q8(const PluginDescriptor& descriptor) const noexcept {
    return latency_samples(descriptor) << 8;
  }

  /// CONTROL thread: per-output-port Q8 latency WITHOUT instantiating, matching
  /// the instance's rt::ProcessorBase::output_latency_samples_q8(port). For
  /// multi-out instruments whose ports do not share a single latency (e.g. a
  /// look-ahead main out vs. a zero-latency aux out), this resolves PDC
  /// per-port. `output_port` indexes the descriptor's kMainOutput/kAuxOutput
  /// buses in bus_descriptor() order. Default: every port shares
  /// latency_samples_q8() (the common single-latency case).
  virtual int output_latency_samples_q8(const PluginDescriptor& descriptor,
                                        int output_port) const noexcept {
    (void)output_port;
    return latency_samples_q8(descriptor);
  }

  /// CONTROL thread: report the processor's tail length in samples without
  /// instantiating it. Must match rt::ProcessorBase::tail_samples() for the
  /// created instance. Default 0 (no audible tail).
  virtual int tail_samples(const PluginDescriptor& descriptor) const noexcept {
    (void)descriptor;
    return 0;
  }

  /// CONTROL thread: whether the created processor can be bypassed through
  /// rt::ProcessorBase::set_bypassed(). Default true because the core container
  /// path provides dry pass-through bypass for ProcessorBase instances.
  virtual bool supports_bypass(const PluginDescriptor& descriptor) const noexcept {
    return can_create(descriptor);
  }

  /// CONTROL thread: number of parameters this descriptor exposes. Default 0.
  virtual size_t parameter_count(const PluginDescriptor& descriptor) const noexcept {
    (void)descriptor;
    return 0;
  }

  /// CONTROL thread: write parameter metadata for `index` into `out`. Returns
  /// false for unsupported descriptors, out-of-range indices, or null `out`.
  virtual bool parameter_descriptor(const PluginDescriptor& descriptor, size_t index,
                                    PluginParameterDescriptor* out) const noexcept {
    (void)descriptor;
    (void)index;
    (void)out;
    return false;
  }

  /// CONTROL thread: number of audio buses exposed for this descriptor.
  /// Instruments commonly expose one main output; effects usually expose a
  /// main input/output pair and may expose sidechain inputs or aux outputs.
  virtual size_t bus_count(const PluginDescriptor& descriptor) const noexcept {
    (void)descriptor;
    return 0;
  }

  /// CONTROL thread: write bus layout metadata for `index` into `out`. Returns
  /// false for unsupported descriptors, out-of-range indices, or null `out`.
  virtual bool bus_descriptor(const PluginDescriptor& descriptor, size_t index,
                              PluginBusDescriptor* out) const noexcept {
    (void)descriptor;
    (void)index;
    (void)out;
    return false;
  }

  /// CONTROL thread: number of factory/user presets this descriptor exposes.
  /// Default 0 (no enumerable presets). Names/indices only — invariant 6 keeps
  /// SDK preset objects and payload bytes out of the seam.
  virtual size_t preset_count(const PluginDescriptor& descriptor) const noexcept {
    (void)descriptor;
    return 0;
  }

  /// CONTROL thread: write preset metadata for `index` into `out`. Returns false
  /// for unsupported descriptors, out-of-range indices, or null `out`. A host
  /// selects the preset by passing its `PluginPresetDescriptor::index` back
  /// through the plugin's program-change / parameter path; the preset's data is
  /// never returned here (it rides the opaque save_state blob).
  virtual bool preset_descriptor(const PluginDescriptor& descriptor, size_t index,
                                 PluginPresetDescriptor* out) const noexcept {
    (void)descriptor;
    (void)index;
    (void)out;
    return false;
  }
};

#if defined(SONARE_WITH_GRAPH)
/// Optional graph-node-factory variant: when the processing graph is compiled
/// in, a provider may also yield a node's processor for direct insertion into a
/// graph::Node. This stays a core rt::ProcessorBase factory — the host builds
/// the graph::Node from it — so no graph/SDK coupling leaks into this seam.
class GraphNodeProvider {
 public:
  virtual ~GraphNodeProvider() = default;

  /// CONTROL thread: make the processor for a graph node hosting `descriptor`.
  /// Returns nullptr if unsupported. The host wraps it in a graph::Node with the
  /// reported port count. May allocate.
  virtual std::unique_ptr<rt::ProcessorBase> make_processor(const PluginDescriptor& descriptor) = 0;

  /// Number of ports the node hosting `descriptor` needs (host sizes the
  /// graph::Node from this). Default 2 (stereo in-place).
  virtual int num_ports(const PluginDescriptor& descriptor) const noexcept {
    (void)descriptor;
    return 2;
  }

  /// Same PDC query as InstrumentProvider::latency_samples (best-effort
  /// estimate; authoritative value is the instance's latency once created).
  /// Default 0.
  virtual int latency_samples(const PluginDescriptor& descriptor) const noexcept {
    (void)descriptor;
    return 0;
  }

  /// Q8 fixed-point latency without instantiating; see
  /// InstrumentProvider::latency_samples_q8. Default derives from
  /// latency_samples() (integer floor << 8).
  virtual int latency_samples_q8(const PluginDescriptor& descriptor) const noexcept {
    return latency_samples(descriptor) << 8;
  }

  /// Per-output-port Q8 latency without instantiating; see
  /// InstrumentProvider::output_latency_samples_q8. `output_port` indexes the
  /// node's ports. Default: every port shares latency_samples_q8().
  virtual int output_latency_samples_q8(const PluginDescriptor& descriptor,
                                        int output_port) const noexcept {
    (void)output_port;
    return latency_samples_q8(descriptor);
  }

  virtual int tail_samples(const PluginDescriptor& descriptor) const noexcept {
    (void)descriptor;
    return 0;
  }

  virtual bool supports_bypass(const PluginDescriptor& descriptor) const noexcept {
    (void)descriptor;
    return true;
  }

  virtual size_t parameter_count(const PluginDescriptor& descriptor) const noexcept {
    (void)descriptor;
    return 0;
  }

  virtual bool parameter_descriptor(const PluginDescriptor& descriptor, size_t index,
                                    PluginParameterDescriptor* out) const noexcept {
    (void)descriptor;
    (void)index;
    (void)out;
    return false;
  }

  virtual size_t bus_count(const PluginDescriptor& descriptor) const noexcept {
    (void)descriptor;
    return 0;
  }

  virtual bool bus_descriptor(const PluginDescriptor& descriptor, size_t index,
                              PluginBusDescriptor* out) const noexcept {
    (void)descriptor;
    (void)index;
    (void)out;
    return false;
  }
};
#endif  // SONARE_WITH_GRAPH

}  // namespace sonare::host
