/** Request-object versus positional equivalence for music, pitch, onset and decomposition features; the harness is `request_equivalence/harness.ts`. */

import { describe } from 'vitest';
import { describeRows } from './request_equivalence/harness';
import { ROWS } from './request_equivalence/rows_features';

describe('request-object equivalence', () => {
  describeRows(ROWS);
});
