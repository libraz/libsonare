import { readdirSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { describe, expect, it } from 'vitest';
import { addonSources } from './_addon_sources.js';

/**
 * Audio-thread C-ABI entry points neither clear nor record
 * `sonare_last_error_message` — first-touch TLS setup would break their
 * no-allocation contract — so after one of them fails, that slot still holds
 * whatever an earlier control-thread call left behind. Reporting it describes an
 * unrelated call, and does so intermittently, since it follows the call history
 * rather than the failure. The addon must therefore route every call into that
 * set through `ThrowIfRealtimeError`, which maps the code alone, and never
 * through `ThrowIfError` / `ThrowSonareError`.
 *
 * The set is read out of the C++ rather than listed here: `SONARE_C_RT_API_ENTRY`
 * is the marker the core itself uses, so a newly marked entry point joins this
 * check without anyone remembering to add it.
 */
const C_API_ROOT = new URL('../../../src/c_api/', import.meta.url).pathname;

/** Strips line comments so prose naming a symbol is not read as a call. */
function withoutLineComments(text: string): string {
  return text.replace(/\/\/.*$/gm, '');
}

function realtimeEntryPoints(): string[] {
  const names = new Set<string>();
  for (const file of readdirSync(C_API_ROOT)) {
    if (!file.endsWith('.cpp')) continue;
    const text = withoutLineComments(readFileSync(join(C_API_ROOT, file), 'utf8'));
    for (const match of text.matchAll(/SONARE_C_RT_API_ENTRY/g)) {
      // The enclosing definition is the last C-ABI name introduced before the
      // marker; parameter types are `SonareX`, which does not match `sonare_`.
      const preceding = [
        ...text.slice(0, match.index ?? 0).matchAll(/\b(sonare_[a-z0-9_]+)\s*\(/g),
      ];
      const owner = preceding[preceding.length - 1];
      if (owner) names.add(owner[1]);
    }
  }
  return [...names].sort();
}

/** Every addon call site of @p name, with the statement text it sits in. */
function callSites(name: string): Array<{ file: string; line: number; statement: string }> {
  const sites: Array<{ file: string; line: number; statement: string }> = [];
  const pattern = new RegExp(`\\b${name}\\s*\\(`, 'g');
  for (const { file, text } of addonSources()) {
    const stripped = withoutLineComments(text);
    for (const match of stripped.matchAll(pattern)) {
      const start = match.index ?? 0;
      const head = stripped.slice(0, start);
      // The statement starts after the nearest preceding `;`, `{` or `}`.
      const statementStart = Math.max(
        head.lastIndexOf(';'),
        head.lastIndexOf('{'),
        head.lastIndexOf('}'),
      );
      sites.push({
        file,
        line: head.split('\n').length,
        statement: stripped.slice(statementStart + 1, start),
      });
    }
  }
  return sites;
}

describe('audio-thread C-ABI calls never report the stale thread-local message', () => {
  it('reads a non-empty realtime entry-point set out of the C++, so the check is not vacuous', () => {
    const names = realtimeEntryPoints();
    expect(names.length).toBeGreaterThanOrEqual(6);
    // Spot-check both families the marker covers; if the extraction regex ever
    // stops resolving the enclosing definition, these disappear first.
    expect(names).toContain('sonare_engine_process');
    expect(names).toContain('sonare_realtime_voice_changer_process_mono');
  });

  it('resolves the addon call sites it is meant to police', () => {
    const total = realtimeEntryPoints().reduce((sum, name) => sum + callSites(name).length, 0);
    expect(total).toBeGreaterThan(0);
  });

  it('routes every addon call through ThrowIfRealtimeError', () => {
    const offenders: string[] = [];
    for (const name of realtimeEntryPoints()) {
      for (const site of callSites(name)) {
        if (!site.statement.includes('ThrowIfRealtimeError')) {
          offenders.push(`${site.file}:${site.line} ${name}(...)`);
        }
      }
    }
    expect(
      offenders,
      'An audio-thread C-ABI call must be wrapped in ThrowIfRealtimeError (sonare_wrap_utils.h), ' +
        'not ThrowIfError: the failing call records no message of its own, so the thread-local ' +
        'one belongs to an unrelated earlier call. The check reads the statement around the ' +
        'call, so keep the call inside the ThrowIfRealtimeError(...) argument list rather than ' +
        'storing the code first.',
    ).toEqual([]);
  });
});
