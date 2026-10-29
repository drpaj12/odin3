# odin3_commit.cmake — the commit this build was made from, included by CMakeLists.txt.
#
# Sets ODIN3_COMMIT (40 lowercase hex digits, or "unknown") and ODIN3_COMMIT_DIRTY (0 or 1);
# src/api/version.c reports them through odin3_version_commit() and `odin3 --version`.
# - A git checkout (${PROJECT_SOURCE_DIR}/.git exists, a directory or a worktree file):
#   `git -C <src> rev-parse HEAD` and `git -C <src> diff --quiet HEAD`. HEAD becomes a configure
#   dependency, so a new commit re-runs configure; the dirty flag is as of the last configure.
# - A source tarball or sdist has no .git: the stamp file ${PROJECT_SOURCE_DIR}/.odin3-commit
#   (written by tools/release/make-tarball, never committed) holds the SHA; dirty is 0.
# - Neither, or git absent or failing: "unknown". Configure never fails here.
set(ODIN3_COMMIT "unknown")
set(ODIN3_COMMIT_DIRTY 0)
set(_o3c_sha "")
if(EXISTS "${PROJECT_SOURCE_DIR}/.git")
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${PROJECT_SOURCE_DIR}" rev-parse HEAD
                        OUTPUT_VARIABLE _o3c_sha OUTPUT_STRIP_TRAILING_WHITESPACE
                        ERROR_QUIET RESULT_VARIABLE _o3c_rc)
        if(NOT _o3c_rc EQUAL 0)
            set(_o3c_sha "")
        else()
            execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${PROJECT_SOURCE_DIR}" diff --quiet HEAD
                            ERROR_QUIET RESULT_VARIABLE _o3c_dirty_rc)
            if(_o3c_dirty_rc EQUAL 1)
                set(ODIN3_COMMIT_DIRTY 1)
            endif()
            execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${PROJECT_SOURCE_DIR}" rev-parse
                                    --path-format=absolute --git-path HEAD
                            OUTPUT_VARIABLE _o3c_head OUTPUT_STRIP_TRAILING_WHITESPACE
                            ERROR_QUIET RESULT_VARIABLE _o3c_head_rc)
            if(_o3c_head_rc EQUAL 0 AND EXISTS "${_o3c_head}")
                set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_o3c_head}")
            endif()
        endif()
    endif()
elseif(EXISTS "${PROJECT_SOURCE_DIR}/.odin3-commit")
    file(STRINGS "${PROJECT_SOURCE_DIR}/.odin3-commit" _o3c_sha LIMIT_COUNT 1)
endif()
string(STRIP "${_o3c_sha}" _o3c_sha)
string(TOLOWER "${_o3c_sha}" _o3c_sha)
string(LENGTH "${_o3c_sha}" _o3c_len)
if(_o3c_len EQUAL 40 AND _o3c_sha MATCHES "^[0-9a-f]+$")
    set(ODIN3_COMMIT "${_o3c_sha}")
endif()
message(STATUS "Odin III commit: ${ODIN3_COMMIT} (dirty ${ODIN3_COMMIT_DIRTY})")
