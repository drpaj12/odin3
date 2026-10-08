# Odin III — Phase 0: Environment, Repos, and Agent Setup (executable)

Companion to `odin3-design-spec.md` §12 Phase 0 and §15 (code standard). This file is meant to be **run by Claude Code**: copy it into the repo as `docs/PHASE0.md` and start a session with the prompt in §A.

## A. How to execute this document

Every step is tagged:

- `[AGENT]` — Claude Code does it, then ticks the box and continues.
- `[HUMAN]` — Claude Code stops, prints the exact instructions for Peter, and waits for "done" before continuing.
- `[REVIEW]` — Claude Code does the work, then stops for Peter to review before moving on.

Prompt to start the run (in `~/odin3-ws`, Opus, `high` effort, plan mode off for this one — it's a checklist, not a design task):

> Read ~/odin3-ws/odin3-phase0-setup.md and execute it top to bottom, starting at §3 (§1–2 are done). Until §5 tick boxes in that file; in §5 it moves to odin3/docs/PHASE0.md and you tick boxes there from then on. At each `[HUMAN]` step, stop and tell me exactly what to do and wait for me to say done. At each `[REVIEW]` step, stop and summarize what you did and what I should look at. Commit at the end of each section once the repo exists. Never edit anything under ~/odin3-ws/external/.

Steps 1–2 happen before Claude Code exists, so Peter does them from this document directly. (2026-10-08: §1 and the root-level installs were done from a Windows-side Claude session via `wsl -u root`; VTR's `install_apt_packages.sh` has already been run as root, so §4 needs no sudo.)

### A.1 Multi-agent execution (added 2026-10-08, from §5 on)

The main session is the **orchestrator**: it owns git, this checklist, GitHub settings, and every `[HUMAN]`/`[REVIEW]` stop. It hands work to subagents only where the pieces are independent and the interface between them has been written down first.

- **Implementer agents** — one per independent slice, each owning a disjoint set of files. Before dispatch the orchestrator writes the shared interface (directory layout, CMake preset and target names, CLI flags, file formats) into every implementer's prompt. Implementers never run git, never touch files outside their slice, never edit `~/odin3-ws/external/`.
- **Critique agents** — fresh context, read-only, adversarial. Given the relevant spec sections and the diff, they hunt for spec violations, bugs, and claims nothing tests, and return findings ranked by severity. They do not fix. The orchestrator triages every finding (fix, or reject with a one-line reason) and lists rejected findings in the PR description. A critique runs before every PR is opened and before every `[REVIEW]` stop; findings rated high get one re-check after the fix.
- **Not split:** git, branch protection and other GitHub settings, the long upstream builds (one background task), and anything whose pieces would edit the same files.
- **Models:** implementers per §9 (Sonnet for mechanical slices, Opus for tools and harnesses); critics Opus at `high`, except Fable for the §6 rules-file critique and for IR-design critiques from Phase 1 on.
- **Budget:** at most 4 agents in flight; one critique pass per PR plus the re-check above.

| § | Implementers (in parallel) | Critique |
|---|---|---|
| 5 | (a) C core — CMake, presets, `odin3.h`, `src/`, `plugins/`, Unity tests — orchestrator; (b) Python tools — `netlist-compare`, `equiv-check`, their tests, `pyproject.toml` — Opus; (c) gate and CI — `lint.sh`, `.clang-tidy`, pre-commit, `ci.yml`, `nightly.yml` — Sonnet. `run-oracle.sh` is the orchestrator's once §4's build exists | one critic over the whole PR #1 diff against spec §15 and this §5 before `gh pr create` |
| 6 | (a) ADRs D1–D9 + D8′ from spec §2 — Sonnet; (b) the four skills — Sonnet; `CLAUDE.md` and `settings.json` — orchestrator | one critic (**Fable**, per decision #14): `CLAUDE.md` vs spec §12/§15; `settings.json` vs the current settings reference (hook stdin, deny-path syntax, sandbox network) |
| 7 | none — one scripted loop | one critic independently re-runs a sample of goldens and the `blink` equiv-check and compares hashes |
| 8 | none | the exit checklist is the critique |

### A.2 Decisions log

2026-10-08, Peter, on PR #1 (numbers match the question list):

1. Unit tests: **Unity** (spec §13 #8 resolved).
2. Keep `src/api/` for `odin3.h` entry points owned by no subsystem; added to the §5 tree.
3. §6 files (`CLAUDE.md`, `.claude/`, ADRs) go in PR #2, separate from PR #1.
4. Pre-commit runs `lint.sh --no-tidy`; clang-tidy runs in CI (required).
5. CI's "netlist-compare on `tests/micro`" step is wired when Odin III first writes BLIF (Phase 1–2).
6. Banned outside `check`: `abort()`, `raise(SIGABRT)`, `__builtin_trap()`; `assert()` allowed (compiles out of release). Spec §15.1 updated.
7. `equiv-check` treats latch init 2/3 as 0 with a notice; `--strict-init` refuses.
8. Add a functional `multiply` hard-block model to `equiv-check` in §7; memories wait for Phase 4.
9. Keep the `ODIN3_WERROR` option (default ON; CI never disables it).
10. Host-side plugin ABI check (exported `odin3_plugin_abi_version`) lands in Phase 1.
11. One-letter names: only `i`, `j`, `k`.
12. `readability-magic-numbers` off under `tests/` only.
13. §7 runs **all** `.v` files under `odin_ii/regression_test/benchmark/` plus VTR-19; failures are recorded (`status=failed`), not skipped.
14. Fable is the critic for §6.

## B. Who does what — summary

| Section | Agent | Human |
|---|---|---|
| 1. WSL2 | package installs, layout | `wsl --install`, `.wslconfig`, reboot (admin PowerShell) |
| 2. Claude Code + GitHub auth | — | install, `claude` login, `gh auth login`, install plugins |
| 3. Repos | create repos, labels, milestones, protection via `gh api` | verify branch protection in the GitHub UI |
| 4. Upstream builds | clone, build, smoke-test VTR/Yosys | nothing (optionally run in a cloud session) |
| 5. Repo skeleton | everything | review PR #1 line by line |
| 6. Agent config | drafts CLAUDE.md, settings, skills, ADRs | edit CLAUDE.md and ADRs; set `/model`, `/effort`, `/advisor` |
| 7. Golden netlists | run oracles, archive, compare | nothing |
| 8. Exit checklist | runs the checks | confirms, checks `/usage` |

Peter's total hands-on time: roughly one hour plus one PR review.

## 0. Where code lives (and where it doesn't)

**Not on Google Drive.** Git repos under a sync client get corrupted lock files and half-synced objects; WSL2 builds on a Windows-mounted path (`/mnt/c/...`, or a Drive folder) are 5–20× slower than the Linux filesystem; and Drive for Desktop can't see WSL's ext4 volume anyway. GitHub is the backup and the sync. Drive is fine for papers, notes, and the large golden-netlist archive.

Layout, all inside WSL2's own filesystem (`~`, not `/mnt/c`):

```
~/odin3-ws/
  odin3/                        # OUR repo  → github.com/<you>/odin3  (MIT)
  external/                     # upstream, read + build, never edited
    vtr-verilog-to-routing/     # VTR (brings yosys+parmys, abc, vpr, benchmarks, arch files)
    yosys/                      # standalone Yosys: differential oracle + source to read
    slang/                      # SystemVerilog front end library (Phase 5)
    titan/                      # Titan benchmarks (large download; Phase 2/6)
  golden/                       # archived Parmys / Odin II BLIFs  → github.com/<you>/odin3-golden (git LFS)
  work/                         # scratch: VTR flow run dirs, experiments; gitignored
```

Windows side: VS Code with the WSL extension opens `~/odin3-ws`; Windows Terminal → Ubuntu profile.

## 1. WSL2 + Ubuntu

- [x] `[HUMAN]` In an admin PowerShell: `wsl --install -d Ubuntu-24.04`. Reboot, create the Linux user.
- [x] `[HUMAN]` Create `C:\Users\<you>\.wslconfig`:
  ```ini
  [wsl2]
  memory=24GB
  processors=12
  swap=8GB
  ```
  (No inline `#` comments — WSL's parser may not accept them. Host: 32 GB RAM, 16 cores / 24 threads.)
  then `wsl --shutdown` in PowerShell and reopen Ubuntu.
- [x] `[HUMAN]` In Ubuntu, create the workspace and install the base toolchain (the agent needs `git`, `gh`, and a compiler to exist before it can take over):
  ```bash
  sudo apt update && sudo apt upgrade -y
  sudo apt install -y build-essential cmake ninja-build git git-lfs gh \
    flex bison clang clang-format clang-tidy clangd cppcheck lld gdb valgrind ccache \
    python3 python3-venv python3-pip pipx graphviz verilator iverilog ghdl \
    libreadline-dev tcl-dev libffi-dev zlib1g-dev pkg-config \
    bubblewrap socat jq          # clangd: C/C++ plugin; bubblewrap+socat: sandbox; jq: hooks
  pipx install lizard ruff mypy pre-commit
  pipx ensurepath              # then reopen the shell
  git lfs install
  mkdir -p ~/odin3-ws/{external,golden,work}
  ```
- [x] `[HUMAN]` Check: `nproc` and `free -g` show the `.wslconfig` numbers; `which ghdl verilator iverilog dot lizard` all resolve.

## 2. Claude Code and GitHub authentication

- [x] `[HUMAN]` Install Claude Code **inside WSL** per the "Install in WSL" section of https://code.claude.com/docs/en/setup. Run `claude` in `~/odin3-ws` and log in with your claude.ai account.
- [x] `[HUMAN]` `gh auth login` (GitHub.com, HTTPS, browser). The agent must never hold your GitHub credentials; `gh` holds them.
- [x] `[HUMAN]` Plugins and skills, inside `claude`:
  - Superpowers (already installed): `/superpowers:brainstorm`, `write-plan`, `execute-plan` are the per-pass workflow.
  - `/plugin` → Anthropic marketplace → install the C/C++ code-intelligence (clangd-based) plugin, so the agent gets compiler diagnostics instead of guessing.
  - Nothing else yet; project-local skills are written in §6.
- [x] `[HUMAN]` Model settings, inside `claude`: `/model` (Opus; note whether Fable is offered), `/effort high` as the default, `/advisor` → Opus. See §9 for when to raise to `xhigh`. (2026-10-08: Fable **is** offered — use it for the Phase 1 IR row in §9.)
- [x] `[HUMAN]` Hand over: `cd ~/odin3-ws && claude`, paste the §A prompt. Everything below is the agent's unless tagged.

## 3. Repositories

- [x] `[AGENT]` Peter created both repos on GitHub by hand (`odin3` with MIT license, `odin3-golden` empty). Clone them:
  ```bash
  cd ~/odin3-ws && gh repo clone odin3 && gh repo clone odin3-golden
  ```
  If either does not exist, stop and ask — do not create it.
  (2026-10-08: `odin3-golden` was moved to `~/odin3-ws/golden/` to match the §0 layout and the `../golden/` path in `run-oracle.sh`.)
- [x] `[AGENT]` In `odin3`: labels `phase-0`…`phase-7`, `ir`, `frontend`, `partial-map`, `matcher`, `re`, `writer`, `reader`, `test`, `oracle`, `tooling`; milestones = phases 0–7.
- [x] `[AGENT]` Protect `main` via `gh api` (require PR, require status checks `ci`, squash-merge only, delete branch on merge, no force-push). Branch protection on a **private** repo needs GitHub Pro/Team; if the API returns 403/"Upgrade to GitHub Pro", stop and tell Peter. The required check context must equal the job name in `ci.yml` — name that job `ci`.
- [x] `[HUMAN]` Verify branch protection in the GitHub UI (Settings → Branches). This is the safety rail for everything after; confirm it with your own eyes.
- [x] `[AGENT]` In `odin3-golden`: `git lfs track "*.blif"`, commit `.gitattributes` and a README stating that every golden records the VTR commit hash and arch file that produced it.

## 4. Upstream builds (the oracles)

- [x] `[AGENT]` Clone and build VTR with Odin II enabled (30–90 min; the agent may run this as a background task or in a cloud session):
  ```bash
  cd ~/odin3-ws/external
  git clone https://github.com/verilog-to-routing/vtr-verilog-to-routing.git
  cd vtr-verilog-to-routing && git submodule init && git submodule update
  # install_apt_packages.sh already run as root during setup — skip (needs sudo)
  make env && source .venv/bin/activate && pip install -r requirements.txt
  make CMAKE_PARAMS="-DWITH_ODIN=on" -j$(nproc)
  echo 'export VTR_ROOT=~/odin3-ws/external/vtr-verilog-to-routing' >> ~/.bashrc
  ```
  The `~/.bashrc` line only affects new shells. In every agent command that uses VTR, set it inline: `export VTR_ROOT=~/odin3-ws/external/vtr-verilog-to-routing && source $VTR_ROOT/.venv/bin/activate && ...`
- [x] `[AGENT]` Smoke test (from the VTR quickstart); must print `EArch/blink OK`:
  ```bash
  export VTR_ROOT=~/odin3-ws/external/vtr-verilog-to-routing && source $VTR_ROOT/.venv/bin/activate
  mkdir -p ~/odin3-ws/work/blink && cd ~/odin3-ws/work/blink
  $VTR_ROOT/vtr_flow/scripts/run_vtr_flow.py $VTR_ROOT/doc/src/quickstart/blink.v \
      $VTR_ROOT/vtr_flow/arch/timing/EArch.xml --route_chan_width 100
  ```
  Keep `temp/blink.parmys.blif` and `temp/blink.abc.blif`: the latter shows the `.subckt adder` / `.latch` black-box contract that Odin III's `write_blif` must honor.
- [x] `[AGENT]` Odin II on the same file (if the binary is not at this path, find it under `build/odin_ii/`): `$VTR_ROOT/odin_ii/odin_ii -a $VTR_ROOT/vtr_flow/arch/timing/EArch.xml -V $VTR_ROOT/doc/src/quickstart/blink.v -o blink.odin.blif`
- [x] `[AGENT]` Standalone Yosys: `cd ~/odin3-ws/external && git clone https://github.com/YosysHQ/yosys && cd yosys && git submodule update --init && make -j$(nproc)` — (2026-10-08: Yosys has moved to CMake; built with `cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release . && ninja -C build`, binary at `build/yosys`.)
- [x] `[AGENT]` Record the VTR and Yosys commit hashes in `odin3/docs/ORACLES.md`.

## 5. Repo skeleton for `odin3` (PR #1)

- [x] `[AGENT]` On branch `feat/skeleton`, create:
  ```
  odin3/
    CLAUDE.md  README.md  LICENSE  CMakeLists.txt  CMakePresets.json
    .clang-format  .clang-tidy  .pre-commit-config.yaml  .gitignore  pyproject.toml
    docs/   DESIGN.md (copy of the spec)  PHASE0.md (this file)  IR.md (stub)  PASSES.md (stub)
            ORACLES.md  ADR/ (D1–D9 as one file each)
    include/odin3/odin3.h          # the public C ABI: every plugin and Python tool uses only this
    src/    util/  (arena, vec, hashmap, str, log — the only generic containers in the tree)
            ir/  ast/  frontends/{verilog,blif,vqm,edif}/  passes/  backends/{blif,verilog,json,dot}/
            sim/  cli/  api/ (odin3.h entry points owned by no subsystem — decision #2)
    adapters/slang/                # C++ shim exporting one C function; the only C++ allowed
    plugins/                       # example .so plugin + example Python (cffi) plugin
    tools/  netlist-compare/  equiv-check/  run-oracle.sh  lint.sh
    tests/  unit/  golden/  micro/
    third_party/abc/               # submodule, built as libabc
    .github/workflows/ci.yml  nightly.yml
    .claude/settings.json  .claude/skills/{new-pass,run-micro,oracle,lint}/SKILL.md
  ```
- [x] `[AGENT]` CMake: C17, `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes -Werror`; `debug` preset with ASan+UBSan; `release` preset; ccache; Unity or CMocka for unit tests; `odin3` CLI target; `libodin3` shared library exporting `odin3.h`.
- [x] `[AGENT]` Lint gate (`tools/lint.sh`, also run by pre-commit and CI): `clang-format --dry-run -Werror`; `clang-tidy` with the config in spec §15; `cppcheck --error-exitcode=1`; `lizard -C 15 -L 60 -a 5 src/` (cyclomatic complexity ≤ 15, function length ≤ 60 lines, ≤ 5 parameters); `ruff check` and `mypy --strict` on `tools/` and `plugins/`.
- [x] `[AGENT]` `tools/netlist-compare` (Python): canonicalize two BLIFs (sort models, sort `.names/.latch/.subckt` lines, rename internal nets by topological order) and diff; exit 0 on identical.
- [x] `[AGENT]` `tools/equiv-check` (Python): run ABC `cec` or `dsec` on two BLIFs; exit 0 on equivalent.
- [x] `[AGENT]` `tools/run-oracle.sh <design.v> <arch.xml>`: run Parmys and Odin II, store both BLIFs under `../golden/<arch>/<design>/` with the VTR commit hash.
- [x] `[AGENT]` CI `ci.yml`: build gcc and clang, run lint gate, run unit tests, run `netlist-compare` on `tests/micro`. `nightly.yml`: scheduled VTR-19 flow vs. golden QoR (stub until Phase 3).
- [x] `[AGENT]` Open PR #1 with `gh pr create`. No IR code in this PR. (https://github.com/drpaj12/odin3/pull/1; §6 files deferred to PR #2.)
- [ ] `[REVIEW]` Peter reviews PR #1 line by line — it sets every convention the agent will copy for the next year — then merges.

## 6. Agent configuration

- [ ] `[AGENT]` Draft `CLAUDE.md` (under ~150 lines): what Odin III is; "read `docs/DESIGN.md` and `docs/IR.md` before touching `src/ir`"; build/test commands; working rules (one pass per PR; golden test per pass; `check` around passes in Debug; never edit `../external/`; never commit to `main`; run `/run-micro` before claiming a micro passes; run `/lint` before every commit); code standard summary from spec §15; "when unsure about an IR invariant, stop and ask; do not guess."
- [ ] `[AGENT]` `.claude/settings.json` (shared):
  ```json
  {
    "permissions": {
      "allow": ["Bash(cmake *)", "Bash(ctest *)", "Bash(ninja *)", "Bash(git status*)", "Bash(git diff*)",
                "Bash(git log*)", "Bash(git add *)", "Bash(git commit *)", "Bash(gh pr *)",
                "Bash(./tools/*)", "Bash(python3 *)", "Bash(lizard *)", "Bash(clang-tidy *)"],
      "ask":   ["Bash(git push *)", "Bash(rm -rf *)"],
      "deny":  ["Read(./.env)", "Edit(//home/jamiespa/odin3-ws/external/**)", "Write(//home/jamiespa/odin3-ws/external/**)"]
    },
    "sandbox": { "enabled": true },
    "hooks": {
      "PostToolUse": [{ "matcher": "Edit|Write",
        "hooks": [{ "type": "command", "command": "f=$(jq -r '.tool_input.file_path // empty'); case \"$f\" in *.c|*.h) clang-format -i \"$f\" ;; esac; true" }] }]
    }
  }
  ```
  Notes (verify each against the current settings reference at https://code.claude.com/docs/en/settings before committing):
  - Hooks get the tool call as JSON on **stdin**; there is no `$CLAUDE_FILE_PATH`. The hook formats only `.c`/`.h` so it never touches Markdown or Python.
  - Deny paths are absolute (`//` prefix = filesystem root); `../` is not a reliable rule pattern.
  - With the sandbox on, `gh` and `git push` need network access to `github.com`/`api.github.com`; configure the sandbox's allowed domains (or exclude `gh`/`git` from the sandbox) so PR creation works. Test with `gh pr list` after writing the file.
- [ ] `[AGENT]` Project skills in `.claude/skills/`:
  - `new-pass` — scaffold `src/passes/<name>.c`, its unit test, its golden test, and a `docs/PASSES.md` entry; opens a branch `feat/<name>`.
  - `run-micro` — build debug, run the microbenchmark suite through `netlist-compare` against golden, print a pass/fail table.
  - `oracle` — wrapper around `tools/run-oracle.sh` for one or many designs.
  - `lint` — run `tools/lint.sh` and summarize violations by file.
- [ ] `[AGENT]` ADRs `docs/ADR/D1..D9.md` from spec §2, plus `D8-prime.md` (C17 core, C ABI plugins).
- [ ] `[REVIEW]` Peter edits `CLAUDE.md` and the ADRs — these encode his rules, not the agent's.

## 7. Golden netlists

- [ ] `[AGENT]` Run `tools/run-oracle.sh` over every file in `$VTR_ROOT/odin_ii/regression_test/benchmark/` (micros; all 563 `.v`, failures recorded — decision #13) and `$VTR_ROOT/vtr_flow/benchmarks/verilog/` (VTR-19) against `EArch.xml` and `k6_frac_N10_frac_chain_mem32K_40nm.xml`; commit to `odin3-golden` with hashes.
- [ ] `[AGENT]` Sanity: `netlist-compare` of a golden against itself is identical; Parmys vs Odin II on `blink` is different but `equiv-check` proves them equivalent.
- [ ] `[AGENT]` Copy the micro Verilog sources into `odin3/tests/micro/` with a `SOURCES.md` noting origin and VTR commit.

## 8. Phase 0 exit checklist

- [ ] `[AGENT]` All repos under `~/odin3-ws` on the Linux filesystem; `nproc`/`free -g` match `.wslconfig`.
- [ ] `[AGENT]` VTR built with Odin II; `blink.v` passes through Parmys→ABC→VPR and through Odin II.
- [ ] `[AGENT]` Standalone Yosys built.
- [ ] `[AGENT]` `odin3`: PR #1 merged, CI green, lint gate green on the skeleton.
- [ ] `[AGENT]` `odin3-golden`: micros + VTR-19 goldens for two arches with commit hashes.
- [ ] `[AGENT]` `docs/DESIGN.md` matches the Claude project doc; ADRs written.
- [ ] `[HUMAN]` Check `/usage` after the first two sessions and adjust the model table in §9.

## 9. Model and effort guide (for Phases 1–7)

Run `/model` to see what the plan offers; the lineup names Opus, Sonnet, and Fable families.

| Work | Model | Effort |
|---|---|---|
| IR object model, invariants, `check`, provenance design (Phase 1) | Opus, or Fable if `/model` offers it | `xhigh` |
| Partial-mapping algorithms, matcher, memory inference, FSM | Opus | `xhigh` → `high` once settled |
| Grammar, preprocessor, writers/readers, adapters, test harnesses | Opus | `high` |
| Bulk mechanical work: op-registry entries, golden files, docs, boilerplate | Sonnet | `medium`/`high` |
| Exploration subagents ("where does VTR emit `.subckt`?") | Sonnet or Haiku | default |

`/effort <level>` saves per model under `modelSettings`; `claude --effort high` sets it for one session. Levels: `low`, `medium`, `high`, `xhigh`. Use the advisor (`/advisor` → Opus) in Phases 1 and 4.

Multi-agent rules: §A.1. Budget rules for the $100 Max plan (quotas vary; check `/usage` and https://support.claude.com): one task per session, `/clear` between; explore with subagents, implement in main; `xhigh` only where thinking changes the design; let CI run the heavy regression loops; plan mode (or `/superpowers:write-plan`) before every pass; cloud sessions for long builds and nightly runs; `autoContinueAtUsageLimit` in settings if you hit the window mid-task.

Sources: VTR quickstart (https://docs.verilogtorouting.org/en/latest/quickstart/); Claude Code setup, settings reference, feature availability (https://code.claude.com/docs/en/setup, /settings-reference, /feature-availability); Superpowers (https://github.com/obra/superpowers).
