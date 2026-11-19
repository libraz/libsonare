#!/usr/bin/env node
/**
 * Derive the shared public type corpus from the Node and WASM declarations.
 *
 * `shared_type_shapes.json` names the types both packages export under one
 * identifier; for each it records the field names (`types`) and which of them
 * the declaration marks optional (`optional`). Both are read from each
 * package's own TypeScript program through the checker, from the export of
 * `src/index.ts`, so a field added, renamed or made optional on either side is
 * a difference here rather than a hand edit nobody made.
 *
 *   node tests/conformance/shared_type_shapes.cjs --check   fail on drift or a Node/WASM disagreement
 *   node tests/conformance/shared_type_shapes.cjs --write   rewrite the corpus from the declarations
 *
 * The type list itself stays hand-chosen: the corpus names which types are
 * shared, the declarations say what they hold.
 */
'use strict';

const fs = require('node:fs');
const path = require('node:path');

const root = path.resolve(__dirname, '..', '..');
const CORPUS = path.join(__dirname, 'shared_type_shapes.json');
const PACKAGES = {
  node: 'bindings/node',
  wasm: 'bindings/wasm',
};

function deriveShapes(pkg, typeNames) {
  const dir = path.join(root, pkg);
  const ts = require(path.join(dir, 'node_modules/typescript'));
  const configPath = path.join(dir, 'tsconfig.json');
  const parsed = ts.getParsedCommandLineOfConfigFile(configPath, {}, {
    ...ts.sys,
    onUnRecoverableConfigFileDiagnostic: () => {},
  });
  const entry = path.join(dir, 'src/index.ts');
  const program = ts.createProgram({ rootNames: [entry], options: { ...parsed.options, noEmit: true } });
  const checker = program.getTypeChecker();
  const sf = program.getSourceFile(entry);
  const moduleSymbol = sf && checker.getSymbolAtLocation(sf);
  if (!moduleSymbol) throw new Error(`${pkg}: cannot read the exports of src/index.ts`);
  const exports = new Map();
  for (const sym of checker.getExportsOfModule(moduleSymbol)) exports.set(sym.name, sym);

  const shapes = {};
  const missing = [];
  for (const name of typeNames) {
    let sym = exports.get(name);
    if (!sym) {
      missing.push(name);
      continue;
    }
    if (sym.flags & ts.SymbolFlags.Alias) sym = checker.getAliasedSymbol(sym);
    const type = checker.getDeclaredTypeOfSymbol(sym);
    const fields = {};
    for (const prop of checker.getPropertiesOfType(type)) {
      fields[prop.name] = (prop.flags & ts.SymbolFlags.Optional) !== 0;
    }
    if (Object.keys(fields).length === 0) throw new Error(`${pkg}: ${name} declares no field`);
    shapes[name] = fields;
  }
  return { shapes, missing };
}

/** The corpus view of one derived shape: sorted names and sorted optional names. */
function view(fields) {
  const names = Object.keys(fields).sort();
  return { names, optional: names.filter((n) => fields[n]) };
}

function disagreements(name, node, wasm) {
  const out = [];
  for (const field of new Set([...Object.keys(node), ...Object.keys(wasm)])) {
    if (!(field in node)) out.push(`${name}.${field}: declared on wasm only`);
    else if (!(field in wasm)) out.push(`${name}.${field}: declared on node only`);
    else if (node[field] !== wasm[field]) {
      out.push(`${name}.${field}: optional on ${node[field] ? 'node' : 'wasm'} only`);
    }
  }
  return out.sort();
}

/** One array, inline when its whole line (key and trailing comma included) fits 80 columns. */
function formatArray(items, indent, prefixLength) {
  const inline = `[${items.map((s) => JSON.stringify(s)).join(', ')}]`;
  if (prefixLength + inline.length + 1 <= 80) return inline;
  const pad = `${indent}  `;
  return `[\n${items.map((s) => `${pad}${JSON.stringify(s)}`).join(',\n')}\n${indent}]`;
}

function formatSection(section, indent) {
  const keys = Object.keys(section);
  if (keys.length === 0) return '{}';
  const pad = `${indent}  `;
  const rows = keys.map((k) => {
    const prefix = `${pad}${JSON.stringify(k)}: `;
    return `${prefix}${formatArray(section[k], pad, prefix.length)}`;
  });
  return `{\n${rows.join(',\n')}\n${indent}}`;
}

function render(corpus) {
  return (
    `{\n  "schema_version": ${corpus.schema_version},\n` +
    `  "types": ${formatSection(corpus.types, '  ')},\n` +
    `  "optional": ${formatSection(corpus.optional, '  ')},\n` +
    `  "aliases": ${JSON.stringify(corpus.aliases)}\n}\n`
  );
}

function main() {
  const mode = process.argv.includes('--write') ? 'write' : 'check';
  const corpus = JSON.parse(fs.readFileSync(CORPUS, 'utf8'));
  const typeNames = Object.keys(corpus.types);
  if (typeNames.length === 0) {
    console.error('shared type shapes: the corpus names no type');
    return 1;
  }
  const derived = {};
  const errors = [];
  for (const [surface, pkg] of Object.entries(PACKAGES)) {
    const { shapes, missing } = deriveShapes(pkg, typeNames);
    for (const name of missing) errors.push(`${surface} does not export ${name}`);
    derived[surface] = shapes;
  }
  for (const name of typeNames) {
    if (derived.node[name] && derived.wasm[name]) {
      errors.push(...disagreements(name, derived.node[name], derived.wasm[name]));
    }
  }
  if (errors.length > 0) {
    for (const e of errors) console.error(`  FAIL ${e}`);
    console.error('shared type shapes: the two packages disagree; nothing was compared against the corpus');
    return 1;
  }

  const next = { schema_version: corpus.schema_version, types: {}, optional: {}, aliases: corpus.aliases ?? {} };
  let optionalCount = 0;
  for (const name of typeNames) {
    const { names, optional } = view(derived.node[name]);
    next.types[name] = names;
    if (optional.length > 0) next.optional[name] = optional;
    optionalCount += optional.length;
  }
  const fieldCount = typeNames.reduce((n, t) => n + next.types[t].length, 0);
  console.log(
    `shared type shapes: ${typeNames.length} types, ${fieldCount} fields, ${optionalCount} optional, node and wasm agree`,
  );
  const text = render(next);
  if (mode === 'write') {
    fs.writeFileSync(CORPUS, text);
    console.log(`wrote ${path.relative(root, CORPUS)}`);
    return 0;
  }
  if (fs.readFileSync(CORPUS, 'utf8') !== text) {
    console.error('  FAIL tests/conformance/shared_type_shapes.json is stale; run with --write');
    return 1;
  }
  return 0;
}

process.exit(main());
