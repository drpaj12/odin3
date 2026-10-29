"""Run records for Mimir III: one `mimir-run.json` per odin3-flow run (stdlib only).

The record carries the full how-obtained data of a run: tool commits (how found, clean or dirty),
binary sha256 and `--version` text, the exact command lines (paths replaced by placeholders), every
parameter as {value, conf, source}, seed, host alias, time and peak RSS, input and artifact sha256
with relative paths. The format is the Mimir run-record spec (mimirIII,
docs/specs/2026-10-28-mimir-run-record.md, record_version 1); the importer is
`mimir import-runrecords`. Nothing here deletes a file.

    rec = RunRecorder(source, design, arm, seed, arch, roots={"out": out}, top="x")
    rec.add_step(name, argv, rc, time_s, rss_mib)
    rec.add_tool("odin3", tool_entry(tree, binary))
    rec.add_input(role, path) / rec.add_artifact(role, path)
    rec.write(out / "mimir-run.json")          # RecordRefused: a privacy rule hit

`conf` values are those of the Mimir schema: given, default (cited `file:line` at a named commit),
default-unrecorded, unknown. A value that is only inferred (the commit of a source tree the binary
cannot be shown to come from) is recorded `unknown` with a source starting
`inferred at record time`. The host alias comes from `$MIMIR_HOST_MAP`; without the map the alias
is `unknown`, never the name. Mimir is not imported.
"""

from __future__ import annotations

import contextlib
import fcntl
import getpass
import hashlib
import json
import os
import platform
import re
import subprocess
import tempfile
import time
from collections.abc import Sequence
from pathlib import Path

RECORD_VERSION = 1
KIND = "odin3-flow-run"
FILE_NAME = "mimir-run.json"
SHA40 = re.compile(r"[0-9a-f]{40}")
ALIAS_RE = re.compile(r"host-\d+")
UNKNOWN_HOST = "unknown"
# an absolute path in a value (spec section 6): a slash that starts the string or follows a blank,
# `=`, `,`, `;`, `:`, a quote or a bracket and is followed by a path character; /dev/null is allowed
ABS_PATH = re.compile(r"(?:^|[\s=,;:'\"(\[])/(?!dev/null(?![\w.-]))[\w.~-]")
_PATH_IN_TEXT = re.compile(r"(?<![\w.~>-])/(?!dev/null(?![\w.-]))[\w.~/+@%-]+")
VPR_OPTIONS_HEAD = "VPR was run with the following options:"
_HOST_LINE = re.compile(r'("(?:[^"\\]|\\.)*")\s*=\s*("(?:[^"\\]|\\.)*")')
OPT_LINE = re.compile(r"^([A-Za-z_]\w*\.[\w.]+)\s*[:=]\s*(.*)$")


class RecordRefused(Exception):
    """The record holds a private string or an absolute path; `hits` are 'rule at key.path'."""

    def __init__(self, hits: list[str]) -> None:
        super().__init__("; ".join(hits))
        self.hits = hits


# ---------------------------------------------------------------- host alias


def map_path() -> Path:
    """$MIMIR_HOST_MAP, else work/mimir-data/remote-private/hosts.toml of the workspace."""
    env = os.environ.get("MIMIR_HOST_MAP")
    if env:
        return Path(env)
    return Path.home() / "odin3-ws" / "work" / "mimir-data" / "remote-private" / "hosts.toml"


def _read_hosts(path: Path) -> dict[str, str]:
    """The `[hosts]` table: lines `"name" = "host-<n>"` (append-only, written by Mimir)."""
    table: dict[str, str] = {}
    in_hosts = False
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if line.startswith("["):
            in_hosts = line == "[hosts]"
        elif in_hosts and line and not line.startswith("#"):
            m = _HOST_LINE.fullmatch(line)
            if m is None:
                raise ValueError("host map line is not a quoted name = alias pair")
            table[json.loads(m.group(1)).lower()] = json.loads(m.group(2))
    return table


def host_table(path: Path | None = None) -> dict[str, str]:
    """Lower-cased real name to alias, read under a shared flock when the lock file exists."""
    path = path or map_path()
    lock = path.with_name(path.name + ".lock")
    if not lock.exists():
        return _read_hosts(path)
    fd = os.open(lock, os.O_RDWR)
    try:
        fcntl.flock(fd, fcntl.LOCK_SH)
        return _read_hosts(path)
    finally:
        os.close(fd)


def _append_alias(path: Path, key: str) -> str:
    """Number `key` as host-<max+1> and append one line, as Mimir's host-alias does."""
    lock = path.with_name(path.name + ".lock")
    fd = os.open(lock, os.O_RDWR | os.O_CREAT, 0o600)
    try:
        os.fchmod(fd, 0o600)
        fcntl.flock(fd, fcntl.LOCK_EX)
        table = _read_hosts(path)
        if key in table:
            return table[key]
        used = [int(a.split("-")[1]) for a in table.values() if ALIAS_RE.fullmatch(a)]
        alias = f"host-{max(used, default=0) + 1}"
        line = f"{json.dumps(key)} = {json.dumps(alias)}\n"
        if path.stat().st_size and path.read_bytes()[-1:] != b"\n":
            line = "\n" + line
        afd = os.open(path, os.O_WRONLY | os.O_APPEND)
        try:
            os.fchmod(afd, 0o600)
            os.write(afd, line.encode("utf-8"))
            os.fsync(afd)
        finally:
            os.close(afd)
        return alias
    finally:
        os.close(fd)


def host_alias(real: str | None = None, path: Path | None = None) -> str:
    """The alias of this host. No map, an unreadable map or an empty name gives `unknown`: the
    real name is never returned."""
    real = (real or platform.node()).strip()
    path = path or map_path()
    if not real or not path.is_file():
        return UNKNOWN_HOST
    if ALIAS_RE.fullmatch(real):
        return real
    key = real.lower()
    try:
        table = host_table(path)
        return table[key] if key in table else _append_alias(path, key)
    except (OSError, ValueError):
        return UNKNOWN_HOST


# ---------------------------------------------------------------- git and binaries


def git(root: Path, *args: str) -> str:
    res = subprocess.run(["git", "-C", str(root), *args], capture_output=True, text=True,
                         check=False)
    return res.stdout.strip() if res.returncode == 0 else ""


def git_show(root: Path | None, commit: str, path: str) -> list[str] | None:
    """Lines of `path` as committed at `commit` (None without a tree, commit or file)."""
    if root is None or not commit:
        return None
    res = subprocess.run(["git", "-C", str(root), "show", f"{commit}:{path}"], capture_output=True,
                         text=True, errors="replace", check=False)
    return res.stdout.split("\n") if res.returncode == 0 else None


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def tool_entry(tree: Path | None, binary: Path | None, version_args: Sequence[str] | None = (
        "--version",)) -> dict[str, object]:
    """Facts about one tool: HEAD of its source tree and how it was found, whether the tree is clean
    AT RECORD TIME (and the sha256 of its diff when not), the binary's sha256 and `--version` text,
    whether that text names HEAD, and whether it says `-dirty` (`built_dirty`)."""
    head = git(tree, "rev-parse", "HEAD") if tree else ""
    head = head if SHA40.fullmatch(head) else ""
    clean: bool | None = None
    diff_sha = ""
    if head and tree:
        clean = git(tree, "status", "--porcelain", "--untracked-files=no") == ""
        if not clean:
            diff_sha = hashlib.sha256(git(tree, "diff", "HEAD").encode()).hexdigest()
    version = ""
    is_file = binary is not None and binary.is_file()
    if binary is not None and is_file and version_args is not None:
        res = subprocess.run([str(binary), *version_args], capture_output=True, text=True,
                             check=False, timeout=60, cwd=tempfile.gettempdir())
        version = (res.stdout + res.stderr).strip()
    how = "git rev-parse HEAD of the source tree" if head else "no git source tree"
    # Without a source tree, the `commit <sha>[-dirty]` line of the binary's own --version
    # (odin3 since v1.2.0 post-release) names the commit it was built from.
    stated = re.search(r"(?m)^commit ([0-9a-f]{40})(-dirty)?$", version)
    if not head and stated:
        head, how = stated.group(1), "the commit line of the binary's --version"
    return {"commit": head, "commit_how": how, "clean": clean, "dirty_diff_sha256": diff_sha,
            "binary_sha256": file_sha256(binary) if binary is not None and is_file else "",
            "binary_version": version,
            "built_dirty": (re.search(r"-dirty\b", version) is not None) if version else None,
            "version_names_commit": bool(head) and head[:10] in version}


def parse_time_v(text: str) -> tuple[float | None, float | None]:
    """(wall seconds, peak RSS MiB) from `/usr/bin/time -v` text; None for an absent value."""
    el = re.search(r"Elapsed \(wall clock\) time \(h:mm:ss or m:ss\): ([0-9:.]+)", text)
    rss = re.search(r"Maximum resident set size \(kbytes\): (\d+)", text)
    secs = None
    if el:
        secs = 0.0
        for part in el.group(1).split(":"):
            secs = secs * 60 + float(part)
    return secs, (int(rss.group(1)) / 1024.0 if rss else None)


def option_default(lines: list[str], option: str) -> tuple[str, str] | None:
    """(value, 'first-last') of the `.default_value("x")` after the argparse entry of `option` in
    the lines of a VPR source file."""
    for i, ln in enumerate(lines):
        if f'"{option}")' in ln:
            for j in range(i, min(i + 20, len(lines))):
                m = re.search(r'\.default_value\("([^"]*)"\)', lines[j])
                if m:
                    return m.group(1), f"{i + 1}-{j + 1}"
            return None
    return None


def line_of(lines: list[str] | None, needle: str) -> int:
    """1-based line of the first line holding `needle`; 0 when absent."""
    return next((i + 1 for i, ln in enumerate(lines or []) if needle in ln), 0)


def cmd_options(flags: list[str]) -> list[tuple[str, list[str]]]:
    """`--key v1 v2 -key2` as [(key, [v1, v2]), (key2, [])]; the dashes are dropped."""
    out: list[tuple[str, list[str]]] = []
    for item in flags:
        if item.startswith("-") and len(item) > 1 and not item[1:].replace(".", "").isdigit():
            out.append((item.lstrip("-"), []))
        elif out:
            out[-1][1].append(item)
    return out


# ---------------------------------------------------------------- the record


class RunRecorder:
    """Builds one run record. `roots` maps a placeholder name to a directory: a path under it is
    written `<name>/relative`; any other absolute path `<abs>/basename`; a file in `roles` as
    `<role>`. `top` (the design's top module) is written `<top>` where it is a whole argument or a
    file stem, so the command text of one setup does not change from design to design."""

    def __init__(self, source: str, design: str, arm: str, seed: int, arch: str,
                 roots: dict[str, Path], roles: dict[Path, str] | None = None,
                 top: str = "") -> None:
        self.rec: dict[str, object] = {
            "record_version": RECORD_VERSION, "kind": KIND, "source": source, "design": design,
            "arm": arm, "seed": int(seed), "family": "", "arch": arch, "host": host_alias(),
            "kernel": platform.release(),
            "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "steps": [], "tools": {}, "params": {}, "inputs": {}, "artifacts": {}}
        self.roots = {k: v.resolve() for k, v in roots.items()}
        self.roles = {k.resolve(): v for k, v in (roles or {}).items()}
        self.top = top
        self.literals: dict[str, str] = {}  # exact text (a design's own file name) -> placeholder
        self.missing: list[tuple[str, str]] = []  # (table, role) of files absent at record time

    # paths
    def _one(self, text: str) -> str:
        path = Path(text)
        rp = path.resolve()
        if rp in self.roles:
            return self.roles[rp]
        for name, root in sorted(self.roots.items(), key=lambda kv: -len(str(kv[1]))):
            if rp == root or root in rp.parents:
                return f"<{name}>/{rp.relative_to(root).as_posix()}".rstrip("/")
        return f"<abs>/{path.name}"

    def ph(self, text: str) -> str:
        """`text` with every absolute path a placeholder and the top module `<top>`."""
        out = text
        for literal, holder in self.literals.items():
            out = out.replace(literal, holder)
        out = _PATH_IN_TEXT.sub(lambda m: self._one(m.group(0)), out)
        top = self.top
        if top and (out == top or out.endswith("=" + top)):
            return out[: len(out) - len(top)] + "<top>"
        if top and len(top) >= 3:
            out = re.sub(rf"(?<![\w$.-]){re.escape(top)}(?=\.\w|$)", "<top>", out)
        return out

    def argv(self, argv: Sequence[str]) -> list[str]:
        return [self.ph(a) for a in argv]

    # contents
    def add_step(self, name: str, argv: Sequence[str], rc: int, time_s: float | None,
                 rss_mib: float | None, shared: bool = False) -> None:
        """One tool run. A time or RSS that was not measured is null with conf `unknown`, never 0.
        `shared`: the time is contained in another step of the record (left out of the sum)."""
        steps = self.rec["steps"]
        assert isinstance(steps, list)
        steps.append({
            "name": name, "argv": self.argv(argv), "exit_code": int(rc),
            "time_s": None if time_s is None else round(time_s, 3),
            "time_conf": "unknown" if time_s is None else "given",
            "peak_rss_mib": None if rss_mib is None else round(rss_mib, 1),
            "locked": False, "shared": bool(shared)})

    def param(self, key: str, value: str, conf: str, source: str) -> None:
        params = self.rec["params"]
        assert isinstance(params, dict)
        params[key] = {"value": self.ph(value) or "(empty)", "conf": conf, "source": source}

    def add_tool(self, name: str, entry: dict[str, object]) -> None:
        """A tool and its parameters. The commit is `given` only when the binary's `--version`
        text names HEAD and does not say `-dirty`; otherwise it is `unknown` (inferred)."""
        tools = self.rec["tools"]
        assert isinstance(tools, dict)
        tools[name] = entry
        commit = str(entry["commit"])
        built_dirty = entry.get("built_dirty")
        if commit and entry["version_names_commit"] and built_dirty is False:
            self.param(f"tool.{name}_commit", commit, "given",
                       f"{name} --version names HEAD of the source tree, not '-dirty'")
        else:
            why = ("there is no source tree" if not commit else
                   "the binary was built from a dirty tree (its --version says -dirty)"
                   if built_dirty else "the --version text does not name HEAD")
            self.param(f"tool.{name}_commit", commit, "unknown",
                       f"inferred at record time: HEAD of the source tree; not given because {why}")
        if built_dirty is not None:
            self.param(f"tool.{name}_built_dirty", str(bool(built_dirty)).lower(), "given",
                       f"{name} --version text ('-dirty' suffix)")
        clean = entry["clean"]
        self.param(f"tool.{name}_source_clean", "unknown" if clean is None else str(clean).lower(),
                   "given" if clean is not None else "unknown",
                   "at record time: git status --porcelain of the source tree (tracked files)")
        if entry["dirty_diff_sha256"]:
            self.param(f"tool.{name}_dirty_diff_sha256", str(entry["dirty_diff_sha256"]), "given",
                       "at record time: sha256 of git diff HEAD of the source tree")
        sha = str(entry["binary_sha256"])
        self.param(f"tool.{name}_binary_sha256", sha, "given" if sha else "unknown",
                   "sha256 of the binary file")

    def add_linked_tool(self, name: str, commit: str, how: str) -> None:
        """A tool linked into another binary, whose commit that binary states itself."""
        tools = self.rec["tools"]
        assert isinstance(tools, dict)
        tools[name] = {"commit": commit, "commit_how": how, "clean": None,
                       "dirty_diff_sha256": "", "binary_sha256": "", "binary_version": "",
                       "built_dirty": None, "version_names_commit": bool(commit)}
        self.param(f"tool.{name}_commit", commit, "given" if commit else "unknown",
                   how if commit else f"{how}: not stated")

    def add_input(self, role: str, path: Path) -> None:
        self._file("inputs", role, path)

    def add_artifact(self, role: str, path: Path) -> None:
        self._file("artifacts", role, path)

    def _file(self, table: str, role: str, path: Path) -> None:
        if path.is_file():
            tab = self.rec[table]
            assert isinstance(tab, dict)
            tab[role] = {"path": self.ph(str(path.resolve())), "sha256": file_sha256(path)}
        else:
            self.missing.append((table, role))

    def add_file_entry(self, table: str, role: str, path: str, sha256: str) -> None:
        """A file entry whose path is already relative (a combined input)."""
        tab = self.rec[table]
        assert isinstance(tab, dict)
        tab[role] = {"path": path, "sha256": sha256}

    def add_vpr_echo(self, log: Path, prefix: str = "vpr") -> None:
        """Every option VPR echoed in its log (its own statement of the effective value, cited
        `vpr.out:line`). A log without the echo header is one `unknown` row."""
        lines = log.read_text(encoding="utf-8", errors="replace").split("\n") if log.is_file() \
            else []
        if VPR_OPTIONS_HEAD not in lines:
            self.param(f"{prefix}.options_echo", "", "unknown",
                       f"vpr.out lacks the line '{VPR_OPTIONS_HEAD}': the options are not recorded")
            return
        for n in range(lines.index(VPR_OPTIONS_HEAD) + 1, len(lines)):
            m = OPT_LINE.match(lines[n])
            if m:
                self.param(f"{prefix}.{m.group(1)}", m.group(2).strip() or "(empty)", "given",
                           f"vpr.out:{n + 1} (VPR's echo of the effective option)")
            elif (lines[n].strip() and not lines[n].startswith(" ")
                  and not lines[n].startswith("Using up to")):
                break

    def write(self, path: Path, required_inputs: tuple[str, ...] = ("netlist",)) -> None:
        """Write the record. A required role that was not added is an `unknown` parameter (never
        dropped), as is every file absent at record time. RecordRefused (nothing written) when a
        privacy rule hits."""
        steps: list[dict[str, object]] = self.rec["steps"]  # type: ignore[assignment]
        inputs = self.rec["inputs"]
        assert isinstance(inputs, dict)
        for role in required_inputs:
            if role not in inputs and ("inputs", role) not in self.missing:
                self.missing.append(("inputs", role))
        for table, role in self.missing:
            self.param(f"{table}.{role}_sha256", "", "unknown", "file absent at record time")
        own = [s for s in steps if not s["shared"]]
        times = [s["time_s"] for s in own]
        rss = [s["peak_rss_mib"] for s in own]
        self.rec["time_s"] = None if (not own or None in times) else round(
            sum(times), 3)  # type: ignore[arg-type]
        self.rec["time_conf"] = "unknown" if self.rec["time_s"] is None else "given"
        self.rec["peak_rss_mib"] = None if (not rss or None in rss) else max(
            rss)  # type: ignore[type-var]
        self.rec["ok"] = bool(steps) and steps[-1]["exit_code"] == 0
        hits = privacy_hits(self.rec)
        if hits:
            raise RecordRefused(hits)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(self.rec, indent=1, sort_keys=True) + "\n", encoding="utf-8")


def _strings(obj: object, where: str = "record") -> list[tuple[str, str]]:
    if isinstance(obj, str):
        return [(where, obj)]
    if isinstance(obj, dict):
        return [x for k, v in obj.items() for x in _strings(v, f"{where}.{k}")]
    if isinstance(obj, list):
        return [x for i, v in enumerate(obj) for x in _strings(v, f"{where}[{i}]")]
    return []


def private_strings() -> list[tuple[str, str]]:
    """(rule, string) no record may hold: the home directory, user name and host name of this
    machine and every real host name of the host map. The rule names are what a log may show."""
    out = [("home-directory", str(Path.home())), ("user-name", getpass.getuser()),
           ("host-name", platform.node())]
    with contextlib.suppress(OSError, ValueError):
        out += [("host-map-name", n) for n in host_table()]
    return [(r, s) for r, s in out if len(s) >= 3]


def privacy_hits(rec: object) -> list[str]:
    """Rule and key path of every string of `rec` that holds a private string, a home path or an
    absolute path (spec section 6)."""
    privates = [(r, s.lower()) for r, s in private_strings()]
    hits = []
    for where, text in _strings(rec):
        low = text.lower()
        hits += [f"{rule} at {where}" for rule, s in privates if s in low]
        if "/home" + "/" in text or "/Users/" in text:
            hits.append(f"home-path at {where}")
        elif ABS_PATH.search(text):
            hits.append(f"absolute-path at {where}")
    return hits
