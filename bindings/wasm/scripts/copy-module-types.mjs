import { writeFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';

// `dist/sonare.js` is the emscripten artefact, copied into dist/ by the CMake
// build; its declarations are the hand-written `src/sonare.js.d.ts`. tsup marks
// './sonare.js' external so it never emits one, yet the bundled `dist/*.d.ts`
// import types from './sonare.js' — which resolves to `dist/sonare.d.ts`.
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
