/** Request-object versus positional equivalence for effects and repair; the harness is `request_equivalence/harness.ts`. */

import { describe } from 'vitest';
import { describeRows } from './request_equivalence/harness';
import { ROWS } from './request_equivalence/rows_effects';

describe('request-object equivalence', () => {
  describeRows(ROWS);
});
