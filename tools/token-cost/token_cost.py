"""token-cost: report the tokens, models and time used to build Odin III.

Usage::

    token-cost [--projects DIR ...] [--out FILE] [--idle-minutes N]

Reads Claude Code transcripts (``<projects dir>/*.jsonl`` for main sessions and
``<projects dir>/*/subagents/*.jsonl`` for subagents) and writes a Markdown report: tokens by
model (input, cache writes, cache reads, output), API-equivalent cost at list prices, sessions,
subagents, days, wall-clock and active time (API-equivalent cost only as a
reference line). Each API request is logged once per content block,
so records are de-duplicated by request ID. Malformed lines (e.g. from a crash) are skipped and
counted.

Costs are what the same tokens would cost on the Claude API at the list prices in ``PRICES``
(from the claude-api reference, cached 2026-10-06); the project actually runs on a Claude Max
subscription, so this is a comparable figure, not a bill. Update ``PRICES`` when prices change.

Exit 0 on success, 2 on usage or input error.
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import defaultdict
from collections.abc import Callable, Iterable, Sequence
from dataclasses import dataclass, field
from datetime import datetime, timedelta
from pathlib import Path

DEFAULT_PROJECTS = Path.home() / ".claude" / "projects" / "-home-jamiespa-odin3-ws"
MTOK = 1_000_000
WRITE_5M = 1.25  # cache-write multipliers of the input price
WRITE_1H = 2.0


@dataclass(frozen=True)
class Price:
    """USD per million tokens."""

    input: float
    output: float
    cache_read: float


PRICES: dict[str, Price] = {
    "claude-fable-5-1": Price(10.0, 50.0, 0.25),
    "claude-opus-5-5": Price(4.0, 20.0, 0.20),
    "claude-sonnet-5-5": Price(2.0, 10.0, 0.20),
    "claude-haiku-5-5": Price(0.10, 0.50, 0.01),  # cache read not listed; 0.1x input assumed
}


@dataclass
class Usage:
    """Token counts for one or more requests."""

    requests: int = 0
    input: int = 0
    write_5m: int = 0
    write_1h: int = 0
    cache_read: int = 0
    output: int = 0

    def add(self, other: Usage) -> None:
        self.requests += other.requests
        self.input += other.input
        self.write_5m += other.write_5m
        self.write_1h += other.write_1h
        self.cache_read += other.cache_read
        self.output += other.output

    @property
    def total(self) -> int:
        return self.input + self.write_5m + self.write_1h + self.cache_read + self.output

    def cost(self, model: str) -> float | None:
        price = PRICES.get(model)
        if price is None:
            return None
        return (
            self.input * price.input
            + self.write_5m * price.input * WRITE_5M
            + self.write_1h * price.input * WRITE_1H
            + self.cache_read * price.cache_read
            + self.output * price.output
        ) / MTOK


@dataclass
class Request:
    """One de-duplicated API request."""

    model: str
    when: datetime
    usage: Usage
    session: str
    agent: str  # "main" or the subagent's description


@dataclass
class Scan:
    requests: list[Request] = field(default_factory=list)
    bad_lines: int = 0
    files: int = 0


def usage_of(raw: dict[str, object]) -> Usage:
    def num(key: str, src: dict[str, object] = raw) -> int:
        value = src.get(key, 0)
        return value if isinstance(value, int) else 0

    creation = raw.get("cache_creation")
    write_1h = num("ephemeral_1h_input_tokens", creation) if isinstance(creation, dict) else 0
    write_total = num("cache_creation_input_tokens")
    return Usage(
        requests=1,
        input=num("input_tokens"),
        write_5m=max(write_total - write_1h, 0),
        write_1h=write_1h,
        cache_read=num("cache_read_input_tokens"),
        output=num("output_tokens"),
    )


def agent_label(path: Path) -> str:
    if path.parent.name != "subagents":
        return "main"
    meta = path.with_suffix(".meta.json")
    try:
        data = json.loads(meta.read_text())
    except (OSError, ValueError):
        return "subagent"
    desc = data.get("description") if isinstance(data, dict) else None
    return str(desc) if desc else "subagent"


def session_of(path: Path) -> str:
    return path.stem if path.parent.name != "subagents" else path.parent.parent.name


def parse_record(line: str) -> dict[str, object] | None:
    try:
        record = json.loads(line)
    except ValueError:
        return None
    return record if isinstance(record, dict) else None


def scan_file(path: Path, scan: Scan, seen: set[str]) -> None:
    label, session = agent_label(path), session_of(path)
    latest: dict[str, Request] = {}
    with path.open(encoding="utf-8", errors="replace") as fh:
        for line in fh:
            record = parse_record(line)
            if record is None:
                scan.bad_lines += 1
                continue
            req = request_of(record, session, label)
            if req is not None:
                key = str(record.get("requestId") or record.get("uuid"))
                latest[key] = req
    for key, req in latest.items():
        if key not in seen:
            seen.add(key)
            scan.requests.append(req)
    scan.files += 1


def request_of(record: dict[str, object], session: str, label: str) -> Request | None:
    msg = record.get("message")
    if record.get("type") != "assistant" or not isinstance(msg, dict):
        return None
    model, usage, stamp = msg.get("model"), msg.get("usage"), record.get("timestamp")
    if not isinstance(model, str) or model.startswith("<") or not isinstance(usage, dict):
        return None
    if not isinstance(stamp, str):
        return None
    when = datetime.fromisoformat(stamp.replace("Z", "+00:00"))
    return Request(model, when, usage_of(usage), session, label)


def scan_projects(dirs: Iterable[Path]) -> Scan:
    scan, seen = Scan(), set[str]()
    for root in dirs:
        if not root.is_dir():
            raise FileNotFoundError(f"{root}: not a directory")
        files = sorted(root.glob("*.jsonl")) + sorted(root.glob("*/subagents/*.jsonl"))
        for path in files:
            scan_file(path, scan, seen)
    scan.requests.sort(key=lambda r: r.when)
    return scan


def active_time(times: Sequence[datetime], idle: timedelta) -> timedelta:
    """Sum of gaps between consecutive requests, each gap capped at `idle`."""
    total = timedelta()
    for prev, cur in zip(times, times[1:], strict=False):
        total += min(cur - prev, idle)
    return total


def fmt_tokens(n: int) -> str:
    return f"{n:,}"


def fmt_cost(c: float | None) -> str:
    return "n/a" if c is None else f"${c:,.2f}"


def hours(td: timedelta) -> str:
    return f"{td.total_seconds() / 3600:.1f} h"


TOKEN_HEADER = "| Requests | Input | Cache write | Cache read | Output | Total tokens |"
TOKEN_ALIGN = "---:|---:|---:|---:|---:|---:|"


def token_cells(u: Usage) -> str:
    cells = [u.input, u.write_5m + u.write_1h, u.cache_read, u.output, u.total]
    return f"{u.requests:,} | " + " | ".join(fmt_tokens(c) for c in cells)


GroupKey = tuple[str, ...]


def sum_by(scan: Scan, key_of: Callable[[Request], GroupKey]) -> dict[GroupKey, Usage]:
    groups: dict[GroupKey, Usage] = defaultdict(Usage)
    for req in scan.requests:
        groups[key_of(req)].add(req.usage)
    return groups


def model_table(scan: Scan) -> list[str]:
    groups = sum_by(scan, lambda r: (r.model,))
    rows = ["| Model " + TOKEN_HEADER, "|---|" + TOKEN_ALIGN]
    total = Usage()
    for (model,), u in sorted(groups.items(), key=lambda kv: -kv[1].total):
        total.add(u)
        rows.append(f"| `{model}` | {token_cells(u)} |")
    rows.append(f"| **total** | {token_cells(total)} |")
    return rows


def who(req: Request) -> str:
    return "main session" if req.agent == "main" else "subagent"


def role_table(scan: Scan) -> list[str]:
    groups = sum_by(scan, lambda r: (who(r), r.model))
    rows = ["| Who | Model " + TOKEN_HEADER, "|---|---|" + TOKEN_ALIGN]
    for (role, model), u in sorted(groups.items(), key=lambda kv: (kv[0][0], -kv[1].total)):
        rows.append(f"| {role} | `{model}` | {token_cells(u)} |")
    n_main = len({r.session for r in scan.requests if r.agent == "main"})
    n_sub = len({(r.session, r.agent) for r in scan.requests if r.agent != "main"})
    rows += ["", f"{n_main} main sessions; {n_sub} subagent runs."]
    return rows


def day_table(scan: Scan, idle: timedelta) -> list[str]:
    groups = sum_by(scan, lambda r: (r.when.date().isoformat(), r.model))
    rows = ["| Day (UTC) | Model " + TOKEN_HEADER + " Active time (day) |",
            "|---|---|" + TOKEN_ALIGN + "---:|"]
    for (day, model), u in sorted(groups.items(), key=lambda kv: (kv[0][0], -kv[1].total)):
        times = [r.when for r in scan.requests if r.when.date().isoformat() == day]
        rows.append(f"| {day} | `{model}` | {token_cells(u)} | "
                    f"{hours(active_time(times, idle))} |")
    return rows


def top_subagents(scan: Scan, limit: int = 20) -> list[str]:
    subs = [r for r in scan.requests if r.agent != "main"]
    groups = sum_by(Scan(requests=subs), lambda r: (r.agent, r.model))
    rows = ["| Subagent (description) | Model " + TOKEN_HEADER, "|---|---|" + TOKEN_ALIGN]
    ranked = sorted(groups.items(), key=lambda kv: -kv[1].total)[:limit]
    for (name, model), u in ranked:
        rows.append(f"| {name} | `{model}` | {token_cells(u)} |")
    return rows


def cost_note(scan: Scan) -> str:
    total = sum(r.usage.cost(r.model) or 0.0 for r in scan.requests)
    prices = "; ".join(f"`{m}` ${p.input:g}/${p.output:g}/${p.cache_read:g}"
                       for m, p in PRICES.items())
    return (f"For reference only: at Claude API list prices these tokens would cost about "
            f"{fmt_cost(total)} (the work runs on a Claude Max subscription). Prices per "
            f"million tokens, input/output/cache read: {prices}; cache writes 1.25x input "
            "(5-minute) or 2x (1-hour). Source: the claude-api reference in Claude Code, "
            "cached 2026-10-06; Haiku 5.5's cache-read price is assumed 0.1x input.")


def tilde(path: Path) -> str:
    home = str(Path.home())
    text = str(path)
    return "~" + text[len(home):] if text.startswith(home) else text


def render(scan: Scan, idle: timedelta, sources: Sequence[Path]) -> str:
    if not scan.requests:
        return "# Tokens, models and time\n\nNo requests found.\n"
    first, last = scan.requests[0].when, scan.requests[-1].when
    active = active_time([r.when for r in scan.requests], idle)
    lines = [
        "# Tokens, models and time — building Odin III",
        "",
        "Generated by `tools/token-cost/token-cost` from the Claude Code transcripts in "
        + ", ".join(f"`{tilde(s)}`" for s in sources)
        + f". Regenerate after each session. Span: {first:%Y-%m-%d %H:%M} → "
        f"{last:%Y-%m-%d %H:%M} UTC; {len(scan.requests):,} API requests from {scan.files} "
        f"transcript files ({scan.bad_lines} malformed lines skipped).",
        "",
        "Columns: *Input* = uncached input tokens; *Cache write* / *Cache read* = input tokens "
        "written to / served from the prompt cache (re-sent context, mostly reads); *Output* = "
        "generated tokens, including thinking; *Total* = all four.",
        "",
        f"**Time:** wall-clock span {hours(last - first)}; active time {hours(active)} "
        f"(gaps between requests capped at {int(idle.total_seconds() // 60)} minutes, so idle "
        "time and crashes do not count).",
        "",
        "**Not counted:** design chats on claude.ai (the spec's origin), any advisor-model calls "
        "outside a request's own usage, and sessions run outside this workspace.",
        "",
        "## By model",
        "",
        *model_table(scan),
        "",
        "## Main sessions vs subagents, by model",
        "",
        *role_table(scan),
        "",
        "## By day and model",
        "",
        *day_table(scan, idle),
        "",
        "## Subagents using the most tokens",
        "",
        *top_subagents(scan),
        "",
        "## Reference: API-equivalent cost",
        "",
        cost_note(scan),
        "",
    ]
    return "\n".join(lines)


def main(argv: Sequence[str]) -> int:
    ap = argparse.ArgumentParser(prog="token-cost", description=__doc__.split("\n\n")[0])
    ap.add_argument("--projects", type=Path, nargs="+", default=[DEFAULT_PROJECTS])
    ap.add_argument("--out", type=Path, help="write the report here (default: stdout)")
    ap.add_argument("--idle-minutes", type=int, default=15)
    args = ap.parse_args(argv)
    try:
        scan = scan_projects(args.projects)
    except OSError as exc:
        print(f"token-cost: {exc}", file=sys.stderr)
        return 2
    report = render(scan, timedelta(minutes=args.idle_minutes), args.projects)
    if args.out:
        args.out.write_text(report)
    else:
        sys.stdout.write(report)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
