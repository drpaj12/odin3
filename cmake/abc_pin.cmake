# abc_pin.cmake — the pinned ABC commit (3D spec AB-5; docs/ORACLES.md), included by
# CMakeLists.txt when ODIN3_WITH_ABC is on.
#
# Sets ODIN3_ABC_COMMIT, the commit docs/ORACLES.md pins, and compares it with
# `git -C <ODIN3_ABC_SOURCE_DIR> rev-parse HEAD`. On a difference (or when git cannot tell) it
# stops with "third_party/abc is at <sha>; ORACLES.md pins <pin>".
#
# - Configure: ODIN3_ABC_SOURCE_DIR defaults to ${PROJECT_SOURCE_DIR}/third_party/abc, and the
#   submodule's HEAD file becomes a configure dependency, so moving the submodule re-runs the check.
# - A source tarball (tools/release/make-tarball) has no .git: the stamp file
#   third_party/abc/.odin3-commit, written at tarball time, holds the ABC commit and is compared
#   with the pin the same way. With neither .git nor stamp the message below is printed.
# - An uninitialised submodule stops with the `git submodule update --init` command to run.
# - Script mode (`cmake -DODIN3_ABC_SOURCE_DIR=third_party/abc -P cmake/abc_pin.cmake`): the same
#   check. ODIN3_ABC_HEAD_OVERRIDE replaces git's answer (tests/tools/test_abc_build.py only).
set(ODIN3_ABC_COMMIT "a3001b72edc5de22442e942165487fddf150e3d0")

if(NOT DEFINED ODIN3_ABC_SOURCE_DIR)
    if(CMAKE_SCRIPT_MODE_FILE)
        message(FATAL_ERROR "abc_pin.cmake: set -DODIN3_ABC_SOURCE_DIR=<path to third_party/abc>")
    endif()
    set(ODIN3_ABC_SOURCE_DIR "${PROJECT_SOURCE_DIR}/third_party/abc")
endif()

# An uninitialised submodule is an empty directory inside the superproject's work tree, where git
# would answer with the superproject's commit: say what to run instead.
get_filename_component(_abc_dir "${ODIN3_ABC_SOURCE_DIR}" ABSOLUTE)
set(_abc_stamp "${_abc_dir}/.odin3-commit")
set(_abc_use_stamp OFF)
if(NOT DEFINED ODIN3_ABC_HEAD_OVERRIDE AND NOT EXISTS "${_abc_dir}/.git" AND EXISTS "${_abc_stamp}")
    set(_abc_use_stamp ON)
endif()
if(NOT DEFINED ODIN3_ABC_HEAD_OVERRIDE AND NOT _abc_use_stamp AND NOT EXISTS "${_abc_dir}/.git")
    message(FATAL_ERROR " third_party/abc is not checked out (${ODIN3_ABC_SOURCE_DIR} has no .git);"
                        " run: git submodule update --init third_party/abc\n"
                        " (in a git worktree: git -c protocol.file.allow=always submodule update"
                        " --init third_party/abc)")
endif()

if(DEFINED ODIN3_ABC_HEAD_OVERRIDE)
    set(_abc_head "${ODIN3_ABC_HEAD_OVERRIDE}")
elseif(_abc_use_stamp)
    file(STRINGS "${_abc_stamp}" _abc_head LIMIT_COUNT 1)
    string(STRIP "${_abc_head}" _abc_head)
    if(NOT CMAKE_SCRIPT_MODE_FILE)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_abc_stamp}")
    endif()
else()
    find_program(ODIN3_GIT_EXE git)
    set(_abc_head "unknown")
    if(ODIN3_GIT_EXE)
        execute_process(COMMAND "${ODIN3_GIT_EXE}" -C "${ODIN3_ABC_SOURCE_DIR}" rev-parse HEAD
                        OUTPUT_VARIABLE _abc_out RESULT_VARIABLE _abc_rc
                        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(_abc_rc EQUAL 0)
            set(_abc_head "${_abc_out}")
        endif()
        if(NOT CMAKE_SCRIPT_MODE_FILE)
            execute_process(COMMAND "${ODIN3_GIT_EXE}" -C "${ODIN3_ABC_SOURCE_DIR}" rev-parse
                                    --absolute-git-dir
                            OUTPUT_VARIABLE _abc_gitdir RESULT_VARIABLE _abc_rc
                            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
            if(_abc_rc EQUAL 0 AND EXISTS "${_abc_gitdir}/HEAD")
                set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                                                       "${_abc_gitdir}/HEAD")
            endif()
        endif()
    endif()
endif()

# The leading blank marks the line preformatted, so CMake prints it on one line, unwrapped.
if(NOT _abc_head STREQUAL ODIN3_ABC_COMMIT)
    message(FATAL_ERROR " third_party/abc is at ${_abc_head}; ORACLES.md pins ${ODIN3_ABC_COMMIT}")
endif()
unset(_abc_head)
unset(_abc_out)
unset(_abc_rc)
unset(_abc_gitdir)
unset(_abc_dir)
unset(_abc_stamp)
unset(_abc_use_stamp)
