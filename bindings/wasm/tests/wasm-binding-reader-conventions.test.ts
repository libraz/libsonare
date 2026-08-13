/**
 * Mechanical enforcement of the WASM binding's validated JS-object-field
 * reader convention (the C++ side of the input-validation consolidation:
 * `wasm/bindings/common/common.h`'s `requireProperty<T>` /
 * `requireOrdinalInRange` / `hasProperty`+friends).
 *
 * The 2026-08-02 audit found 11 findings from unknown enum names,
 * out-of-range ordinals, and partial config objects being silently defaulted
 * instead of rejected, and the remediation consolidated validation into
 * `wasm/bindings/common/`. New code and unrepaired code then bypassed that
 * consolidated path, and this audit found 8 more — most visibly,
 * `realtimeVoiceChangerConfigFromPodVal` reading every flat POD field with a
 * bare `pod[key].as<T>()`, so a partial object silently zero-filled missing
 * fields (a missing `limiterEnableIspLimiter` read as JS `false`, turning the
 * ISP limiter off).
 *
 * A full C++ parse to prove every reader goes through the shared helpers
 * isn't practical here, so this is a ratchet instead: `src/wasm/bindings/`
 * already has 100+ pre-existing bare `["key"].as<T>()` reads that are out of
 * this task's scope to individually re-review (each would need its own
 * judgment call about whether embind's default coercion on a missing field
 * is actually safe there). Snapshotting the full current list means any
 * DELTA — a new bare read added anywhere, including a ninth silently
 * defaulting instance of the exact pattern this task fixed — changes the
 * snapshot file, which is a visible diff in review and fails CI until
 * acknowledged. This does not retroactively grade the pre-existing sites;
 * it only stops the count from growing unnoticed.
 */

import { describe, expect, it } from 'vitest';
import { bareFieldReadSites, wasmBindingSources } from './_wasm_binding_sources';

describe('WASM binding sources stay on the shared common.h field readers', () => {
  it('self-checks the source scanner against a known source file', () => {
    // If this regex stops matching, the snapshot test below would pass
    // vacuously (an empty site list always "matches" a shrinking snapshot),
    // so pin a baseline that must keep resolving.
    const sources = wasmBindingSources();
    expect(sources.length).toBeGreaterThan(20);
    const quick = sources.find((s) => s.file === 'analysis/quick.cpp');
    expect(quick, 'analysis/quick.cpp should be part of the scanned tree').toBeDefined();
    expect(quick?.text).toContain('modesFromVal');
  });

  it('the realtime voice changer POD reader has no bare field reads (regression check)', () => {
    // The specific site this task fixed: every SONARE_WASM_VC_POD_FIELDS
    // entry and limiterEnableIspLimiter now read through requireProperty<T>,
    // not a bare pod[key].as<T>(). This is checked directly (not just via
    // the snapshot below) because it is the highest-priority named finding —
    // a partial object could silently turn off the ISP limiter and let the
    // DAC clip.
    const sites = bareFieldReadSites().filter(
      (s) => s.file === 'effects/realtime_voice_changer.cpp',
    );
    expect(sites).toEqual([]);
  });

  it('has no new bare literal-key field reads beyond the tracked baseline (snapshot ratchet)', () => {
    const sites = bareFieldReadSites()
      .map((s) => `${s.file}:${s.line} ["${s.key}"]`)
      .sort();
    expect(sites).toMatchSnapshot();
  });
});
