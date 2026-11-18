// Type-checks every fenced TypeScript example in the Node and WASM doc comments against the
// built declarations (`dist/index.d.ts`), so an example calling a function or passing a field
// the package does not export fails here instead of in a reader's editor.
//
// usage (repo root, after `yarn build:ts` / `yarn build:js`):
//   node tools/api/check_doc_examples.mjs [node|wasm ...]
//
// Each example runs as the body of an async function with every package export in scope.
// Its `import ... from '<package>'` lines are checked against the exports and dropped; its other
// imports and its `declare` lines (the inputs it names for itself) move to module level. Names an
// example takes as given (the audio it works on) are declared in PLACEHOLDERS; any other free
// name is a finding. Exit 1 on any finding, or when a package yields no examples.

import fs from 'node:fs';
import { createRequire } from 'node:module';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');

const PACKAGES = {
  node: { dir: 'bindings/node', name: '@libraz/libsonare-native' },
  wasm: { dir: 'bindings/wasm', name: '@libraz/libsonare' },
};

// What an example may use without defining it: the input it works on (audio, a block of it, or
// a pitch track: f0 per frame, voiced flags, frames per second) and the handle it is shown on.
// A handle is typed, so a call on it is checked too.
const PLACEHOLDERS = `
declare let samples: Float32Array;
declare let left: Float32Array;
declare let right: Float32Array;
declare let leftSamples: Float32Array;
declare let rightSamples: Float32Array;
declare let channels: Float32Array[];
declare const block: Float32Array;
declare const f0Hz: Float32Array;
declare const voiced: Uint8Array;
declare const frameRate: number;
declare const sampleRate: number;
declare const context: AudioContext;
declare const project: Project;
declare const mixer: Mixer;
declare const engine: RealtimeEngine;
declare const changer: RealtimeVoiceChanger;
`;

const FENCE = /```(?:ts|typescript)\n([\s\S]*?)```/g;
// Module-level lines an example may carry: its own imports, and `declare` lines naming its inputs.
const HOISTED = /^\s*(import|declare)\s/;
const PKG_IMPORT = /^\s*import\s+(type\s+)?\{([^}]*)\}\s+from\s+'([^']+)';?\s*$/;

function sourceFiles(dir) {
  const out = [];
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, entry.name);
    if (entry.isDirectory()) {
      if (entry.name !== 'addon') out.push(...sourceFiles(p));
    } else if (entry.name.endsWith('.ts') && !entry.name.endsWith('.d.ts')) {
      out.push(p);
    }
  }
  return out.sort();
}

/** Every fenced TS block inside a doc comment: { file, line (1-based, first code line), code }. */
function examples(pkgDir) {
  const found = [];
  for (const file of sourceFiles(path.join(ROOT, pkgDir, 'src'))) {
    const text = fs.readFileSync(file, 'utf8');
    for (const doc of text.matchAll(/\/\*\*[\s\S]*?\*\//g)) {
      const lines = doc[0].split('\n');
      const body = lines.map((l) => l.replace(/^\s*\/?\*+\/?\s?/, '').replace(/\*\/\s*$/, ''));
      const joined = body.join('\n');
      const docLine = text.slice(0, doc.index).split('\n').length;
      for (const block of joined.matchAll(FENCE)) {
        const fenceLine = joined.slice(0, block.index).split('\n').length;
        found.push({
          file: path.relative(ROOT, file),
          line: docLine + fenceLine,
          code: block[1],
        });
      }
    }
  }
  return found;
}

function check(surface) {
  const { dir, name } = PACKAGES[surface];
  const require = createRequire(path.join(ROOT, dir, 'package.json'));
  const ts = require('typescript');
  const pkg = JSON.parse(fs.readFileSync(path.join(ROOT, dir, 'package.json'), 'utf8'));
  // Every entry point with declarations: the specifier a reader imports, and its .d.ts.
  const entries = pkg.exports
    ? Object.entries(pkg.exports)
        .filter(([, v]) => v && typeof v === 'object' && v.types)
        .map(([k, v]) => [k === '.' ? name : `${name}/${k.slice(2)}`, v.types])
    : [[name, pkg.types]];
  const dists = entries.map(([spec, types]) => [spec, path.join(ROOT, dir, types)]);
  for (const [, d] of dists) {
    if (!fs.existsSync(d)) return [`${surface}: ${path.relative(ROOT, d)} is missing; build first`];
  }

  const options = {
    target: ts.ScriptTarget.ES2022,
    module: ts.ModuleKind.ESNext,
    moduleResolution: ts.ModuleResolutionKind.Bundler,
    strict: true,
    noEmit: true,
    skipLibCheck: true,
    lib: ['lib.es2022.d.ts', 'lib.esnext.disposable.d.ts', 'lib.dom.d.ts'],
    types: surface === 'node' ? ['node'] : [],
    typeRoots: [path.join(ROOT, dir, 'node_modules', '@types')],
  };

  const probe = ts.createProgram(
    dists.map(([, d]) => d),
    options,
  );
  const checker = probe.getTypeChecker();
  const exportsOf = new Map(
    dists.map(([spec, d]) => [
      spec,
      new Set(
        checker
          .getExportsOfModule(checker.getSymbolAtLocation(probe.getSourceFile(d)))
          .map((x) => x.name),
      ),
    ]),
  );
  // Scope: every export of every entry, each name imported once from the first entry naming it.
  const scope = [];
  const seen = new Set();
  for (const [spec, d] of dists) {
    const names = [...exportsOf.get(spec)].filter((n) => !seen.has(n));
    for (const n of names) seen.add(n);
    if (names.length)
      scope.push(
        `import { ${names.join(', ')} } from ${JSON.stringify(d.replace(/\.d\.ts$/, '.js'))};`,
      );
  }

  const problems = [];
  const virtual = new Map();
  const origin = new Map();
  examples(dir).forEach((ex, i) => {
    const kept = [];
    const body = ex.code.split('\n').map((l, j) => {
      const m = PKG_IMPORT.exec(l);
      if (!m || !(m[3] === name || m[3].startsWith(`${name}/`))) return l;
      const exported = exportsOf.get(m[3]);
      if (!exported) {
        problems.push(
          `${ex.file}:${ex.line + j}: imports from ${m[3]}, which is not an entry point`,
        );
        return '';
      }
      for (const raw of m[2].split(',')) {
        const id = raw
          .trim()
          .split(/\s+as\s+/)[0]
          .replace(/^type\s+/, '');
        if (id && !exported.has(id)) {
          problems.push(`${ex.file}:${ex.line + j}: imports ${id}, which ${m[3]} does not export`);
        }
      }
      return '';
    });
    for (const l of body) if (HOISTED.test(l)) kept.push(l);
    const head = [
      ...scope,
      ...kept,
      PLACEHOLDERS,
      'export async function example(): Promise<void> {',
    ].join('\n');
    const file = path.join(ROOT, dir, `__doc_example_${i}.ts`);
    const text = `${head}\n${body.map((l) => (HOISTED.test(l) ? '' : l)).join('\n')}\n}\n`;
    virtual.set(file, text);
    origin.set(file, { ...ex, offset: head.split('\n').length });
  });
  if (virtual.size === 0) return [`${surface}: no examples found; the extractor read nothing`];

  const host = ts.createCompilerHost(options);
  const getSourceFile = host.getSourceFile.bind(host);
  host.getSourceFile = (f, lang, ...rest) =>
    virtual.has(f) ? ts.createSourceFile(f, virtual.get(f), lang) : getSourceFile(f, lang, ...rest);
  const fileExists = host.fileExists.bind(host);
  host.fileExists = (f) => virtual.has(f) || fileExists(f);
  const readFile = host.readFile.bind(host);
  host.readFile = (f) => virtual.get(f) ?? readFile(f);

  const program = ts.createProgram([...virtual.keys()], options, host);
  for (const d of ts.getPreEmitDiagnostics(program)) {
    const o = d.file && origin.get(d.file.fileName);
    const msg = ts.flattenDiagnosticMessageText(d.messageText, ' ');
    if (!o) {
      problems.push(`${surface}: ${msg}`);
      continue;
    }
    const { line } = d.file.getLineAndCharacterOfPosition(d.start ?? 0);
    const at = line - o.offset;
    problems.push(`${o.file}:${o.line + Math.max(at, 0)}: TS${d.code} ${msg}`);
  }
  console.log(`${surface}: ${virtual.size} examples checked against ${dists.length} entry points`);
  return problems;
}

const surfaces = process.argv.slice(2);
const problems = (surfaces.length ? surfaces : Object.keys(PACKAGES)).flatMap(check);
for (const p of problems) console.log(`  ${p}`);
console.log(`doc examples: ${problems.length} finding${problems.length === 1 ? '' : 's'}`);
process.exit(problems.length ? 1 : 0);
