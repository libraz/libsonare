# Security policy

## Reporting a vulnerability

Report privately, not through the public issue tracker:

- **Preferred:** GitHub's private vulnerability reporting, from the repository's
  [Security tab](https://github.com/libraz/libsonare/security/advisories/new).
- **Alternative:** email `libraz@libraz.net`.

Include the file that triggers it, what happened, the affected version and
binding, and a minimal reproduction if you have one. Expect an acknowledgement
within a few days.

## Supported versions

Fixes land on the latest minor of the current major line and on the minor before
it, so a deployment has one minor release of room to upgrade.

| Version | Supported |
|---------|-----------|
| v1.x    | latest minor + previous minor |
| v0.x    | unsupported |

## What is in scope

libsonare decodes audio, MIDI, SoundFonts and project files it did not produce,
so every decoder is the interesting surface. In scope:

- A crafted audio file, MIDI file, SoundFont or project file that causes a
  crash, a hang, unbounded memory growth, or a read or write outside an
  allocation.
- The same through any binding — the WebAssembly build, the Python wheel, the
  Node addon or the CLI.
- Anything that escapes the WebAssembly sandbox, or that lets a loaded file
  reach the network or a path the caller did not open.

## What is not in scope

- Wrong analysis output. A misdetected key, tempo or pitch is an accuracy bug;
  report it as a normal issue.
- Documented limits behaving as documented — sample-count, duration and
  polyphony caps exist so a hostile file cannot exhaust the host. A limit that
  can be bypassed is in scope.
- Vulnerabilities in the codecs supplied by the host platform rather than by
  this project.
- Findings that require an attacker to already control the process embedding the
  library.
