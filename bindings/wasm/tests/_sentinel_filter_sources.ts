/**
 * Static reader for the "`> 0` used as a sentinel filter" shape across the three
 * C++ trees that accept a caller-supplied optional scalar: the C ABI
 * (`src/c_api/`), the WASM embind bindings (`src/wasm/bindings/`) and the N-API
 * addon (`bindings/node/src/addon/`).
 *
 * THE SHAPE. A field whose documented sentinel is `0` must be tested with
 * `== 0`. `> 0` answers a negative or non-finite request with the library
 * default instead of the refusal it had earned, and the substitute is
 * indistinguishable downstream from a value the caller chose. The scan anchors
 * on the COMPARISON, never on `sonare::ZeroIsDefault` or a field name: the
 * helper's call sites are only the sites already correct.
 *
 * THE POPULATION. Both spellings are scanned -- ternary `x > 0 ? x : fallback`
 * and guard `if (x > 0) target = x;` -- but only where the true branch applies
 * the subject's own value. That one structural filter separates a sentinel
 * filter from the far larger family of length and count guards: an
 * `if (count > 0) { memcpy(...) }` never assigns `count`, so no value of it can
 * select a default. It over-includes rather than under-includes.
 *
 * IDENTITY IS `file:field`, NEVER `file:line`. The reason is written about the
 * field, so it must survive the comparison moving; a line key also breaks on any
 * unrelated insertion, and once regenerating is routine a real new site rides in
 * with the noise (the reasoning `_wasm_binding_sources.ts` records for its own
 * id). The field is the terminal identifier, so `config->n_fft` and a bare
 * `n_fft` in one file are one entry.
 *
 * WHAT THE SCAN CANNOT SEE: a compound condition, a comparison against a call
 * result, and a ternary whose true branch spans lines. A site behind one of
 * those is outside this guard, not accounted for by it.
 *
 * PYTHON IS NOT SCANNED, structurally rather than by omission. It does not
 * resolve sentinels: 361 int/float keyword parameters default to 0, and a probe
 * over nineteen sentinel names found one Python-side comparison among them,
 * which sizes a validation span rather than the value handed on. Python passes
 * the caller's number to ctypes and the C ABI decides what its zero means, so
 * this branch has nowhere to live. If a Python-side substitution is ever
 * written this guard is blind to it, and Python is the published PyPI artifact.
 *
 * Regex-level rather than a real C++ parse, mirroring `_addon_sources.ts`.
 */

import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join } from 'node:path';

/** Scanned trees, each relative to the repository root. */
export const SCANNED_TREES = ['src/c_api', 'src/wasm/bindings', 'bindings/node/src/addon'] as const;

const REPO_ROOT = new URL('../../../', import.meta.url).pathname;

export interface SentinelSource {
  /** Path relative to the repository root, e.g. `src/c_api/project_bounce.cpp`. */
  file: string;
  text: string;
}

/** Which spelling of the idiom a site is written in. */
export type FilterForm = 'ternary' | 'guard';

export interface FilterSite {
  file: string;
  /** The compared expression as written, e.g. `config->n_fft`. */
  subject: string;
  /** Terminal identifier of {@link subject}, e.g. `n_fft`. */
  field: string;
  form: FilterForm;
  /** Source line. Diagnostic only -- deliberately NOT part of {@link id}. */
  line: number;
  /** Field-level identity: `file:field`. */
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

let cachedSources: SentinelSource[] | undefined;

/** Every `.cpp` / `.h` under {@link SCANNED_TREES}, read once. */
export function sentinelSources(): SentinelSource[] {
  if (!cachedSources) {
    cachedSources = [];
    for (const tree of SCANNED_TREES) {
      const root = join(REPO_ROOT, tree);
      for (const file of walk(root, '').sort()) {
        cachedSources.push({
          file: `${tree}/${file}`,
          text: readFileSync(join(root, file), 'utf8'),
        });
      }
    }
  }
  return cachedSources;
}

/** An identifier, optionally reached through `->` / `.` member access. */
const SUBJECT = String.raw`(?<![\w.])([A-Za-z_]\w*(?:(?:->|\.)\w+)*)`;

/** `> 0` in any of the literal spellings this tree uses. */
const GREATER_THAN_ZERO = String.raw`\s*>\s*0(?:\.0)?[fFuUlL]*\s*`;

/**
 * `subject > 0 ? <true branch> :`. The `:` is matched only when it is not part
 * of a `::`, so a true branch calling a qualified name (`std::min(...)`) is read
 * whole instead of being cut at the first colon -- which silently dropped such
 * sites from an earlier version of this scan.
 */
const TERNARY_FILTER = new RegExp(`${SUBJECT}${GREATER_THAN_ZERO}\\?([^;\\n]*?)(?<!:):(?!:)`, 'g');

/** `if (subject > 0) <first statement>;`, the whole condition being that one comparison. */
const GUARD_FILTER = new RegExp(
  `\\bif\\s*\\(\\s*${SUBJECT}${GREATER_THAN_ZERO}\\)([^;\\n]*(?:\\n[^;]*)?);`,
  'g',
);

/** A plain or compound assignment, excluding the comparison operators. */
const ASSIGNMENT = /[^=!<>]=(?!=)/;

function terminalField(subject: string): string {
  const parts = subject.split(/->|\./);
  return parts[parts.length - 1] ?? subject;
}

function mentions(text: string, field: string): boolean {
  return new RegExp(`\\b${field}\\b`).test(text);
}

/** Whether an `if` body's first statement assigns the subject's own value. */
function assignsSubject(statement: string, field: string): boolean {
  const at = statement.search(ASSIGNMENT);
  return at >= 0 && mentions(statement.slice(at), field);
}

function lineOf(text: string, offset: number): number {
  return text.slice(0, offset).split('\n').length;
}

/**
 * Every `> 0` comparison whose true branch applies the compared subject's own
 * value, across @p sources.
 */
export function filterSites(sources: readonly SentinelSource[] = sentinelSources()): FilterSite[] {
  const sites: FilterSite[] = [];
  for (const { file, text } of sources) {
    for (const match of text.matchAll(TERNARY_FILTER)) {
      const field = terminalField(match[1]);
      if (mentions(match[2], field)) {
        const line = lineOf(text, match.index ?? 0);
        sites.push({
          file,
          subject: match[1],
          field,
          form: 'ternary',
          line,
          id: `${file}:${field}`,
        });
      }
    }
    for (const match of text.matchAll(GUARD_FILTER)) {
      const field = terminalField(match[1]);
      if (assignsSubject(match[2], field)) {
        const line = lineOf(text, match.index ?? 0);
        sites.push({ file, subject: match[1], field, form: 'guard', line, id: `${file}:${field}` });
      }
    }
  }
  return sites;
}

/**
 * Every `> 0` comparison naming @p field in @p file, whatever its shape --
 * ternary, guard, compound condition or negated. This is deliberately wider
 * than {@link filterSites}: it backs the removed-filter ratchet, where the
 * question is not "is this a sentinel filter" but "has the comparison come
 * back at all", and a restored filter must be caught in whichever spelling it
 * returns in.
 */
export function comparisonsAgainstZero(
  file: string,
  field: string,
  sources: readonly SentinelSource[] = sentinelSources(),
): number[] {
  const source = sources.find((entry) => entry.file === file);
  if (!source) {
    return [];
  }
  const pattern = new RegExp(`\\b${field}\\b${GREATER_THAN_ZERO}`, 'g');
  return [...source.text.matchAll(pattern)].map((match) => lineOf(source.text, match.index ?? 0));
}

/**
 * Whether @p file states a refusal naming @p subject: the expression appears,
 * SPELLED THE SAME WAY the filter spells it, on a line that also rejects a
 * negative, out-of-range or non-finite value.
 *
 * Matching the subject rather than its terminal identifier is what gives this
 * any force. A bare-identifier match is satisfied by an unrelated refusal of a
 * same-named parameter elsewhere in the file -- `sonare_c_effects.cpp` refuses a
 * positional `n_components` in one entry point and filters
 * `config->n_components` in another, so deleting the second refusal left the
 * first one answering for it.
 *
 * Deliberately FILE-scoped rather than windowed on the comparison. A window
 * needs a size, and the distance from a refusal to the site it protects ranges
 * from two lines to the far end of the file here (`dither_bits` is refused 76
 * lines above its use), so any window wide enough to hold the real cases is wide
 * enough to be meaningless. What this buys: deleting the refusal turns the entry
 * red. What it does not: it cannot tell that the refusal runs BEFORE the
 * comparison on every path, so it is a presence check, not a dominance proof.
 */
export function refusalMentioning(
  file: string,
  subject: string,
  sources: readonly SentinelSource[] = sentinelSources(),
): number[] {
  const source = sources.find((entry) => entry.file === file);
  if (!source) {
    return [];
  }
  const refusal = /<\s*0|<=\s*0|isfinite|finite_|valid_c_enum/;
  const escaped = subject.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
  const names = new RegExp(`${escaped}(?![\\w])`);
  return source.text
    .split('\n')
    .map((line, index) => ({ line, number: index + 1 }))
    .filter((entry) => refusal.test(entry.line) && names.test(entry.line))
    .map((entry) => entry.number);
}

/** Every distinct spelling the scan found for one `file:field` site. */
export function subjectsOf(
  id: string,
  sources: readonly SentinelSource[] = sentinelSources(),
): string[] {
  return [
    ...new Set(
      filterSites(sources)
        .filter((site) => site.id === id)
        .map((s) => s.subject),
    ),
  ];
}

export interface Finding {
  heading: string;
  lines: string[];
}

/**
 * Compares the scanned population against @p reasons, reporting a site with no
 * recorded reason and a reason that matches no site. Both directions matter: an
 * unanswered site is the defect reopening, and a reason that excuses nothing
 * keeps asserting a reviewed decision about a field, so the next field to take
 * that name inherits the blessing unexamined.
 */
export function evaluateSentinelFilterScope(
  sources: readonly SentinelSource[],
  reasons: ReadonlyMap<string, string>,
  floor = 50,
): Finding[] {
  const sites = filterSites(sources);
  const findings: Finding[] = [];
  if (sites.length < floor) {
    findings.push({
      heading: 'the sentinel-filter scan no longer finds the population it is sized for',
      lines: [`sites: found ${sites.length}, floor is ${floor}`],
    });
  }
  const unaccounted = sites.filter((site) => !reasons.has(site.id));
  if (unaccounted.length > 0) {
    findings.push({
      heading: 'these `> 0` filters carry no recorded reason',
      lines: unaccounted.map((site) => `${site.id} (${site.file}:${site.line} ${site.form})`),
    });
  }
  const live = new Set(sites.map((site) => site.id));
  const dead = [...reasons.keys()].filter((id) => !live.has(id));
  if (dead.length > 0) {
    findings.push({ heading: 'these recorded reasons matched no `> 0` filter', lines: dead });
  }
  return findings;
}
