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

/** A function DEFINITION whose parameter list has the shape of a key reader. */
export interface ReaderShapedSite {
  file: string;
  name: string;
  line: number;
  /** `file:name` — stable across line moves, so allowlists do not rot. */
  id: string;
}

/** The one file a key reader is allowed to live in. */
export const SHARED_READER_FILE = 'sonare_wrap_options.h';

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
 * Top-level config keys the streaming-chain addon skips before flattening.
 *
 * `StreamingMasteringChain`'s TypeScript constructor validates its config leaf
 * types through the shared flattener and must skip the same keys, or it would
 * reject a valid config. That list is a hand-copy of a C++ literal this package
 * cannot import, so read the C++ back instead of trusting the copy. Regex-level
 * like the rest of this module; the caller asserts the result is non-empty so a
 * regex that stops matching fails loudly rather than comparing nothing.
 */
export function streamingSkippedConfigKeys(): string[] {
  const source = addonSources().find(({ file }) => file === 'sonare_wrap_streaming.cpp');
  if (!source) {
    return [];
  }
  const guard = /prefix\.empty\(\)\s*&&\s*\(([^)]*)\)/.exec(source.text);
  if (!guard) {
    return [];
  }
  return [...guard[1].matchAll(/key\s*==\s*"([^"]+)"/g)].map((match) => match[1]).sort();
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
 * The sanctioned readers. The two families from `sonare_wrap_options.h`
 * (including `MidiByteProperty`, the byte-width member of the `*Property`
 * family), plus `OptionAt` — mixing.cpp's per-strip scalar-or-array accessor,
 * which the shared families do not model and which type-checks at each call
 * site.
 *
 * This is a list of NAMES, so on its own it can only see readers it already
 * knows about — which is how a file-local copy under a fresh name once made a
 * 25-key entry point read as taking no options at all. {@link
 * readerShapedDefinitions} is the half that closes it: it finds reader
 * DEFINITIONS by their parameter shape, so a copy cannot exist under any name,
 * and the accompanying test also pins this list against the shared header so
 * the two cannot drift.
 */
const OPTION_READER =
  /\b(?:node_(?:int|float|double|bool|int64|string)_option|(?:Int|Int64|Uint32|Word|Float|Double|Bool|MidiByte)Property|OptionAt)\s*\(/;

/** Matches a reader call and captures its literal key, for either arity. */
const OPTION_READER_KEY =
  /(?:node_(?:int|float|double|bool|int64|string)_option|(?:Int|Int64|Uint32|Word|Float|Double|Bool|MidiByte)Property|OptionAt)\s*\(\s*(?:env\s*,\s*)?[\w.>-]+\s*,\s*"([A-Za-z0-9_]+)"/g;

/**
 * A definition that READS A KEY OFF A JS OBJECT, recognised by its parameter
 * list rather than by its name: an optional leading `Napi::Env`, then a
 * `Napi::Object`, then a string-ish key. That is what every member of both
 * shared families looks like, and it is what a file-local copy of one has to
 * look like too — which is the point. Deliberately loose about `const` /
 * reference spelling and about `char*` vs `std::string` so a copy cannot slip
 * through by writing its parameters differently.
 */
const READER_SHAPED_DEFINITION =
  /^[A-Za-z_][\w:<>&*,\s]*?\b(\w+)\s*\(\s*(?:(?:const\s+)?Napi::Env\s*&?\s*\w+\s*,\s*)?(?:const\s+)?Napi::Object\s*&?\s*\w+\s*,\s*(?:const\s+)?(?:char\s*\*|std::string\s*&?|std::string_view\s*&?)\s*\w+\s*[,)]/gm;

/**
 * Every reader-shaped definition across the addon sources.
 *
 * The convention says a key reader lives in {@link SHARED_READER_FILE} and
 * nowhere else. A name-based scan can never enforce that — it only recognises
 * the names it was told about — so this recognises the *shape* instead, and the
 * test requires every hit outside the shared header to carry a written reason.
 */
export function readerShapedDefinitions(): ReaderShapedSite[] {
  const sites: ReaderShapedSite[] = [];
  for (const { file, text } of addonSources()) {
    for (const match of text.matchAll(READER_SHAPED_DEFINITION)) {
      const start = match.index ?? 0;
      sites.push({
        file,
        name: match[1],
        line: text.slice(0, start).split('\n').length,
        id: `${file}:${match[1]}`,
      });
    }
  }
  return sites;
}

/** Whether {@link OPTION_READER} recognises a call to @p name. */
export function isSanctionedReaderName(name: string): boolean {
  return new RegExp(OPTION_READER.source).test(`${name}(`);
}

/**
 * A definition that READS A POSITIONAL ARGUMENT off a Napi::CallbackInfo,
 * recognised by its parameter list rather than by its name: an optional leading
 * `Napi::Env`, then a `Napi::CallbackInfo`, then an integer index. That is what
 * every member of the shared positional family looks like, and it is what a
 * file-local copy of one has to look like too.
 */
const POSITIONAL_READER_DEFINITION =
  /^[A-Za-z_][\w:<>&*,\s]*?\b(\w+)\s*\(\s*(?:(?:const\s+)?Napi::Env\s*&?\s*\w+\s*,\s*)?(?:const\s+)?Napi::CallbackInfo\s*&\s*\w+\s*,\s*(?:const\s+)?(?:size_t|std::size_t|uint32_t|int)\s+\w+\s*[,)]/gm;

/**
 * The shared readers that REJECT a positional argument: each returns false
 * without touching its out-parameter, refuses to throw while an exception is
 * already pending, and so lets its caller bail out before any C-ABI call.
 *
 * The lenient `node_arg_*` family is otherwise absent. Those type-check and fall
 * back to a default instead of throwing, so they leave no pending exception and
 * cannot reach the C ABI with a dummy value alongside one. `node_arg_int_no_wrap`
 * is the exception and belongs here: it keeps that family's type-checked
 * fallback but refuses a number the narrowing would wrap, so it can leave an
 * exception pending like the rest of this list. Omitting it would drop its
 * callers out of {@link positionalArgEntryPoints} — they would stop being
 * covered while the table that names them still read as green.
 */
const BAILOUT_READER =
  /\b(?:Optional(?:Int|Uint32|Int64|Float|Double|Bool|String|MidiByte)Arg|Required(?:Int|Int64)Arg|NonNegativeSizeTArg|Int32Arg|node_arg_int_no_wrap)\s*\(/;

/**
 * Every positional-reader definition across the addon sources.
 *
 * The positional counterpart of {@link readerShapedDefinitions}: a reader lives
 * in {@link SHARED_READER_FILE} and nowhere else, and the test requires every
 * hit outside it to carry a written reason. A file-local copy is how the
 * bail-out contract went missing four times over — the copies read the argument
 * with an unchecked `info[i].As<Napi::Number>()`, which under
 * NAPI_DISABLE_CPP_EXCEPTIONS yields a dummy value next to a pending exception.
 */
export function positionalReaderDefinitions(): ReaderShapedSite[] {
  const sites: ReaderShapedSite[] = [];
  for (const { file, text } of addonSources()) {
    for (const match of text.matchAll(POSITIONAL_READER_DEFINITION)) {
      const start = match.index ?? 0;
      sites.push({
        file,
        name: match[1],
        line: text.slice(0, start).split('\n').length,
        id: `${file}:${match[1]}`,
      });
    }
  }
  return sites;
}

/**
 * Whether the source text immediately preceding a bail-out reader call puts it
 * in a bail-out position — `if (!Reader(...)`, or a `||` / `&&` continuation of
 * one, with an optional namespace qualifier in between.
 *
 * Exported so a test can drive it with a synthetic guarded and unguarded
 * spelling; a predicate that answered "guarded" to everything would report a
 * clean sweep of the whole addon while checking nothing.
 */
export function isBailoutGuarded(precedingText: string): boolean {
  return /(?:if\s*\(|\|\||&&)\s*!\s*(?:\w+::)*$/.test(precedingText);
}

/**
 * Every call to a bail-out reader, with whether its false return is consumed as
 * a bail-out.
 *
 * The canonical form is `if (!Reader(...)) return ...;`, optionally chained with
 * `||`. Anything else means the false return is being ignored, which puts the
 * addon back in the shape this whole family exists to prevent: the argument is
 * rejected, an exception is pending, and the C-ABI call runs anyway.
 *
 * {@link SHARED_READER_FILE} is skipped because that is where the family is
 * DEFINED; a definition looks exactly like an unguarded call to a regex. The
 * caller asserts the returned list is non-empty, so a regex that stops matching
 * fails loudly instead of reporting a clean sweep of nothing.
 */
export function bailoutReaderCalls(): Array<ReaderShapedSite & { guarded: boolean }> {
  const sites: Array<ReaderShapedSite & { guarded: boolean }> = [];
  const call = new RegExp(BAILOUT_READER.source, 'g');
  for (const { file, text } of addonSources()) {
    if (file === SHARED_READER_FILE) {
      continue;
    }
    for (const match of text.matchAll(call)) {
      const start = match.index ?? 0;
      const before = text.slice(0, start);
      const name = match[0].replace(/\s*\($/, '');
      sites.push({
        file,
        name,
        line: before.split('\n').length,
        id: `${file}:${name}`,
        guarded: isBailoutGuarded(before),
      });
    }
  }
  return sites;
}

/**
 * Blank out `//` and block comments, keeping every other byte at its offset so
 * line numbers still resolve. Prose that DESCRIBES a broken read — and the
 * shared header's own doc comment quotes one verbatim — must not itself read as
 * a broken read.
 */
function withoutComments(text: string): string {
  return text
    .replace(/\/\*[\s\S]*?\*\//g, (m) => m.replace(/[^\n]/g, ' '))
    .replace(/\/\/[^\n]*/g, (m) => ' '.repeat(m.length));
}

/** The explicit value accessors an `As<Napi::X>()` can be followed by. */
const VALUE_ACCESSOR =
  '(?:Uint32Value|Int32Value|Int64Value|FloatValue|DoubleValue|Utf8Value|Value)';

/**
 * An inline typed read of a positional argument, in either of its two forms.
 *
 * The **explicit** form is `info[i].As<Napi::X>()` followed by a value
 * accessor. The accessor is the part that matters — it is what fails on a type
 * mismatch, leaving a pending exception and a dummy value. A bare
 * `.As<Napi::Object>()` with no accessor cannot fail and is not matched.
 *
 * The **implicit** form is `info[i].As<Napi::Number>()` with no accessor at
 * all, consumed straight into a numeric parameter. `Napi::Number` carries
 * conversion operators (`operator float`, `operator double`, `operator
 * int32_t`, `operator uint32_t`, `operator int64_t`), so the conversion still
 * runs and can still fail — the accessor call is simply written by the
 * compiler instead of by the author. This scanner's comment used to assert
 * that an accessor-less `.As<>()` "cannot fail", which is true of
 * `Napi::Object` and false of `Napi::Number`, and the regex was built on that
 * premise; an addon-wide sweep found 15 reads of the implicit form that no
 * scan had ever looked at. The exclusion is therefore per type, not blanket.
 */
const INLINE_TYPED_READ = new RegExp(
  '\\binfo\\s*\\[\\s*([A-Za-z0-9_]+)\\s*\\]\\s*\\.\\s*As\\s*<\\s*Napi::(?:' +
    // Any Napi type, when an explicit accessor follows.
    `\\w+\\s*>\\s*\\(\\s*\\)\\s*\\.\\s*${VALUE_ACCESSOR}\\s*\\(` +
    '|' +
    // Napi::Number only, when none does: the conversion is implicit.
    `Number\\s*>\\s*\\(\\s*\\)\\s*(?!\\s*\\.\\s*${VALUE_ACCESSOR})` +
    ')',
  'g',
);

/**
 * Inline typed positional reads with NO type check on that same index anywhere
 * in the enclosing function.
 *
 * This is the scan that closes the gap the other three leave open. They are all
 * anchored on the shared reader family — by its definition shape, by its call
 * sites, or by which entry points call it — so all three are blind to an entry
 * point that never uses the family at all and just writes
 * `sonare_x(h, info[0].As<Napi::Number>().Uint32Value())` inline. That is the
 * exact form `Uint32Arg` and `NumberArg` were originally factored out of, and
 * an addon-wide sweep found 26 live instances of it.
 *
 * A read IS accepted (`typeChecked`) when the same body type-checks the index
 * (`info[1].IsNumber()`, `IsString()`, ...), because the accessor then cannot
 * fail. `IsUndefined()` and `IsNull()` deliberately do not count: presence is
 * not type, and every defect in this class was presence-checked and type-blind.
 *
 * Every match is returned, `typeChecked` or not, so the caller can assert the
 * population is large before asserting the violation subset is empty. Returning
 * only violations would make a dead regex read as a clean sweep.
 */
export function inlineTypedArgumentReads(): Array<ReaderShapedSite & { typeChecked: boolean }> {
  const sites: Array<ReaderShapedSite & { typeChecked: boolean }> = [];
  const definition = /^[A-Za-z_][\w:<>&*,\s]*?\b((?:\w+::)?\w+)\s*\([^;]*?\)\s*\{$/gm;
  for (const { file, text: raw } of addonSources()) {
    const text = withoutComments(raw);
    const defs = [...text.matchAll(definition)];
    for (const match of text.matchAll(INLINE_TYPED_READ)) {
      const at = match.index ?? 0;
      let index = -1;
      for (let i = 0; i < defs.length; i++) {
        if ((defs[i].index ?? 0) < at) {
          index = i;
        }
      }
      const start = index >= 0 ? (defs[index].index ?? 0) : 0;
      const end = index + 1 < defs.length ? (defs[index + 1].index ?? text.length) : text.length;
      const typeChecked = new RegExp(
        `info\\s*\\[\\s*${match[1]}\\s*\\]\\s*\\.\\s*Is(?!Undefined|Null)\\w+\\s*\\(`,
      ).test(text.slice(start, end));
      const name = index >= 0 ? defs[index][1] : file;
      sites.push({
        file,
        name,
        line: text.slice(0, at).split('\n').length,
        id: `${file}:${name}`,
        typeChecked,
      });
    }
  }
  return sites;
}

/**
 * The JS names of every entry point that can REJECT a positional argument —
 * those whose body, or a helper it calls, goes through a bail-out reader.
 *
 * This is the population the abort-guard table has to account for. Deriving it
 * from the sources rather than listing it is the point: four generations of
 * fixes each enumerated the entry points that existed at the time, and each time
 * a new same-shaped one appeared that no table had ever named.
 */
export function positionalArgEntryPoints(): string[] {
  const bodies = functionBodies();
  const reads = new Set<string>();
  for (const [name, body] of bodies) {
    if (new RegExp(BAILOUT_READER.source).test(body)) {
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
  return addonEntryPoints()
    .filter((entry) => {
      const bare = entry.symbol.includes('::') ? entry.symbol.split('::')[1] : entry.symbol;
      return reads.has(entry.symbol) || reads.has(bare);
    })
    .map((entry) => entry.jsName)
    .sort();
}

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

/** A `.As<Napi::Number>().<width>Value()` conversion of a caller's number. */
export interface NarrowingSite {
  file: string;
  line: number;
  /** `Int32Value`, `Uint32Value` or `Int64Value`. */
  accessor: string;
  /** The expression the conversion is taken on. */
  receiver: string;
  /** `file:receiver:accessor` — stable across line moves, so allowlists do not rot. */
  id: string;
}

/**
 * The N-API accessors that WRAP. `DoubleValue` and `FloatValue` are absent on
 * purpose: neither is a modular conversion, so neither can turn a caller's
 * number into a different legal one the way ToInt32 does.
 */
const WRAPPING_ACCESSOR =
  /\.\s*As<Napi::Number>\(\)\s*\.\s*(Int32Value|Uint32Value|Int64Value)\s*\(\s*\)/g;

/**
 * Every integer narrowing of a caller's number, across the addon sources.
 *
 * `Int32Value()` is ToInt32 and therefore WRAPS: `2**32` arrives as 0, which is
 * what most of these fields read as "keep the default", and `2**32 + 1` as 1.
 * A wrapped value is always inside the target type, so nothing downstream can
 * tell it from a setting the caller chose — which is why the conversion has to
 * live in one place that range-checks it, rather than being spelled out per
 * site. The shared header is that place, and it is the only file where a hit
 * here is expected.
 *
 * Takes its sources so the self-tests can drive this exact function rather than
 * a re-implementation of it, which would only ever agree with itself.
 */
export function integerNarrowingSites(sources: AddonSource[] = addonSources()): NarrowingSite[] {
  const sites: NarrowingSite[] = [];
  for (const { file, text } of sources) {
    const code = withoutComments(text);
    for (const match of code.matchAll(new RegExp(WRAPPING_ACCESSOR.source, 'g'))) {
      const at = match.index ?? 0;
      const before = code.slice(0, at);
      const receiver = (/([\w[\]().>"'-]+)$/.exec(before)?.[1] ?? '').slice(-60);
      sites.push({
        file,
        line: before.split('\n').length,
        accessor: match[1],
        receiver,
        id: `${file}:${receiver}:${match[1]}`,
      });
    }
  }
  return sites;
}

/** A registered entry point and whether its body carries the catch harness. */
export interface EntryPointGuardSite {
  jsName: string;
  symbol: string;
  file: string;
  guarded: boolean;
}

/** Function bodies keyed by their bare name, located by brace balance. */
function bodiesByName(sources: AddonSource[]): Map<string, { file: string; body: string }> {
  const found = new Map<string, { file: string; body: string }>();
  for (const { file, text } of sources) {
    const code = withoutComments(text);
    for (const match of code.matchAll(
      // `}` is excluded from the parameter list on purpose: without it the
      // class spans a newline and swallows the previous function's closing
      // brace, so the NEXT declaration is read as that macro's parameters and
      // disappears from the population.
      /^[\w:<>,&*\s]*?\b(?:[\w]+::)?(\w+)\s*\(([^;{}]*?)\)\s*(?:const\s*)?\{/gm,
    )) {
      if (!match[2].includes('CallbackInfo')) {
        continue;
      }
      const open = code.indexOf('{', (match.index ?? 0) + match[0].length - 1);
      let depth = 0;
      for (let i = open; i < code.length; i++) {
        if (code[i] === '{') {
          depth++;
        } else if (code[i] === '}') {
          depth--;
          if (depth === 0) {
            found.set(match[1], { file, body: code.slice(open, i + 1) });
            break;
          }
        }
      }
    }
  }
  return found;
}

/**
 * Every registered entry point, with whether its body can catch.
 *
 * A reader that refuses a value does it by THROWING, because the alternative —
 * leaving a pending JS exception and returning a fallback — lets the entry point
 * keep working, and every N-API allocation after a pending exception returns
 * null for the result builders to write into. So an entry point without the
 * harness turns a refusal into an uncaught exception and takes the process with
 * it: the harness is the precondition that makes the refusal reportable.
 *
 * Takes its sources so the self-tests can drive this exact function.
 */
export function entryPointGuards(sources: AddonSource[] = addonSources()): EntryPointGuardSite[] {
  const bodies = bodiesByName(sources);
  const out: EntryPointGuardSite[] = [];
  for (const entry of addonEntryPointsFrom(sources)) {
    const bare = entry.symbol.includes('::') ? entry.symbol.split('::')[1] : entry.symbol;
    const found = bodies.get(bare);
    if (!found) {
      continue;
    }
    out.push({
      jsName: entry.jsName,
      symbol: entry.symbol,
      file: found.file,
      guarded: found.body.includes('SONARE_NODE_TRY') || /\bcatch\s*\(/.test(found.body),
    });
  }
  return out.sort((a, b) => a.jsName.localeCompare(b.jsName));
}

/** {@link addonEntryPoints} over an explicit source set, for the self-tests. */
function addonEntryPointsFrom(sources: AddonSource[]): { jsName: string; symbol: string }[] {
  const found = new Map<string, string>();
  for (const { text } of sources) {
    const flat = text.replace(/\s+/g, ' ');
    for (const m of flat.matchAll(
      /(?:Instance|Static)Method<&([\w:]+)>\s*\(\s*"([A-Za-z0-9_]+)"/g,
    )) {
      found.set(m[2], m[1]);
    }
    for (const m of flat.matchAll(
      /exports\.Set\(\s*"([A-Za-z0-9_]+)"\s*,\s*Napi::Function::New\(\s*env\s*,\s*&?([\w:]+)/g,
    )) {
      found.set(m[1], m[2]);
    }
  }
  return [...found].map(([jsName, symbol]) => ({ jsName, symbol }));
}

/** One failure class, with the lines that made it fire. */
export interface NarrowingFinding {
  heading: string;
  lines: string[];
}

/**
 * Every narrowing-scope failure class, as data.
 *
 * Separated from the assertions so the self-tests can revert one class at a
 * time and require exactly that class to fire: a class asserted through a
 * re-implementation of this function would only ever agree with itself.
 */
export function evaluateNarrowingScope(
  sources: AddonSource[] = addonSources(),
  allowlist: ReadonlyMap<string, string> = new Map(),
  floor = { sources: 40, entryPoints: 400 },
): NarrowingFinding[] {
  const findings: NarrowingFinding[] = [];
  const narrowings = integerNarrowingSites(sources);
  const guards = entryPointGuards(sources);

  // Two empty sets agree perfectly: with either scan matching nothing, every
  // check below passes and certifies a scanner that has stopped working.
  const shrunk: string[] = [];
  if (sources.length < floor.sources) {
    shrunk.push(`sources: found ${sources.length}, floor is ${floor.sources}`);
  }
  if (guards.length < floor.entryPoints) {
    shrunk.push(`entry points: found ${guards.length}, floor is ${floor.entryPoints}`);
  }
  if (shrunk.length > 0) {
    findings.push({
      heading: 'The scan no longer finds the population it is sized for',
      lines: shrunk,
    });
  }

  const stray = narrowings
    .filter((site) => site.file !== SHARED_READER_FILE)
    .filter((site) => !allowlist.has(site.id));
  if (stray.length > 0) {
    findings.push({
      heading:
        'These integer narrowings sit outside the shared header, so a value the target type ' +
        'cannot hold arrives as a different legal one',
      lines: stray.map((site) => `${site.file}:${site.line} ${site.receiver}.${site.accessor}()`),
    });
  }

  const unguarded = guards.filter((entry) => !entry.guarded);
  if (unguarded.length > 0) {
    findings.push({
      heading:
        'These registered entry points have nowhere to catch a refusal, so a reader that ' +
        'throws escapes the N-API callback and terminates the process',
      lines: unguarded.map((entry) => `${entry.file} ${entry.symbol} (${entry.jsName})`),
    });
  }

  const live = new Set(narrowings.map((site) => site.id));
  const stale = [...allowlist.keys()].filter((id) => !live.has(id));
  if (stale.length > 0) {
    findings.push({
      heading:
        'These allowlist entries matched nothing. One that suppresses nothing still asserts a ' +
        'reviewed decision about a spelling, so the next narrowing to take it inherits the ' +
        'blessing unexamined',
      lines: stale,
    });
  }
  return findings;
}
