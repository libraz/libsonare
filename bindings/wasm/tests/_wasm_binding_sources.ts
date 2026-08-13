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
  line: number;
  /** `file:line:key` — stable identity for this call site. */
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

/** A bracket string-literal index immediately followed by `.as<...>()`. */
const BARE_FIELD_READ = /\["([A-Za-z0-9_]+)"\]\s*\.\s*as\s*<[^>]+>\s*\(\s*\)/g;

/**
 * Every bare literal-keyed field read across `src/wasm/bindings/`, excluding
 * `common/common.{h,cpp}` (the validated readers' own implementation, which
 * legitimately indexes by literal key without chaining `.as<>()` directly on
 * the bracket) and the `.length`/`.byteLength` family.
 */
export function bareFieldReadSites(): BareFieldReadSite[] {
  const sites: BareFieldReadSite[] = [];
  for (const { file, text } of wasmBindingSources()) {
    if (file.startsWith('common/common.')) {
      continue;
    }
    for (const match of text.matchAll(BARE_FIELD_READ)) {
      const key = match[1];
      if (EXCLUDED_KEYS.has(key)) {
        continue;
      }
      const start = match.index ?? 0;
      sites.push({
        file,
        key,
        line: text.slice(0, start).split('\n').length,
        id: `${file}:${text.slice(0, start).split('\n').length}:${key}`,
      });
    }
  }
  return sites;
}
