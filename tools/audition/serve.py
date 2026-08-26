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

WHAT IS SOUNDING IS ADDRESSABLE. Every set, take and version has an address —
`#<set>/<take>/<version>` — which the page rewrites as it is navigated, and the
per-set form is printed below on startup. A listening report that names the
wrong render is worse than none, and once several sets of one instrument are up
at once, each holding a reference, an unmodified build and a few candidate
settings, nobody can be sure from memory which one they heard.

ONE SERVER, FOR AS LONG AS THE WORK LASTS. The set list is rebuilt on every
request for it, so a set rendered after the server started shows up on a
refresh; and starting this again while it is running does not start a second
one, it opens a browser on the first. Between them there is never a reason to
pick a different port, which is what stops a tuning session ending with a row of
servers each showing a subset of the renders.

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
import urllib.error
import urllib.request
import webbrowser
from pathlib import Path

APP_DIR = Path(__file__).resolve().parent
REPO_ROOT = APP_DIR.parents[1]
AUDIO_SUFFIXES = (".wav", ".flac", ".mp3", ".ogg", ".m4a", ".aac")
#: The scratch root the whole harness renders into, and the one `capture.py`
#: reads. Untracked on purpose — the reference side of a comparison is captured
#: from a commercial plugin and cannot be redistributed — so a fresh clone finds
#: nothing here, which is a supported state rather than a failure.
#: Resolved, because `discover` resolves what it finds and `set_id` compares a
#: set's parent against this: one symlink anywhere above the checkout and the
#: two spellings stop matching, which would rename every set silently.
SCRATCH_ROOT = Path(
    os.environ.get("SONARE_VOICEMATCH_ROOT") or REPO_ROOT / ".cache" / "voicematch"
).expanduser().resolve()
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


#: Leaf names that say nothing about which set a directory holds.
GENERIC_NAMES = ("audition", "renders", "out")


def set_id(root: Path) -> str:
    """A short URL-safe name for a set, and the one its links carry.

    Taken from the parent directory when the leaf says nothing, so that two sets
    both written to a directory called `audition` are still told apart by the
    directory holding them — except directly under the scratch root, where the
    parent is the scratch root and names the harness rather than the set.
    """
    name = root.name
    if name in GENERIC_NAMES and root.parent not in (root, SCRATCH_ROOT):
        name = root.parent.name
    return re.sub(r"[^a-zA-Z0-9._-]+", "-", name).strip("-") or "set"


def take_dirs(root: Path) -> set[Path]:
    """The directories a set's own manifest names as holding its takes."""
    mf = root / "manifest.json"
    if not mf.exists():
        return set()
    try:
        manifest = json.loads(mf.read_text())
    except (OSError, ValueError):
        return set()
    out: set[Path] = set()
    for item in manifest.get("items", []):
        for rel in (item.get("tracks") or {}).values():
            parent = (root / rel).parent.resolve()
            if parent != root:
                out.add(parent)
    return out


def is_set(root: Path) -> bool:
    """Whether this directory holds renders of its own.

    A manifest settles it. Without one the layout does: a directory whose
    subdirectories hold audio, or which holds loose audio itself, is a set, and
    one holding neither is a container.
    """
    if (root / "manifest.json").exists():
        return True
    try:
        return bool(infer_manifest(root).get("items"))
    except OSError:
        return False


def expand(root: Path) -> list[Path]:
    """A named directory, or the sets inside it when it is a directory of sets.

    One level, and only when the directory is not a set itself. A run that
    renders several voices writes one set per voice under a root, and naming
    that root is the obvious way to ask for all of them — without this it is
    reported as empty, which is true of the root and false of what is in it.
    """
    if not root.is_dir() or is_set(root):
        return [root]
    inside = [p for p in sorted(root.iterdir()) if p.is_dir() and is_set(p)]
    return inside or [root]


def discover(paths: list[str]) -> list[Path]:
    """The directories to serve: the ones named, else whatever the scratch root holds."""
    if paths:
        named = [Path(p).expanduser().resolve() for p in paths]
        return list(dict.fromkeys(q for p in named for q in expand(p)))
    found: list[Path] = []
    for pattern in FALLBACK_GLOBS:
        found += [p.resolve() for p in sorted(SCRATCH_ROOT.glob(pattern)) if p.is_dir()]
    unique = list(dict.fromkeys(found))
    # A directory a set's manifest names as one of its own takes is part of that
    # set, not a set beside it. The default output directory is the parent of
    # every named one, so `audition/*` matches a set's take directories as
    # readily as it matches the sets: a kit set of six takes came out as six
    # play-only "sets" of two versions each -- and the kit set itself was gone,
    # dropped by the leaf rule below for being their parent.
    claimed: set[Path] = set()
    for p in unique:
        claimed |= take_dirs(p)
    unique = [p for p in unique if p not in claimed]
    # A directory with a manifest is a set whatever it contains. One without is
    # a set only if it is a leaf: two of these globs can match a directory and
    # its parent, and a parent holding no renders of its own would otherwise put
    # a name in the picker that plays nothing.
    return [p for p in unique
            if (p / "manifest.json").exists()
            or not any(p in other.parents for other in unique)]


class Sets:
    """The served sets, by id, and the index the page reads to list them.

    Reloaded on every request for the index rather than once at startup, so one
    server outlives the renders it is showing. A tuning session produces a set
    every few minutes and the alternative is a server -- and a port, and a stale
    browser tab -- per set, which is how eight of them came to be running at
    once. Discovery is a handful of `glob` calls against a directory that holds
    tens of entries, so doing it per index request costs nothing worth naming.
    """

    #: What `main` was asked to serve, so a reload can repeat the same search.
    paths: list[str] = []
    by_id: dict[str, Path] = {}
    index: list[dict] = []

    @classmethod
    def reload(cls) -> None:
        cls.load(discover(cls.paths))

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
                # A heading the picker files this set under. Optional and
                # generic — the manifest says what it is, this only carries it
                # through — but a picker of a hundred and thirty sets is a wall
                # of names without one.
                "group": manifest.get("group") or "",
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
            # Re-scan here, so a set rendered after the server started appears
            # on a refresh instead of needing a second server.
            Sets.reload()
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


def already_serving(port: int) -> bool:
    """Whether an audition server is answering on this port already.

    Checked before binding rather than after failing to, because the useful
    answer to "the port is taken" is almost always "by the one you started an
    hour ago" -- and starting a second server on a second port is what leaves a
    row of them running, each showing a subset of the sets. Anything else
    holding the port is left to the bind to report as itself.
    """
    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{port}/sets.json", timeout=1.0) as r:
            json.load(r)
        return True
    except (urllib.error.URLError, OSError, ValueError):
        return False


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("directory", nargs="*",
                    help="directories of renders (default: whatever the voicematch "
                         "scratch root holds; SONARE_VOICEMATCH_ROOT moves it)")
    ap.add_argument("--port", type=int, default=8730)
    ap.add_argument("--no-open", action="store_true", help="do not launch a browser")
    args = ap.parse_args()

    # A running server rediscovers its sets on every index request, so one that
    # is already up is showing this set too and there is nothing to start.
    if already_serving(args.port):
        url = f"http://127.0.0.1:{args.port}/"
        print(f"already serving at {url} — refresh it; new sets appear on their own")
        if not args.no_open:
            webbrowser.open(url)
        return 0

    Sets.paths = list(args.directory)
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
    url = f"http://127.0.0.1:{args.port}/"
    # The per-set address, not just the name: a set is chosen for someone else
    # to listen to at least as often as for oneself, and `#<set>/<take>/<version>`
    # is the only form of "listen to this one" that cannot be misread.
    for entry in Sets.index:
        kind = "compare" if entry["compare"] else "play only"
        print(f"  {entry['id']:<16} {entry['takes']:>3} takes  [{kind:^10}]  {url}#{entry['id']}")

    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.TCPServer(("127.0.0.1", args.port), Handler) as httpd:
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
