"""Tests for tools/token-cost."""

from __future__ import annotations

import json
import tempfile
import unittest
from datetime import timedelta
from pathlib import Path

import token_cost

from . import helpers


def _assistant(req: str, model: str, stamp: str, **usage: int) -> str:
    body = {
        "input_tokens": usage.get("inp", 0),
        "output_tokens": usage.get("out", 0),
        "cache_read_input_tokens": usage.get("read", 0),
        "cache_creation_input_tokens": usage.get("w5", 0) + usage.get("w1", 0),
        "cache_creation": {"ephemeral_1h_input_tokens": usage.get("w1", 0)},
    }
    record = {"type": "assistant", "requestId": req, "timestamp": stamp,
              "message": {"model": model, "usage": body}}
    return json.dumps(record)


class TokenCostTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def _write(self, rel: str, lines: list[str]) -> None:
        path = self.root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("\n".join(lines) + "\n")

    def test_dedupes_repeated_request_lines_and_skips_bad_lines(self) -> None:
        line = _assistant("r1", "claude-opus-5-5", "2026-10-08T10:00:00Z", out=1000)
        self._write("s1.jsonl", [line, line, "{truncated", '{"type": "user"}'])
        scan = token_cost.scan_projects([self.root])
        self.assertEqual(len(scan.requests), 1)
        self.assertEqual(scan.bad_lines, 1)

    def test_cost_uses_cache_write_multipliers(self) -> None:
        u = token_cost.Usage(requests=1, input=1_000_000, write_5m=1_000_000,
                             write_1h=1_000_000, cache_read=1_000_000, output=1_000_000)
        # opus 5.5: 4 + 4*1.25 + 4*2 + 0.20 + 20
        self.assertAlmostEqual(u.cost("claude-opus-5-5") or 0.0, 37.2)
        self.assertIsNone(u.cost("unknown-model"))

    def test_subagent_label_from_meta(self) -> None:
        sub = "s1/subagents/agent-x.jsonl"
        self._write(sub, [_assistant("r2", "claude-sonnet-5-5", "2026-10-08T10:01:00Z", out=5)])
        (self.root / "s1/subagents/agent-x.meta.json").write_text(
            json.dumps({"description": "Review Task 1"}))
        scan = token_cost.scan_projects([self.root])
        self.assertEqual(scan.requests[0].agent, "Review Task 1")
        self.assertEqual(scan.requests[0].session, "s1")

    def test_active_time_caps_idle_gaps(self) -> None:
        self._write("s1.jsonl", [
            _assistant("a", "claude-opus-5-5", "2026-10-08T10:00:00Z"),
            _assistant("b", "claude-opus-5-5", "2026-10-08T10:05:00Z"),
            _assistant("c", "claude-opus-5-5", "2026-10-08T12:05:00Z"),
        ])
        times = [r.when for r in token_cost.scan_projects([self.root]).requests]
        self.assertEqual(token_cost.active_time(times, timedelta(minutes=15)),
                         timedelta(minutes=20))

    def test_main_writes_report(self) -> None:
        self._write("s1.jsonl", [_assistant("r1", "claude-fable-5-1", "2026-10-08T10:00:00Z",
                                            out=1_000_000)])
        out = self.root / "report.md"
        code, _, err = helpers.run_main(token_cost.main,
                                        ["--projects", str(self.root), "--out", str(out)])
        self.assertEqual(code, 0, err)
        text = out.read_text()
        self.assertIn("claude-fable-5-1", text)
        self.assertIn("1,000,000", text)
        self.assertIn("$50.00", text)  # reference cost line
    def _report(self, windows: str, out: Path | None = None) -> str:
        win = self.root / "windows.txt"
        win.write_text(windows)
        out = out or self.root / "report.md"
        code, _, err = helpers.run_main(
            token_cost.main,
            ["--projects", str(self.root), "--backends", str(win), "--out", str(out)])
        self.assertEqual(code, 0, err)
        return out.read_text()

    def _two_requests(self) -> None:
        self._write("s1.jsonl", [
            _assistant("a", "claude-opus-5-5", "2026-10-23T06:00:00Z", inp=100, out=10),
            _assistant("b", "claude-opus-5-5", "2026-10-23T08:00:00Z", inp=200, out=20),
            _assistant("c", "claude-sonnet-5-5", "2026-10-23T09:00:00Z", inp=300, out=30),
        ])

    def test_no_windows_keeps_anthropic_tables(self) -> None:
        self._two_requests()
        text = self._report("# nothing\n")
        self.assertIn("| **total** | 3 | 600 | 0 | 0 | 60 | 660 |", text)
        self.assertIn("| anthropic | 3 | 660 |", text)
        self.assertIn("| deepseek | 0 | 0 |", text)
        self.assertIn("No requests fall inside", text)

    def test_window_moves_requests_and_leaves_anthropic_tables_unchanged(self) -> None:
        self._two_requests()
        before = self._report("")
        after = self._report("deepseek 2026-10-23T07:00:00Z 2026-10-23T12:00:00Z\n")
        self.assertIn("| deepseek | 2 | 550 |", after)
        self.assertIn("| anthropic | 1 | 110 |", after)
        self.assertIn("| `deepseek-v4-pro` | `claude-opus-5-5` | 1 | 200", after)
        self.assertIn("| `deepseek-flash` | `claude-sonnet-5-5` | 1 | 300", after)
        self.assertIn("| **total** | 1 | 100 | 0 | 0 | 10 | 110 |", after)
        self.assertNotIn("| **total** | 3 | 600", after)
        # the Anthropic by-model table of a run without the moved requests is the same
        self.assertEqual(self._section(before, "By model").count("claude-opus-5-5"), 1)
        self.assertEqual(self._section(after, "By model").count("claude-opus-5-5"), 1)

    @staticmethod
    def _section(text: str, title: str) -> str:
        return text.split(f"## {title}\n", 1)[1].split("\n## ", 1)[0]

    def test_span_covers_all_backends(self) -> None:
        self._two_requests()
        text = self._report("deepseek 2026-10-23T07:00:00Z open\n")
        self.assertIn("Span: 2026-10-23 06:00 → 2026-10-23 09:00 UTC (all backends)", text)

    def test_open_window(self) -> None:
        self._two_requests()
        text = self._report("deepseek 2026-10-23T07:00:00Z open\n")
        self.assertIn("| deepseek | 2 | 550 |", text)

    def test_boundary_start_inclusive_end_exclusive(self) -> None:
        self._two_requests()
        text = self._report("deepseek 2026-10-23T08:00:00Z 2026-10-23T09:00:00Z\n")
        self.assertIn("| deepseek | 1 | 220 |", text)
        self.assertIn("| anthropic | 2 | 440 |", text)

    def test_console_table_carried_forward(self) -> None:
        self._two_requests()
        out = self.root / "report.md"
        first = self._report("", out)
        self.assertIn("_fill from platform.deepseek.com usage page_", first)
        row = "| 2026-10-23 | deepseek-v4-pro | 1,000 | 50 | $0.01 | console |"
        out.write_text(first.replace(token_cost.CONSOLE_PLACEHOLDER, row))
        again = self._report("deepseek 2026-10-23T07:00:00Z open\n", out)
        self.assertIn(row, again)
        self.assertNotIn("_fill from", again)

    def test_bad_windows_file_is_input_error(self) -> None:
        self._two_requests()
        win = self.root / "windows.txt"
        win.write_text("openai 2026-10-23T07:00:00Z open\n")
        code, _, err = helpers.run_main(token_cost.main,
                                        ["--projects", str(self.root), "--backends", str(win)])
        self.assertEqual(code, 2)
        self.assertIn("windows.txt", err)

    def test_committed_windows_file_parses(self) -> None:
        path = helpers.REPO_ROOT / "docs" / "token_backends.txt"
        token_cost.load_windows(path)

    def test_missing_dir_is_input_error(self) -> None:
        code, _, err = helpers.run_main(token_cost.main, ["--projects", str(self.root / "no")])
        self.assertEqual(code, 2)
        self.assertIn("token-cost:", err)


if __name__ == "__main__":
    unittest.main()
