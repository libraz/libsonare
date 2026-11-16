"""Run an autofit search on the build host against a numeric oracle cache, then confirm it here.

    python tools/voicematch/remote_fit.py run --slug S --out-dir DIR --local-lib LIB \\
        [--host-workers 12] [--resume] -- <autofit arguments>
    python tools/voicematch/remote_fit.py confirm --slug S --out-dir DIR --local-lib LIB \\
        -- <autofit arguments>
    python tools/voicematch/remote_fit.py pull --slug S --out-dir DIR
    python tools/voicematch/remote_fit.py probe --slug S --out-dir DIR --local-lib LIB \\
        [--host H] -- <autofit arguments>
    python tools/voicematch/remote_fit.py queue --list FILE --status FILE --out-dir DIR \\
        --local-lib LIB [--host H] [--host-workers 12] [--parallel 1]

`run` writes the oracle caches from the corpus on this Mac, runs the leave-check over
the synced tree and the caches (failing closed), runs the search on the host through
sendaway with the host's own BUILD_TUNING library, and pulls the host's `fit.json` and
per-evaluation log into the output directory. No capture audio leaves this machine.

sendaway stops the host command when its connection drops, so the search is made
resumable instead: its measured candidates are appended to a store in the host's
output directory, and the driver reconnects and re-issues the same search, which
replays what it finished and renders on from the first evaluation it had not. Each
attempt keeps its own log, and `host.json` counts what each one replayed and rendered.
A fresh `run` clears the output directory; `--resume` keeps it. `pull` collects a
search that finished while this side was not waiting for it.

`queue` runs a list of searches inside a single host run, `--parallel` of them at a
time sharing the host's workers (a coordinate search is serial, so it leaves extra
workers idle), each resumable from its own store, and appends one line per ended search to a status file
(slug, start and best loss, evaluations, seconds, exit). The same command re-issued
after a drop skips what already ended at this commit and continues the rest. `--host`
routes the tree to a named host through a private sendaway config.

`probe` scores a search's start once on the host and once here and compares them
term by term; it is how an engine's place on the refusal list is measured, so it runs
for a refused engine too.

`confirm` scores the search's start and its selected point once each here, with the
full objective against the audio. The host and this
Mac may round differently, so the pair is the verdict: it is written into the pulled
report as `confirm`, with its render count and seconds, and a start the two machines
score apart by more than `HOST_AGREEMENT` refuses the report.

An engine in `HOST_REFUSED_ENGINES` is refused before anything is sent, for one a
different libm moves by more than float noise; `probe` is what puts one there.
"""

from __future__ import annotations

import argparse
import base64
import gzip
import hashlib
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from _repo import REPO_ROOT

#: The sendaway binary: `SENDAWAY` names it, else it is looked up on PATH.
SENDAWAY = Path(os.environ.get("SENDAWAY") or shutil.which("sendaway") or "sendaway")
#: sendaway's own failure exit, a dropped connection among them: the host command is stopped.
SENDAWAY_LOST = 125
#: The host line's exit when sendaway ran it on this Mac because the host was unreachable.
FELL_BACK = 97
#: Attempts at one search, and the wait before the second; each later wait doubles.
ATTEMPTS = 6
BACKOFF_S = 30
HERE = Path(__file__).resolve().parent
CACHE_ROOT = REPO_ROOT / ".remote-fit-cache"
HOST_OUT = ".remote-fit-out"
#: Marks a checkout as the dedicated copy remote fits are sent from. It is gitignored, so it
#: never syncs, and the main working tree, which may hold work in progress, lacks it.
TREE_MARK = REPO_ROOT / ".cache" / "remote-fit-tree"
HOST_PYTHON = ".venv/bin/python"
HOST_LIB = "build-tuning-linux/lib/libsonare.so"

#: Engines whose host rendering differs from this Mac's, each with the mechanism that moves
#: it. Empty: every self-sustaining engine (bow, reed, lip, jet, free reed, FM feedback)
#: scored its start within `HOST_AGREEMENT` on both build hosts' libm in a `probe`.
HOST_REFUSED_ENGINES: dict[str, str] = {}

#: The relative difference in the start's aggregate loss the two machines may show.
HOST_AGREEMENT = 0.005

#: Flags the driver sets itself on each side; a caller giving one is refused.
_OWNED = (
    "--lib",
    "--out",
    "--oracle-cache",
    "--oracle-cache-write",
    "--oracle-cache-only",
    "--cache-dir",
)
_OWNED_BOOL = ("--dry-run", "--oracle-cache-write", "--oracle-cache-only", "--no-cache")
#: The first command of every host line: sendaway runs a line locally when the host is
#: unreachable, and this one must never start a search or clear a directory on the Mac.
ON_HOST_ONLY = (
    'test "$(uname -s)" = Linux || { echo "remote_fit: sendaway fell back to this machine; '
    f'the host line refuses to run here" >&2; exit {FELL_BACK}; }}; '
)


def check_args(argv: list[str]) -> None:
    given = [a.split("=", 1)[0] for a in argv if a.startswith("--")]
    clash = sorted(set(given) & set(_OWNED + _OWNED_BOOL + ("--workers",)))
    if clash:
        raise ValueError(f"the driver sets {', '.join(clash)} itself; leave it out")
    if "--corpus" not in given:
        raise ValueError("a remote fit scores against a corpus cache; give --corpus")


def engine_refusal(mode: str | None) -> str | None:
    why = HOST_REFUSED_ENGINES.get(mode or "")
    if why is None:
        return None
    return (
        f"the {mode} engine renders through {why}, which the host's libm can move by more "
        "than float noise; fit it on this Mac"
    )


def voice_engine(argv: list[str], local_lib: Path) -> str | None:
    """The engine the library voices this fit's program with, read from its catalogue."""
    import autofit
    from catalogue import dump_catalogue

    args = autofit.build_parser().parse_args(argv)
    if args.drum_note is not None:
        return "percussion"
    cat = dump_catalogue(args.program, "sustain", str(local_lib), sr=48000, bank=args.bank)
    return cat.mode_for(args.program, args.bank)


def cache_dir(slug: str) -> Path:
    return CACHE_ROOT / slug


def _autofit(argv: list[str], python: str = sys.executable) -> list[str]:
    return [python, str(HERE / "autofit.py"), *argv]


def write_caches(argv: list[str], slug: str, local_lib: Path) -> Path:
    directory = cache_dir(slug)
    directory.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        cmd = _autofit(
            [*argv, "--lib", str(local_lib), "--dry-run", "--out", str(Path(tmp) / "w.json")]
            + ["--oracle-cache", str(directory), "--oracle-cache-write", "--no-cache"]
        )
        proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        raise RuntimeError(f"writing the oracle caches failed:\n{proc.stderr[-3000:]}")
    if not any(directory.glob("*.json.gz")):
        raise RuntimeError(f"no oracle cache was written under {directory}")
    return directory


def leave() -> str:
    """Run the leave-check over the synced tree and every cache in it; raise on any refusal.

    Every slug's caches sit inside the tree, so each sync carries all of them.
    """
    caches = ["--cache", str(CACHE_ROOT)] if any(CACHE_ROOT.rglob("*.json.gz")) else []
    out = subprocess.run(
        [sys.executable, str(HERE / "leave_check.py"), "--tree", str(REPO_ROOT), *caches],
        capture_output=True,
        text=True,
        check=False,
    )
    verdict = out.stdout.strip()
    if out.returncode != 0 or not verdict.startswith("leave-check PASS"):
        raise RuntimeError(f"nothing was sent:\n{verdict or out.stderr}")
    return verdict.splitlines()[0]


def sendaway_tree(root: Path = REPO_ROOT) -> Path:
    """The tree sendaway would send, refused unless it is the marked copy and its own project.

    sendaway takes the nearest `.sendaway.yml` above the working directory as the project
    root, so a tree without its own would send whatever enclosing checkout has one.
    """
    if not (root / ".sendaway.yml").is_file():
        raise RuntimeError(
            f"{root} has no .sendaway.yml of its own, so sendaway would send an enclosing tree"
        )
    if not (root / TREE_MARK.relative_to(REPO_ROOT)).is_file():
        raise RuntimeError(
            f"{root} is not marked as the copy remote fits are sent from "
            f"({TREE_MARK.relative_to(REPO_ROOT)} is missing); use a dedicated clone"
        )
    return root


def sendaway_env(host: str | None) -> dict | None:
    """The environment that routes this tree to `host`, through a private sendaway config.

    The config sits under the tree's gitignored `.cache/`, so it never syncs. Each host
    also gets its own state directory: sendaway locks a host copy by its path, which is the
    same on every host, so a shared one would serialize two hosts. Without a host the
    user's own routing applies.
    """
    if host is None:
        return None
    if not re.fullmatch(r"[\w.-]+", host):
        raise ValueError(f"{host!r} is not a host alias")
    home = TREE_MARK.parent / f"sendaway-{host}"
    (home / "sendaway").mkdir(parents=True, exist_ok=True)
    (home / "sendaway" / "config.yml").write_text(
        f"routes:\n  - path: {REPO_ROOT}\n    host: {host}\n"
    )
    # Under the user's state root: sendaway's ssh control socket path must stay short.
    state = Path.home() / ".local" / "state" / f"sendaway-{host}"
    return {**os.environ, "XDG_CONFIG_HOME": str(home), "XDG_STATE_HOME": str(state)}


def mac_head() -> str:
    """This tree's commit, marked `-dirty` when tracked files differ from it: the host has no `.git`."""
    git = ["git", "-C", str(REPO_ROOT)]
    sha = subprocess.run([*git, "rev-parse", "HEAD"], capture_output=True, text=True, check=True)
    dirty = subprocess.run(
        [*git, "status", "--porcelain", "--untracked-files=no"],
        capture_output=True,
        text=True,
        check=True,
    )
    return sha.stdout.strip() + ("-dirty" if dirty.stdout.strip() else "")


#: The first line of each attempt's log: the interpreter and numeric libraries it ran on,
#: since the Mac/host agreement was measured at particular versions.
VERSIONS_MARK = "remote_fit-versions "
_VERSIONS = (
    "import json, sys, importlib.util as u, importlib.metadata as m; "
    "print(json.dumps({'python': sys.version.split()[0]} | "
    "{p: m.version(p) for p in ('numpy', 'scipy') if u.find_spec(p)}))"
)


def _fit_block(argv: list[str], slug: str, workers: int, head: str, fresh: bool) -> str:
    """One search on the host, its log and `host.json`, leaving its exit in `$rc`."""
    root = str(REPO_ROOT)
    out = f"{root}/{HOST_OUT}/{slug}"
    fit = [
        HOST_PYTHON,
        "tools/voicematch/autofit.py",
        *argv,
        "--lib",
        f"{root}/{HOST_LIB}",
        "--workers",
        str(workers),
        "--dry-run",
        "--out",
        f"{out}/fit.json",
        "--oracle-cache",
        str(cache_dir(slug)),
        "--cache-dir",
        out,
    ]
    load = """awk '{print "["$1", "$2", "$3"]"}' /proc/loadavg"""
    fields = (
        '{"wall_s": %d, "rc": %d, "mac_head": "%s", "load_start": %s, "load_end": %s, '
        '"versions": %s}\\n'
    )
    q = shlex.quote(out)
    if fresh:
        prepare = f"rm -rf {q}; mkdir -p {q}; "
    else:
        prepare = (
            f"mkdir -p {q}; rm -f {q}/host.json; n=1; "
            f"while [ -f {q}/fit.attempt$n.log ]; do n=$((n + 1)); done; "
            f"if [ -f {q}/fit.log ]; then mv {q}/fit.log {q}/fit.attempt$n.log; fi; "
        )
    return (
        prepare
        + f"v=$({HOST_PYTHON} -c {shlex.quote(_VERSIONS)} || true); "
        + """[ -n "$v" ] || v='{}'; """
        f'printf "%s%s\\n" {shlex.quote(VERSIONS_MARK)} "$v" > {q}/fit.log; '
        f"l0=$({load}); t0=$(date +%s); rc=0; "
        f"nice -n 10 {shlex.join(fit)} >> {q}/fit.log 2>&1 || rc=$?; "
        f"l1=$({load}); "
        f'printf \'{fields}\' $(( $(date +%s) - t0 )) $rc {shlex.quote(head)} "$l0" "$l1" "$v" '
        f"> {q}/host.json; "
    )


def _host_prelude() -> str:
    return (
        ON_HOST_ONLY
        + f"set -e; export PYTHONPATH={shlex.quote(str(REPO_ROOT))}/bindings/python/src; "
    )


def host_command(argv: list[str], slug: str, workers: int, head: str, fresh: bool = True) -> str:
    """The shell line the host runs: the search, its log, and `host.json`.

    `host.json` holds the wall time, the exit, `head` (the Mac commit the synced tree
    is) and the host's load averages when the search started and ended. A fresh line
    clears the output directory; any other keeps its store and moves the previous
    attempt's log aside as the next free `fit.attempt<n>.log`.
    """
    return _host_prelude() + _fit_block(argv, slug, workers, head, fresh) + "exit $rc"


#: Each named output directory's files (not its store) as text, keyed by directory.
_BUNDLE = (
    "import json, pathlib, sys; "
    f"sys.platform == 'linux' or sys.exit({FELL_BACK}); "
    "print(json.dumps({a: {p.name: p.read_text() for p in sorted(pathlib.Path(a).iterdir()) "
    "if p.is_file()} if pathlib.Path(a).is_dir() else {} for a in sys.argv[1:]}))"
)


def pull_many(
    slugs: list[str], out_root: Path, sendaway: Path = SENDAWAY, host: str | None = None
) -> dict[str, list[str]]:
    """Bring each slug's host output directory back as text, in one sync, into `out_root/<slug>`.

    A pull syncs the tree like any run, so the leave-check runs first.
    """
    print(leave())
    proc = subprocess.run(
        [str(sendaway), "run", HOST_PYTHON, "-c", _BUNDLE]
        + [f"{HOST_OUT}/{slug}" for slug in slugs],
        cwd=sendaway_tree(),
        env=sendaway_env(host),
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        raise RuntimeError(f"pulling {', '.join(slugs)} failed:\n{proc.stderr[-2000:]}")
    bundle = json.loads(proc.stdout)
    got = {}
    for slug in slugs:
        files = bundle.get(f"{HOST_OUT}/{slug}", {})
        (out_root / slug).mkdir(parents=True, exist_ok=True)
        for name, text in files.items():
            (out_root / slug / name).write_text(text)
        got[slug] = sorted(files)
    return got


def pull(slug: str, out_dir: Path, sendaway: Path = SENDAWAY, host: str | None = None) -> list[str]:
    """Bring the host's output directory back as text over stdout into `out_dir`."""
    print(leave())
    proc = subprocess.run(
        [str(sendaway), "run", HOST_PYTHON, "-c", _BUNDLE, f"{HOST_OUT}/{slug}"],
        cwd=sendaway_tree(),
        env=sendaway_env(host),
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        raise RuntimeError(f"pulling {HOST_OUT}/{slug} failed:\n{proc.stderr[-2000:]}")
    files = json.loads(proc.stdout)[f"{HOST_OUT}/{slug}"]
    out_dir.mkdir(parents=True, exist_ok=True)
    for name, text in files.items():
        (out_dir / name).write_text(text)
    return sorted(files)


def run_remote(
    argv,
    slug,
    out_dir,
    local_lib,
    workers,
    *,
    resume: bool = False,
    host: str | None = None,
    sendaway: Path = SENDAWAY,
    wait=time.sleep,
) -> dict:
    """Run the search on the host, re-issuing it after each dropped connection.

    The leave-check runs before every attempt, because every attempt syncs the tree.
    """
    check_args(argv)
    sendaway_tree()
    refusal = engine_refusal(voice_engine(argv, local_lib))
    if refusal:
        raise ValueError(refusal)
    write_caches(argv, slug, local_lib)
    return _run_host(argv, slug, out_dir, workers, resume, host, sendaway, wait)


def _run_host(argv, slug, out_dir, workers, resume, host, sendaway, wait) -> dict:
    head = mac_head()
    attempts: list[dict] = []
    for attempt in range(1, ATTEMPTS + 1):
        if attempts:
            wait(BACKOFF_S * 2 ** (len(attempts) - 1))
        verdict = leave()
        print(verdict)
        t0 = time.monotonic()
        proc = subprocess.run(
            [
                str(sendaway),
                "run",
                "bash",
                "-c",
                host_command(argv, slug, workers, head, fresh=attempt == 1 and not resume),
            ],
            cwd=sendaway_tree(),
            env=sendaway_env(host),
            check=False,
        )
        attempts.append(
            {"attempt": attempt, "rc": proc.returncode, "seconds": round(time.monotonic() - t0, 1)}
        )
        if proc.returncode not in (SENDAWAY_LOST, FELL_BACK):
            break
        print(
            f"remote_fit: attempt {attempt} lost the host (exit {proc.returncode})", file=sys.stderr
        )
    else:
        raise RuntimeError(
            f"the host was lost on {len(attempts)} attempts in a row and sendaway stopped the "
            "search each time; what it finished is kept on the host, and "
            f"`remote_fit.py run --resume --slug {slug} ...` with the same arguments continues it"
        )
    return {"leave_check": verdict} | collect(slug, out_dir, sendaway, attempts, host)


#: One evaluation line of an autofit log: rendered here (`builds=`) or replayed (`stored`).
_EVAL_LINE = re.compile(r"^  eval #\d+ (builds=\d+|stored) ", re.MULTILINE)


def _eval_counts(log: str) -> dict:
    kinds = _EVAL_LINE.findall(log)
    stored = kinds.count("stored")
    out = {"evals_new": len(kinds) - stored, "evals_replayed": stored}
    if log.startswith(VERSIONS_MARK):
        out["versions"] = json.loads(log.splitlines()[0][len(VERSIONS_MARK) :])
    return out


def _attempt_logs(pulled: list[str]) -> list[str]:
    """Each attempt's log in order; the last attempt's is `fit.log`."""
    numbered = sorted(
        (int(name[len("fit.attempt") : -len(".log")]), name)
        for name in pulled
        if name.startswith("fit.attempt") and name.endswith(".log")
    )
    return [name for _, name in numbered] + (["fit.log"] if "fit.log" in pulled else [])


def collect(
    slug: str,
    out_dir: Path,
    sendaway: Path = SENDAWAY,
    attempts: list[dict] | None = None,
    host: str | None = None,
) -> dict:
    """Pull the host's output and read how its search ended from `host.json`.

    `host.json` gains one entry per attempt with what its log replayed and rendered,
    merged with the exit and seconds this side saw when it ran them; `evals_per_s`
    is the last attempt's rendered evaluations over its wall time.
    """
    return read_host(slug, out_dir, pull(slug, out_dir, sendaway, host), attempts)


def read_host(slug: str, out_dir: Path, pulled: list[str], attempts: list[dict] | None) -> dict:
    """Read a pulled search's `host.json`, adding its per-attempt counts and rate."""
    if "host.json" not in pulled:
        raise RuntimeError(f"the host search for {slug} has not finished (no host.json yet)")
    host = json.loads((out_dir / "host.json").read_text())
    logs = _attempt_logs(pulled)
    # This side's records belong to the last attempts; earlier logs are from an earlier invocation.
    ours = [{}] * (len(logs) - len(attempts or [])) + list(attempts or [])[-len(logs) :]
    host["attempts"] = [
        {"log": name}
        | {k: v for k, v in mine.items() if k != "attempt"}
        | _eval_counts((out_dir / name).read_text())
        for name, mine in zip(logs, ours)
    ]
    if host["rc"] != 0:
        (out_dir / "host.json").write_text(json.dumps(host) + "\n")
        raise RuntimeError(f"the host search failed (exit {host['rc']}); see {out_dir}/fit.log")
    if host["wall_s"] > 0 and host["attempts"]:
        host["evals_per_s"] = round(host["attempts"][-1]["evals_new"] / host["wall_s"], 4)
    (out_dir / "host.json").write_text(json.dumps(host) + "\n")
    return {"host": host, "pulled": pulled}


# --------------------------------------------------------------------------
# queue

#: Prefix of the line the host prints as each queued search ends.
QUEUE_MARK = "remote_fit-status "
_SLUG = re.compile(r"[\w.-]+")

#: Prints one queued search's status line from its `host.json` and `fit.json`.
_STATUS = (
    "import json, pathlib, sys; o = pathlib.Path(sys.argv[1]); "
    "h = json.loads((o / 'host.json').read_text()); "
    "row = {'slug': sys.argv[2], 'rc': h['rc'], 'seconds': h['wall_s'], "
    "'finished_earlier': sys.argv[3] == '1', 'versions': h.get('versions')}; "
    "f = o / 'fit.json'; "
    "r = json.loads(f.read_text()) if f.is_file() else None; "
    "q = r and r['selected']['quality']; "
    "row.update(q and {'start_loss': q['baseline']['absolute_weighted_mean_units'], "
    "'best_loss': q['absolute_weighted_mean_units'], 'evals': r['evaluations']} or {}); "
    f"print({QUEUE_MARK!r} + json.dumps(row), flush=True)"
)


def load_queue(path: Path) -> list[dict]:
    """The queue file: one JSON object per line with `slug`, `args` and any other fields."""
    items = [json.loads(line) for line in path.read_text().splitlines() if line.strip()]
    slugs = [item["slug"] for item in items]
    bad = [slug for slug in slugs if not _SLUG.fullmatch(slug)]
    if bad:
        raise ValueError(f"slugs name directories; refused: {', '.join(bad)}")
    dup = sorted({slug for slug in slugs if slugs.count(slug) > 1})
    if dup:
        raise ValueError(f"each slug may be queued once; repeated: {', '.join(dup)}")
    return items


def queue_key(head: str, argv: list[str]) -> str:
    """What a finished search on the host must match to be skipped: the commit and its arguments."""
    return f"{head} {hashlib.sha256(json.dumps(argv).encode()).hexdigest()[:16]}"


def queue_command(
    items: list[dict], workers: int, keys: dict[str, str], head: str, parallel: int = 1
) -> str:
    """One host line running the queued searches, skipping those already finished.

    The items are dealt in turn onto `parallel` streams that run side by side, each search
    in its stream after the one before, so the host's `workers` are shared between streams.
    Each search keeps its store, so one stopped by a dropped connection replays what it
    measured; `queue.key` marks one that ended at this commit with these arguments.
    """
    share = workers // parallel
    streams = [""] * parallel
    for n, item in enumerate(items):
        slug = item["slug"]
        q = shlex.quote(f"{REPO_ROOT}/{HOST_OUT}/{slug}")
        key = shlex.quote(keys[slug])
        streams[n % parallel] += (
            f'if [ "$(cat {q}/queue.key 2>/dev/null)" = {key} ]; then earlier=1; else earlier=0; '
            f"rm -f {q}/queue.key; "
            + _fit_block(item["args"], slug, share, head, fresh=False)
            + f"printf %s {key} > {q}/queue.key; fi; "
            f"{HOST_PYTHON} -c {shlex.quote(_STATUS)} {q} {shlex.quote(slug)} $earlier || true; "
        )
    return (
        _host_prelude()
        + "".join(f"( {stream}) & " for stream in streams if stream)
        + "wait; exit 0"
    )


def packed(line: str) -> str:
    """`line` sent compressed: a long queue outgrows the host's limit on one argument."""
    body = base64.b64encode(gzip.compress(line.encode(), mtime=0)).decode()
    return ON_HOST_ONLY + f"set -o pipefail; printf %s {body} | base64 -d | gunzip | bash"


def _reported(status: Path) -> set[tuple[str, str]]:
    if not status.is_file():
        return set()
    rows = [json.loads(line) for line in status.read_text().splitlines() if line.strip()]
    return {(row["slug"], row.get("key", "")) for row in rows}


def run_queue(
    items: list[dict],
    out_root: Path,
    local_lib: Path,
    workers: int,
    status: Path,
    *,
    parallel: int = 1,
    host: str | None = None,
    sendaway: Path = SENDAWAY,
    wait=time.sleep,
) -> dict:
    """Run the queued searches in one host run, re-issuing it after each dropped connection.

    Each search that ends appends one line to `status` (once per commit and arguments);
    afterwards every output directory is pulled in one sync into `out_root/<slug>`.
    """
    sendaway_tree()
    for item in items:
        check_args(item["args"])
        refusal = engine_refusal(voice_engine(item["args"], local_lib))
        if refusal:
            raise ValueError(f"{item['slug']}: {refusal}")
    for item in items:
        write_caches(item["args"], item["slug"], local_lib)
    head = mac_head()
    keys = {item["slug"]: queue_key(head, item["args"]) for item in items}
    extra = {item["slug"]: {k: v for k, v in item.items() if k != "args"} for item in items}
    line = queue_command(items, workers, keys, head, parallel)
    seen = _reported(status)
    status.parent.mkdir(parents=True, exist_ok=True)
    attempts: list[dict] = []
    for attempt in range(1, ATTEMPTS + 1):
        if attempts:
            wait(BACKOFF_S * 2 ** (len(attempts) - 1))
        print(leave(), flush=True)
        t0 = time.monotonic()
        proc = subprocess.Popen(
            [str(sendaway), "run", "bash", "-c", packed(line)],
            cwd=sendaway_tree(),
            env=sendaway_env(host),
            stdout=subprocess.PIPE,
            text=True,
        )
        for out in proc.stdout:
            if not out.startswith(QUEUE_MARK):
                sys.stdout.write(out)
                continue
            row = json.loads(out[len(QUEUE_MARK) :])
            slug = row["slug"]
            if (slug, keys[slug]) in seen:
                continue
            seen.add((slug, keys[slug]))
            row = extra[slug] | row | {"host": host, "key": keys[slug]}
            with status.open("a") as fh:
                fh.write(json.dumps(row) + "\n")
            print(f"remote_fit: {slug} ended (exit {row['rc']})", flush=True)
        rc = proc.wait()
        attempts.append({"attempt": attempt, "rc": rc, "seconds": round(time.monotonic() - t0, 1)})
        if rc not in (SENDAWAY_LOST, FELL_BACK):
            break
        print(f"remote_fit: queue attempt {attempt} lost the host (exit {rc})", file=sys.stderr)
    else:
        raise RuntimeError(
            f"the host was lost on {len(attempts)} attempts in a row; what finished is kept on "
            "the host, and the same `remote_fit.py queue` command continues the queue"
        )
    if attempts[-1]["rc"] != 0:
        raise RuntimeError(f"the queue's host run failed (exit {attempts[-1]['rc']})")
    slugs = [item["slug"] for item in items]
    pulled = pull_many(slugs, out_root, sendaway, host)
    ended = {}
    for slug in slugs:
        try:
            ended[slug] = read_host(slug, out_root / slug, pulled[slug], None)["host"]
        except RuntimeError as exc:
            ended[slug] = {"error": str(exc)}
    return {"attempts": attempts, "searches": ended}


# --------------------------------------------------------------------------
# confirm


def _terms(quality: dict) -> dict[str, float]:
    return {
        k: t["weighted_units"]
        for k, t in quality["terms"].items()
        if t.get("weighted_units") is not None
    }


def _point(quality: dict) -> dict:
    return {"loss": quality["absolute_weighted_mean_units"], "terms": _terms(quality)}


def remote_points(report: dict) -> dict:
    quality = report["selected"]["quality"]
    return {"start": _point(quality["baseline"]), "best": _point(quality)}


def start_overrides(report: dict) -> str:
    """The selected point as `--start-overrides`, at full precision.

    Only the knobs off their start: one at its start needs no override, and one the search
    now holds (a velocity-axis knob on a one-velocity probe) is refused as a start override.
    """
    values = report["selected"]["values"]
    return ",".join(
        f"{k['key']}={v!r}" for k, v in zip(report["knobs"], values, strict=True) if v != k["start"]
    )


def _without(argv: list[str], *flags: str) -> list[str]:
    """`argv` with each of `flags` and its value removed."""
    out, skip = [], False
    for a in argv:
        if skip:
            skip = False
            continue
        if a in flags:
            skip = True
            continue
        if a.split("=", 1)[0] in flags:
            continue
        out.append(a)
    return out


def score_once(argv: list[str], local_lib: Path, out: Path, start: str | None) -> dict:
    """One full-objective evaluation here, at the search's start or at `start`."""
    extra = ["--start-overrides", start] if start else []
    # A per-knob budget outranks --max-evals, so it goes, or the one evaluation becomes a search.
    base = _without(argv, "--evals-per-knob", *(["--start-overrides"] if start else []))
    cmd = _autofit(
        [*base, *extra, "--lib", str(local_lib), "--short-run", "--max-evals", "1"]
        + ["--dry-run", "--out", str(out)]
    )
    with out.with_suffix(".log").open("w") as log:
        proc = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, check=False)
    if proc.returncode != 0 or not out.exists():
        raise RuntimeError(f"the confirm evaluation failed; see {out.with_suffix('.log')}")
    report = json.loads(out.read_text())
    if report["evaluations"] != 1:
        raise RuntimeError(f"the confirm evaluation ran {report['evaluations']} evaluations, not 1")
    return report


def confirm(argv, slug, out_dir, local_lib) -> dict:
    check_args(argv)
    fit = out_dir / "fit.json"
    report = json.loads(fit.read_text())
    remote = remote_points(report)
    t0 = time.monotonic()
    here_start = score_once(argv, local_lib, out_dir / "confirm_start.json", None)
    here_best = score_once(argv, local_lib, out_dir / "confirm_best.json", start_overrides(report))
    seconds = time.monotonic() - t0
    local = {
        "start": _point(here_start["selected"]["quality"]),
        "best": _point(here_best["selected"]["quality"]),
    }
    gap = abs(local["start"]["loss"] - remote["start"]["loss"]) / remote["start"]["loss"]
    block = {
        "renders": here_start["renders"] + here_best["renders"],
        "seconds": round(seconds, 1),
        "remote": remote,
        "local": local,
        "start_gap": gap,
        "agrees": gap <= HOST_AGREEMENT,
        "improves": local["best"]["loss"] < local["start"]["loss"],
    }
    report["confirm"] = block
    if not block["agrees"]:
        (out_dir / "confirm_refused.json").write_text(json.dumps(block, indent=2) + "\n")
        raise RuntimeError(
            f"the host and this Mac score the start {gap:.2%} apart (over {HOST_AGREEMENT:.1%}); "
            f"the report is refused, see {out_dir}/confirm_refused.json"
        )
    fit.write_text(json.dumps(report, indent=2) + "\n")
    return block


# --------------------------------------------------------------------------
# probe


def _gap(here: float, there: float) -> float:
    return abs(here - there) / abs(there) if there else abs(here)


def probe(
    argv: list[str],
    slug: str,
    out_dir: Path,
    local_lib: Path,
    workers: int,
    *,
    host: str | None = None,
    sendaway: Path = SENDAWAY,
    wait=time.sleep,
) -> dict:
    """Score the search's start once on the host and once here, per term, whatever the engine.

    This is the measurement the engine refusal stands on, so it is the one host run the
    refusal does not stop. `probe.json` holds both points and each term's relative gap.
    """
    check_args(argv)
    sendaway_tree()
    one = [*_without(argv, "--evals-per-knob", "--max-evals"), "--short-run", "--max-evals", "1"]
    engine = voice_engine(argv, local_lib)
    write_caches(one, slug, local_lib)
    ran = _run_host(one, slug, out_dir, workers, False, host, sendaway, wait)
    there = remote_points(json.loads((out_dir / "fit.json").read_text()))["start"]
    here = _point(
        score_once(argv, local_lib, out_dir / "probe_here.json", None)["selected"]["quality"][
            "baseline"
        ]
    )
    terms = {
        k: {"host": there["terms"][k], "here": v, "gap": _gap(v, there["terms"][k])}
        for k, v in here["terms"].items()
        if k in there["terms"]
    }
    block = {
        "slug": slug,
        "engine": engine,
        "refused": engine in HOST_REFUSED_ENGINES,
        "host": host,
        "loss": {"host": there["loss"], "here": here["loss"]},
        "loss_gap": _gap(here["loss"], there["loss"]),
        "max_term_gap": max((t["gap"] for t in terms.values()), default=0.0),
        "terms": terms,
        "only_host": sorted(set(there["terms"]) - set(here["terms"])),
        "only_here": sorted(set(here["terms"]) - set(there["terms"])),
        "host_seconds": ran["host"]["wall_s"],
    }
    block["agrees"] = block["loss_gap"] <= HOST_AGREEMENT
    (out_dir / "probe.json").write_text(json.dumps(block, indent=2) + "\n")
    return block


def main(argv: list[str] | None = None) -> int:
    argv = sys.argv[1:] if argv is None else argv
    split = argv.index("--") if "--" in argv else len(argv)
    ours, fit_args = argv[:split], argv[split + 1 :]
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("stage", choices=("run", "pull", "confirm", "queue", "probe"))
    ap.add_argument("--slug")
    ap.add_argument("--out-dir", required=True, type=Path, help="queue: one subdirectory per slug")
    ap.add_argument("--local-lib", type=Path)
    ap.add_argument("--host-workers", type=int, default=12)
    ap.add_argument(
        "--host-cap", type=int, default=12, help="the workers this host may be given in all"
    )
    ap.add_argument("--host", help="send to this host instead of the configured route")
    ap.add_argument(
        "--resume",
        action="store_true",
        help="run: keep the host's output directory and continue the search it holds",
    )
    ap.add_argument("--list", type=Path, help="queue: the queued searches, one JSON per line")
    ap.add_argument("--status", type=Path, help="queue: where each ended search appends a line")
    ap.add_argument(
        "--parallel", type=int, default=1, help="queue: searches running at once on the host"
    )
    args = ap.parse_args(ours)
    if args.host_workers > args.host_cap:
        ap.error(f"the host takes at most {args.host_cap} workers (--host-cap)")
    if args.stage != "pull" and args.local_lib is None:
        ap.error(f"{args.stage} needs --local-lib, the BUILD_TUNING library on this Mac")
    if args.stage == "queue":
        if args.list is None or args.status is None:
            ap.error("queue needs --list and --status")
        if fit_args:
            ap.error("queue takes each search's arguments from --list")
        if not 1 <= args.parallel <= args.host_workers:
            ap.error("--parallel runs between one search and one per host worker")
    elif args.slug is None:
        ap.error(f"{args.stage} needs --slug")
    try:
        if args.stage == "run":
            out = run_remote(
                fit_args,
                args.slug,
                args.out_dir,
                args.local_lib.resolve(),
                args.host_workers,
                resume=args.resume,
                host=args.host,
            )
        elif args.stage == "pull":
            out = collect(args.slug, args.out_dir, host=args.host)
        elif args.stage == "queue":
            out = run_queue(
                load_queue(args.list),
                args.out_dir,
                args.local_lib.resolve(),
                args.host_workers,
                args.status,
                parallel=args.parallel,
                host=args.host,
            )
        elif args.stage == "probe":
            out = probe(
                fit_args,
                args.slug,
                args.out_dir,
                args.local_lib.resolve(),
                args.host_workers,
                host=args.host,
            )
        else:
            out = confirm(fit_args, args.slug, args.out_dir, args.local_lib.resolve())
    except (ValueError, RuntimeError) as exc:
        print(f"remote_fit: {exc}", file=sys.stderr)
        return 1
    print(json.dumps(out, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
