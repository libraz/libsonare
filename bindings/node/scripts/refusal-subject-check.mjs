// Every refusal subject the suite provokes names a parameter of the public entry the caller invoked.
//
// Reads a refusal capture (refusal-capture-setup.mjs) and resolves each recorded entry against the
// declarations reachable from src/index.ts: a subject's leading segment (`options` in
// `options.nFft`, `ranges` in `ranges[2].start`) must be one of the entry's parameter names or a
// property of a parameter's object type. A loader refusing the content of a document it was handed
// (InvalidFormat) names a path inside that document and is not checked. Only what a test provokes
// is seen, so the reach is printed beside the verdict.
//
// Usage: node scripts/refusal-subject-check.mjs CAPTURE.jsonl [--out FILE]
import { readFileSync, writeFileSync } from 'node:fs';
import path from 'node:path';
import ts from 'typescript';

const INVALID_FORMAT = 2;
const ROOT_SEGMENT = /^[A-Za-z_$][\w$]*/;

const args = process.argv.slice(2);
const capturePath = args[0];
const outIndex = args.indexOf('--out');
const outPath = outIndex >= 0 ? args[outIndex + 1] : undefined;
if (!capturePath) {
  console.error('usage: refusal-subject-check.mjs CAPTURE.jsonl [--out FILE]');
  process.exit(2);
}

const packageDir = path.resolve(import.meta.dirname, '..');
const entryFile = path.join(packageDir, 'src/index.ts');
const config = ts.getParsedCommandLineOfConfigFile(
  path.join(packageDir, 'tsconfig.json'),
  {},
  { ...ts.sys, onUnRecoverableConfigFileDiagnostic: () => {} },
);
const program = ts.createProgram([entryFile], { ...config.options, noEmit: true });
const checker = program.getTypeChecker();
const moduleSymbol = checker.getSymbolAtLocation(program.getSourceFile(entryFile));
const exported = new Map(
  checker.getExportsOfModule(moduleSymbol).map((s) => [s.getName(), resolveAlias(s)]),
);

function resolveAlias(symbol) {
  return symbol.flags & ts.SymbolFlags.Alias ? checker.getAliasedSymbol(symbol) : symbol;
}

/** The property names of an object-typed parameter, through unions and optionality. */
function objectKeys(type, into) {
  if (type.isUnion()) {
    for (const member of type.types) objectKeys(member, into);
    return;
  }
  if (!(type.flags & ts.TypeFlags.Object) || checker.isArrayType(type) || checker.isTupleType(type)) {
    return;
  }
  if (type.getCallSignatures().length > 0) return;
  for (const prop of checker.getPropertiesOfType(type)) into.add(prop.getName());
}

function signatureRoots(signatures, into) {
  for (const signature of signatures) {
    for (const param of signature.getParameters()) {
      into.add(param.getName());
      const decl = param.valueDeclaration;
      if (decl) objectKeys(checker.getTypeOfSymbolAtLocation(param, decl), into);
    }
  }
}

function symbolType(symbol) {
  const decl = symbol.valueDeclaration ?? symbol.declarations?.[0];
  return decl ? checker.getTypeOfSymbolAtLocation(symbol, decl) : undefined;
}

/** Parameter roots of a recorded entry, or undefined when no public declaration answers to it. */
function entryRoots(entry) {
  const [owner, member] = entry.split('.', 2);
  const symbol = exported.get(owner);
  if (symbol === undefined) return undefined;
  const ownerType = symbolType(symbol);
  if (ownerType === undefined) return undefined;
  const roots = new Set();
  if (member === undefined) {
    signatureRoots(ownerType.getCallSignatures(), roots);
    return roots;
  }
  if (member === 'constructor') {
    signatureRoots(ownerType.getConstructSignatures(), roots);
    return roots;
  }
  const instance = ownerType.getConstructSignatures()[0]?.getReturnType();
  const prop = ownerType.getProperty(member) ?? instance?.getProperty(member);
  if (prop === undefined) return undefined;
  const propType = symbolType(prop);
  if (propType === undefined) return undefined;
  signatureRoots(propType.getCallSignatures(), roots);
  return roots;
}

/** Every public callable taking an argument: exported functions, class constructors and methods. */
function publicEntries() {
  const out = new Set();
  const takesArgument = (type) => type?.getCallSignatures().some((s) => s.getParameters().length > 0);
  for (const [name, symbol] of exported) {
    const type = symbolType(symbol);
    if (type === undefined) continue;
    if (takesArgument(type)) out.add(name);
    const constructs = type.getConstructSignatures();
    if (constructs.length === 0) continue;
    if (constructs.some((s) => s.getParameters().length > 0)) out.add(`${name}.constructor`);
    for (const holder of [type, constructs[0].getReturnType()]) {
      for (const prop of checker.getPropertiesOfType(holder)) {
        if (prop.getName() !== 'prototype' && takesArgument(symbolType(prop))) {
          out.add(`${name}.${prop.getName()}`);
        }
      }
    }
  }
  return out;
}

const pairs = new Map();
for (const line of readFileSync(capturePath, 'utf8').split('\n')) {
  if (!line) continue;
  const row = JSON.parse(line);
  const refusal = row.refusal;
  if (!refusal) continue;
  for (const subject of refusal.subjects) {
    const key = `${row.entry}\u0000${subject}`;
    const pair = pairs.get(key) ?? {
      entry: row.entry,
      subject,
      code: refusal.code,
      count: 0,
      cEntry: refusal.cEntry ?? null,
      content: false,
    };
    pair.count += 1;
    if (refusal.code === INVALID_FORMAT) pair.content = true;
    pairs.set(key, pair);
  }
}

const misses = [];
const unresolved = new Set();
const rootsCache = new Map();
for (const pair of pairs.values()) {
  if (pair.content) continue;
  if (!rootsCache.has(pair.entry)) rootsCache.set(pair.entry, entryRoots(pair.entry));
  const roots = rootsCache.get(pair.entry);
  if (roots === undefined) {
    unresolved.add(pair.entry);
    continue;
  }
  const head = pair.subject.match(ROOT_SEGMENT)?.[0];
  if (head === undefined || !roots.has(head)) misses.push(pair);
}

const provoked = new Set([...pairs.values()].map((p) => p.entry));
const callable = publicEntries();
const neverProvoked = [...callable].filter((e) => !provoked.has(e)).sort();
const surface = path.basename(packageDir);
console.log(
  `refusal subjects (${surface}): ${pairs.size} (entry, subject) pairs over ${provoked.size} entries; ` +
    `${misses.length} name no parameter of their entry; ` +
    `${neverProvoked.length} of ${callable.size} public callables never provoke a core refusal`,
);
misses.sort((a, b) => `${a.entry} ${a.subject}`.localeCompare(`${b.entry} ${b.subject}`));
for (const m of misses) console.log(`  ${m.entry}: ${m.subject}`);
for (const entry of [...unresolved].sort()) console.log(`  unresolved entry: ${entry}`);

if (outPath) {
  const cEntries = new Set([...pairs.values()].map((p) => p.cEntry).filter((e) => e !== null));
  const report = {
    surface,
    pairs: [...pairs.values()].map(({ content, ...p }) => p),
    provokedCEntries: [...cEntries].sort(),
    provokedEntries: [...provoked].sort(),
    refusingEntries: [...callable].sort(),
  };
  writeFileSync(outPath, `${JSON.stringify(report, null, 1)}\n`);
}
process.exitCode = misses.length > 0 || unresolved.size > 0 ? 1 : 0;
