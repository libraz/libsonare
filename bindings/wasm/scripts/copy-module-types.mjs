import { writeFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';

// `dist/sonare.js` is the emscripten artefact, copied into dist/ by the CMake
// build; its declarations are the hand-written `src/sonare.js.d.ts`. The emitted
// `dist/*.d.ts` import types from './sonare.js', which resolves to
// `dist/sonare.d.ts`, and nothing else puts that file there: `src/sonare.js.d.ts`
// is a declaration input, so tsc consumes it rather than emitting it into dist/.
// Without this the import dangles for anyone consuming the package.
//
// A re-export rather than a copy: the declaration pulls further types from
// `./public_types`, which only resolves next to the source, and the package
// ships `src/` alongside `dist/`.
const shim = `export * from '../src/sonare.js';
export { default } from '../src/sonare.js';
`;

const output = new URL('../dist/sonare.d.ts', new URL('.', import.meta.url));
await writeFile(fileURLToPath(output), shim);
