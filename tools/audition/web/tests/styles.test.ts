/* Every class the components write is styled by something.
 *
 * A class with no rule is a region that silently lost its styling in a rename,
 * and nothing renders red for it. Class names are read from `className`
 * literals and the static parts of template literals and `roleClass` calls. */

import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join } from 'node:path';
import { describe, expect, it } from 'vitest';
import { ROLE_ORDER } from '../src/state/selectors';

const SRC = join(__dirname, '..', 'src');

function files(dir: string, ext: RegExp): string[] {
  return readdirSync(dir).flatMap((name) => {
    const p = join(dir, name);
    if (statSync(p).isDirectory()) {
      return files(p, ext);
    }
    return ext.test(name) ? [p] : [];
  });
}

const css = files(join(SRC, 'styles'), /\.css$/)
  .map((f) => readFileSync(f, 'utf8'))
  .join('\n')
  .replace(/\/\*[\s\S]*?\*\//g, '');
const styled = new Set([...css.matchAll(/\.(-?[_a-zA-Z][\w-]*)/g)].map((m) => m[1]));

/** Class names written by the components: literals, and the literal words of a template. */
function usedClasses(): Map<string, string> {
  const out = new Map<string, string>();
  for (const file of files(SRC, /\.tsx?$/)) {
    const text = readFileSync(file, 'utf8');
    const rel = file.slice(SRC.length + 1);
    const plain = [...text.matchAll(/className=\{?(["'])([^"']*)\1/g)].map((m) => m[2] ?? '');
    const templates = [...text.matchAll(/className=\{`([^`]*)`\}/g)].map((m) => m[1] ?? '');
    // Inside a template, the words around `${…}` and the quoted words inside it are classes; a role name passed to roleClass is not.
    const quoted = templates.flatMap((tpl) =>
      [...tpl.matchAll(/\$\{([^}]*)\}/g)].flatMap((m) =>
        [...(m[1] ?? '').replace(/roleClass\([^)]*\)/g, '').matchAll(/'([^']*)'/g)].map(
          (q) => q[1] ?? '',
        ),
      ),
    );
    const values = [
      ...plain,
      ...templates.map((tpl) => tpl.replace(/\$\{[^}]*\}/g, ' ')),
      ...quoted,
    ];
    for (const value of values) {
      for (const word of value.split(/\s+/)) {
        if (/^-?[_a-zA-Z][\w-]*$/.test(word)) {
          out.set(word, rel);
        }
      }
    }
  }
  return out;
}

describe('the stylesheets', () => {
  it('style every class a component writes', () => {
    const used = usedClasses();
    expect(used.size).toBeGreaterThan(50);
    const missing = [...used]
      .filter(([cls]) => !styled.has(cls))
      .map(([cls, file]) => `${cls} (${file})`);
    expect(missing).toEqual([]);
  });

  it('carry a colour for every role and for the unlabelled one', () => {
    for (const role of [...ROLE_ORDER, 'other']) {
      expect(styled.has(`role-${role}`), role).toBe(true);
      expect(css).toContain(`--role-${role}-rgb`);
    }
  });

  it('key the Japanese layout on the document language, never on a script-toggled class', () => {
    expect(css).not.toMatch(/lang-ja/);
    expect(css).toMatch(/:lang\(ja\)/);
  });

  it('style the body states the page toggles', () => {
    for (const state of ['bank-view', 'run-view', 'blind-on', 'no-compare', 'run-pending']) {
      expect(css, state).toMatch(new RegExp(`body\\.${state}\\b`));
    }
  });
});
