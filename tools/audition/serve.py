#!/usr/bin/env python3
"""Serve a directory of audio renders as an A/B listening page.

    python tools/audition/serve.py <audition-dir>

Opens a browser on a page that plays every version of a take from one
transport, so switching between them is instant. Two renders compared by
stopping one and starting the other are compared across a gap of a second or
two, which is long enough for the ear to lose what it was holding; here they
run together and the switch costs nothing.

A switch seeks back to the start by default, so the attack is the first thing
heard. Turning that off leaves the versions sample-aligned instead — the switch
becomes a plain gain change on a sound that never stopped, which is what a
sustain or a decay has to be compared on.

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
import socketserver
import sys
import threading
import webbrowser
from pathlib import Path

APP_DIR = Path(__file__).resolve().parent
AUDIO_SUFFIXES = (".wav", ".flac", ".mp3", ".ogg", ".m4a", ".aac")


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


class Handler(http.server.SimpleHTTPRequestHandler):
    """Serve the app from `tools/audition/` and the audio from the render directory."""

    app_dir = APP_DIR
    root = Path()

    def translate_path(self, path: str) -> str:
        rel = path.split("?", 1)[0].split("#", 1)[0].lstrip("/")
        if rel in ("", "index.html"):
            return str(self.app_dir / "index.html")
        if rel in ("app.js", "style.css"):
            return str(self.app_dir / rel)
        if rel == "manifest.json":
            explicit = self.root / "manifest.json"
            if explicit.exists():
                return str(explicit)
            return "/dev/null"  # handled in do_GET before reaching here
        return str(self.root / rel)

    def do_GET(self) -> None:  # noqa: N802 - http.server's spelling
        if self.path.split("?")[0] == "/manifest.json" and not (self.root / "manifest.json").exists():
            body = json.dumps(infer_manifest(self.root)).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
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
    ap.add_argument("directory", help="directory of renders (with or without manifest.json)")
    ap.add_argument("--port", type=int, default=8730)
    ap.add_argument("--no-open", action="store_true", help="do not launch a browser")
    args = ap.parse_args()

    root = Path(args.directory).expanduser().resolve()
    if not root.is_dir():
        print(f"not a directory: {root}", file=sys.stderr)
        return 2

    Handler.root = root
    manifest = root / "manifest.json"
    count = len(json.loads(manifest.read_text())["items"]) if manifest.exists() \
        else len(infer_manifest(root)["items"])
    print(f"serving {count} takes from {root}")

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
