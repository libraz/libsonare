/**
 * Static reader over the N-API addon C++ sources.
 *
 * The addon's JS-object reads must all go through the two helper families in
 * `sonare_wrap_options.h`. That rule is only as strong as its enforcement, so
 * these helpers let a test assert it mechanically instead of relying on review.
 *
 * Everything here is deliberately regex-level rather than a real C++ parse: the
 * accompanying test self-checks the scanner against known entry points, so a
 * regex that stops matching fails loudly instead of silently passing.
 */

import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join } from 'node:path';

const ADDON_ROOT = new URL('../src/addon/', import.meta.url).pathname;

export interface AddonSource {
  /** Path relative to `src/addon/`, e.g. `engine/clips_capture.cpp`. */
  file: string;
  text: string;
}

/** An `obj.Has("key")` read that is not paired with an `IsUndefined()` guard. */
export interface BareHasSite {
  file: string;
  key: string;
  line: number;
  /** `file:key` — stable across line moves, so allowlists do not rot. */
  id: string;
}

export interface AddonEntryPoint {
  /** The name the entry point is registered under on the JS side. */
  jsName: string;
  /** `Class::Method` or the free-function name backing the registration. */
  symbol: string;
  /** True when the body, or a helper it calls, reads a JS-object option. */
  readsOptionsBag: boolean;
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

let cachedSources: AddonSource[] | undefined;

export function addonSources(): AddonSource[] {
  if (!cachedSources) {
    cachedSources = walk(ADDON_ROOT)
      .sort()
      .map((file) => ({ file, text: readFileSync(join(ADDON_ROOT, file), 'utf8') }));
  }
  return cachedSources;
}

/**
 * Every `Has("key")` that is not paired with an `IsUndefined()` check on the
 * same key. The paired form already treats an explicit `undefined` like an
 * omitted field, so it is not what this check is hunting.
 */
export function bareHasSites(): BareHasSite[] {
  const sites: BareHasSite[] = [];
  for (const { file, text } of addonSources()) {
    for (const match of text.matchAll(/\.Has\("([A-Za-z0-9_]+)"\)/g)) {
      const key = match[1];
      const start = match.index ?? 0;
      // The guard may sit a few tokens later (`Has(k) && !Get(k).IsUndefined()`)
      // and clang-format may have wrapped the line, so scan a small window.
      const window = text.slice(start, start + 240);
      if (window.includes(`Get("${key}").IsUndefined()`)) {
        continue;
      }
      sites.push({
        file,
        key,
        line: text.slice(0, start).split('\n').length,
        id: `${file}:${key}`,
      });
    }
  }
  return sites;
}

/**
 * The sanctioned readers. The two families from `sonare_wrap_options.h`, plus
 * `OptionAt` — mixing.cpp's per-strip scalar-or-array accessor, which the shared
 * families do not model and which type-checks at each call site.
 */
const OPTION_READER =
  /\b(?:node_(?:int|float|double|bool|int64)_option|(?:Int|Int64|Uint32|Float|Double|Bool)Property|OptionAt)\s*\(/;

/** Matches a reader call and captures its literal key, for either arity. */
const OPTION_READER_KEY =
  /(?:node_(?:int|float|double|bool|int64)_option|(?:Int|Int64|Uint32|Float|Double|Bool)Property|OptionAt)\s*\(\s*(?:env\s*,\s*)?[\w.>-]+\s*,\s*"([A-Za-z0-9_]+)"/g;

/** Function definitions, as `name -> body`, across every addon translation unit. */
function functionBodies(): Map<string, string> {
  const bodies = new Map<string, string>();
  for (const { text } of addonSources()) {
    // A definition starts at column 0 and runs to the next column-0 definition.
    const matches = [
      ...text.matchAll(/^[A-Za-z_][\w:<>&*,\s]*?\b((?:\w+::)?\w+)\s*\([^;]*?\)\s*\{$/gm),
    ];
    for (let i = 0; i < matches.length; i++) {
      const start = matches[i].index ?? 0;
      const end = i + 1 < matches.length ? (matches[i + 1].index ?? text.length) : text.length;
      const name = matches[i][1];
      bodies.set(name, (bodies.get(name) ?? '') + text.slice(start, end));
    }
  }
  return bodies;
}

/**
 * Names of functions that read a JS-object option, including those that only do
 * so through a helper. Resolved to a fixed point so one level of indirection
 * (`SetBuiltinInstrument` -> `ReadEngineBuiltinSynthConfig`) is not missed.
 */
function optionReadingFunctions(): Set<string> {
  const bodies = functionBodies();
  const reads = new Set<string>();
  for (const [name, body] of bodies) {
    if (OPTION_READER.test(body)) {
      reads.add(name);
    }
  }
  for (let pass = 0; pass < 4; pass++) {
    let grew = false;
    for (const [name, body] of bodies) {
      if (reads.has(name)) {
        continue;
      }
      for (const callee of reads) {
        const bare = callee.includes('::') ? callee.split('::')[1] : callee;
        if (new RegExp(`\\b${bare}\\s*\\(`).test(body)) {
          reads.add(name);
          grew = true;
          break;
        }
      }
    }
    if (!grew) {
      break;
    }
  }
  return reads;
}

/**
 * The option keys an entry point reads, taken from the reader calls in its body
 * and in the helpers it calls. Deriving these from source rather than listing
 * them in the test means a newly added option is exercised automatically.
 */
export function optionKeysFor(symbol: string): string[] {
  const bodies = functionBodies();
  const bare = symbol.includes('::') ? symbol.split('::')[1] : symbol;
  const seen = new Set<string>();
  const keys = new Set<string>();
  const visit = (name: string, depth: number) => {
    if (depth > 3 || seen.has(name)) {
      return;
    }
    seen.add(name);
    const body = bodies.get(name) ?? bodies.get(name.split('::')[1] ?? '') ?? '';
    if (body === '') {
      return;
    }
    for (const m of body.matchAll(OPTION_READER_KEY)) {
      keys.add(m[1]);
    }
    for (const [candidate] of bodies) {
      const candidateBare = candidate.includes('::') ? candidate.split('::')[1] : candidate;
      if (candidateBare === name) {
        continue;
      }
      if (new RegExp(`\\b${candidateBare}\\s*\\(`).test(body)) {
        visit(candidate, depth + 1);
      }
    }
  };
  visit(bodies.has(symbol) ? symbol : bare, 0);
  return [...keys].sort();
}

/** Every JS-visible addon entry point, with whether it reads an options bag. */
export function addonEntryPoints(): AddonEntryPoint[] {
  const reads = optionReadingFunctions();
  const found = new Map<string, AddonEntryPoint>();
  const add = (jsName: string, symbol: string) => {
    const bare = symbol.includes('::') ? symbol.split('::')[1] : symbol;
    const readsOptionsBag = reads.has(symbol) || reads.has(bare);
    const previous = found.get(jsName);
    if (previous === undefined || (!previous.readsOptionsBag && readsOptionsBag)) {
      found.set(jsName, { jsName, symbol, readsOptionsBag });
    }
  };
  for (const { text } of addonSources()) {
    const flat = text.replace(/\s+/g, ' ');
    for (const m of flat.matchAll(
      /(?:Instance|Static)Method<&([\w:]+)>\s*\(\s*"([A-Za-z0-9_]+)"/g,
    )) {
      add(m[2], m[1]);
    }
    for (const m of flat.matchAll(
      /exports\.Set\(\s*"([A-Za-z0-9_]+)"\s*,\s*Napi::Function::New\(\s*env\s*,\s*&?([\w:]+)/g,
    )) {
      add(m[1], m[2]);
    }
  }
  return [...found.values()].sort((a, b) => a.jsName.localeCompare(b.jsName));
}
