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

# ---- 0. §15.1 rules no other tool checks (exit/abort/goto; the .y/.l hygiene of 2C §6; no assert(
# in the ABC adapter, 3D §4.1) --------------------------------------------------------------------
read_list rule_files 'src/*.c' 'src/*.h' 'src/*.y' 'src/*.l' 'plugins/*.c' 'plugins/*.h' \
    'adapters/abc/*.c' 'adapters/abc/*.h'
if [ "${#rule_files[@]}" -eq 0 ]; then
    record rules PASS
else
    run_tool rules python3 tools/check_rules.py "${rule_files[@]}"
fi

# ---- 1. clang-format -------------------------------------------------------------------------
read_list c_h_files '*.c' '*.h' '*.cpp' '*.hpp'
if [ "${#c_h_files[@]}" -eq 0 ]; then
    record clang-format PASS
else
    run_tool clang-format clang-format --dry-run -Werror "${c_h_files[@]}"
fi

# ---- 2. clang-tidy ---------------------------------------------------------------------------
# The C17 core (and the C ABC adapter) runs the profile in the root .clang-tidy. The SystemVerilog
# adapter runs the C++ profile in adapters/slang/.clang-tidy (DESIGN 15.2): inheriting the root
# file, it re-enables the checks that are C++-only there and documents every suppression with its
# reason. clang-tidy finds that file by itself for sources below adapters/slang/.
tidy_step() {
    local files cxx_files
    read_list files 'src/*.c' 'plugins/*.c' 'tests/unit/*.c' 'adapters/abc/*.c'
    read_list cxx_files 'adapters/slang/*.cpp' 'adapters/slang/*.hpp'
    if [ "${#files[@]}" -eq 0 ] && [ "${#cxx_files[@]}" -eq 0 ]; then
        record clang-tidy PASS
        return
    fi
    if ! command -v cmake >/dev/null 2>&1; then
        echo "==> clang-tidy" >&2
        echo "    cmake not found; needed to create build/lint/compile_commands.json" >&2
        record clang-tidy FAIL
        return
    fi
    # ABC and slang are on, so the ABC adapter (adapters/abc/, 3D §4.1) and the SystemVerilog
    # adapter (adapters/slang/, DESIGN 4.2) have compile commands; nothing is built here. A
    # build/lint configured before either adapter existed is reconfigured.
    local need_cfg=0
    if [ ! -f build/lint/compile_commands.json ]; then
        need_cfg=1
    elif [ "${#files[@]}" -gt 0 ] &&
        ! grep -q '/adapters/abc/' build/lint/compile_commands.json; then
        need_cfg=1
    elif [ "${#cxx_files[@]}" -gt 0 ] &&
        ! grep -q '/adapters/slang/' build/lint/compile_commands.json; then
        need_cfg=1
    fi
    if [ "$need_cfg" -eq 1 ]; then
        echo "==> configuring build/lint for clang-tidy"
        local cfg_log="$LOG_DIR/tidy-configure.log"
        if ! cmake -S . -B build/lint -G Ninja -DODIN3_WITH_ABC=ON -DODIN3_WITH_SLANG=ON \
            -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >"$cfg_log" 2>&1; then
            sed 's/^/    /' "$cfg_log" >&2
            record clang-tidy FAIL
            return
        fi
    fi
    # The Verilog parser's sources include the Bison/Flex output, which lives only in the build
    # tree (2C spec §6): generate it (a second or two). The generated directory is outside the
    # git file lists and the header filter below, so nothing in it is linted.
    local gen_log="$LOG_DIR/tidy-generate.log"
    if ! cmake --build build/lint --target verilog_generated >"$gen_log" 2>&1; then
        echo "==> clang-tidy" >&2
        sed 's/^/    /' "$gen_log" >&2
        record clang-tidy FAIL
        return
    fi
    # slang's diagnostic and syntax headers are generated into the build tree by its codegen
    # rules; shim.cpp does not parse without them. Asking for three of their outputs runs those
    # two rules (a second or two) and compiles none of slang.
    if [ "${#cxx_files[@]}" -gt 0 ]; then
        local slang_gen_log="$LOG_DIR/tidy-slang-generate.log"
        if ! cmake --build build/lint --target \
            third_party/slang/source/slang/diagnostics/GeneralDiags.h \
            third_party/slang/source/slang/syntax/AllSyntax.h \
            third_party/slang/source/slang/parsing/TokenKind.h >"$slang_gen_log" 2>&1; then
            echo "==> clang-tidy" >&2
            sed 's/^/    /' "$slang_gen_log" >&2
            record clang-tidy FAIL
            return
        fi
    fi
    # clang-tidy 18 has no ExcludeHeaderFilterRegex, and third_party/unity/src/ would match a
    # relative "src/" pattern, so the header filter is anchored on the absolute repo root here
    # (overriding the looser HeaderFilterRegex in .clang-tidy). adapters/slang/ is in it so the
    # C++ profile's own header is checked; third_party/slang/ stays out.
    local root_re
    root_re=$(printf '%s' "$ROOT" | sed 's/[][\.^$*+?(){}|]/\\&/g')
    run_tool clang-tidy clang-tidy -p build/lint --quiet \
        "--header-filter=^${root_re}/(include|src|plugins|tests/unit|adapters/abc|adapters/slang)/" \
        "${files[@]}" "${cxx_files[@]}"
}
if [ "$run_tidy" -eq 1 ]; then
    tidy_step
else
    record clang-tidy SKIP
fi

# ---- 3. cppcheck -----------------------------------------------------------------------------
read_list cpp_files 'src/*.c' 'src/*.h' 'plugins/*.c' 'plugins/*.h' 'adapters/abc/*.c' \
    'adapters/abc/*.h'
read_list cxx_files 'adapters/slang/*.cpp' 'adapters/slang/*.hpp'
if [ "${#cpp_files[@]}" -eq 0 ] && [ "${#cxx_files[@]}" -eq 0 ]; then
    record cppcheck PASS
else
    if [ "${#cpp_files[@]}" -gt 0 ]; then
        run_tool cppcheck cppcheck -q --enable=warning,style,performance,portability \
            --error-exitcode=1 --std=c11 --inline-suppr -I include -I src "${cpp_files[@]}"
    fi
    # The C++ adapter (adapters/slang, DESIGN 4.2) under the C++ standard; the slang headers are
    # not included (cppcheck reads the translation unit on its own).
    if [ "${#cxx_files[@]}" -gt 0 ]; then
        run_tool cppcheck-cxx cppcheck -q --enable=warning,style,performance,portability \
            --error-exitcode=1 --std=c++20 --inline-suppr -I include -I src "${cxx_files[@]}"
    fi
fi

# ---- 4. lizard -------------------------------------------------------------------------------
lizard_dirs=()
for d in src adapters/abc adapters/slang; do
    if [ -d "$d" ]; then
        lizard_dirs+=("$d/")
    fi
done
if [ "${#lizard_dirs[@]}" -eq 0 ]; then
    record lizard PASS
else
    run_tool lizard lizard -C 15 -L 60 -a 5 -w "${lizard_dirs[@]}"
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
