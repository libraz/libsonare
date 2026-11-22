/**
 * An ordinal the native library returns that the binding's table does not know is a coded
 * `Unknown` error naming the token family, never an argument refusal: the caller passed nothing.
 */

import { describe, expect, it } from 'vitest';
import { ErrorCode, SonareError } from '../src/errors.js';
import { tokenName } from '../src/internal/project_tokens.js';

const TABLE = { off: 0, loop: 1 } as const;

describe('tokenName', () => {
  it('maps a known ordinal to its token', () => {
    expect(tokenName(TABLE, 1, 'loop mode')).toBe('loop');
  });

  it('reports an unknown ordinal as a coded Unknown error', () => {
    let caught: unknown;
    try {
      tokenName(TABLE, 7, 'loop mode');
    } catch (error) {
      caught = error;
    }
    expect(caught).toBeInstanceOf(SonareError);
    expect((caught as SonareError).code).toBe(ErrorCode.Unknown);
    expect((caught as Error).message).toBe('unknown loop mode ordinal 7 from the native library');
  });
});
