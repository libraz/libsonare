/** Request-object versus positional equivalence for mastering, metering and mixing; the harness is `request_equivalence/harness.ts`. */

import { describe } from 'vitest';
import { describeRows } from './request_equivalence/harness';
import { ROWS } from './request_equivalence/rows_mastering';

describe('request-object equivalence', () => {
  describeRows(ROWS);
});
