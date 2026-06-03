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
  virtual int latency_samples(const PluginDescriptor& descriptor) const noexcept {
    (void)descriptor;
    return 0;
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

  /// Same PDC query as InstrumentProvider::latency_samples. Default 0.
  virtual int latency_samples(const PluginDescriptor& descriptor) const noexcept {
    (void)descriptor;
    return 0;
  }
};
#endif  // SONARE_WITH_GRAPH

}  // namespace sonare::host
