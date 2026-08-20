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
import {
  bareArrayLengthReadSites,
  bareFieldReadSites,
  offlineRenderWrapperSites,
  wasmBindingSources,
} from './_wasm_binding_sources';

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

  it('identifies a read by what it does, not by where it sits', () => {
    // The snapshots below are keyed on this id. If it ever picked up a line
    // number again, every unrelated insertion into a scanned file would turn
    // them red, and regenerating would become routine enough to wave a genuine
    // new read through. Two guarantees make the key usable as an identity:
    // it carries the source expression, and it stays unique when the same
    // expression appears twice in one file.
    const sites = [...bareFieldReadSites(), ...bareArrayLengthReadSites()];
    expect(sites.length).toBeGreaterThan(100);
    for (const site of sites) {
      expect(site.id).toContain(site.expression);
      expect(site.id).not.toContain(`:${site.line}`);
    }
    const ids = sites.map((s) => s.id);
    expect(new Set(ids).size, 'site ids must be unique').toBe(ids.length);

    // A file that reads the same expression twice must keep two entries.
    const repeated = bareArrayLengthReadSites().filter(
      (s) => s.file === 'project/project_bounce.cpp' && s.expression.startsWith('bindings['),
    );
    expect(repeated.length).toBeGreaterThan(1);
    expect(repeated.map((s) => s.occurrence)).toEqual(repeated.map((_, index) => index + 1));
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
      .map((s) => s.id)
      .sort();
    expect(sites).toMatchSnapshot();
  });
});

describe('WASM binding sources budget every caller-supplied array length', () => {
  it('the entry points that size an allocation from `.length` use the shared guard', () => {
    // A fabricated, negative, fractional, or non-finite `.length` used to reach
    // a reserve/resize directly here. These read through wasmArrayLikeLength,
    // which rejects all of those as an InvalidParameter SonareError before any
    // allocation happens.
    const guarded = ['features/core.cpp', 'metering/metering.cpp', 'realtime/params.cpp'];
    const sites = bareArrayLengthReadSites().filter((s) => guarded.includes(s.file));
    expect(sites).toEqual([]);
  });

  it('has no new bare `.length` / `.byteLength` reads beyond the tracked baseline', () => {
    // Same ratchet rationale as the named-field scan above: the remaining sites
    // are pre-existing and each needs its own review, but the list must not
    // grow silently — an unbudgeted length is how an untrusted JS number turns
    // into an unbounded allocation.
    const sites = bareArrayLengthReadSites()
      .map((s) => s.id)
      .sort();
    expect(sites).toMatchSnapshot();
  });
});

describe('every WASM offline render reaches the core entry point that validates it', () => {
  it('self-checks the scanner against the three shipped offline wrappers', () => {
    // Without this the assertion below passes vacuously the moment the
    // definition regex stops matching (a rename, a macro, a reformat).
    const names = offlineRenderWrapperSites()
      .map((s) => s.name)
      .sort();
    expect(names).toEqual(
      expect.arrayContaining(['bounceOffline', 'freezeOffline', 'renderOffline']),
    );
  });

  it('no offline wrapper renders without calling the core render_offline', () => {
    // This is the check that keeps the seam closed rather than re-closing it.
    // The prepared-channel precondition is enforced once, inside
    // RealtimeEngine::render_offline; a wrapper that drives its own
    // engine_.process() loop instead silences every plane past
    // prepared_channels() and reports only through telemetry, so an offline
    // caller reads a completed render of pure silence — which is exactly what
    // renderOffline / bounceOffline / freezeOffline used to do here.
    const unrouted = offlineRenderWrapperSites().filter((s) => !s.rendersThroughCore);
    const detail = unrouted.map((s) => `${s.file}:${s.line} RealtimeEngineWasm::${s.name}`);
    expect(
      detail,
      'These offline wrappers never call engine_.render_offline(...). Render through it ' +
        'instead of driving your own engine_.process() loop, and do not hand-copy the ' +
        'prepared-channel / not-prepared guards into the wrapper: the core entry point ' +
        'already raises SonareException(InvalidParameter) for a request wider than ' +
        'prepared_channels(), and a copy here is what drifts away from it. If a new ' +
        'wrapper genuinely cannot use it, push the precondition down into the core ' +
        'alongside the existing one rather than adding a fourth per-site guard.',
    ).toEqual([]);
  });
});
