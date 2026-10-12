# sweep_test_check.cmake — configure-time checks of odin3_add_sweep_test (cmake/Odin3Sweep.cmake;
# 2E plan point (p)), run by tests/tools/test_heavy_run.py:
#
#   cmake -DODIN3_SOURCE=<repo> -DWORK=<empty dir> -P tests/tools/sweep_test_check.cmake
#
# Configures three tiny projects under WORK: with ODIN3_HEAVY_RUN unset the call must stop with
# FATAL_ERROR; with a bad label too; with it set, a sweep and a sweep-xl test must be registered
# through heavy-run (--xl for sweep-xl) with TIMEOUT 43200, RUN_SERIAL, SKIP_RETURN_CODE 77 and
# their label (checked with get_test_property at configure time). Exits nonzero on any mismatch.
cmake_minimum_required(VERSION 3.25)
if(NOT ODIN3_SOURCE OR NOT WORK)
    message(FATAL_ERROR "usage: cmake -DODIN3_SOURCE=<repo> -DWORK=<dir> -P sweep_test_check.cmake")
endif()

# Writes WORK/<name>/CMakeLists.txt with body after the include, configures it; rc and log out.
function(configure_case name body rc_var log_var)
    set(dir ${WORK}/${name})
    file(MAKE_DIRECTORY ${dir})
    file(WRITE ${dir}/CMakeLists.txt
         "cmake_minimum_required(VERSION 3.25)\n"
         "project(${name} NONE)\n"
         "enable_testing()\n"
         "include(${ODIN3_SOURCE}/cmake/Odin3Sweep.cmake)\n"
         "${body}\n")
    execute_process(COMMAND ${CMAKE_COMMAND} -S ${dir} -B ${dir}/build
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    set(${rc_var} ${rc} PARENT_SCOPE)
    set(${log_var} "${out}${err}" PARENT_SCOPE)
endfunction()

configure_case(unset "odin3_add_sweep_test(s LABEL sweep COMMAND true)" rc log)
if(rc EQUAL 0 OR NOT log MATCHES "ODIN3_HEAVY_RUN is not set")
    message(FATAL_ERROR "unset ODIN3_HEAVY_RUN did not stop the configure:\n${log}")
endif()

configure_case(badlabel
    "set(ODIN3_HEAVY_RUN /h/heavy-run)\nodin3_add_sweep_test(s LABEL nightly COMMAND true)" rc log)
if(rc EQUAL 0 OR NOT log MATCHES "LABEL must be sweep or sweep-xl")
    message(FATAL_ERROR "a bad label did not stop the configure:\n${log}")
endif()

set(_checks [=[
set(ODIN3_HEAVY_RUN /h/heavy-run)
odin3_add_sweep_test(s6 LABEL sweep COMMAND echo six)
odin3_add_sweep_test(s9 LABEL sweep-xl COMMAND echo nine)
foreach(_t s6 s9)
    get_test_property(${_t} TIMEOUT _timeout)
    get_test_property(${_t} RUN_SERIAL _serial)
    get_test_property(${_t} SKIP_RETURN_CODE _skip)
    get_test_property(${_t} LABELS _labels)
    message(STATUS "sweep-check ${_t}: ${_timeout} ${_serial} ${_skip} ${_labels}")
endforeach()
]=])
configure_case(registered "${_checks}" rc log)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "registering sweeps failed:\n${log}")
endif()
foreach(_want "sweep-check s6: 43200 TRUE 77 sweep" "sweep-check s9: 43200 TRUE 77 sweep-xl")
    string(FIND "${log}" "${_want}" _at)
    if(_at EQUAL -1)
        message(FATAL_ERROR "missing '${_want}' in:\n${log}")
    endif()
endforeach()
file(READ ${WORK}/registered/build/CTestTestfile.cmake _tests)
foreach(_want [==[add_test([=[s6]=] "/h/heavy-run" "echo" "six")]==]
              [==[add_test([=[s9]=] "/h/heavy-run" "--xl" "echo" "nine")]==])
    string(FIND "${_tests}" "${_want}" _at)
    if(_at EQUAL -1)
        message(FATAL_ERROR "missing '${_want}' in CTestTestfile.cmake:\n${_tests}")
    endif()
endforeach()
message(STATUS "odin3_add_sweep_test: ok")
