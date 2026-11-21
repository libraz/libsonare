// Derives, for the Node and WASM packages, which public entry reaches each native key.
//
// Usage: node native_entries.cjs [--write | --check] [repo root]
//   (no flag) prints the derivation as JSON; --write regenerates each package's
//   src/internal/native_entries.ts; --check prints the counts and exits 1 on a table
//   that differs from the derivation, a key reached by two entries, an ambiguous or
//   unresolved site, or a surface where nothing was scanned.
//
// A native key is a free native function ("analyzeWithProgress"), a native class
// ("PlaybackRenderer", whose key is also its constructor) or a native method
// ("Project.setMarkerEx"). Native names come from the C++ registrations; call sites
// come from a TypeScript AST walk of the facade sources. Each call site resolves to
// the outermost public entries that reach it: a function or class in the value
// re-export closure of a package entry point, or a public member of such a class.
// Calls through non-public helpers are followed across files; an enclosing
// `withEntry('<name>', ...)` is the entry for everything inside it. AudioWorklet
// processor modules (a class with a `process(inputs, outputs)` method, and the
// modules only they import) are listed and left out: their native calls run on the
// audio thread, not inside a call a consumer made.
'use strict';

const fs = require('node:fs');
const path = require('node:path');

const flags = process.argv.slice(2).filter((a) => a.startsWith('--'));
const root = path.resolve(process.argv.slice(2).find((a) => !a.startsWith('--')) || '.');

const SURFACES = {
  node: {
    facade: 'bindings/node/src',
    table: 'bindings/node/src/internal/native_entries.ts',
    skip: ['bindings/node/src/addon', 'bindings/node/src/native.ts', 'bindings/node/src/internal/entry.ts'],
    entries: ['index.ts'],
    typescript: 'bindings/node/node_modules/typescript',
    tsconfig: 'bindings/node/tsconfig.json',
    natives: 'bindings/node/src/addon',
    moduleReceivers: new Set(['addon']),
    portEndpoints: [],
    // Refusals raised where no consumer entry is on the stack; they stay unprefixed.
    unentered: { checkAbiVersion: 'module load' },
  },
  wasm: {
    facade: 'bindings/wasm/src',
    table: 'bindings/wasm/src/internal/native_entries.ts',
    skip: ['bindings/wasm/src/internal/entry.ts', 'bindings/wasm/src/module_state.ts'],
    entries: ['index.ts', 'analysis.ts', 'worklet.ts', 'worker.ts', 'vocal_edit_worker.ts'],
    typescript: 'bindings/wasm/node_modules/typescript',
    tsconfig: 'bindings/wasm/tsconfig.json',
    natives: 'src/wasm',
    // A worker endpoint answers a client over a port; the client relabels the reply with its own entry.
    portEndpoints: ['installVocalEditWorkerEndpoint', 'installOfflineWorkerEndpoint'],
    unentered: {},
    moduleReceivers: new Set(['requireModule()', 'getSonareModule()', 'module', 'mod', 'm', 'wasm', 'raw', 'sonare']),
  },
};

function walkFiles(dir, accept, skip, out = []) {
  for (const name of fs.readdirSync(dir)) {
    const p = path.join(dir, name);
    if (skip.some((s) => p === path.join(root, s))) continue;
    if (fs.statSync(p).isDirectory()) walkFiles(p, accept, skip, out);
    else if (accept(p)) out.push(p);
  }
  return out;
}

function nativeNames(surface) {
  const cfg = SURFACES[surface];
  const files = walkFiles(path.join(root, cfg.natives), (p) => /\.(cpp|h)$/.test(p), []);
  const free = new Set();
  const classes = new Map();
  const wrapToClass = new Map();
  const methodsByWrap = new Map();
  const addMethod = (wrap, name) => {
    if (!methodsByWrap.has(wrap)) methodsByWrap.set(wrap, new Set());
    methodsByWrap.get(wrap).add(name);
  };
  for (const file of files) {
    const text = fs.readFileSync(file, 'utf8');
    if (surface === 'node') {
      for (const m of text.matchAll(/exports\.Set\(\s*"(\w+)"/g)) free.add(m[1]);
      for (const m of text.matchAll(/DefineClass\(\s*env,\s*"(\w+)"([\s\S]*?)\}\s*\)/g)) {
        const wrap = /<&(\w+)::/.exec(m[2]);
        if (wrap) wrapToClass.set(wrap[1], m[1]);
        if (!classes.has(m[1])) classes.set(m[1], new Set());
      }
      for (const m of text.matchAll(/(?:Instance|Static)Method<&(\w+)::\w+>\(\s*"(\w+)"/g)) addMethod(m[1], m[2]);
    } else {
      for (const m of text.matchAll(/(?<![.\w])(?:emscripten::)?function\(\s*"(\w+)"/g)) free.add(m[1]);
      for (const m of text.matchAll(/class_<(\w+)[^>]*>\s*(?:\w+\s*)?\(\s*"(\w+)"/g)) {
        wrapToClass.set(m[1], m[2]);
        if (!classes.has(m[2])) classes.set(m[2], new Set());
      }
      // A block never runs into the next class_<>, so a bare `class_<X>("X");` keeps no methods.
      for (const block of text.matchAll(/class_<(\w+)[^>]*>[^;{]*?(?:\(|&\s*\w+\s*\)\s*\{)((?:(?!class_<)[\s\S])*?);\s*(?:\n\s*\}|\n\n|(?=\n\s*class_<))/g)) {
        for (const m of block[2].matchAll(/\.(?:function|class_function)\(\s*"(\w+)"/g)) addMethod(block[1], m[1]);
      }
    }
  }
  for (const [wrap, methods] of methodsByWrap) {
    const cls = wrapToClass.get(wrap);
    if (cls) for (const m of methods) classes.get(cls).add(m);
  }
  for (const cls of classes.keys()) free.delete(cls);
  return { free, classes };
}

function resolveImport(fromFile, spec) {
  if (!spec.startsWith('.')) return null;
  const base = path.resolve(path.dirname(fromFile), spec.replace(/\.js$/, ''));
  for (const candidate of [`${base}.ts`, path.join(base, 'index.ts')]) if (fs.existsSync(candidate)) return candidate;
  return null;
}

function derive(surface) {
  const cfg = SURFACES[surface];
  const ts = require(path.join(root, cfg.typescript));
  const { free, classes } = nativeNames(surface);
  const methodClasses = new Map();
  for (const [cls, methods] of classes) {
    for (const m of methods) {
      if (!methodClasses.has(m)) methodClasses.set(m, []);
      methodClasses.get(m).push(cls);
    }
  }
  const files = walkFiles(
    path.join(root, cfg.facade),
    (p) => p.endsWith('.ts') && !p.endsWith('.d.ts') && !p.endsWith('native_entries.ts'),
    cfg.skip,
  );

  const configPath = path.join(root, cfg.tsconfig);
  const parsed = ts.getParsedCommandLineOfConfigFile(configPath, {}, { ...ts.sys, onUnRecoverableConfigFileDiagnostic: () => {} });
  const program = ts.createProgram({ rootNames: files, options: { ...parsed.options, noEmit: true } });
  const checker = program.getTypeChecker();
  const publishedTypes = new Set();
  for (const entry of cfg.entries) {
    const sf = program.getSourceFile(path.join(root, cfg.facade, entry));
    const modSym = sf && checker.getSymbolAtLocation(sf);
    if (!modSym) continue;
    for (const exp of checker.getExportsOfModule(modSym)) {
      publishedTypes.add(exp.flags & ts.SymbolFlags.Alias ? checker.getAliasedSymbol(exp) : exp);
    }
  }
  const modules = new Map();
  for (const file of files) {
    modules.set(file, scanFile(ts, program.getSourceFile(file), { free, classes, methodClasses, cfg, checker, publishedTypes }));
  }

  // Public names: the value re-export closure of the entry modules.
  const publicNames = new Set();
  const seen = new Set();
  const visitExports = (file, only) => {
    const mod = modules.get(file);
    if (!mod) return;
    const key = `${file}|${only ? [...only].join(',') : '*'}`;
    if (seen.has(key)) return;
    seen.add(key);
    for (const name of mod.localExports) if (!only || only.has(name)) publicNames.add(`${file}#${name}`);
    for (const re of mod.reExports) {
      const target = resolveImport(file, re.from);
      if (!target) continue;
      if (re.star) visitExports(target, only);
      else {
        const names = new Set(re.names.filter((n) => !only || only.has(n.as)).map((n) => n.name));
        if (names.size) visitExports(target, names);
      }
    }
  };
  for (const entry of cfg.entries) visitExports(path.join(root, cfg.facade, entry), null);

  // Processor-owned modules.
  const importers = new Map(files.map((f) => [f, []]));
  for (const [file, mod] of modules) {
    for (const spec of mod.importSpecs) {
      const target = resolveImport(file, spec);
      if (target && importers.has(target)) importers.get(target).push(file);
    }
  }
  const owned = new Set([...modules].filter(([, mod]) => mod.isProcessor).map(([f]) => f));
  for (let changed = true; changed; ) {
    changed = false;
    for (const file of files) {
      if (owned.has(file)) continue;
      const from = importers.get(file);
      if (from.length && from.every((f) => owned.has(f))) {
        owned.add(file);
        changed = true;
      }
    }
  }

  const unitPublic = (file, unit) => {
    if (unit.cls) return publicNames.has(`${file}#${unit.cls}`) && !unit.privateMember;
    return publicNames.has(`${file}#${unit.name}`);
  };
  // Callers of each unit, across files.
  const callers = new Map();
  for (const [file, mod] of modules) {
    for (const c of mod.helperCalls) {
      let target = null;
      if (c.kind === 'this') target = `${file}#${c.cls}.${c.name}`;
      else if (c.kind === 'new') target = mod.units.has(`${c.name}#ctor`) ? `${file}#${c.name}#ctor` : null;
      else if (c.kind === 'ns') {
        const tfile = resolveImport(file, mod.namespaces.get(c.ns) || '');
        if (tfile && modules.has(tfile)) target = `${tfile}#${c.name}`;
      } else if (c.kind === 'member') {
        target = modules.has(c.file) ? `${c.file}#${c.id}` : null;
      } else if (mod.units.has(c.name)) target = `${file}#${c.name}`;
      if (!target && (c.kind === 'ident' || c.kind === 'new')) {
        const imp = mod.imports.get(c.name);
        const tfile = imp && resolveImport(file, imp.from);
        if (tfile && modules.has(tfile)) target = c.kind === 'new' ? `${tfile}#${imp.name}#ctor` : `${tfile}#${imp.name}`;
      }
      if (!target) continue;
      for (const id of [target, `${target}@get`]) {
        if (!callers.has(id)) callers.set(id, []);
        callers.get(id).push({ file, call: c });
      }
    }
  }
  const unitById = (id) => {
    const [file, ...rest] = id.split('#');
    const mod = modules.get(file);
    return mod ? mod.units.get(rest.join('#')) : undefined;
  };
  // Each unit resolves to the outermost public entries reaching it, split by path: `bare` paths
  // carry no withEntry, so the wrapper's own name for the key is what the consumer sees; `named`
  // paths pass through a withEntry, which renames whatever the wrapper said. A public unit is an
  // entry itself and also reachable from public callers, whose name wins (outermost entry).
  // A unit an @entryClass decorator wraps is a naming layer too: an undecorated public entry above
  // it does not rename, so the consumer would read the inner name; that path is `mislabeled`.
  const memo = new Map();
  const empty = () => ({ bare: new Set(), named: new Set(), mislabeled: new Set() });
  const resolveUnit = (file, unit, stack = new Set()) => {
    if (!unit) return empty();
    const id = `${file}#${unit.id}`;
    if (memo.has(id)) return memo.get(id);
    if (stack.has(id)) return empty();
    stack.add(id);
    const out = empty();
    const isPublic = unitPublic(file, unit);
    const naming = isPublic && (unit.named || cfg.portEndpoints.includes(unit.name));
    if (isPublic) (naming ? out.named : out.bare).add(unit.entry);
    for (const { file: cf, call } of callers.get(id) || []) {
      if (owned.has(cf)) continue;
      if (call.entryOverride) out.named.add(call.entryOverride);
      else if (call.unit) {
        const r = resolveUnit(cf, unitById(`${cf}#${call.unit}`), stack);
        for (const e of r.bare) {
          if (!naming) out.bare.add(e);
          else if (e !== unit.entry) out.mislabeled.add(`${e} (reads ${unit.entry})`);
        }
        for (const e of r.named) out.named.add(e);
        for (const e of r.mislabeled) out.mislabeled.add(e);
      }
    }
    stack.delete(id);
    memo.set(id, out);
    return out;
  };

  const keys = {};
  const unresolved = [];
  const mislabeled = [];
  let excludedSites = 0;
  let siteCount = 0;
  for (const [file, mod] of modules) {
    const rel = path.relative(root, file);
    for (const c of mod.nativeCalls) {
      if (owned.has(file)) {
        excludedSites += 1;
        continue;
      }
      siteCount += 1;
      let r;
      if (c.entryOverride) r = { ...empty(), named: new Set([c.entryOverride]) };
      else if (c.unit) r = resolveUnit(file, mod.units.get(c.unit));
      else r = empty();
      const site = `${rel}:${c.line}`;
      if (r.bare.size + r.named.size === 0) unresolved.push({ key: c.key, site, unit: c.unit });
      // `entries` holds the bare entries only: the names the wrapper itself must produce.
      const k = (keys[c.key] ||= { entries: {}, named: {}, sites: [] });
      for (const e of r.bare) {
        k.entries[e] = (k.entries[e] || 0) + 1;
        k.sites.push(`${site} -> ${e}`);
      }
      for (const e of r.named) {
        k.named[e] = (k.named[e] || 0) + 1;
        k.sites.push(`${site} (named) -> ${e}`);
      }
      for (const e of r.mislabeled) mislabeled.push(`${c.key} at ${site}: ${e}`);
    }
  }
  const natives = [...free, ...classes.keys()];
  for (const [cls, methods] of classes) for (const m of methods) natives.push(`${cls}.${m}`);
  const publicEntries = new Set();
  for (const [file, mod] of modules) for (const unit of mod.units.values()) if (unitPublic(file, unit)) publicEntries.add(unit.entry);
  const unitNames = new Set();
  for (const mod of modules.values()) for (const unit of mod.units.values()) if (unit.name) unitNames.add(unit.name);
  return {
    unenteredMissing: Object.keys(cfg.unentered).filter((n) => !unitNames.has(n)),
    scanned: files.length,
    sites: siteCount,
    excluded: [...owned].map((f) => path.relative(root, f)).sort(),
    excludedSites,
    natives: natives.sort(),
    publicEntries: [...publicEntries].sort(),
    keys,
    unresolved,
    mislabeled,
    decorated: [...modules].flatMap(([f, mod]) =>
      [...mod.decorated].map(([cls, d]) => ({ file: path.relative(root, f), cls, ...d }))),
  };
}

function scanFile(ts, src, { free, classes, methodClasses, cfg, checker, publishedTypes }) {
  const mods = (node) => (ts.canHaveModifiers(node) ? ts.getModifiers(node) || [] : []);
  const isExported = (node) => mods(node).some((m) => m.kind === ts.SyntaxKind.ExportKeyword);
  const isPrivateMember = (node) =>
    (node.name && node.name.kind === ts.SyntaxKind.PrivateIdentifier) ||
    mods(node).some((m) => m.kind === ts.SyntaxKind.PrivateKeyword || m.kind === ts.SyntaxKind.ProtectedKeyword);
  // A skip member names its own refusals with try/catch -> nameNativeError('<Class>.<member>').
  const skipNaming = (bodies, entry) => {
    let named = false;
    const bare = [];
    const visit = (n, inside) => {
      if (ts.isTryStatement(n) && n.catchClause) {
        const catches = [];
        const find = (c) => {
          if (ts.isCallExpression(c) && ts.isIdentifier(c.expression) && c.expression.text === 'nameNativeError' &&
              c.arguments[1] && ts.isStringLiteralLike(c.arguments[1]) && c.arguments[1].text === entry) catches.push(c);
          ts.forEachChild(c, find);
        };
        find(n.catchClause);
        if (catches.length) named = true;
        visit(n.tryBlock, inside || catches.length > 0);
        if (!catches.length) visit(n.catchClause, inside);
        if (n.finallyBlock) visit(n.finallyBlock, inside);
        return;
      }
      // A free-function call may refuse (a refuser or a shared check); a native member call is named by the table.
      if (!inside && ((ts.isCallExpression(n) && ts.isIdentifier(n.expression)) || ts.isThrowStatement(n))) {
        bare.push(src.getLineAndCharacterOfPosition(n.getStart(src)).line + 1);
      }
      ts.forEachChild(n, (c) => visit(c, inside));
    };
    for (const b of bodies) visit(b, false);
    return { named, bare: [...new Set(bare)] };
  };
  const out = {
    units: new Map(),
    nativeCalls: [],
    helperCalls: [],
    imports: new Map(),
    namespaces: new Map(),
    importSpecs: [],
    localExports: new Set(),
    reExports: [],
    isProcessor: false,
    decorated: new Map(),
  };
  const fileClasses = new Set();
  const typeAliases = new Map();
  const interfaceMembers = new Map();
  const memberNames = (members) =>
    members.filter((m) => m.name && (ts.isMethodSignature(m) || ts.isPropertySignature(m))).map((m) => m.name.getText(src));
  for (const st of src.statements) {
    if (ts.isTypeAliasDeclaration(st)) typeAliases.set(st.name.text, st.type.getText(src));
    if (ts.isInterfaceDeclaration(st)) interfaceMembers.set(st.name.text, memberNames(st.members));
    if (ts.isTypeAliasDeclaration(st) && ts.isTypeLiteralNode(st.type)) interfaceMembers.set(st.name.text, memberNames(st.type.members));
    if (ts.isImportDeclaration(st) && ts.isStringLiteral(st.moduleSpecifier)) {
      out.importSpecs.push(st.moduleSpecifier.text);
      const clause = st.importClause;
      if (clause && !clause.isTypeOnly && clause.namedBindings && ts.isNamespaceImport(clause.namedBindings)) {
        out.namespaces.set(clause.namedBindings.name.text, st.moduleSpecifier.text);
      }
      if (clause && !clause.isTypeOnly && clause.namedBindings && ts.isNamedImports(clause.namedBindings)) {
        for (const el of clause.namedBindings.elements) {
          if (el.isTypeOnly) continue;
          out.imports.set(el.name.text, { from: st.moduleSpecifier.text, name: (el.propertyName || el.name).text });
        }
      }
    }
    if (ts.isExportDeclaration(st) && st.moduleSpecifier && ts.isStringLiteral(st.moduleSpecifier)) {
      out.importSpecs.push(st.moduleSpecifier.text);
      if (st.isTypeOnly) continue;
      if (!st.exportClause) out.reExports.push({ from: st.moduleSpecifier.text, star: true });
      else if (ts.isNamedExports(st.exportClause)) {
        const names = st.exportClause.elements
          .filter((el) => !el.isTypeOnly)
          .map((el) => ({ name: (el.propertyName || el.name).text, as: el.name.text }));
        out.reExports.push({ from: st.moduleSpecifier.text, star: false, names });
      }
    } else if (ts.isExportDeclaration(st) && !st.moduleSpecifier && st.exportClause && ts.isNamedExports(st.exportClause)) {
      for (const el of st.exportClause.elements) if (!el.isTypeOnly) out.localExports.add((el.propertyName || el.name).text);
    }
    if ((ts.isFunctionDeclaration(st) || ts.isClassDeclaration(st)) && st.name && isExported(st)) out.localExports.add(st.name.text);
    if (ts.isVariableStatement(st) && isExported(st)) {
      for (const d of st.declarationList.declarations) if (ts.isIdentifier(d.name)) out.localExports.add(d.name.text);
    }
  }

  // `@entryClass('Name', ['skipped', ...])` on a class: every member it wraps names its own refusals.
  const entryClassOf = (cls) => {
    for (const d of (ts.canHaveDecorators(cls) && ts.getDecorators(cls)) || []) {
      const e = d.expression;
      if (!ts.isCallExpression(e) || !ts.isIdentifier(e.expression) || e.expression.text !== 'entryClass') continue;
      const [nameArg, skipArg] = e.arguments;
      const skip = skipArg && ts.isArrayLiteralExpression(skipArg)
        ? skipArg.elements.filter(ts.isStringLiteralLike).map((el) => el.text)
        : [];
      if (!out.decorated.has(cls.name.text)) {
        const name = nameArg && ts.isStringLiteralLike(nameArg) ? nameArg.text : null;
        const members = cls.members.filter((m) =>
          (ts.isMethodDeclaration(m) || ts.isGetAccessor(m) || ts.isSetAccessor(m)) && m.name && ts.isIdentifier(m.name));
        // TS `private` is public at runtime, so the decorator would wrap it and its name would stick.
        const unskippedPrivate = [...new Set(members.filter((m) => isPrivateMember(m) && !skip.includes(m.name.text))
          .map((m) => m.name.text))];
        // A private skip member is a helper its calling member names; a public one is a per-block entry.
        const skipped = skip.map((member) => {
          const own = members.filter((m) => m.name.text === member && m.body);
          const helper = own.length > 0 && own.every(isPrivateMember);
          return { member, helper, ...(helper ? { named: false, bare: [] } : skipNaming(own.map((m) => m.body), `${name}.${member}`)) };
        });
        out.decorated.set(cls.name.text, { name, skip, unskippedPrivate, skipped });
      }
      return { skip };
    }
    return null;
  };
  const unitOf = (node) => {
    if (ts.isFunctionDeclaration(node) && node.name) return { id: node.name.text, name: node.name.text, entry: node.name.text };
    if (ts.isVariableDeclaration(node) && node.initializer && ts.isIdentifier(node.name) &&
        (ts.isArrowFunction(node.initializer) || ts.isFunctionExpression(node.initializer)) &&
        node.parent && node.parent.parent && ts.isVariableStatement(node.parent.parent) &&
        ts.isSourceFile(node.parent.parent.parent)) {
      return { id: node.name.text, name: node.name.text, entry: node.name.text };
    }
    const cls = node.parent && ts.isClassLike(node.parent) && node.parent.name ? node.parent : null;
    if (!cls) return null;
    const cname = cls.name.text;
    const deco = entryClassOf(cls);
    if (ts.isConstructorDeclaration(node)) {
      return { id: `${cname}#ctor`, cls: cname, entry: cname, privateMember: isPrivateMember(node), named: !!deco };
    }
    if ((ts.isMethodDeclaration(node) || ts.isGetAccessor(node) || ts.isSetAccessor(node) ||
         (ts.isPropertyDeclaration(node) && node.initializer && ts.isArrowFunction(node.initializer))) &&
        node.name && !ts.isComputedPropertyName(node.name)) {
      const mname = node.name.getText(src).replace(/^#/, '');
      const kind = ts.isGetAccessor(node) ? '@get' : ts.isSetAccessor(node) ? '@set' : '';
      if (ts.isMethodDeclaration(node) && mname === 'process' && node.parameters.length >= 2 &&
          /^inputs?$/.test(node.parameters[0].name.getText(src))) {
        out.isProcessor = true;
      }
      // The decorator wraps prototype and static members; an arrow-function field is an instance value it never sees.
      const named = !!deco && !deco.skip.includes(mname) && !ts.isPropertyDeclaration(node) && !isPrivateMember(node);
      return { id: `${cname}.${mname}${kind}`, cls: cname, entry: `${cname}.${mname}`, privateMember: isPrivateMember(node), named };
    }
    return null;
  };
  const enclosing = (node) => {
    let unit = null;
    let entryOverride = null;
    // withEntry names what runs inside its callback, so a function defined there and called later
    // (a returned closure, an event handler) is outside it: only the callback itself may be crossed.
    const crossed = [];
    for (let n = node.parent; n; n = n.parent) {
      if (!unit) {
        const u = unitOf(n);
        if (u) unit = u.id;
      }
      if (!entryOverride && ts.isCallExpression(n) && ts.isIdentifier(n.expression) && n.expression.text === 'withEntry' &&
          n.arguments.length > 1 && ts.isStringLiteralLike(n.arguments[0]) &&
          crossed.length <= 1 && crossed.every((f) => f === n.arguments[1])) {
        entryOverride = n.arguments[0].text;
      }
      if (ts.isFunctionLike(n)) crossed.push(n);
    }
    return { unit, entryOverride };
  };
  const classFromText = (text, depth = 0) => {
    if (!text) return null;
    let m = /(?:addon|module|requireModule\(\)|getSonareModule\(\))\.(\w+)/.exec(text);
    if (m && classes.has(m[1])) return m[1];
    m = /\bWasm(\w+)\b/.exec(text);
    if (m && classes.has(m[1])) return m[1];
    const id = /^\s*(\w+)\s*(?:\|\s*(?:null|undefined))*\s*$/.exec(text);
    if (!id) return null;
    if (typeAliases.has(id[1]) && depth < 3) {
      const viaAlias = classFromText(typeAliases.get(id[1]), depth + 1);
      if (viaAlias) return viaAlias;
    }
    // A hand-written interface standing for a native class: the one native class holding every member.
    const members = interfaceMembers.get(id[1]);
    if (members && members.length) {
      const fits = [...classes].filter(([, ms]) => members.every((m) => ms.has(m))).map(([c]) => c);
      if (fits.length === 1) return fits[0];
    }
    if (classes.has(id[1])) return id[1];
    return null;
  };
  const NOT_NATIVE = Symbol('not-native');
  const FUNCTION_METHODS = new Set(['apply', 'call', 'bind']);
  // Only a JS built-in collection is known not to be a native; a facade interface may alias one.
  const BUILTIN = /^\s*(?:new\s+)?(?:Readonly)?(?:Set|Map|WeakMap|WeakSet|Array|Promise)\b|\[\]\s*$/;
  const typedAs = (text) => (text ? classFromText(text) || (BUILTIN.test(text) ? NOT_NATIVE : null) : null);
  const declaredText = (decl) => (decl.type ? decl.type.getText(src) : decl.initializer ? decl.initializer.getText(src) : null);
  // A receiver whose type is a facade class, or an object type no native class satisfies, is not a native:
  // the facade's own RealtimeEngine shares the native class's name and method names.
  // Callable members, without the embind handle's own (delete, clone, isDeleted, deleteLater).
  const HANDLE_METHODS = new Set(['delete', 'clone', 'isDeleted', 'deleteLater']);
  const callableMembers = (type) =>
    type.getProperties()
      .filter((p) => !HANDLE_METHODS.has(p.getName()) && p.valueDeclaration &&
        checker.getTypeOfSymbolAtLocation(p, p.valueDeclaration).getCallSignatures().length > 0)
      .map((p) => p.getName());
  const facadeOwned = (recv) => {
    const type = checker.getNonNullableType(checker.getTypeAtLocation(recv));
    const sym = type.aliasSymbol || type.getSymbol();
    const decl = sym && sym.declarations && sym.declarations[0];
    if (!decl || decl.getSourceFile().isDeclarationFile) return false;
    if (ts.isClassLike(decl)) return true;
    // A type the package publishes is the consumer's object (an ExternalInstrument), never a native alias;
    // an unpublished one is a native alias only under the Native/Wasm naming the facades use for them.
    return publishedTypes.has(sym) || !/Native|Wasm/.test(sym.getName());
  };
  // Fallback: the checker's view of the receiver's type, read the same way as a declared type.
  const checkedClass = (recv) => {
    const type = checker.getTypeAtLocation(recv);
    const text = checker.typeToString(type, undefined, ts.TypeFormatFlags.NoTruncation | ts.TypeFormatFlags.UseFullyQualifiedType);
    const short = checker.typeToString(type);
    const named = typedAs(short) || typedAs(text.replace(/^.*\./, ''));
    if (named) return named;
    // Structural: the one native class holding every member of an object type declared elsewhere.
    const props = type.isUnion() ? [] : callableMembers(type);
    if (props.length && (type.getFlags() & ts.TypeFlags.Object)) {
      const fits = [...classes].filter(([, ms]) => props.every((m) => ms.has(m))).map(([c]) => c);
      if (fits.length === 1) return fits[0];
    }
    return null;
  };
  const receiverClass = (recv) => {
    if (ts.isPropertyAccessExpression(recv) && recv.expression.kind === ts.SyntaxKind.ThisKeyword) {
      const field = recv.name.text;
      for (let n = recv.parent; n; n = n.parent) {
        if (!ts.isClassLike(n)) continue;
        for (const member of n.members) {
          if ((ts.isPropertyDeclaration(member) || ts.isGetAccessor(member)) && member.name &&
              member.name.getText(src).replace(/^#/, '') === field) {
            return typedAs(declaredText(member));
          }
          if (ts.isConstructorDeclaration(member)) {
            for (const prm of member.parameters) if (prm.name.getText(src) === field) return typedAs(declaredText(prm));
          }
        }
        return null;
      }
    }
    if (ts.isIdentifier(recv)) {
      for (let n = recv.parent; n; n = n.parent) {
        let found = null;
        if (ts.isFunctionLike(n) && n.parameters) for (const prm of n.parameters) if (prm.name.getText(src) === recv.text) found = prm;
        if (!found && (ts.isBlock(n) || ts.isSourceFile(n))) {
          for (const st of n.statements) {
            if (!ts.isVariableStatement(st)) continue;
            for (const d of st.declarationList.declarations) if (d.name.getText(src) === recv.text) found = d;
          }
        }
        if (found) return typedAs(declaredText(found));
      }
    }
    return null;
  };

  const visit = (node) => {
    if (ts.isClassLike(node) && node.name) fileClasses.add(node.name.text);
    const u = unitOf(node);
    if (u && !out.units.has(u.id)) out.units.set(u.id, u);
    if (ts.isPropertyAccessExpression(node)) {
      const recv = node.expression.getText(src);
      const name = node.name.text;
      let key = null;
      if (cfg.moduleReceivers.has(recv) && (free.has(name) || classes.has(name))) {
        key = name;
      } else if (recv !== 'this' && !cfg.moduleReceivers.has(recv) && methodClasses.has(name) &&
                 ts.isCallExpression(node.parent) && node.parent.expression === node) {
        let owners = methodClasses.get(name);
        const nsReceiver = ts.isIdentifier(node.expression) && out.namespaces.has(node.expression.text);
        let typed = nsReceiver || facadeOwned(node.expression) ? NOT_NATIVE : receiverClass(node.expression);
        // `fn.apply(target, args)` on an untyped function value is Function.prototype.apply, not a native method.
        if (!typed && FUNCTION_METHODS.has(name)) typed = NOT_NATIVE;
        if (!typed) typed = checkedClass(node.expression);
        if (typed === NOT_NATIVE) owners = [];
        else if (typed && owners.includes(typed)) owners = [typed];
        if (owners.length > 1) {
          const local = owners.filter((c) => fileClasses.has(c));
          if (local.length) owners = local;
        }
        if (owners.length === 1) key = `${owners[0]}.${name}`;
        else if (owners.length > 1) key = `?{${owners.join('|')}}.${name}`;
      }
      if (key) {
        const line = src.getLineAndCharacterOfPosition(node.getStart(src)).line + 1;
        out.nativeCalls.push({ key, line, ...enclosing(node) });
      }
    }
    if (ts.isCallExpression(node)) {
      const e = node.expression;
      if (ts.isIdentifier(e)) out.helperCalls.push({ kind: 'ident', name: e.text, ...enclosing(node) });
      else if (ts.isPropertyAccessExpression(e) && ts.isIdentifier(e.expression) && out.namespaces.has(e.expression.text)) {
        out.helperCalls.push({ kind: 'ns', ns: e.expression.text, name: e.name.text, ...enclosing(node) });
      } else if (ts.isPropertyAccessExpression(e) && e.expression.kind !== ts.SyntaxKind.ThisKeyword) {
        // A method of a facade class called through an instance (SonareEngine forwarding to its RealtimeEngine).
        const decl = (checker.getSymbolAtLocation(e.name)?.declarations || [])[0];
        const cls = decl && decl.parent && ts.isClassLike(decl.parent) && decl.parent.name ? decl.parent.name.text : null;
        if (cls && !decl.getSourceFile().isDeclarationFile && (ts.isMethodDeclaration(decl) || ts.isPropertyDeclaration(decl))) {
          out.helperCalls.push({ kind: 'member', file: decl.getSourceFile().fileName, id: `${cls}.${e.name.text}`, ...enclosing(node) });
        }
      }
      else if (ts.isPropertyAccessExpression(e) && e.expression.kind === ts.SyntaxKind.ThisKeyword) {
        const en = enclosing(node);
        const cls = en.unit ? en.unit.split(/[.#]/)[0] : null;
        if (cls) out.helperCalls.push({ kind: 'this', cls, name: e.name.text, ...en });
      }
    }
    if (ts.isPropertyAccessExpression(node) && node.expression.kind === ts.SyntaxKind.ThisKeyword &&
        !(ts.isCallExpression(node.parent) && node.parent.expression === node)) {
      const en = enclosing(node);
      const cls = en.unit ? en.unit.split(/[.#]/)[0] : null;
      if (cls) out.helperCalls.push({ kind: 'this', cls, name: node.name.text, ...en });
    }
    if (ts.isNewExpression(node) && ts.isIdentifier(node.expression)) {
      out.helperCalls.push({ kind: 'new', name: node.expression.text, ...enclosing(node) });
    }
    ts.forEachChild(node, visit);
  };
  visit(src);
  return out;
}

// The table maps a key to the one entry reaching it, where that entry is not the key itself.
function tableFor(d) {
  const table = {};
  for (const [key, k] of Object.entries(d.keys)) {
    const entries = Object.keys(k.entries);
    if (entries.length === 1 && entries[0] !== key && !key.startsWith('?')) table[key] = entries[0];
  }
  return table;
}

function renderTable(table) {
  const prop = (k) => (/^[A-Za-z_$][\w$]*$/.test(k) ? k : `'${k}'`);
  const lines = Object.keys(table)
    .sort()
    .map((k) => {
      const line = `  ${prop(k)}: '${table[k]}',`;
      return line.length <= 100 ? line : `  ${prop(k)}:\n    '${table[k]}',`;
    });
  return [
    '// Generated by tests/conformance/native_entries.cjs --write; do not edit.',
    '/** Public entry name for each native key whose own name is not the one a consumer called. */',
    lines.length
      ? `export const NATIVE_ENTRIES: Readonly<Record<string, string>> = {\n${lines.join('\n')}\n};`
      : 'export const NATIVE_ENTRIES: Readonly<Record<string, string>> = {};',
    '',
  ].join('\n');
}

const out = {};
for (const surface of Object.keys(SURFACES)) out[surface] = derive(surface);

if (flags.includes('--write')) {
  for (const [surface, d] of Object.entries(out)) {
    const file = path.join(root, SURFACES[surface].table);
    fs.writeFileSync(file, renderTable(tableFor(d)));
    process.stdout.write(`${surface}: wrote ${Object.keys(tableFor(d)).length} entries to ${SURFACES[surface].table}\n`);
  }
} else if (flags.includes('--check')) {
  let failed = false;
  const fail = (line) => {
    failed = true;
    process.stdout.write(`  FAIL ${line}\n`);
  };
  for (const [surface, d] of Object.entries(out)) {
    const keys = Object.entries(d.keys);
    const shared = keys.filter(([k, v]) => !k.startsWith('?') && Object.keys(v.entries).length > 1);
    const ambiguous = keys.filter(([k]) => k.startsWith('?'));
    const used = new Set(keys.map(([k]) => k));
    const dead = d.natives.filter((n) => !used.has(n));
    const table = tableFor(d);
    process.stdout.write(
      `${surface}: scanned=${d.scanned} sites=${d.sites} excluded-modules=${d.excluded.length} ` +
        `excluded-sites=${d.excludedSites} renames=${Object.keys(table).length} shared=${shared.length} ` +
        `ambiguous=${ambiguous.length} unresolved=${d.unresolved.length} dead-natives=${dead.length} ` +
        `decorated=${d.decorated.length} mislabeled=${d.mislabeled.length}\n`,
    );
    for (const e of SURFACES[surface].portEndpoints) process.stdout.write(`  port endpoint ${e}\n`);
    for (const [n, why] of Object.entries(SURFACES[surface].unentered)) process.stdout.write(`  unentered ${n} (${why})\n`);
    for (const n of d.unenteredMissing) fail(`unentered ${n} names no function; drop it`);
    for (const c of d.decorated) {
      if (c.name !== c.cls) fail(`${c.file}: @entryClass('${c.name}') on class ${c.cls}`);
      if (c.skip.length) {
        const entries = c.skipped.filter((k) => !k.helper);
        const helpers = c.skipped.filter((k) => k.helper);
        const label = (k) => `${k.member}${k.named ? ' (names its refusals)' : ''}`;
        if (entries.length) process.stdout.write(`  skip ${c.cls}: ${entries.map(label).join(', ')}\n`);
        if (helpers.length) process.stdout.write(`  private ${c.cls}: ${helpers.map((k) => k.member).join(', ')}\n`);
      }
      for (const k of c.skipped) {
        if (k.bare.length) fail(`${c.file}:${k.bare.join(',')}: skip member ${c.cls}.${k.member} refuses outside try/catch -> nameNativeError('${c.name}.${k.member}')`);
      }
      if (c.unskippedPrivate.length) fail(`${c.file}: private members of ${c.cls} not on its skip list: ${c.unskippedPrivate.join(', ')}`);
    }
    for (const m of d.mislabeled) fail(`${m}; decorate the outer entry or name it with withEntry`);
    for (const f of d.excluded) process.stdout.write(`  excluded ${f}\n`);
    if (d.scanned === 0 || d.sites === 0) fail(`${surface}: nothing scanned`);
    for (const [k, v] of shared) fail(`${k} is reached by ${Object.keys(v.entries).join(', ')}; name the entry with withEntry`);
    for (const [k, v] of ambiguous) fail(`${k} has an untyped receiver at ${v.sites[0].split(' -> ')[0]}`);
    for (const u of d.unresolved) fail(`${u.key} at ${u.site} is reached by no public entry`);
    const file = path.join(root, SURFACES[surface].table);
    const held = fs.existsSync(file) ? fs.readFileSync(file, 'utf8') : '';
    if (held !== renderTable(table)) fail(`${SURFACES[surface].table} differs from the derivation; run --write`);
  }
  process.exitCode = failed ? 1 : 0;
} else {
  process.stdout.write(`${JSON.stringify(out, null, 1)}\n`);
}
