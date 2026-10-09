cmake_minimum_required(VERSION 3.25)
project(odin3 VERSION 0.0.0 LANGUAGES C)

# --- Options -----------------------------------------------------------------
option(ODIN3_WITH_ABC "Build third_party/abc (abc executable now; libabc linked from Phase 3)" ON)
option(ODIN3_SANITIZE "Build Odin III targets with ASan + UBSan" OFF)
option(ODIN3_WERROR "Treat warnings as errors in Odin III targets" ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()

# --- Warning and sanitizer flags (Odin III targets only; never third_party) ---
add_library(odin3_flags INTERFACE)
target_compile_options(odin3_flags INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes -Wmissing-prototypes -Wvla
    $<$<BOOL:${ODIN3_WERROR}>:-Werror>)
target_compile_definitions(odin3_flags INTERFACE _POSIX_C_SOURCE=200809L)
if(ODIN3_SANITIZE)
    target_compile_options(odin3_flags INTERFACE
        -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    target_link_options(odin3_flags INTERFACE -fsanitize=address,undefined)
endif()

# --- libodin3: the shared library behind include/odin3/odin3.h -----------------
# All of src/ except src/cli/ compiles into odin3_core; libodin3 re-links those objects and
# exports only what carries ODIN3_EXPORT. To add a module, append its .c file to this list.
add_library(odin3_core OBJECT
    src/api/version.c
    src/api/plugin.c
    src/util/alloc.c
    src/util/arena.c
    src/util/hash.c
    src/util/vec.c
    src/util/pagevec.c
    src/util/u64map.c
    src/util/idindex.c
    src/util/str.c
    src/util/log.c
    src/frontends/blif/lexer.c
    src/frontends/blif/reader.c
    src/backends/blif/writer.c
    src/ir/value.c
    src/ir/design.c
    src/ir/celltype.c
    src/ir/module.c
    src/ir/node.c
    src/ir/net.c
    src/ir/pinpool.c
    src/ir/wire.c
    src/ir/attr.c
    src/ir/prov.c
    src/ir/check.c
    src/ir/compact.c
    src/ir/ir_test.c
    src/ir/cells/builtin.c
    src/ir/cells/port.c
    src/ir/cells/const.c
    src/ir/cells/common.c
    src/ir/cells/arith.c
    src/ir/cells/logic.c
    src/ir/cells/shift.c
    src/ir/cells/compare.c
    src/ir/cells/reduce.c
    src/ir/cells/mux.c
    src/ir/cells/ff.c
    src/ir/cells/mem.c
    src/ir/cells/tribuf.c
    src/ir/cells/gates.c
    src/ir/cells/dff.c
    src/ir/cells/sop.c
    src/techlib/expr.c
    src/techlib/expr_eval.c
    src/techlib/width.c
    src/techlib/reader.c
    src/techlib/reader_cell.c
    src/techlib/fnsim_compile.c
    src/techlib/fnsim_eval.c
    src/sim/build.c
    src/sim/build_hier.c
    src/sim/build_names.c
    src/sim/eval.c
    src/sim/levelize.c
    src/sim/prng.c
    src/sim/word.c)
set_target_properties(odin3_core PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_VISIBILITY_PRESET hidden)
target_include_directories(odin3_core
    PUBLIC ${PROJECT_SOURCE_DIR}/include
    PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(odin3_core PUBLIC ${CMAKE_DL_LIBS} PRIVATE odin3_flags)

add_library(libodin3 SHARED $<TARGET_OBJECTS:odin3_core>)
set_target_properties(libodin3 PROPERTIES
    OUTPUT_NAME odin3
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})
target_include_directories(libodin3 PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(libodin3 PRIVATE odin3_flags ${CMAKE_DL_LIBS})

# --- odin3: the CLI -------------------------------------------------------------
add_executable(odin3 src/cli/main.c)
target_link_libraries(odin3 PRIVATE libodin3 odin3_flags)

# --- Plugins --------------------------------------------------------------------
add_library(example_plugin MODULE plugins/example_c/example_plugin.c)
set_target_properties(example_plugin PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/plugins/example_c)
# Linked against libodin3 so that, from Phase 1, its calls into the ABI resolve even when the host
# (e.g. the cffi binding) loaded libodin3 with RTLD_LOCAL.
target_link_libraries(example_plugin PRIVATE libodin3 odin3_flags)

# --- Third-party ----------------------------------------------------------------
if(ODIN3_WITH_ABC)
    # ABC's own CMake project enables C++ and drives its Makefile for flags; it
    # builds the `abc` executable (used by tools/equiv-check) and, on demand, libabc.
    # A function scope keeps our strict C17 settings out of ABC, which needs GNU extensions.
    function(odin3_add_abc)
        unset(CMAKE_C_STANDARD)
        unset(CMAKE_C_EXTENSIONS)
        set(READLINE_FOUND FALSE)
        set(ABC_SKIP_TESTS ON CACHE BOOL "Skip ABC googletest suite (needs network at configure)")
        add_subdirectory(third_party/abc EXCLUDE_FROM_ALL)
    endfunction()
    odin3_add_abc()
    add_custom_target(abc_tool ALL DEPENDS abc)
endif()

# --- odin3-blif-rt: BLIF round-trip driver (used by tools/blif-roundtrip/blif-roundtrip) --------
add_executable(odin3-blif-rt tools/blif-roundtrip/odin3-blif-rt.c)
target_include_directories(odin3-blif-rt PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(odin3-blif-rt PRIVATE odin3_core odin3_flags)

# --- odin3-sim-vectors: the simulator's random-vector driver (tools/sim-check, 1E) --------------
add_executable(odin3-sim-vectors tools/sim-check/odin3-sim-vectors.c)
target_include_directories(odin3-sim-vectors PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(odin3-sim-vectors PRIVATE odin3_core odin3_flags)

# --- Benchmark (built by default, not a CTest) --------------------------------------
add_executable(bench_util tests/bench/bench_util.c)
target_include_directories(bench_util PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(bench_util PRIVATE odin3_core odin3_flags)

add_executable(bench_ir tests/bench/bench_ir.c)
target_include_directories(bench_ir PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(bench_ir PRIVATE odin3_core odin3_flags)

# --- Tests ----------------------------------------------------------------------
include(CTest)
if(BUILD_TESTING)
    add_library(unity STATIC third_party/unity/src/unity.c)
    target_include_directories(unity PUBLIC third_party/unity/src)

    # odin3_add_unit_test(<name>): builds tests/unit/<name>.c against odin3_core (internals).
    function(odin3_add_unit_test name)
        add_executable(${name} tests/unit/${name}.c)
        target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/src)
        target_link_libraries(${name} PRIVATE odin3_core unity odin3_flags)
        add_test(NAME ${name} COMMAND ${name})
        set_tests_properties(${name} PROPERTIES LABELS unit)
    endfunction()
    odin3_add_unit_test(test_util_alloc)
    odin3_add_unit_test(test_util_arena)
    odin3_add_unit_test(test_util_hash)
    odin3_add_unit_test(test_util_vec)
    odin3_add_unit_test(test_util_pagevec)
    odin3_add_unit_test(test_util_u64map)
    odin3_add_unit_test(test_util_idindex)
    odin3_add_unit_test(test_util_str)
    odin3_add_unit_test(test_util_log)
    odin3_add_unit_test(test_ir_value)
    odin3_add_unit_test(test_ir_celltype)
    odin3_add_unit_test(test_ir_netlist)
    odin3_add_unit_test(test_ir_cells)
    odin3_add_unit_test(test_ir_wire)
    odin3_add_unit_test(test_ir_prov)
    odin3_add_unit_test(test_ir_check)
    odin3_add_unit_test(test_ir_compact)
    odin3_add_unit_test(test_blif_lexer)
    odin3_add_unit_test(test_blif_reader)
    target_compile_definitions(test_blif_reader PRIVATE
        ODIN3_BLIF_FIXTURES="${PROJECT_SOURCE_DIR}/tests/golden/blif")
    odin3_add_unit_test(test_blif_writer)
    target_compile_definitions(test_blif_writer PRIVATE
        ODIN3_BLIF_FIXTURES="${PROJECT_SOURCE_DIR}/tests/golden/blif")
    odin3_add_unit_test(test_techlib_expr)
    odin3_add_unit_test(test_techlib_reader)
    odin3_add_unit_test(test_techlib_libs)
    target_compile_definitions(test_techlib_libs PRIVATE
        ODIN3_LIB_DIR="${PROJECT_SOURCE_DIR}/lib"
        ODIN3_TECHLIB_FIXTURES="${PROJECT_SOURCE_DIR}/tests/golden/techlib"
        ODIN3_BLIF_FIXTURES="${PROJECT_SOURCE_DIR}/tests/golden/blif")
    odin3_add_unit_test(test_sim_cells)
    odin3_add_unit_test(test_sim_build)
    odin3_add_unit_test(test_sim_cycle)
    target_compile_definitions(test_sim_cycle PRIVATE
        ODIN3_BLIF_FIXTURES="${PROJECT_SOURCE_DIR}/tests/golden/blif")
    odin3_add_unit_test(test_sim_word)
    target_compile_definitions(test_sim_word PRIVATE ODIN3_LIB_DIR="${PROJECT_SOURCE_DIR}/lib")

    add_executable(test_api tests/unit/test_api.c)
    target_link_libraries(test_api PRIVATE libodin3 unity odin3_flags)
    target_compile_definitions(test_api PRIVATE
        EXAMPLE_PLUGIN_PATH="$<TARGET_FILE:example_plugin>")
    add_dependencies(test_api example_plugin)
    add_test(NAME test_api COMMAND test_api)
    set_tests_properties(test_api PROPERTIES LABELS unit)

    add_test(NAME blif_roundtrip_fixtures
             COMMAND ${PROJECT_SOURCE_DIR}/tools/blif-roundtrip/blif-roundtrip
                     --rt $<TARGET_FILE:odin3-blif-rt> --fixtures
                     ${PROJECT_SOURCE_DIR}/tests/golden/blif)
    set_tests_properties(blif_roundtrip_fixtures PROPERTIES LABELS unit)
    # Negative check: a deliberately corrupted output must make the gate fail.
    add_test(NAME blif_roundtrip_detects_corruption
             COMMAND ${PROJECT_SOURCE_DIR}/tools/blif-roundtrip/blif-roundtrip
                     --rt ${PROJECT_SOURCE_DIR}/tools/blif-roundtrip/rt-corrupt --fixtures
                     ${PROJECT_SOURCE_DIR}/tests/golden/blif)
    set_tests_properties(blif_roundtrip_detects_corruption PROPERTIES LABELS unit WILL_FAIL TRUE
        ENVIRONMENT BRT_REAL_RT=$<TARGET_FILE:odin3-blif-rt>)
    # The goldens' distinct black-box stanzas (tools/golden-blackboxes) against lib/vtr.o3lib.
    add_test(NAME blif_roundtrip_techlib
             COMMAND ${PROJECT_SOURCE_DIR}/tools/blif-roundtrip/blif-roundtrip
                     --rt $<TARGET_FILE:odin3-blif-rt> --techlib ${PROJECT_SOURCE_DIR}/lib/vtr.o3lib
                     --fixtures ${PROJECT_SOURCE_DIR}/tests/golden/techlib)
    set_tests_properties(blif_roundtrip_techlib PROPERTIES LABELS unit)

    add_test(NAME symbol_rules
        COMMAND ${PROJECT_SOURCE_DIR}/tools/check-symbols.sh
                $<TARGET_FILE:libodin3> $<TARGET_FILE:example_plugin>)
    set_tests_properties(symbol_rules PROPERTIES LABELS unit)

    add_test(NAME abi_exports
        COMMAND ${PROJECT_SOURCE_DIR}/tools/check-symbols.sh
                --exports ${PROJECT_SOURCE_DIR}/include/odin3/odin3.h $<TARGET_FILE:libodin3>)
    set_tests_properties(abi_exports PROPERTIES LABELS unit)

    add_test(NAME cli_version COMMAND odin3 --version)
    set_tests_properties(cli_version PROPERTIES
        LABELS unit PASS_REGULAR_EXPRESSION "^odin3 ${PROJECT_VERSION} \\(ABI 2\\)")
    add_test(NAME cli_example_plugin COMMAND odin3 --plugin $<TARGET_FILE:example_plugin>)
    set_tests_properties(cli_example_plugin PROPERTIES LABELS unit)

    find_package(Python3 3.10 COMPONENTS Interpreter REQUIRED)
    set(_tools_env "ODIN3_SIM_VECTORS=$<TARGET_FILE:odin3-sim-vectors>")
    if(ODIN3_WITH_ABC)
        # REQUIRE makes the ABC-backed tests fail, not skip, if this ABC is unusable.
        list(APPEND _tools_env "ODIN3_ABC=$<TARGET_FILE:abc>" "ODIN3_REQUIRE_ABC=1")
    endif()
    # sim-check (1E exit test, tools/sim-check) also needs Yosys and Icarus Verilog; without them
    # sim_check_fixtures is not registered and the sim-check Python tests skip.
    find_program(ODIN3_YOSYS yosys HINTS $ENV{HOME}/odin3-ws/external/yosys/build)
    find_program(ODIN3_IVERILOG iverilog)
    find_program(ODIN3_VVP vvp)
    if(ODIN3_WITH_ABC AND ODIN3_YOSYS AND ODIN3_IVERILOG AND ODIN3_VVP)
        list(APPEND _tools_env "ODIN3_YOSYS=${ODIN3_YOSYS}" "ODIN3_REQUIRE_SIM_CHECK=1")
        file(GLOB _sim_fixtures ${PROJECT_SOURCE_DIR}/tests/golden/blif/*.*.blif
                                ${PROJECT_SOURCE_DIR}/tests/golden/techlib/*.blif)
        # -m 0: no address-space cap (the debug driver is an ASan build).
        add_test(NAME sim_check_fixtures
                 COMMAND ${PROJECT_SOURCE_DIR}/tools/sim-check/sim-check -m 0
                         --driver $<TARGET_FILE:odin3-sim-vectors> --abc $<TARGET_FILE:abc>
                         --yosys ${ODIN3_YOSYS} --iverilog ${ODIN3_IVERILOG} --vvp ${ODIN3_VVP}
                         --techlib ${PROJECT_SOURCE_DIR}/lib/vtr.o3lib --fixtures ${_sim_fixtures})
        set_tests_properties(sim_check_fixtures PROPERTIES LABELS tools)
    else()
        message(STATUS "sim_check_fixtures not registered: needs ABC, yosys, iverilog and vvp")
    endif()
    add_test(NAME python_tools
        COMMAND ${Python3_EXECUTABLE} -m unittest discover -s tests/tools -t .
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(python_tools PROPERTIES LABELS tools ENVIRONMENT "${_tools_env}")
    # The project-input corpus (tests/golden/projects) parses to its expected records in every
    # format. The oracle half (`project-fixtures oracle`) needs Yosys and GHDL and runs locally.
    add_test(NAME project_fixtures
        COMMAND ${Python3_EXECUTABLE}
                ${PROJECT_SOURCE_DIR}/tools/project-fixtures/project_fixtures.py check)
    set_tests_properties(project_fixtures PROPERTIES LABELS tools)

    # Python loading a sanitized libodin3 needs the sanitizer runtime preloaded.
    set(_python_env "ODIN3_LIB=$<TARGET_FILE:libodin3>")
    if(ODIN3_SANITIZE)
        # gcc needs both runtimes; clang's ASan runtime includes UBSan. Clang names it with or
        # without an arch suffix depending on the runtime-directory layout, so try both.
        if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
            set(_san_groups "libasan.so" "libubsan.so")
        else()
            set(_san_groups "libclang_rt.asan-${CMAKE_SYSTEM_PROCESSOR}.so|libclang_rt.asan.so")
        endif()
        set(_preload "")
        foreach(_group IN LISTS _san_groups)
            string(REPLACE "|" ";" _candidates "${_group}")
            set(_found "")
            foreach(_rt IN LISTS _candidates)
                execute_process(COMMAND ${CMAKE_C_COMPILER} -print-file-name=${_rt}
                    OUTPUT_VARIABLE _rt_path OUTPUT_STRIP_TRAILING_WHITESPACE)
                if(NOT _found AND IS_ABSOLUTE "${_rt_path}" AND EXISTS "${_rt_path}")
                    set(_found "${_rt_path}")
                endif()
            endforeach()
            if(NOT _found)
                message(FATAL_ERROR "sanitizer runtime not found (tried ${_group})")
            endif()
            list(APPEND _preload "${_found}")
        endforeach()
        list(JOIN _preload ":" _preload)
        list(APPEND _python_env "LD_PRELOAD=${_preload}" "ASAN_OPTIONS=detect_leaks=0")
    endif()
    # The Python plugin needs cffi; skip (not fail) where it is not installed.
    add_test(NAME python_example_plugin
        COMMAND ${CMAKE_COMMAND} -E env ${_python_env}
                ${Python3_EXECUTABLE} plugins/python/example_plugin.py
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(python_example_plugin PROPERTIES
        LABELS unit SKIP_REGULAR_EXPRESSION "No module named 'cffi'")
endif()
