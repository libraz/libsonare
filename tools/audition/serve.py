#!/usr/bin/env python3
"""Serve directories of audio renders as an A/B listening page.

    python tools/audition/serve.py [<audition-dir> ...]

Opens a browser on a page that plays every version of a take from one
transport, so switching between them is instant. Two renders compared by
stopping one and starting the other are compared across a gap of a second or
two, which is long enough for the ear to lose what it was holding; here they
run together and the switch costs nothing.

A switch seeks back to the start by default, so the attack is the first thing
heard. Turning that off leaves the versions sample-aligned instead — the switch
becomes a plain gain change on a sound that never stopped, which is what a
sustain or a decay has to be compared on.

More than one directory can be served at once, and the page gets a control to
move between them. Each is a separate instrument or a separate experiment: a
piano set and a harpsichord set have different takes and different references,
so they are separate sets rather than one long list, and one server serves both.

WHAT IS SERVED, AND WHAT IS NOT. Given no directory, the sets are discovered
under the scratch root the rest of the harness uses — `.cache/voicematch/` in
the checkout, or wherever `SONARE_VOICEMATCH_ROOT` points. None of it is
committed, which matters for anyone who has only cloned the repository: there
is nothing there until they render something, and the reference side cannot be
rendered at all without the commercial plugin it captures. So a set holding a
single version of each take is expected rather than broken, and the page turns
the comparison controls off for it and plays instead. Nothing here requires a
reference to exist.

The directory needs a `manifest.json` naming the takes and their versions. If
there is none, one is inferred from the layout — each subdirectory is a take
and each WAV inside it is a version of it — so a directory somebody assembled
by hand still opens.

Nothing here is specific to any instrument or to this repository: point it at
any directory of renders. Only the standard library is used, so it runs from
any interpreter without an environment.
"""

from __future__ import annotations

import argparse
import http.server
import json
import os
import re
import socketserver
import sys
import threading
import webbrowser
from pathlib import Path

APP_DIR = Path(__file__).resolve().parent
REPO_ROOT = APP_DIR.parents[1]
AUDIO_SUFFIXES = (".wav", ".flac", ".mp3", ".ogg", ".m4a", ".aac")
#: The scratch root the whole harness renders into, and the one `capture.py`
#: reads. Untracked on purpose — the reference side of a comparison is captured
#: from a commercial plugin and cannot be redistributed — so a fresh clone finds
#: nothing here, which is a supported state rather than a failure.
SCRATCH_ROOT = Path(
    os.environ.get("SONARE_VOICEMATCH_ROOT") or REPO_ROOT / ".cache" / "voicematch"
).expanduser()
#: Searched under the scratch root, in order. The second form is for a root that
#: holds one set directly rather than a directory of them.
FALLBACK_GLOBS = ("audition/*", "audition", "*/audition")


def infer_manifest(root: Path) -> dict:
    """Build a manifest from a directory that has none.

    A subdirectory holding two or more audio files is a take whose versions are
    those files. A directory of loose files is one take per file, which gives a
    page that plays rather than compares — still useful, and honest about
    having nothing to compare against.
    """
    items = []
    for sub in sorted(p for p in root.iterdir() if p.is_dir()):
        tracks = {p.stem: str(p.relative_to(root)) for p in sorted(sub.iterdir())
                  if p.suffix.lower() in AUDIO_SUFFIXES}
        if tracks:
            items.append({"id": sub.name, "label": sub.name, "tracks": tracks})
    loose = [p for p in sorted(root.iterdir())
             if p.is_file() and p.suffix.lower() in AUDIO_SUFFIXES]
    for p in loose:
        items.append({"id": p.stem, "label": p.stem, "tracks": {p.stem: p.name}})
    return {
        "title": root.name,
        "notes": "Inferred from the directory layout; no manifest.json was found.",
        "items": items,
    }


def read_manifest(root: Path) -> dict:
    explicit = root / "manifest.json"
    if explicit.exists():
        return json.loads(explicit.read_text())
    return infer_manifest(root)


def set_id(root: Path) -> str:
    """A short URL-safe name for a set.

    Taken from the parent directory when the leaf says nothing — `pianolab` and
    `harpsichordlab` distinguish two sets that are both called `audition`.
    """
    name = root.name
    if name in ("audition", "renders", "out") and root.parent != root:
        name = root.parent.name
    return re.sub(r"[^a-zA-Z0-9._-]+", "-", name).strip("-") or "set"


def discover(paths: list[str]) -> list[Path]:
    """The directories to serve: the ones named, else whatever the scratch root holds."""
    if paths:
        return [Path(p).expanduser().resolve() for p in paths]
    found: list[Path] = []
    for pattern in FALLBACK_GLOBS:
        found += [p.resolve() for p in sorted(SCRATCH_ROOT.glob(pattern)) if p.is_dir()]
    unique = list(dict.fromkeys(found))
    # A set is a leaf. Two of these globs can match a directory and its parent,
    # and the parent holds no renders of its own — offering it produces a name
    # in the picker that plays nothing, reported as a skip nobody caused.
    return [p for p in unique if not any(p in other.parents for other in unique)]


class Sets:
    """The served sets, by id, and the index the page reads to list them."""

    by_id: dict[str, Path] = {}
    index: list[dict] = []

    @classmethod
    def load(cls, roots: list[Path]) -> None:
        cls.by_id = {}
        cls.index = []
        for root in roots:
            if not root.is_dir():
                print(f"skipping (not a directory): {root}", file=sys.stderr)
                continue
            ident = set_id(root)
            # Two directories with the same leaf name would otherwise shadow
            # each other, and the second would silently never be reachable.
            base = ident
            n = 2
            while ident in cls.by_id:
                ident = f"{base}-{n}"
                n += 1
            manifest = read_manifest(root)
            items = manifest.get("items", [])
            if not items:
                # An empty directory is not a set. Listing it would put a name in
                # the picker that plays nothing.
                print(f"skipping (no renders in it): {root}", file=sys.stderr)
                continue
            # A set whose takes each hold one version has nothing to compare, so
            # the page drops the comparison controls rather than showing a
            # switcher with one entry.
            compare = any(len(it.get("tracks", {})) > 1 for it in items)
            cls.by_id[ident] = root
            cls.index.append({
                "id": ident,
                "title": manifest.get("title") or root.name,
                "takes": len(items),
                "compare": compare,
                "path": str(root),
            })


class Handler(http.server.SimpleHTTPRequestHandler):
    """Serve the app from `tools/audition/` and the audio from the render dirs."""

    app_dir = APP_DIR

    def _set_and_rest(self, rel: str) -> tuple[Path | None, str]:
        """Split `s/<id>/<rest>` into the set's root and the path inside it."""
        if not rel.startswith("s/"):
            return None, rel
        parts = rel[2:].split("/", 1)
        root = Sets.by_id.get(parts[0])
        return root, (parts[1] if len(parts) > 1 else "")

    def _resolve(self, rel: str) -> Path | None:
        """The file a request names, or None if it names nothing servable.

        A path that resolves outside its own set's directory is None rather than
        an error page's worth of detail: `..` in a URL is not a mistake anyone
        makes by accident.
        """
        if rel in ("", "index.html"):
            return self.app_dir / "index.html"
        if rel in ("app.js", "style.css"):
            return self.app_dir / rel
        root, rest = self._set_and_rest(rel)
        if root is None or not rest:
            return None
        target = (root / rest).resolve()
        return target if root in target.parents else None

    def translate_path(self, path: str) -> str:
        rel = path.split("?", 1)[0].split("#", 1)[0].lstrip("/")
        target = self._resolve(rel)
        # Unreachable in practice: do_GET answers before this is consulted. The
        # path keeps SimpleHTTPRequestHandler's other verbs from serving the
        # process's working directory if one is ever added.
        return str(target) if target is not None else str(self.app_dir / "index.html")

    def _json(self, payload) -> None:
        body = json.dumps(payload).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:  # noqa: N802 - http.server's spelling
        rel = self.path.split("?", 1)[0].lstrip("/")
        if rel == "sets.json":
            self._json(Sets.index)
            return
        root, rest = self._set_and_rest(rel)
        if root is not None and rest == "manifest.json":
            self._json(read_manifest(root))
            return
        if self._resolve(rel) is None:
            self.send_error(404, "not found")
            return
        super().do_GET()

    def end_headers(self) -> None:
        # A render is overwritten in place by the next tuning iteration, and a
        # cached one would be audited instead of the new one.
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def log_message(self, fmt: str, *args) -> None:
        if "404" in (fmt % args):
            super().log_message(fmt, *args)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("directory", nargs="*",
                    help="directories of renders (default: whatever the voicematch "
                         "scratch root holds; SONARE_VOICEMATCH_ROOT moves it)")
    ap.add_argument("--port", type=int, default=8730)
    ap.add_argument("--no-open", action="store_true", help="do not launch a browser")
    args = ap.parse_args()

    roots = discover(args.directory)
    Sets.load(roots)
    if not Sets.index:
        # Not an error: a fresh clone has no renders and cannot make the
        # reference side of one at all. Say where they would go and serve the
        # page anyway, so a set rendered now needs only a refresh.
        where = args.directory or [f"{SCRATCH_ROOT}/{g}" for g in FALLBACK_GLOBS]
        print("no renders found in: " + ", ".join(str(w) for w in where), file=sys.stderr)
        print("render one with tools/voicematch/make_audition.py (--model-only needs no plugin)",
              file=sys.stderr)
    for entry in Sets.index:
        kind = "compare" if entry["compare"] else "play only"
        print(f"  {entry['id']:<16} {entry['takes']:>3} takes  [{kind}]  {entry['path']}")

    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.TCPServer(("127.0.0.1", args.port), Handler) as httpd:
        url = f"http://127.0.0.1:{args.port}/"
        print(f"  {url}   (ctrl-c to stop)")
        if not args.no_open:
            threading.Timer(0.4, lambda: webbrowser.open(url)).start()
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
