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
 * The text of one addon translation unit, by its path relative to `src/addon/`.
 *
 * A test that needs a single addon file reads it through here rather than
 * spelling the tree's location a second time: `src/addon/` is named once, in
 * {@link ADDON_ROOT}, so a file that moves surfaces as a failed lookup naming
 * the file instead of as a stale path constant elsewhere. Throws rather than
 * returning an empty string, because a caller scanning `''` finds nothing and
 * reads as a clean sweep.
 */
export function addonSourceText(file: string): string {
  const sources = addonSources();
  const found = sources.find((source) => source.file === file);
  if (found === undefined) {
    throw new Error(`no addon source named ${file} among the ${sources.length} under src/addon/`);
  }
  return found.text;
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
  /\b(?:node_(?:int|float|double|bool|int64|string|uint32)_option|(?:Int|Int32|Int64|Uint32|Word|Float|FiniteFloat|Double|Bool|String|MidiByte|NonNegativeSizeT)Property|OptionAt)\s*\(/;

/**
 * Matches a reader call and captures its literal key, for either arity.
 *
 * `StringProperty` carries a lookbehind the other members do not need: this
 * pattern has no leading boundary, so without it `RequiredStringProperty` — a
 * member of the Required* family, whose keys are not options — would read as a
 * `StringProperty` call and put six required field names into the option-key
 * set of the graph entry points.
 */
const OPTION_READER_KEY =
  /(?:node_(?:int|float|double|bool|int64|string|uint32)_option|(?:Int|Int32|Int64|Uint32|Word|Float|FiniteFloat|Double|Bool|MidiByte|NonNegativeSizeT)Property|(?<!Required)StringProperty|OptionAt)\s*\(\s*(?:env\s*,\s*)?[\w.>-]+\s*,\s*"([A-Za-z0-9_]+)"/g;

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
  const named = new Set(
    addonEntryPointRegistrations(addonSources())
      .filter((entry) => {
        const bare = entry.symbol.includes('::') ? entry.symbol.split('::')[1] : entry.symbol;
        return reads.has(entry.symbol) || reads.has(bare);
      })
      .map((entry) => entry.jsName),
  );
  return [...named].sort();
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

/**
 * Every JS-visible addon entry point, with whether it reads an options bag.
 *
 * One jsName can be registered by several symbols — `setConfig` and `destroy`
 * are each on more than one ObjectWrap class — so the options-reading one wins:
 * the coverage register is keyed by jsName, and letting a no-options namesake
 * take the slot would drop a 25-key entry point out of it.
 */
export function addonEntryPoints(): AddonEntryPoint[] {
  const reads = optionReadingFunctions();
  const found = new Map<string, AddonEntryPoint>();
  for (const { jsName, symbol } of addonEntryPointRegistrations(addonSources())) {
    const bare = symbol.includes('::') ? symbol.split('::')[1] : symbol;
    const readsOptionsBag = reads.has(symbol) || reads.has(bare);
    const previous = found.get(jsName);
    if (previous === undefined || (!previous.readsOptionsBag && readsOptionsBag)) {
      found.set(jsName, { jsName, symbol, readsOptionsBag });
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
  /** The function the conversion is written inside, `''` above the first one. */
  owner: string;
  /** `file:receiver:accessor` — stable across line moves, so allowlists do not rot. */
  id: string;
}

/**
 * The family a narrowing is allowed to be written inside.
 *
 * A SYMBOL rather than a filename, and that is the whole of it: the trust
 * belongs to the readers that range-check before converting, not to the file
 * they happen to sit in. A file grows, and the last four predicates added to
 * this one wrote their own comparison next to the family without ever asking to
 * be trusted — they inherited it from the address. Naming the family means a
 * helper added beside it is reported, and an accepted narrowing has to say which
 * reader performed it.
 */
export const SHARED_NARROWING_READER = /^node_narrow_\w+$/;

/** Whether a narrowing's enclosing function IS the shared narrowing family. */
export function isSharedNarrowingReader(owner: string): boolean {
  return SHARED_NARROWING_READER.test(owner);
}

/**
 * The head of a function definition: a return type, a name, a parameter list,
 * then the body's opening brace.
 *
 * `}` is excluded from the parameter list on purpose: without it the class spans
 * a newline and swallows the previous function's closing brace, so the NEXT
 * declaration is read as that macro's parameters and disappears from the
 * population.
 */
const DEFINITION_HEAD = /^[\w:<>,&*\s]*?\b(?:\w+::)?(\w+)\s*\(([^;{}]*?)\)\s*(?:const\s*)?\{/;

/**
 * Heads that open a BLOCK rather than a function body.
 *
 * `if (cond) {` matches {@link DEFINITION_HEAD} exactly — a name, a parenthesised
 * list, a brace — so without this a narrowing inside a branch is owned by `if`
 * rather than by the function the branch is in, and a correctly guarded reader is
 * reported against itself.
 */
const CONTROL_FLOW_HEAD = /^(?:if|for|while|switch|catch)$/;

/** A function definition with the extent of its body. */
interface DefinitionSpan {
  name: string;
  params: string;
  /** Offset of the body's opening brace. */
  open: number;
  /** Offset one past the body's closing brace. */
  end: number;
}

/**
 * Every function definition in @p code, located by brace balance.
 *
 * THE extractor: everything that needs to know which function an offset belongs
 * to comes through here, because an owner that disagreed between two extractors
 * would be the same misattribution one level up. The body's EXTENT is what makes
 * this more than a list of starts — a site written after a body has closed
 * belongs to no function, and crediting it to the definition above it is how a
 * file-scope narrowing silently inherited the shared family's exemption.
 */
function definitionSpans(code: string): DefinitionSpan[] {
  const spans: DefinitionSpan[] = [];
  for (const match of code.matchAll(new RegExp(DEFINITION_HEAD.source, 'gm'))) {
    if (CONTROL_FLOW_HEAD.test(match[1])) {
      continue;
    }
    const open = code.indexOf('{', (match.index ?? 0) + match[0].length - 1);
    if (open < 0) {
      continue;
    }
    let depth = 0;
    for (let i = open; i < code.length; i++) {
      if (code[i] === '{') {
        depth++;
      } else if (code[i] === '}') {
        depth--;
        if (depth === 0) {
          spans.push({ name: match[1], params: match[2], open, end: i + 1 });
          break;
        }
      }
    }
  }
  return spans;
}

/**
 * The innermost body containing @p at, or `''` when the offset is at file scope.
 *
 * Containment, never proximity. `''` is a real answer rather than a fallback: a
 * narrowing at file scope has no owner, so it can match no trusted name and is
 * reported.
 */
function enclosingDefinition(spans: DefinitionSpan[], at: number): string {
  let owner = '';
  let narrowest = Number.POSITIVE_INFINITY;
  for (const span of spans) {
    if (span.open <= at && at < span.end && span.end - span.open < narrowest) {
      owner = span.name;
      narrowest = span.end - span.open;
    }
  }
  return owner;
}

/**
 * The N-API accessors that WRAP. `DoubleValue` and `FloatValue` are absent
 * because neither is a modular conversion — not because neither can change a
 * caller's number. `FloatValue` saturates a double past FLT_MAX to an infinity,
 * which folds the caller's quantity onto another legal value by a non-modular
 * route. That route is closed at the reader rather than here, for the sites that
 * reach one: `node_narrow_float` refuses the overflow and
 * `node_narrow_finite_float` refuses a written non-finite as well. The sites
 * still reading `FloatValue()` inline — required arguments and object-key reads —
 * remain uncovered by this pattern and by those readers alike.
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
 * site. The {@link SHARED_NARROWING_READER} family is that place, and each site
 * carries the function it was written inside so a caller can say so.
 *
 * Takes its sources so the self-tests can drive this exact function rather than
 * a re-implementation of it, which would only ever agree with itself.
 */
export function integerNarrowingSites(sources: AddonSource[] = addonSources()): NarrowingSite[] {
  const sites: NarrowingSite[] = [];
  for (const { file, text } of sources) {
    const code = withoutComments(text);
    const spans = definitionSpans(code);
    for (const match of code.matchAll(new RegExp(WRAPPING_ACCESSOR.source, 'g'))) {
      const at = match.index ?? 0;
      const before = code.slice(0, at);
      const receiver = (/([\w[\]().>"'-]+)$/.exec(before)?.[1] ?? '').slice(-60);
      sites.push({
        file,
        line: before.split('\n').length,
        accessor: match[1],
        receiver,
        owner: enclosingDefinition(spans, at),
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

/** Entry-point bodies keyed by their bare name, from {@link definitionSpans}. */
function bodiesByName(sources: AddonSource[]): Map<string, { file: string; body: string }> {
  const found = new Map<string, { file: string; body: string }>();
  for (const { file, text } of sources) {
    const code = withoutComments(text);
    for (const span of definitionSpans(code)) {
      if (!span.params.includes('CallbackInfo')) {
        continue;
      }
      found.set(span.name, { file, body: code.slice(span.open, span.end) });
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
  const seen = new Set<string>();
  for (const entry of addonEntryPointRegistrations(sources)) {
    const bare = entry.symbol.includes('::') ? entry.symbol.split('::')[1] : entry.symbol;
    const found = bodies.get(bare);
    if (!found || seen.has(`${entry.jsName}:${entry.symbol}`)) {
      continue;
    }
    seen.add(`${entry.jsName}:${entry.symbol}`);
    out.push({
      jsName: entry.jsName,
      symbol: entry.symbol,
      file: found.file,
      guarded: found.body.includes('SONARE_NODE_TRY') || /\bcatch\s*\(/.test(found.body),
    });
  }
  return out.sort((a, b) => a.jsName.localeCompare(b.jsName));
}

/**
 * Every (jsName, symbol) registration in the addon, in all three spellings.
 *
 * THE one matcher. It used to exist twice, once here and once inside
 * {@link addonEntryPoints}, and the population function has been wrong twice —
 * so a correction applied to one copy left the other asserting the old
 * population with nothing comparing them.
 *
 * Deliberately NOT deduplicated: one jsName can be registered by several
 * symbols, and which one a caller wants differs (the coverage register wants the
 * options-reading one; the catch-harness check wants all of them, since a
 * namesake hiding another symbol's missing harness is the failure it exists to
 * find).
 */
function addonEntryPointRegistrations(
  sources: AddonSource[],
): Array<{ jsName: string; symbol: string }> {
  const found: Array<{ jsName: string; symbol: string }> = [];
  for (const { text } of sources) {
    const flat = text.replace(/\s+/g, ' ');
    for (const m of flat.matchAll(
      /(?:Instance|Static)Method<&([\w:]+)>\s*\(\s*"([A-Za-z0-9_]+)"/g,
    )) {
      found.push({ jsName: m[2], symbol: m[1] });
    }
    for (const m of flat.matchAll(
      /exports\.Set\(\s*"([A-Za-z0-9_]+)"\s*,\s*Napi::Function::New\(\s*env\s*,\s*&?([\w:]+)/g,
    )) {
      found.push({ jsName: m[1], symbol: m[2] });
    }
    // An ObjectWrap class reaches JS as DefineClass plus `exports.Set(name,
    // func)`, so neither spelling above names its CONSTRUCTOR — and a
    // constructor is an entry point like any method: a caller's value reaches a
    // reader there too, and a throw it cannot catch aborts the process instead
    // of reporting. Recognised by the definition's shape rather than by the
    // registration, because `Wrap::Wrap(const Napi::CallbackInfo&)` is the one
    // spelling an ObjectWrap subclass cannot avoid. The JS name comes from the
    // DefineClass inside that class's own Init, so a file holding several
    // classes still names each one correctly.
    for (const m of text.matchAll(/\b(\w+)::\1\s*\(\s*const\s+Napi::CallbackInfo/g)) {
      const cls = m[1];
      const init = text.indexOf(`${cls}::Init`);
      const declared =
        init < 0
          ? null
          : text
              .slice(init)
              .replace(/\s+/g, ' ')
              .match(/DefineClass\(\s*env\s*,\s*"([A-Za-z0-9_]+)"/);
      found.push({ jsName: declared ? declared[1] : cls, symbol: `${cls}::${cls}` });
    }
  }
  return found;
}

/** A count of each registration SPELLING, taken without reading a symbol. */
export interface RegistrationCensus {
  /** `InstanceMethod` / `StaticMethod`, in either the template or the argument form. */
  methods: number;
  /** `Napi::Function::New`, in either form. */
  functionNews: number;
  /** `exports.Set(` — each is a function registration or a class export. */
  exportSets: number;
  /** `DefineClass(` — one per ObjectWrap class, and so one per constructor. */
  defineClasses: number;
}

/**
 * Counts each registration spelling by its bare token.
 *
 * Deliberately NOT built on the matcher: this exists to DISAGREE with it, and a
 * census derived from it could only ever agree with itself. The patterns here
 * are coarser on purpose — a token rather than a parsed registration — so a
 * registration whose fuller shape the matcher has stopped reading is still
 * counted here and the two totals part company. Never refactor this onto
 * `addonEntryPointRegistrations`.
 */
export function registrationCensus(sources: AddonSource[] = addonSources()): RegistrationCensus {
  const census: RegistrationCensus = {
    methods: 0,
    functionNews: 0,
    exportSets: 0,
    defineClasses: 0,
  };
  for (const { text } of sources) {
    const code = withoutComments(text);
    census.methods += [...code.matchAll(/(?:Instance|Static)Method\s*[(<]/g)].length;
    census.functionNews += [...code.matchAll(/Napi::Function::New\b/g)].length;
    census.exportSets += [...code.matchAll(/exports\.Set\s*\(/g)].length;
    census.defineClasses += [...code.matchAll(/DefineClass\s*\(/g)].length;
  }
  return census;
}

/**
 * Registration spellings the matcher cannot read.
 *
 * Each reason is about the SPELLING, not about whether the tree uses it today:
 * "not used here" stops being true the moment someone uses it, whereas what a
 * form does to the matcher stays true and is why its count has to be zero. A
 * spelling that appears is invisible to every guard built on the matcher at
 * once — the narrowing sweep, the catch-harness check and the positional-reader
 * table all lose the same entry point in the same silence — so it is named here
 * rather than left to the totals, which would report only that a number moved.
 */
const UNRECOGNISED_REGISTRATION: ReadonlyArray<{
  spelling: string;
  pattern: RegExp;
  why: string;
}> = [
  {
    spelling: 'InstanceMethod("name", &Class::Method)',
    pattern: /(?:Instance|Static)Method\s*\(/g,
    why: 'passes its symbol as an argument instead of as the <&Symbol> template parameter the matcher captures',
  },
  {
    spelling: 'Napi::Function::New<&Fn>(env)',
    pattern: /Napi::Function::New\s*</g,
    why: 'carries no &Fn argument for the matcher to take the symbol from',
  },
  {
    spelling: 'InstanceAccessor / StaticAccessor',
    pattern: /(?:Instance|Static)Accessor\s*[(<]/g,
    why: 'reaches JS as a property whose getter and setter are entry points with no method registration behind them',
  },
  {
    spelling: 'InstanceValue / StaticValue',
    pattern: /(?:Instance|Static)Value\s*\(/g,
    why: 'attaches a value directly, so a function attached this way is an entry point with no registration at all',
  },
  {
    spelling: 'exports.Set(Napi::String::New(env, "name"), ...)',
    pattern: /exports\.Set\s*\(\s*Napi::String/g,
    why: 'builds the export name rather than spelling it as the string literal the matcher reads',
  },
];

/** Where an unrecognised spelling appears, with what it costs the matcher. */
export interface UnrecognisedSpellingSite {
  file: string;
  line: number;
  spelling: string;
  why: string;
}

/** Every appearance of a spelling the matcher cannot read. */
export function unrecognisedRegistrationSpellings(
  sources: AddonSource[] = addonSources(),
): UnrecognisedSpellingSite[] {
  const sites: UnrecognisedSpellingSite[] = [];
  for (const { file, text } of sources) {
    const code = withoutComments(text);
    for (const { spelling, pattern, why } of UNRECOGNISED_REGISTRATION) {
      for (const match of code.matchAll(new RegExp(pattern.source, 'g'))) {
        const at = match.index ?? 0;
        sites.push({ file, line: code.slice(0, at).split('\n').length, spelling, why });
      }
    }
  }
  return sites;
}

/**
 * Every registration-census failure class, as data.
 *
 * Two accounting identities plus the absences. The identities are what keeps the
 * matcher honest: it reads a registration's SHAPE, so a shape it stops reading
 * drops an entry point out of every population at once, while the token counts
 * below still see it. Separated from the assertions for the same reason {@link
 * evaluateNarrowingScope} is.
 */
export function evaluateRegistrationCensus(
  sources: AddonSource[] = addonSources(),
  floor = { registrations: 400 },
): NarrowingFinding[] {
  const findings: NarrowingFinding[] = [];
  const census = registrationCensus(sources);
  const matched = entryPointGuards(sources).length;

  // Two zeroes reconcile perfectly, so pin the size before comparing anything.
  if (matched < floor.registrations) {
    findings.push({
      heading: 'The scan no longer finds the population it is sized for',
      lines: [`registrations: found ${matched}, floor is ${floor.registrations}`],
    });
  }

  if (census.exportSets !== census.functionNews + census.defineClasses) {
    findings.push({
      heading:
        'An exports.Set( is neither a function registration nor a class export, so the addon ' +
        'reaches JS through a form this census does not account for',
      lines: [
        `exports.Set(: ${census.exportSets}`,
        `Napi::Function::New: ${census.functionNews}`,
        `DefineClass(: ${census.defineClasses}`,
      ],
    });
  }

  const counted = census.methods + census.functionNews + census.defineClasses;
  if (counted !== matched) {
    findings.push({
      heading:
        'The registration spellings present and the registrations the matcher reads disagree: a ' +
        'registration is written in a form the matcher cannot see, or the definition behind one ' +
        'it did read cannot be located, or one of these tokens is being used for something that ' +
        'is not a registration',
      lines: [
        `Instance/StaticMethod: ${census.methods}`,
        `Napi::Function::New: ${census.functionNews}`,
        `DefineClass( (one constructor each): ${census.defineClasses}`,
        `counted ${counted}, matcher reads ${matched}`,
      ],
    });
  }

  const unrecognised = unrecognisedRegistrationSpellings(sources);
  if (unrecognised.length > 0) {
    findings.push({
      heading:
        'These registrations are written in a form the matcher cannot read, so the entry points ' +
        'behind them are absent from every guard built on it',
      lines: unrecognised.map((site) => `${site.file}:${site.line} ${site.spelling} — ${site.why}`),
    });
  }
  return findings;
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
    .filter((site) => !isSharedNarrowingReader(site.owner))
    .filter((site) => !allowlist.has(site.id));
  if (stray.length > 0) {
    findings.push({
      heading:
        'These integer narrowings sit outside the shared narrowing family, so a value the target ' +
        'type cannot hold arrives as a different legal one',
      lines: stray.map(
        (site) =>
          `${site.file}:${site.line} ${site.receiver}.${site.accessor}() in ${site.owner || '(file scope)'}`,
      ),
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

/** An integer key read whose fallback is a literal 0 or the sentinel tag. */
export interface ZeroFallbackSite {
  file: string;
  line: number;
  /** The literal JS key the reader was given. */
  key: string;
  /** `node_int_option`, `IntProperty`, ... */
  reader: string;
  /** True when the fallback is spelled `kZeroIsSentinel` rather than `0`. */
  tagged: boolean;
  /** `file:key` — stable across line moves, so the register does not rot. */
  id: string;
}

/**
 * The object-key integer readers whose fallback position can hold either a
 * literal 0 or the sentinel tag.
 *
 * Only four of these carry a `ZeroIsSentinel` overload; the rest are here
 * because their zero poses the same question, and a site that cannot be tagged
 * must still answer it in writing.
 *
 * Positional arguments are deliberately out of scope: their family puts the
 * fallback after an index that is itself usually 0, so one match cannot separate
 * the two, and only `OptionalUint32Arg` carries the overload at all.
 *
 * The key is matched after ONE OR TWO leading arguments, because the family is
 * not uniform about a leading `Napi::Env`. Anchoring on the single-argument
 * spelling made the other arity invisible rather than uncovered: no site, no
 * reason owed, no finding. `0u` is matched for the same reason — a suffix is not
 * a different value, and requiring the bare spelling hid whole files.
 *
 * `MidiByteProperty` is absent on purpose, and the accompanying test records
 * why: it refuses a non-integer outright, so nothing can truncate onto its zero.
 * That is a property of the READER, not of the field, so writing it as a
 * per-site reason would keep blessing `file:key` after the site moved to a
 * truncating reader. Excluding the reader instead makes exactly that move
 * surface as a site owing a reason.
 */
const ZERO_FALLBACK_READER_NAMES = [
  'node_int_option',
  'node_int64_option',
  'IntProperty',
  'Int64Property',
  'Uint32Property',
  'WordProperty',
] as const;

const ZERO_FALLBACK_READER = new RegExp(
  `\\b(${ZERO_FALLBACK_READER_NAMES.join('|')})\\s*\\(\\s*(?:[^,;()]+\\s*,\\s*){1,2}` +
    // `0u` is the same literal zero as `0`; matching only the bare spelling hid
    // a whole file's worth of reads behind a suffix.
    '"([A-Za-z0-9_]+)"\\s*,\\s*(0[uUlL]*|kZeroIsSentinel)\\s*\\)',
  'g',
);

/** Whether {@link ZERO_FALLBACK_READER} recognises a reader named @p name. */
export function isScannedZeroFallbackReader(name: string): boolean {
  return (ZERO_FALLBACK_READER_NAMES as readonly string[]).includes(name);
}

/**
 * Every integer key read whose fallback is a literal 0, plus every one that
 * carries the sentinel tag instead.
 *
 * `kZeroIsSentinel` made "this key's zero selects the library default" a
 * property of the SPELLING rather than a fact living in the callee, which is
 * what lets a scan read it. Both halves are returned so a caller can require the
 * tagged population to be large before requiring the untagged one to be
 * accounted for: with the regex dead, an empty untagged set would read as a
 * clean sweep.
 *
 * Takes its sources so the self-tests can drive this exact function.
 */
export function zeroFallbackSites(sources: AddonSource[] = addonSources()): ZeroFallbackSite[] {
  const sites: ZeroFallbackSite[] = [];
  for (const { file, text } of sources) {
    const code = withoutComments(text);
    for (const match of code.matchAll(new RegExp(ZERO_FALLBACK_READER.source, 'g'))) {
      const at = match.index ?? 0;
      sites.push({
        file,
        line: code.slice(0, at).split('\n').length,
        key: match[2],
        reader: match[1],
        tagged: match[3] === 'kZeroIsSentinel',
        id: `${file}:${match[2]}`,
      });
    }
  }
  return sites;
}

/**
 * Every zero-fallback failure class, as data.
 *
 * Separated from the assertions for the same reason {@link
 * evaluateNarrowingScope} is: a class asserted through a re-implementation of
 * this function would only ever agree with itself.
 */
export function evaluateZeroSentinelScope(
  sources: AddonSource[] = addonSources(),
  reasons: ReadonlyMap<string, string> = new Map(),
  floor = { tagged: 20, sites: 50 },
): NarrowingFinding[] {
  const findings: NarrowingFinding[] = [];
  const sites = zeroFallbackSites(sources);
  const tagged = sites.filter((site) => site.tagged);
  const untagged = sites.filter((site) => !site.tagged);

  // With the regex dead both sets are empty, every check below passes, and the
  // scan certifies itself. Pin both halves: an untagged population alone would
  // go quiet the day someone tags the last site.
  const shrunk: string[] = [];
  if (tagged.length < floor.tagged) {
    shrunk.push(`tagged reads: found ${tagged.length}, floor is ${floor.tagged}`);
  }
  if (sites.length < floor.sites) {
    shrunk.push(`zero-fallback reads: found ${sites.length}, floor is ${floor.sites}`);
  }
  if (shrunk.length > 0) {
    findings.push({
      heading: 'The scan no longer finds the population it is sized for',
      lines: shrunk,
    });
  }

  const unaccounted = untagged.filter((site) => !reasons.has(site.id));
  if (unaccounted.length > 0) {
    findings.push({
      heading:
        'These integer reads fall back to a literal 0 with neither the sentinel tag nor a ' +
        'recorded reason, so nothing says whether their zero is a quantity or a default',
      lines: unaccounted.map((site) => `${site.id} (${site.file}:${site.line} ${site.reader})`),
    });
  }

  const open = new Set(untagged.map((site) => site.id));
  const stale = [...reasons.keys()].filter((id) => !open.has(id));
  if (stale.length > 0) {
    findings.push({
      heading:
        'These recorded reasons matched no untagged read. One that excuses nothing still ' +
        'asserts a reviewed decision about a key, so the next read to take that name inherits ' +
        'the blessing unexamined',
      lines: stale,
    });
  }
  return findings;
}
