#!/usr/bin/env bash
# Odin III lint gate (docs/DESIGN.md sections 15.1-15.2). Usage: tools/lint.sh [--no-tidy]
#
# Runs every blocking tool, in order, never stopping at the first failure, then prints a
# PASS/FAIL/SKIP table and exits nonzero if anything failed. A missing tool is a FAIL: the
# gate must never pass silently. An empty file list for a tool is a PASS.
#
# File lists come from git (tracked plus untracked-but-not-ignored, so a new file cannot dodge
# the gate before its first `git add`). third_party/ is always excluded.
set -euo pipefail

usage() {
    echo "usage: tools/lint.sh [--no-tidy]" >&2
    exit 2
}

run_tidy=1
for arg in "$@"; do
    case "$arg" in
    --no-tidy) run_tidy=0 ;;
    -h | --help) usage ;;
    *) usage ;;
    esac
done

# Resolve the repo from this script's location so any cwd (even outside the repo) works.
cd "$(dirname "${BASH_SOURCE[0]}")"
if ! TOPLEVEL=$(git rev-parse --show-toplevel); then
    echo "lint: cannot determine repository root" >&2
    exit 2
fi
cd "$TOPLEVEL"
ROOT=$PWD
# Repo-local venv (pip install -r requirements-dev.txt) wins over system tools.
if [ -d "$ROOT/.venv/bin" ]; then
    PATH="$ROOT/.venv/bin:$PATH"
fi

LOG_DIR=$(mktemp -d)
trap 'rm -rf "$LOG_DIR"' EXIT

STEP_NAMES=()
STEP_RESULTS=()
FAILED=0

record() { # name result
    STEP_NAMES+=("$1")
    STEP_RESULTS+=("$2")
    if [ "$2" = FAIL ]; then
        FAILED=1
    fi
}

# run_tool <name> <command...>: runs the command, shows its output only on failure.
run_tool() {
    local name=$1
    shift
    local log="$LOG_DIR/${#STEP_NAMES[@]}.log"
    echo "==> $name"
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "    $name: required tool '$1' not found on PATH (pip install -r requirements-dev.txt)" >&2
        record "$name" FAIL
        return
    fi
    if "$@" >"$log" 2>&1; then
        record "$name" PASS
    else
        # Drop clang-tidy's "N warnings generated" / "Suppressed N warnings" bookkeeping noise.
        grep -Ev '^[0-9]+ warnings? generated\.$|^Suppressed [0-9]+ warnings|^Use -header-filter=' "$log" |
            sed 's/^/    /' >&2 || true
        record "$name" FAIL
    fi
}

# Tracked + untracked-not-ignored files matching the given pathspecs, NUL separated, existing only.
list_files() {
    local f
    git ls-files -z --cached --others --exclude-standard -- "$@" |
        while IFS= read -r -d '' f; do
            case "$f" in third_party/*) continue ;; esac
            if [ -f "$f" ]; then
                printf '%s\0' "$f"
            fi
        done
}

# read_list <array-name> <pathspec...>
read_list() {
    local -n out=$1
    shift
    out=()
    local f
    while IFS= read -r -d '' f; do
        out+=("$f")
    done < <(list_files "$@")
}

# ---- 0. §15.1 rules no other tool checks (exit/abort/goto) ---------------------------------
read_list rule_files 'src/*.c' 'src/*.h' 'plugins/*.c' 'plugins/*.h'
if [ "${#rule_files[@]}" -eq 0 ]; then
    record rules PASS
else
    run_tool rules python3 tools/check_rules.py "${rule_files[@]}"
fi

# ---- 1. clang-format -------------------------------------------------------------------------
read_list c_h_files '*.c' '*.h'
if [ "${#c_h_files[@]}" -eq 0 ]; then
    record clang-format PASS
else
    run_tool clang-format clang-format --dry-run -Werror "${c_h_files[@]}"
fi

# ---- 2. clang-tidy ---------------------------------------------------------------------------
tidy_step() {
    local files
    read_list files 'src/*.c' 'plugins/*.c' 'tests/unit/*.c'
    if [ "${#files[@]}" -eq 0 ]; then
        record clang-tidy PASS
        return
    fi
    if ! command -v cmake >/dev/null 2>&1; then
        echo "==> clang-tidy" >&2
        echo "    cmake not found; needed to create build/lint/compile_commands.json" >&2
        record clang-tidy FAIL
        return
    fi
    if [ ! -f build/lint/compile_commands.json ]; then
        echo "==> configuring build/lint for clang-tidy"
        local cfg_log="$LOG_DIR/tidy-configure.log"
        if ! cmake -S . -B build/lint -G Ninja -DODIN3_WITH_ABC=OFF \
            -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >"$cfg_log" 2>&1; then
            sed 's/^/    /' "$cfg_log" >&2
            record clang-tidy FAIL
            return
        fi
    fi
    # clang-tidy 18 has no ExcludeHeaderFilterRegex, and third_party/unity/src/ would match a
    # relative "src/" pattern, so the header filter is anchored on the absolute repo root here
    # (overriding the looser HeaderFilterRegex in .clang-tidy).
    local root_re
    root_re=$(printf '%s' "$ROOT" | sed 's/[][\.^$*+?(){}|]/\\&/g')
    run_tool clang-tidy clang-tidy -p build/lint --quiet --config-file=.clang-tidy \
        "--header-filter=^${root_re}/(include|src|plugins|tests/unit)/" "${files[@]}"
}
if [ "$run_tidy" -eq 1 ]; then
    tidy_step
else
    record clang-tidy SKIP
fi

# ---- 3. cppcheck -----------------------------------------------------------------------------
read_list cpp_files 'src/*.c' 'src/*.h' 'plugins/*.c' 'plugins/*.h'
if [ "${#cpp_files[@]}" -eq 0 ]; then
    record cppcheck PASS
else
    run_tool cppcheck cppcheck -q --enable=warning,style,performance,portability \
        --error-exitcode=1 --std=c11 --inline-suppr -I include -I src "${cpp_files[@]}"
fi

# ---- 4. lizard -------------------------------------------------------------------------------
if [ -d src ]; then
    run_tool lizard lizard -C 15 -L 60 -a 5 -w src/
else
    record lizard PASS
fi

# ---- 5. ruff ---------------------------------------------------------------------------------
py_dirs=()
for d in tools plugins tests/tools; do
    if [ -d "$d" ]; then
        py_dirs+=("$d")
    fi
done
if [ "${#py_dirs[@]}" -eq 0 ]; then
    record ruff PASS
else
    run_tool ruff ruff check "${py_dirs[@]}"
fi

# ---- 6. mypy ---------------------------------------------------------------------------------
read_list py_files 'tools/*.py' 'plugins/*.py' 'tests/tools/*.py'
if [ "${#py_files[@]}" -eq 0 ]; then
    record mypy PASS
else
    mypy_cfg=()
    if [ -f pyproject.toml ]; then
        mypy_cfg=(--config-file pyproject.toml)
    fi
    run_tool mypy mypy --strict "${mypy_cfg[@]}" "${py_files[@]}"
fi

# ---- summary ---------------------------------------------------------------------------------
echo
echo "lint summary"
echo "------------"
for i in "${!STEP_NAMES[@]}"; do
    printf '%-14s %s\n' "${STEP_NAMES[$i]}" "${STEP_RESULTS[$i]}"
done
if [ "$FAILED" -ne 0 ]; then
    echo "lint: FAIL"
    exit 1
fi
echo "lint: PASS"
