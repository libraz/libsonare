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

import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import {
  bareArrayLengthReadSites,
  bareFieldReadSites,
  integerNarrowingSites,
  localIntReaderSites,
  offlineRenderWrapperSites,
  valReaderFunctions,
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
    // A floor sized to the population this scan is meant to shrink would stop
    // checking the scanner and start checking that remediation did not
    // happen; this one only needs to stay comfortably above zero.
    expect(sites.length).toBeGreaterThan(50);
    for (const site of sites) {
      expect(site.id).toContain(site.expression);
      expect(site.id).not.toContain(`:${site.line}`);
    }
    const ids = sites.map((s) => s.id);
    expect(new Set(ids).size, 'site ids must be unique').toBe(ids.length);

    // A file that reads the same expression twice must keep two entries.
    const repeated = bareArrayLengthReadSites().filter(
      (s) => s.file === 'realtime/transport.cpp' && s.expression.startsWith('segments['),
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

describe('every int64 scratch scalar is normalized before it is declared a number', () => {
  // `protocol.ts` states the convention: embind can hand an int64 scalar to JS
  // as a BigInt, so a facade accessor declaring `: number` has to coerce. The
  // failure is invisible until a second consumer does arithmetic on the value
  // and the AudioWorklet dies with "Cannot mix BigInt", which is why this is a
  // source register rather than a behavioural test — the shipped build's one
  // consumer happens to re-normalize downstream.
  function scratchScalarAccessors(): { name: string; body: string }[] {
    const source = readFileSync(
      new URL('../src/realtime_engine.ts', import.meta.url).pathname,
      'utf8',
    );
    const pattern =
      /^ {2}(\w*(?:RenderFrame|TimelineSample))\(\): number \{\n(?:[^\n]*\/\/[^\n]*\n)*\s*return ([^\n]*);/gm;
    return [...source.matchAll(pattern)].map((match) => ({
      name: match[1],
      body: match[2].trim(),
    }));
  }

  it('self-checks the scanner against the known scratch accessors', () => {
    expect(scratchScalarAccessors().map((accessor) => accessor.name)).toEqual(
      expect.arrayContaining([
        'externalMidiScratchRenderFrame',
        'meterScratchRenderFrame',
        'scopeScratchRenderFrame',
        'telemetryScratchRenderFrame',
      ]),
    );
  });

  it('no accessor returns the raw native int64 scalar', () => {
    const unnormalized = scratchScalarAccessors()
      .filter((accessor) => !accessor.body.startsWith('Number('))
      .map((accessor) => `realtime_engine.ts ${accessor.name} -> ${accessor.body}`);
    expect(
      unnormalized,
      'These accessors declare `: number` but hand back whatever embind produced. ' +
        'Wrap the native call in Number(...), the way the sibling scratch accessors do.',
    ).toEqual([]);
  });
});

describe('WASM inherits the C ABI feature gate on mixing-only engine commands', () => {
  /**
   * Every `CommandType::k...` push site in `text`, with whether it sits inside
   * a `SONARE_WITH_MIXING` conditional (either polarity — the C ABI writes
   * `#if !defined(...)` with the real work in the `#else`).
   */
  function commandPushSites(text: string): { type: string; gated: boolean }[] {
    const sites: { type: string; gated: boolean }[] = [];
    const stack: boolean[] = [];
    for (const line of text.split('\n')) {
      if (/^\s*#\s*(if|ifdef|ifndef)\b/.test(line)) {
        stack.push(line.includes('SONARE_WITH_MIXING'));
      } else if (/^\s*#\s*endif\b/.test(line)) {
        stack.pop();
      }
      const match = line.match(/CommandType::(k\w+)/);
      if (match) {
        sites.push({ type: match[1], gated: stack.some(Boolean) });
      }
    }
    return sites;
  }

  const cAbi = readFileSync(
    new URL('../../../src/c_api/sonare_c_engine.cpp', import.meta.url).pathname,
    'utf8',
  );
  // The oracle decides which commands are mixing-only; this list is read out of
  // it rather than restated here, so a command that gains or loses the gate on
  // the C side changes what WASM is held to without anyone editing this file.
  const mixingOnly = new Set(
    commandPushSites(cAbi)
      .filter((site) => site.gated)
      .map((site) => site.type),
  );

  it('self-checks that the C ABI still gates at least one engine command', () => {
    // Without this the assertion below passes vacuously as soon as the scan
    // stops recognizing the C ABI's conditionals.
    expect([...mixingOnly].sort()).toEqual(['kSetSoloMute', 'kSetTrackMonitorMode']);
  });

  it('no WASM wrapper queues a mixing-only command in an analysis-only build', () => {
    const ungated: string[] = [];
    for (const source of wasmBindingSources()) {
      for (const site of commandPushSites(source.text)) {
        if (mixingOnly.has(site.type) && !site.gated) {
          ungated.push(`${source.file} ${site.type}`);
        }
      }
    }
    expect(
      ungated,
      'These push a command the C ABI answers with NOT_SUPPORTED when mixing is ' +
        'compiled out. Ungated, the analysis-only bundle accepts the call, reports ' +
        "success, and drops it on the engine's unknown-target telemetry. Wrap the " +
        'body in #if defined(SONARE_WITH_MIXING) and throw NotImplemented in the #else.',
    ).toEqual([]);
  });
});

/**
 * The Node addon forbids DEFINING a positional reader outside its shared header
 * and has a test that says so. The WASM surface had the same rule in spirit and
 * nothing enforcing it, which is how a second options-bag integer reader came to
 * live in `effects/repair.cpp` and serve 15 fields: fixing the shared
 * `intProperty` could not reach it, and an acceptance sampling `intProperty`
 * fields could not see it.
 */
/**
 * SCOPE, because this guard's name could be read as more than it checks: it
 * matches functions whose SIGNATURE is that of a reader -- an integer-returning
 * function taking a `val` -- which is the shape `repairIntOption` had and the
 * one the shared reader exists to replace. It does NOT match every inline
 * `as<int>()`. A return-type-agnostic scan finds 14 such sites, most of them
 * array-element reads and enum-ordinal converters rather than options-bag size
 * fields; triaging that population is a separate question from this one.
 *
 * File-local integer narrowing that may stay, with the measured reason.
 *
 * `automationCurveFromVal` narrows an ENUM ORDINAL rather than a size or a
 * count, and the ordinal it produces is range-checked against `[0, SCurve]` by
 * the C ABI it feeds (`src/c_api/project_edit_track.cpp:64`), which rejects a
 * saturated INT_MAX with InvalidParameter. So it matches the shape and is not
 * the defect: unlike a count, an out-of-range ordinal has nowhere plausible to
 * land. Keyed `file:function`.
 */
const LOCAL_INT_READER_ALLOWLIST: ReadonlyMap<string, string> = new Map<string, string>();

describe('WASM options-bag reader functions narrow only through the shared reader', () => {
  it('self-checks the scanner against the reader it was written for', () => {
    // Vacuity guard: this assertion is a set difference, so a scanner that
    // stopped matching would report a clean sweep. Pin that it still recognises
    // a delegating reader as acceptable and still sees the files at all.
    expect(wasmBindingSources().length).toBeGreaterThan(20);
    const repair = wasmBindingSources().find((s) => s.file === 'effects/repair.cpp');
    expect(repair, 'effects/repair.cpp is in the scanned set').toBeDefined();
    expect(repair?.text).toContain('checkedIntFromVal');
  });

  it('defines no file-local options-bag integer reader outside common.cpp', () => {
    expect(
      localIntReaderSites()
        .filter((site) => !LOCAL_INT_READER_ALLOWLIST.has(`${site.file}:${site.name}`))
        .map((site) => `${site.file}:${site.line} ${site.name}(...)`),
      'A function that narrows an embind val to an integer must call ' +
        'checkedIntFromVal from wasm/bindings/common/common.h. A file-local copy uses ' +
        'val::as<int>() directly, which SATURATES: 2^31 and 4294967295 both arrive as ' +
        'INT_MAX and pass every downstream check that only asks for a positive value. ' +
        'That is not hypothetical - repairIntOption did exactly this for 15 fields.',
    ).toEqual([]);
  });

  it('keeps the allowlist free of entries whose site no longer exists', () => {
    // An entry that outlives its site keeps asserting a reviewed decision, so
    // the next reader to take that name inherits the blessing unexamined.
    const live = new Set(localIntReaderSites().map((site) => `${site.file}:${site.name}`));
    expect([...LOCAL_INT_READER_ALLOWLIST.keys()].filter((id) => !live.has(id))).toEqual([]);
  });
});

describe('the two narrowing scans agree, and disagreeing is the failure', () => {
  // A scanner cannot detect its own blind spot by any amount of care in writing
  // it: a regex sweeping this tree has now been wrong three times, and each was
  // found by something outside the regex disagreeing with its count, never by
  // re-reading it. So two scans run over the same tree by different routes -
  // scan A top-down over `val`-taking DEFINITIONS, scan B bottom-up over
  // `.as<IntType>()` EXPRESSIONS - and their disagreement is asserted rather
  // than logged.
  //
  // What this does NOT cross-check: both scans classify a cast type through the
  // one shared list, so a type spelled outside it is invisible to both and the
  // agreement stays green. That is the known common mode, stated here rather
  // than papered over.

  it('self-checks that scan A still sees a tree to scan', () => {
    // The agreement checks below are set differences, and two empty sets agree
    // perfectly, so something has to pin that the scans still match. This counts
    // `val`-taking DEFINITIONS, which remediation does not remove -- a floor on
    // them stays true however many of their narrowings get fixed.
    //
    // Scan B has no floor here on purpose. A count of narrowings falls every
    // time one is remediated, so a floor on it borrows its calibration from the
    // defects still in the tree and tightens toward failure as they are fixed.
    // Scan B's non-vacuity is the fixture below, which goes red when the scanner
    // returns nothing, and `tools`-side the population floor in
    // `tests/conformance/check_wasm_narrowing_scope.py`, which records why its
    // numbers are what they are.
    expect(valReaderFunctions().length).toBeGreaterThan(20);
  });

  it('sees both receiver shapes, on a fixture this test owns', () => {
    // A narrowing chained on a plain `val` variable rather than a literal-keyed
    // index is invisible to the bracket-keyed scan, and a count drawn from that
    // scan alone is an undercount by an amount nobody can name. So the
    // classifier has to be shown to distinguish the two shapes.
    //
    // The evidence is a synthetic source rather than a count of the real tree's
    // sites. Borrowing it from the tree makes the check weaker every time one of
    // those reads is remediated and vacuous once they are all gone -- a guard
    // calibrated on defects rewards leaving them in place. This fixture holds
    // one read of each shape whatever the tree looks like.
    const fixture = [
      {
        file: 'fixture/receiver_shapes.cpp',
        text: [
          'int readBoth(val options) {',
          '  const int keyed = options["nFft"].as<int>();',
          '  val local = options["hopLength"];',
          '  const int chained = local.as<int>();',
          '  return keyed + chained;',
          '}',
        ].join('\n'),
      },
    ];
    const sites = integerNarrowingSites(fixture);
    expect(sites.map((s) => s.receiverShape)).toEqual(['bracket-literal-key', 'other']);
    // Both sites must also resolve to the enclosing definition, or the shapes
    // above were classified by a scan that had lost track of its spans.
    expect(sites.map((s) => s.container)).toEqual(['readBoth', 'readBoth']);
  });

  it('classifies every real site into one of the two shapes', () => {
    // What the real tree is still asked for: that the classification RESOLVES,
    // not how many sites of each kind exist. A site the classifier cannot place
    // would be an undercount nobody can name; a shrinking count is remediation
    // and must not read as a regression.
    const shapes = new Set(integerNarrowingSites().map((s) => s.receiverShape));
    for (const shape of shapes) {
      expect(['bracket-literal-key', 'other']).toContain(shape);
    }
  });

  it('every narrowing expression sits inside a definition scan A found', () => {
    // The direction that catches a definition pattern too narrow to match what
    // it should. Written without a qualified-name alternative, scan A misses
    // every `Type::member` definition and 76 of this tree's narrowings come
    // back with no container - which is the shape of the defect, visible here
    // and invisible to anyone reading the pattern.
    expect(
      integerNarrowingSites()
        .filter((site) => site.container === null)
        .map((site) => `${site.file}:${site.line} .as<${site.castType}>()`),
      'An integer narrowing with no enclosing val-taking definition means the ' +
        'two scans disagree about the tree: either the definition pattern is ' +
        'too narrow to match the function holding this read, or the brace ' +
        'matching that measures its body is wrong. Fix the scan, not this list.',
    ).toEqual([]);
  });

  it('the two scans count the same narrowings in every definition', () => {
    // The other direction: scan A counts from a body slice, scan B counts from
    // a whole-file sweep mapped back into spans. They share a type list and
    // nothing else, so a mis-measured body span shows up as a count that does
    // not reconcile.
    const perContainer = new Map<string, number>();
    for (const site of integerNarrowingSites()) {
      if (site.container === null) {
        continue;
      }
      const id = `${site.file}:${site.container}`;
      perContainer.set(id, (perContainer.get(id) ?? 0) + 1);
    }
    const disagreements = valReaderFunctions()
      .map((fn) => {
        const id = `${fn.file}:${fn.name}`;
        const fromB = perContainer.get(id) ?? 0;
        return { id, line: fn.line, fromA: fn.narrowingCount, fromB };
      })
      .filter((row) => row.fromA !== row.fromB)
      .map(
        (row) => `${row.id} (line ${row.line}): scan A saw ${row.fromA}, scan B saw ${row.fromB}`,
      );

    expect(disagreements, 'The declaration scan and the expression scan must reconcile.').toEqual(
      [],
    );
  });
});
