/**
 * Static reader for "which reader family did each surface put this options field
 * in", across the N-API addon (`bindings/node/src/addon/`) and the embind
 * bindings (`src/wasm/bindings/`).
 *
 * THE SHAPE. One documented options key reaches both surfaces, and each surface
 * reads it through a helper with a contract for a PRESENT value of the wrong
 * type. There are three such contracts in the tree and they are all deliberate:
 * `refuse` throws naming the key, `coerce` converts the value and uses it,
 * `substitute` answers with the default. Which one a field takes is a contract
 * decision. That the two surfaces take DIFFERENT ones for the same field is not:
 * the same options bag then produces different audio, or an error on one surface
 * and a result on the other, and both calls look successful from the caller's
 * side. Parity cannot see this -- it compares signatures, defaults and argument
 * order, and both surfaces spell the field identically in all three.
 *
 * IDENTITY IS `entryPoint:key`, NEVER `file:line`. The entry point is the join:
 * a bare key name collides across bags (`threshold` is an onset picking level in
 * one and a declick detector level in another, read through different families
 * on purpose), so joining on the key alone invents mismatches and hides real
 * ones. A line key rots on an unrelated include, regenerating becomes routine,
 * and a genuine new site then rides in with the noise.
 *
 * ENTRY POINTS ARE PAIRED BY NORMALIZED NAME -- `js_detect_onsets` and
 * `DetectOnsets` both reduce to `detectonsets`, `readDereverbConfig` and
 * `ReadDereverbConfig` to `dereverbconfig`. A field whose two surfaces read it
 * in functions that do not normalize to the same name is invisible here, which
 * is the scan's largest blind spot and the reason the floor below is asserted.
 *
 * WHAT THE SCAN CANNOT SEE: a read whose key is not a string literal; a reader
 * whose name follows neither {@link READER_NAME_SHAPE} nor the extras in
 * {@link READER_FAMILIES}, since nothing then forces it to be classified; the
 * Python surface, which has no options bags to disagree over (it takes keyword
 * arguments, so a wrong-typed value is refused by ctypes at the call); and the
 * enclosing-function attribution, which is a brace-free line scan and so reads a
 * lambda body as belonging to the function around it.
 *
 * Regex-level rather than a real C++ parse, mirroring `_addon_sources.ts` and
 * `_sentinel_filter_sources.ts`.
 */

import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join } from 'node:path';

/** The two surfaces this compares, each relative to the repository root. */
export const NODE_TREE = 'bindings/node/src/addon';
export const WASM_TREE = 'src/wasm/bindings';

const REPO_ROOT = new URL('../../../', import.meta.url).pathname;

/** What a surface does with a PRESENT value of the wrong type. */
export type Family = 'refuse' | 'coerce' | 'substitute';

/**
 * Readers that take a key literal but are outside the question this asks -- a
 * required field (no default to substitute), an array reader (the contract is
 * about element type, not scalar coercion), a bare presence probe, or a
 * positional-argument reader that never touches an options bag.
 */
export type OutOfScope = 'out-of-scope';

/**
 * Names the ratchet demands a family for. A reader added under one of these
 * spellings and left out of {@link READER_FAMILIES} is a finding, so growing the
 * helper set forces the contract decision rather than defaulting it.
 */
export const READER_NAME_SHAPE = /(?:Property|Option|OptionValue|_option)$/;

/**
 * Every options-bag reader, per surface, and what it does with a present
 * wrong-typed value. Read off the helper's own body, not off its name.
 *
 * The entries that are NOT name-shaped (`onsetWindowFrames`) are the extras: the
 * shape ratchet cannot demand them, so a reader added under an unconventional
 * name is a blind spot rather than a finding. Prefer the conventional spelling.
 */
export const READER_FAMILIES: Readonly<
  Record<'node' | 'wasm', Readonly<Record<string, Family | OutOfScope>>>
> = {
  node: {
    // Type-checked fallback: a present wrong-typed value reads as unspecified.
    node_bool_option: 'substitute',
    node_double_option: 'substitute',
    node_float_option: 'substitute',
    node_int64_option: 'substitute',
    node_int_option: 'substitute',
    node_string_option: 'substitute',
    node_uint32_option: 'substitute',
    // Presence + type checked: undefined/null takes the default, anything else
    // of the wrong type is refused by name (node_require_property_type).
    BoolProperty: 'refuse',
    DoubleProperty: 'refuse',
    FiniteFloatProperty: 'refuse',
    FloatProperty: 'refuse',
    Int32Property: 'refuse',
    Int64Property: 'refuse',
    IntProperty: 'refuse',
    NonNegativeSizeTProperty: 'refuse',
    MidiByteProperty: 'refuse',
    StringProperty: 'refuse',
    SynthEnumProperty: 'refuse',
    Uint32Property: 'refuse',
    WordProperty: 'refuse',
    // Out of scope, each for its own reason.
    FloatArrayProperty: 'out-of-scope',
    NodeFloatArrayOption: 'out-of-scope',
    RequiredDoubleProperty: 'out-of-scope',
    RequiredFloatProperty: 'out-of-scope',
    RequiredIntProperty: 'out-of-scope',
    RequiredStringProperty: 'out-of-scope',
    RequiredUint32Property: 'out-of-scope',
    parse_frame_option: 'out-of-scope',
  },
  wasm: {
    // Presence-checked only: the value is read through val::as<T>(), which
    // COERCES -- a numeric string and a one-element array arrive as the number
    // they spell, a boolean as 0 or 1.
    boolProperty: 'coerce',
    byteProperty: 'coerce',
    doubleProperty: 'coerce',
    floatProperty: 'coerce',
    int64Property: 'coerce',
    intProperty: 'coerce',
    setNumberOption: 'coerce',
    stringProperty: 'coerce',
    uintProperty: 'coerce',
    wordProperty: 'coerce',
    onsetWindowFrames: 'coerce', // extra: wraps intProperty, then bounds it
    // Type-checked first, so a wrong type is refused rather than converted.
    enumProperty: 'refuse',
    floatOption: 'refuse',
    typedFloatProperty: 'refuse',
    // Type-checked fallback, the mirror of the addon's node_*_option family.
    repairBoolOption: 'substitute',
    repairFloatOption: 'substitute',
    repairIntOption: 'substitute',
    // Out of scope, each for its own reason.
    hasProperty: 'out-of-scope',
    objectProperty: 'out-of-scope',
    optionAt: 'out-of-scope',
    repairOptionValue: 'out-of-scope',
    requireNumberProperty: 'out-of-scope',
  },
};

export interface ReaderSource {
  /** Path relative to the repository root. */
  file: string;
  text: string;
}

export interface ReadSite {
  surface: 'node' | 'wasm';
  file: string;
  /** Enclosing function as written, e.g. `js_detect_onsets`. */
  fn: string;
  /** Normalized {@link fn}, the cross-surface join key. */
  entryPoint: string;
  /** The JS key as spelled in the source, e.g. `delta`. */
  key: string;
  reader: string;
  family: Family;
  /** Source line. Diagnostic only -- deliberately NOT part of {@link id}. */
  line: number;
  /** Field-level identity: `entryPoint:key`. */
  id: string;
}

function walk(dir: string, prefix: string): string[] {
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

const cachedSources = new Map<string, ReaderSource[]>();

/** Every `.cpp` / `.h` under @p tree, read once. */
export function readerSources(tree: string): ReaderSource[] {
  const cached = cachedSources.get(tree);
  if (cached) {
    return cached;
  }
  const root = join(REPO_ROOT, tree);
  const sources = walk(root, '')
    .sort()
    .map((file) => ({ file: `${tree}/${file}`, text: readFileSync(join(root, file), 'utf8') }));
  cachedSources.set(tree, sources);
  return sources;
}

/**
 * A function definition's opening line: a return type, a name, and an unclosed
 * argument list. Deliberately loose -- attribution only has to be stable enough
 * for the same entry point to normalize the same way on both surfaces.
 */
const DEFINITION = /^[A-Za-z_][\w:<>,\s*&]*?\b([A-Za-z_]\w*)\s*\([^;]*$/;

/** `Reader(bag, "key", …)`, with an optional receiver argument before the bag. */
const READ = /\b([A-Za-z_]\w*)\s*\((?:[^()"\n]*?,\s*)?"([A-Za-z_]\w*)"\s*[,)]/g;

/** A line that is a comment or a preprocessor directive rather than code. */
const NOT_CODE = /^\s*(?:\/\/|\*|#)/;

/**
 * Strips the per-surface spelling off an entry-point name, so the addon's
 * `DetectOnsets` and embind's `js_detect_onsets` join. `Read`/`read` comes off
 * because a config reader is named `ReadDereverbConfig` on one surface and
 * `readDereverbConfig` on the other.
 */
export function normalizeEntryPoint(fn: string): string {
  return fn
    .replace(/^(?:js_|Read|read)/, '')
    .replace(/_/g, '')
    .toLowerCase();
}

const treeOf = (surface: 'node' | 'wasm'): string => (surface === 'node' ? NODE_TREE : WASM_TREE);

/** Every classified options-bag read in @p surface's tree. */
export function readSites(
  surface: 'node' | 'wasm',
  sources: readonly ReaderSource[] = readerSources(treeOf(surface)),
): ReadSite[] {
  const families = READER_FAMILIES[surface];
  const sites: ReadSite[] = [];
  for (const { file, text } of sources) {
    let fn = '?';
    const lines = text.split('\n');
    for (let index = 0; index < lines.length; index++) {
      const line = lines[index];
      const definition = line.match(DEFINITION);
      if (definition && !NOT_CODE.test(line)) {
        fn = definition[1];
      }
      for (const match of line.matchAll(READ)) {
        const family = families[match[1]];
        if (family === undefined || family === 'out-of-scope') {
          continue;
        }
        const entryPoint = normalizeEntryPoint(fn);
        sites.push({
          surface,
          file,
          fn,
          entryPoint,
          key: match[2],
          reader: match[1],
          family,
          line: index + 1,
          id: `${entryPoint}:${match[2]}`,
        });
      }
    }
  }
  return sites;
}

/** A name-shaped reader used with a key literal but carrying no recorded family. */
export function unclassifiedReaders(surface: 'node' | 'wasm'): string[] {
  const families = READER_FAMILIES[surface];
  const found = new Set<string>();
  for (const { text } of readerSources(treeOf(surface))) {
    for (const match of text.matchAll(READ)) {
      if (READER_NAME_SHAPE.test(match[1]) && families[match[1]] === undefined) {
        found.add(match[1]);
      }
    }
  }
  return [...found].sort();
}

export interface PairedField {
  /** `entryPoint:key`. */
  id: string;
  /** Every family the addon reads this field under, sorted. */
  node: Family[];
  /** Every family the embind binding reads it under, sorted. */
  wasm: Family[];
  /** Reader names, for the report and for the class key. */
  nodeReaders: string[];
  wasmReaders: string[];
  /** `nodeReader>wasmReader`; the class a mismatch is accounted for under. */
  readerPair: string;
}

function groupById(sites: readonly ReadSite[]): Map<string, ReadSite[]> {
  const out = new Map<string, ReadSite[]>();
  for (const site of sites) {
    const existing = out.get(site.id);
    if (existing) {
      existing.push(site);
    } else {
      out.set(site.id, [site]);
    }
  }
  return out;
}

const distinct = (values: readonly string[]): string[] => [...new Set(values)].sort();

/** Every field both surfaces read through a classified reader. */
export function pairedFields(
  node: readonly ReadSite[] = readSites('node'),
  wasm: readonly ReadSite[] = readSites('wasm'),
): PairedField[] {
  const byNode = groupById(node);
  const byWasm = groupById(wasm);
  const out: PairedField[] = [];
  for (const id of [...byNode.keys()].sort()) {
    const wasmSites = byWasm.get(id);
    const nodeSites = byNode.get(id);
    if (!wasmSites || !nodeSites) {
      continue;
    }
    const nodeReaders = distinct(nodeSites.map((site) => site.reader));
    const wasmReaders = distinct(wasmSites.map((site) => site.reader));
    out.push({
      id,
      node: distinct(nodeSites.map((site) => site.family)) as Family[],
      wasm: distinct(wasmSites.map((site) => site.family)) as Family[],
      nodeReaders,
      wasmReaders,
      readerPair: `${nodeReaders.join('|')}>${wasmReaders.join('|')}`,
    });
  }
  return out;
}

/** The paired fields whose two surfaces answer a wrong-typed value differently. */
export function mismatchedFields(paired: readonly PairedField[] = pairedFields()): PairedField[] {
  return paired.filter((field) => field.node.join() !== field.wasm.join());
}

export interface Finding {
  heading: string;
  lines: string[];
}

/**
 * Compares the live mismatches against @p accounted, a reader-pair class mapped
 * to the exact fields recorded under it.
 *
 * Both directions matter, for the reason the sentinel register records: an
 * unaccounted field is the defect spreading, and a recorded field that no longer
 * mismatches keeps asserting a reviewed decision, which the next field to take
 * that name would inherit unexamined.
 *
 * The reason sits on the reader PAIR rather than on the field because these
 * divergences are family-level decisions, not per-field ones -- 142 fields
 * diverge for the single reason that the addon's presence-checked readers refuse
 * a wrong type while embind's coerce it. That is safe in the direction the
 * field-level rule guards against: a class key names the readers, so moving a
 * field to a different reader takes it out of the class rather than carrying the
 * old blessing along. What the per-field member lists add is the ratchet -- a
 * NEW field joining an accounted class still has to be entered by hand.
 */
export function evaluateReaderFamilyScope(
  paired: readonly PairedField[],
  accounted: ReadonlyMap<string, readonly string[]>,
  floor = 150,
): Finding[] {
  const findings: Finding[] = [];
  if (paired.length < floor) {
    findings.push({
      heading: 'the cross-surface scan no longer finds the population it is sized for',
      lines: [`paired fields: found ${paired.length}, floor is ${floor}`],
    });
  }
  const mismatched = mismatchedFields(paired);
  const unaccounted = mismatched.filter(
    (field) => !(accounted.get(field.readerPair) ?? []).includes(field.id),
  );
  if (unaccounted.length > 0) {
    findings.push({
      heading: 'these fields are read under different reader families on the two surfaces',
      lines: unaccounted.map(
        (field) =>
          `${field.id} — node ${field.nodeReaders.join('|')} (${field.node.join('|')}) vs ` +
          `wasm ${field.wasmReaders.join('|')} (${field.wasm.join('|')})`,
      ),
    });
  }
  const live = new Map(mismatched.map((field) => [field.id, field.readerPair]));
  const dead: string[] = [];
  for (const [readerPair, ids] of accounted) {
    for (const id of ids) {
      if (live.get(id) !== readerPair) {
        dead.push(`${id} (recorded under ${readerPair})`);
      }
    }
  }
  if (dead.length > 0) {
    findings.push({ heading: 'these recorded divergences are no longer live', lines: dead });
  }
  return findings;
}
