"""A scripted fake vtr-qor job for the runner tests (3H plan Task 5, T4).

Usage: ``python3 -I fakejob.py <spec.json>``. The spec is a JSON object; every key is optional:

- ``stamp`` (path) and ``name``: append ``start <name> <time>`` first and ``end <name> <time>``
  last, so a test can check that no two jobs overlap;
- ``write``: a list of ``{"path", "text", "age_s"}``; each file is written (parents created) and,
  with ``age_s``, dated that many seconds before now;
- ``oom_kill``: ``{"events", "add"}`` adds ``add`` to the ``oom_kill`` line of a fake
  ``memory.events``;
- ``ignore_term``: ignore SIGTERM (the grandchild inherits that across exec);
- ``on_term`` (path): on SIGTERM, write ``term`` there and exit 143;
- ``fd_dump`` (path): write ``<fd> <target>`` lines for the open descriptors there;
- ``spawn_sleep``: start a grandchild sleeping this many seconds in the same process group;
- ``spawn_session``: start that grandchild in a session of its own instead, as VTR's
  ``CommandRunner`` starts every stage (``start_new_session=True``);
- ``stage_on_term`` (path): the grandchild, on SIGTERM, waits 0.3 s, writes ``term`` there and
  exits (a stage shutting down cleanly);
- ``cgroup_procs`` (path): append the grandchild's pid to this fake ``cgroup.procs``, as the
  kernel puts a child in its parent's cgroup;
- ``pids``: write ``<own pid> <grandchild pid>`` here once the grandchild runs;
- ``stdout`` / ``stderr``: text to print on each stream;
- ``env_dump`` (path): write the job's environment there as JSON;
- ``sleep``: seconds to sleep before exiting;
- ``exit``: the exit status (default 0).
"""

from __future__ import annotations

import json
import os
import signal
import subprocess
import sys
import time
from pathlib import Path
from typing import Any


def _stamp(spec: dict[str, Any], what: str) -> None:
    if "stamp" in spec:
        with open(spec["stamp"], "a", encoding="utf-8") as handle:
            handle.write(f"{what} {spec.get('name', '?')} {time.time():.6f}\n")


def _write_files(spec: dict[str, Any]) -> None:
    for item in spec.get("write", []):
        path = Path(item["path"])
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(item.get("text", ""), encoding="utf-8")
        if "age_s" in item:
            when = time.time() - float(item["age_s"])
            os.utime(path, (when, when))


def _add_oom_kill(spec: dict[str, Any]) -> None:
    if "oom_kill" not in spec:
        return
    events = Path(spec["oom_kill"]["events"])
    lines = events.read_text(encoding="utf-8").splitlines() if events.exists() else []
    out, found = [], False
    for line in lines:
        fields = line.split()
        if len(fields) == 2 and fields[0] == "oom_kill":
            line = f"oom_kill {int(fields[1]) + int(spec['oom_kill']['add'])}"
            found = True
        out.append(line)
    if not found:
        out.append(f"oom_kill {int(spec['oom_kill']['add'])}")
    events.write_text("\n".join(out) + "\n", encoding="utf-8")


def _spawn(spec: dict[str, Any]) -> None:
    if "spawn_sleep" not in spec:
        return
    code = f"import time; time.sleep({float(spec['spawn_sleep'])})"
    if "stage_on_term" in spec:
        code = (
            "import os, signal, time\n"
            "def term(signum, frame):\n"
            "    time.sleep(0.3)\n"
            f"    open({spec['stage_on_term']!r}, 'w').write('term\\n')\n"
            "    os._exit(0)\n"
            "signal.signal(signal.SIGTERM, term)\n"
            f"time.sleep({float(spec['spawn_sleep'])})\n"
        )
    child = subprocess.Popen(
        [sys.executable, "-I", "-c", code], start_new_session=bool(spec.get("spawn_session"))
    )
    if "cgroup_procs" in spec:
        with open(spec["cgroup_procs"], "a", encoding="ascii") as handle:
            handle.write(f"{child.pid}\n")
    if "pids" in spec:
        Path(spec["pids"]).write_text(f"{os.getpid()} {child.pid}\n", encoding="utf-8")


def _fd_dump(path: str) -> None:
    lines = []
    for entry in sorted(os.listdir("/proc/self/fd"), key=int):
        try:
            lines.append(f"{entry} {os.readlink(f'/proc/self/fd/{entry}')}\n")
        except OSError:
            continue
    Path(path).write_text("".join(lines), encoding="utf-8")


def main() -> int:
    spec: dict[str, Any] = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
    _stamp(spec, "start")
    if spec.get("ignore_term"):
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
    if "on_term" in spec:
        def on_term(signum: int, frame: object) -> None:
            Path(spec["on_term"]).write_text("term\n", encoding="utf-8")
            os._exit(143)

        signal.signal(signal.SIGTERM, on_term)
    if "fd_dump" in spec:
        _fd_dump(spec["fd_dump"])
    if "env_dump" in spec:
        Path(spec["env_dump"]).write_text(json.dumps(dict(os.environ)), encoding="utf-8")
    _write_files(spec)
    _add_oom_kill(spec)
    sys.stdout.write(spec.get("stdout", ""))
    sys.stderr.write(spec.get("stderr", ""))
    sys.stdout.flush()
    sys.stderr.flush()
    _spawn(spec)
    time.sleep(float(spec.get("sleep", 0)))
    _stamp(spec, "end")
    return int(spec.get("exit", 0))


if __name__ == "__main__":
    sys.exit(main())
