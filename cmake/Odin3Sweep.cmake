# Odin3Sweep.cmake — heavy CTests (2E plan point (p), ruling C1), included by CMakeLists.txt.
#
# odin3_add_sweep_test(<name> LABEL sweep|sweep-xl COMMAND <cmd...>) registers <cmd...> as a
# CTest run through ${ODIN3_HEAVY_RUN} (tools/heavy-run; with --xl for sweep-xl), so it runs alone
# machine-wide inside the 6G slice (the 9G one for sweep-xl), with:
# - RUN_SERIAL TRUE;
# - SKIP_RETURN_CODE 77: heavy-run skips without ODIN3_HEAVY_OK=1 (the gate presets), without
#   ODIN3_HEAVY_XL_OK=1 for sweep-xl, without systemd --user, or after a 4 h wait for the lock;
# - TIMEOUT 43200 (12 h): the lock wait (at most 14400 s) plus the run, where CTest's default 1500 s
#   would kill a waiting sweep.
# Every sweep and sweep-xl CTest of 2D, 2E and 2F is registered through it. It stops with
# FATAL_ERROR when ODIN3_HEAVY_RUN is unset, the label is neither, or COMMAND is missing.
function(odin3_add_sweep_test name)
    cmake_parse_arguments(PARSE_ARGV 1 _sweep "" "LABEL" "COMMAND")
    if(NOT ODIN3_HEAVY_RUN)
        message(FATAL_ERROR "odin3_add_sweep_test(${name}): ODIN3_HEAVY_RUN is not set "
                            "(set it to tools/heavy-run)")
    endif()
    if(_sweep_UNPARSED_ARGUMENTS OR NOT _sweep_COMMAND)
        message(FATAL_ERROR "odin3_add_sweep_test(${name}): usage: odin3_add_sweep_test(<name> "
                            "LABEL sweep|sweep-xl COMMAND <cmd...>)")
    endif()
    if(_sweep_LABEL STREQUAL "sweep")
        set(_heavy ${ODIN3_HEAVY_RUN})
    elseif(_sweep_LABEL STREQUAL "sweep-xl")
        set(_heavy ${ODIN3_HEAVY_RUN} --xl)
    else()
        message(FATAL_ERROR "odin3_add_sweep_test(${name}): LABEL must be sweep or sweep-xl, "
                            "not '${_sweep_LABEL}'")
    endif()
    add_test(NAME ${name} COMMAND ${_heavy} ${_sweep_COMMAND})
    set_tests_properties(${name} PROPERTIES LABELS ${_sweep_LABEL} RUN_SERIAL TRUE
                         SKIP_RETURN_CODE 77 TIMEOUT 43200)
endfunction()
