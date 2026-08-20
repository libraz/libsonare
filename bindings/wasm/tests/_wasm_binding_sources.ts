/**
 * Static reader over the WASM embind binding C++ sources
 * (`src/wasm/bindings/`).
 *
 * Every JS-object field read that feeds a fixed/required config shape should
 * go through one of the validated-read helper families in
 * `wasm/bindings/common/common.h` (`hasProperty`/`objectProperty`,
 * `floatProperty`/`intProperty`/`boolProperty`/`stringProperty` for
 * presence-checked reads with a sane default, `optionalNumber`/`optionalBool`
 * for type-checked optionals, `requireProperty<T>` for mandatory fields, and
 * `requireOrdinalInRange` for enum ordinals). A "bare" read —
 * `something["key"].as<T>()` chained directly on a literal string index, with
 * no intervening call to one of those helpers — bypasses all of that: on a
 * missing field, embind's `.as<T>()` silently coerces (an absent boolean
 * reads as JS `false`, not a thrown error), which is exactly what let a
 * partial `setPodConfig` object turn the realtime voice changer's ISP
 * limiter off.
 *
 * This is deliberately regex-level rather than a real C++ parse, mirroring
 * `bindings/node/tests/_addon_sources.ts`'s reader for the N-API addon.
 */

import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join } from 'node:path';

const BINDINGS_ROOT = new URL('../../../src/wasm/bindings/', import.meta.url).pathname;

export interface WasmBindingSource {
  /** Path relative to `src/wasm/bindings/`, e.g. `analysis/quick.cpp`. */
  file: string;
  text: string;
}

/**
 * A `something["key"].as<T>()` read with a literal string key, not wrapped in
 * one of common.h's validated readers.
 */
export interface BareFieldReadSite {
  file: string;
  key: string;
  /** Receiver the bracket index is chained on, e.g. `opts` or `desc["anchors"]`. */
  receiver: string;
  /** Target type of the `.as<>()` cast, whitespace-collapsed. */
  castType: string;
  /** Normalized source expression, e.g. `opts["nFft"].as<int>()`. */
  expression: string;
  /**
   * 1-based ordinal among reads with an identical {@link expression} in the same
   * file, so two textually identical reads stay two entries.
   */
  occurrence: number;
  /** Source line. Diagnostic only — deliberately NOT part of {@link id}. */
  line: number;
  /**
   * Line-independent identity: `file expression #occurrence`.
   *
   * Keying a ratchet on `file:line` makes it fail on any insertion anywhere in a
   * scanned file, including an unrelated `#include`. The maintainer response to
   * that becomes a reflexive `-u`, and once regenerating is routine the run that
   * carries a genuine new read gets regenerated along with the noise. The
   * invariant worth guarding is the SET of reads, not where they sit, so the
   * identity is built from what the read does rather than from its position.
   *
   * What this cannot tell apart: moving an existing read to a different function
   * in the same file, and deleting a read while adding a textually identical one
   * elsewhere in that file. Neither is a new unbudgeted read, which is the thing
   * being guarded, so both are outside the invariant rather than gaps in it.
   */
  id: string;
}

function walk(dir: string, prefix = ''): string[] {
  const out: string[] = [];
  for (const entry of readdirSync(dir)) {
    const full = join(dir, entry);
    if (statSync(full).isDirectory()) {
      out.push(...walk(full, `${prefix}${entry}/`));
    } else if (entry.endsWith('.cpp') || entry.endsWith('.h')) {
      out.push(`${prefix}${entry}`);
    }
  }
  return out;
}

let cachedSources: WasmBindingSource[] | undefined;

export function wasmBindingSources(): WasmBindingSource[] {
  if (!cachedSources) {
    cachedSources = walk(BINDINGS_ROOT)
      .sort()
      .map((file) => ({ file, text: readFileSync(join(BINDINGS_ROOT, file), 'utf8') }));
  }
  return cachedSources;
}

/**
 * Field names excluded from the scan: `.length` / `.byteLength` reads belong
 * to the separate array-length-validation family
 * (`wasmArrayLikeLength`/`wasmFloat32ArrayLength`/`requireMatchedLength`),
 * not the "named config field" hazard this scanner hunts. common.h/common.cpp
 * are the helpers' own home and are excluded entirely (see below), not just
 * by key name.
 */
const EXCLUDED_KEYS = new Set(['length', 'byteLength']);

/**
 * A bracket string-literal index immediately followed by `.as<...>()`, together
 * with the receiver it is chained on. The receiver may itself be a chain of
 * literal indices (`desc["anchors"]["length"]`), so it is matched greedily
 * rather than as a bare identifier — otherwise such a read would silently drop
 * out of the scan.
 */
const BARE_FIELD_READ =
  /([A-Za-z_]\w*(?:\["[A-Za-z0-9_]+"\])*)\["([A-Za-z0-9_]+)"\]\s*\.\s*as\s*<([^>]+)>\s*\(\s*\)/g;

/** Collapses the internal whitespace of a matched C++ type to one space. */
function normalizeType(castType: string): string {
  return castType.trim().replace(/\s+/g, ' ');
}

/**
 * Scans `src/wasm/bindings/` for bare literal-keyed reads, keeping those whose
 * key membership in {@link EXCLUDED_KEYS} matches @p wantExcludedKeys.
 *
 * `common/common.{h,cpp}` is skipped entirely: it is the validated readers' own
 * implementation, which legitimately indexes by literal key.
 */
function scanBareReads(wantExcludedKeys: boolean): BareFieldReadSite[] {
  const sites: BareFieldReadSite[] = [];
  for (const { file, text } of wasmBindingSources()) {
    if (file.startsWith('common/common.')) {
      continue;
    }
    const seen = new Map<string, number>();
    for (const match of text.matchAll(BARE_FIELD_READ)) {
      const [, receiver, key, castType] = match;
      if (EXCLUDED_KEYS.has(key) !== wantExcludedKeys) {
        continue;
      }
      const expression = `${receiver}["${key}"].as<${normalizeType(castType)}>()`;
      const occurrence = (seen.get(expression) ?? 0) + 1;
      seen.set(expression, occurrence);
      const start = match.index ?? 0;
      sites.push({
        file,
        key,
        receiver,
        castType: normalizeType(castType),
        expression,
        occurrence,
        line: text.slice(0, start).split('\n').length,
        id: `${file} ${expression} #${occurrence}`,
      });
    }
  }
  return sites;
}

/**
 * Every bare literal-keyed field read across `src/wasm/bindings/`, excluding
 * `common/common.{h,cpp}` and the `.length`/`.byteLength` family.
 */
export function bareFieldReadSites(): BareFieldReadSite[] {
  return scanBareReads(false);
}

/**
 * The `.length` / `.byteLength` family that {@link bareFieldReadSites}
 * deliberately skips. A caller-supplied length decides an allocation size, so
 * reading it with a bare `.as<T>()` narrows a non-finite, fractional, negative,
 * or fabricated JS Number straight into a `reserve`/`resize` — the shared
 * `wasmArrayLikeLength` guard rejects those before anything is allocated.
 */
export function bareArrayLengthReadSites(): BareFieldReadSite[] {
  return scanBareReads(true);
}

/** A `RealtimeEngineWasm::*Offline` member definition and how it renders. */
export interface OfflineRenderWrapperSite {
  file: string;
  /** Member name, e.g. `bounceOffline`. */
  name: string;
  line: number;
  /** Whether the body reaches the core's validated offline entry point. */
  rendersThroughCore: boolean;
}

/** A `RealtimeEngineWasm::` member DEFINITION whose name ends in `Offline`. */
const OFFLINE_WRAPPER_DEFINITION = /\bRealtimeEngineWasm::(\w*Offline)\s*\(/g;

/** The core entry point that owns the prepared-channel precondition. */
const CORE_OFFLINE_RENDER = 'engine_.render_offline(';

/**
 * Every offline-render entry point on the WASM engine facade, with whether its
 * body reaches {@link CORE_OFFLINE_RENDER}.
 *
 * The prepared-channel precondition lives in `RealtimeEngine::render_offline`,
 * not in these wrappers: a hand-rolled `engine_.process()` loop silences every
 * plane past `prepared_channels()` and reports only through telemetry, which an
 * offline caller reads as a completed render. Routing through the core entry
 * point is what makes the check impossible to forget; re-implementing the loop
 * here is how the same gap reopened across successive audits.
 *
 * Bodies are sliced from the definition to the next column-0 `}`, which holds
 * for this tree's formatting (clang-format, no nested column-0 braces).
 */
export function offlineRenderWrapperSites(): OfflineRenderWrapperSite[] {
  const sites: OfflineRenderWrapperSite[] = [];
  for (const { file, text } of wasmBindingSources()) {
    for (const match of text.matchAll(OFFLINE_WRAPPER_DEFINITION)) {
      const start = match.index ?? 0;
      const bodyStart = text.indexOf('{', start);
      if (bodyStart < 0) {
        continue;
      }
      const bodyEnd = text.indexOf('\n}', bodyStart);
      const body = text.slice(bodyStart, bodyEnd < 0 ? text.length : bodyEnd);
      sites.push({
        file,
        name: match[1],
        line: text.slice(0, start).split('\n').length,
        rendersThroughCore: body.includes(CORE_OFFLINE_RENDER),
      });
    }
  }
  return sites;
}
