cmake_minimum_required(VERSION 3.25)
project(odin3 VERSION 0.0.0 LANGUAGES C)

# --- Options -----------------------------------------------------------------
option(ODIN3_WITH_ABC "Build third_party/abc (abc executable now; libabc linked from Phase 3)" ON)
option(ODIN3_SANITIZE "Build Odin III targets with ASan + UBSan" OFF)
option(ODIN3_WERROR "Treat warnings as errors in Odin III targets" ON)
# libFuzzer harnesses (tests/fuzz): clang only, in a build directory of their own
# (cmake -S . -B build/fuzz -DCMAKE_C_COMPILER=clang -DODIN3_FUZZ=ON). Every Odin III target is
# instrumented for coverage and built with ASan + UBSan.
option(ODIN3_FUZZ "Build the libFuzzer harnesses in tests/fuzz (clang only; implies ODIN3_SANITIZE)"
       OFF)
if(ODIN3_FUZZ)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "ODIN3_FUZZ needs clang's libFuzzer: configure with "
                            "-DCMAKE_C_COMPILER=clang")
    endif()
    set(ODIN3_SANITIZE ON)
endif()

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
if(ODIN3_FUZZ)
    target_compile_options(odin3_flags INTERFACE -fsanitize=fuzzer-no-link)
endif()

# --- libodin3: the shared library behind include/odin3/odin3.h -----------------
# All of src/ except src/cli/ compiles into odin3_core; libodin3 re-links those objects and
# exports only what carries ODIN3_EXPORT. To add a module, append its .c file to this list.
add_library(odin3_core OBJECT
    src/api/version.c
    src/api/plugin.c
    src/api/common.c
    src/api/design.c
    src/api/pass.c
    src/api/celltype.c
    src/api/prov.c
    src/api/ast_abi.c
    src/api/srcman_abi.c
    src/api/module.c
    src/api/node.c
    src/api/net.c
    src/api/attr.c
    src/api/log.c
    src/util/alloc.c
    src/util/arena.c
    src/util/hash.c
    src/util/vec.c
    src/util/pagevec.c
    src/util/u64map.c
    src/util/idindex.c
    src/util/str.c
    src/util/log.c
    src/util/file.c
    src/frontends/blif/lexer.c
    src/frontends/blif/reader.c
    src/backends/blif/writer.c
    src/backends/blif/vtr.c
    src/passes/manager.c
    src/passes/builtin.c
    src/passes/write_formats.c
    src/passes/sim_pass.c
    src/passes/project_pass.c
    src/passes/proc/pw.c
    src/passes/proc/pw_sig.c
    src/passes/proc/pw_encode.c
    src/passes/proc/proc_emit.c
    src/passes/proc/proc_diag.c
    src/passes/proc/proc_pending.c
    src/passes/proc/proc_clean.c
    src/passes/proc/proc_rmdead.c
    src/passes/proc/ppool.c
    src/passes/proc/proc_pass.c
    src/passes/budget.c
    src/passes/ffinit.c
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
    src/ir/proc.c
    src/ir/ir_test.c
    src/ast/srcman.c
    src/ast/srcman_decode.c
    src/ast/srcman_print.c
    src/ast/kinds.c
    src/ast/ast.c
    src/ast/ast_test.c
    src/ast/walk.c
    src/ast/number.c
    src/ast/attr.c
    src/ast/comment.c
    src/ast/symtab.c
    src/ast/symtab_lookup.c
    src/ast/check.c
    src/ast/check_elab.c
    src/ast/print.c
    src/ast/print_expr.c
    src/ast/print_stmt.c
    src/ast/dump.c
    src/ast/equal.c
    src/frontends/project/project.c
    src/frontends/project/words.c
    src/frontends/project/paths.c
    src/frontends/project/dump.c
    src/frontends/project/flist.c
    src/frontends/project/tcl.c
    src/frontends/project/quartus.c
    src/frontends/project/o3proj.c
    src/frontends/project/entry.c
    src/frontends/project/xml.c
    src/frontends/project/odin2.c
    src/frontends/verilog/preproc.c
    src/frontends/verilog/macro.c
    src/frontends/verilog/cond.c
    src/frontends/verilog/include.c
    src/frontends/verilog/expand.c
    src/frontends/verilog/pp_check.c
    src/frontends/verilog/parser.c
    src/frontends/verilog/actions.c
    src/frontends/verilog/actions_decl.c
    src/frontends/verilog/actions_expr.c
    src/frontends/verilog/actions_stmt.c
    src/frontends/verilog/actions_inst.c
    src/frontends/verilog/lexer_support.c
    src/frontends/verilog/lexer_tokens.c
    src/frontends/verilog/lexer_trivia.c
    src/frontends/verilog/keywords.c
    src/frontends/elab/elab_limits.c
    src/frontends/elab/cval.c
    src/frontends/elab/cval_arith.c
    src/frontends/elab/ctype.c
    src/frontends/elab/ceval.c
    src/frontends/elab/ceval_expr.c
    src/frontends/elab/ceval_interp.c
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
    src/ir/cells/proc.c
    src/ir/cells/lut.c
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
    src/sim/vectors.c
    src/sim/word.c
    src/backends/common/attrs.c
    src/backends/dot/writer.c
    src/backends/json/emit.c
    src/backends/json/cells.c
    src/backends/json/lut.c
    src/backends/json/writer.c
    src/backends/verilog/escape.c
    src/backends/verilog/writer.c
    src/backends/rtlil/writer.c)
set_target_properties(odin3_core PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_VISIBILITY_PRESET hidden)
target_include_directories(odin3_core
    PUBLIC ${PROJECT_SOURCE_DIR}/include
    PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(odin3_core PUBLIC ${CMAKE_DL_LIBS} PRIVATE odin3_flags)

# --- The Verilog parser: Bison and Flex generated into the build tree (2C spec §6) ----------
find_package(BISON 3.8 REQUIRED)        # parse.error detailed, parse.lac, %precedence, -Wcex
find_package(FLEX 2.6.4 REQUIRED)       # reentrant + bison-bridge without the 2.6.0–2.6.3 bugs
set(_gen ${CMAKE_CURRENT_BINARY_DIR}/generated/verilog)
file(MAKE_DIRECTORY ${_gen})  # Flex and Bison do not create it (Ninja does, Make does not)
bison_target(verilog_parser src/frontends/verilog/verilog.y ${_gen}/verilog_parser.c
             DEFINES_FILE ${_gen}/verilog_parser.h
             COMPILE_FLAGS "-Wall -Wcounterexamples -Werror")
flex_target(verilog_lexer src/frontends/verilog/verilog.l ${_gen}/verilog_lexer.c
            DEFINES_FILE ${_gen}/verilog_lexer.h COMPILE_FLAGS "--warn")
add_flex_bison_dependency(verilog_lexer verilog_parser)
add_custom_target(verilog_generated
                  DEPENDS ${BISON_verilog_parser_OUTPUTS} ${FLEX_verilog_lexer_OUTPUTS})
# The generated sources are ordinary members of odin3_core (PIC, hidden visibility,
# odin3_flags, _POSIX_C_SOURCE: everything the other sources get), so libodin3 and the
# unit tests link them as they link any core object. Only the warnings the skeletons cannot
# satisfy are switched off, per generated file: a source-file COMPILE_OPTIONS list is placed
# after the target's options on the command line, so "-Wconversion … -Wno-conversion" leaves
# the warning off (verified with CMake 3.28 and both compilers).
target_sources(odin3_core PRIVATE ${BISON_verilog_parser_OUTPUTS} ${FLEX_verilog_lexer_OUTPUTS})
target_include_directories(odin3_core PRIVATE ${_gen})
# The list as measured with Bison 3.8.2, Flex 2.6.4, gcc 13 and clang 18 at -O0 and -O2: the
# expected conversion, sign, unused-parameter and missing-prototypes warnings do not occur, and
# the scanner needs nothing (re-measured with the full lexer of 2C Task 3: start conditions,
# trailing context, yyless(0), YY_DECL: no warning; with 2C Task 4's grammar core, 353
# states, YYMALLOC/YYFREE through the allocation hook, with Task 5's expressions, 488
# states and the precedence table, with Task 6's statements, 763 states and the
# dangling-else precedence, and with Task 7's instantiation, gates and generate, 941 states:
# still none but the one below). One flag, for the parser only:
#   -Wno-unused-but-set-variable  yyparse's yynerrs, counted but never read under api.pure
#                                 (clang; gcc does not warn)
set_source_files_properties(${BISON_verilog_parser_OUTPUT_SOURCE}
    PROPERTIES COMPILE_OPTIONS "-Wno-unused-but-set-variable")

add_library(libodin3 SHARED $<TARGET_OBJECTS:odin3_core>)
set_target_properties(libodin3 PROPERTIES
    OUTPUT_NAME odin3
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})
target_include_directories(libodin3 PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(libodin3 PRIVATE odin3_flags ${CMAKE_DL_LIBS})

# --- odin3: the CLI -------------------------------------------------------------
# Linked against libodin3 and using only odin3.h, so plugins it loads and the CLI share one pass
# registry (and one cell-type registry).
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

# --- odin3-write: writer driver (used by tools/writer-check/writer-check) -------------------
add_executable(odin3-write tools/writer-check/odin3-write.c)
target_include_directories(odin3-write PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(odin3-write PRIVATE odin3_core odin3_flags)

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

add_executable(bench_ast tests/bench/bench_ast.c)
target_include_directories(bench_ast PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(bench_ast PRIVATE odin3_core odin3_flags)

add_executable(bench_pp tests/bench/bench_pp.c)
target_include_directories(bench_pp PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(bench_pp PRIVATE odin3_core odin3_flags)

# --- Fuzzers (ODIN3_FUZZ) ---------------------------------------------------------------
if(ODIN3_FUZZ)
    # tests/fuzz/fuzz_pp.c: the preprocessor's crash fuzzer (2B §10.5).
    add_executable(fuzz_pp tests/fuzz/fuzz_pp.c)
    target_include_directories(fuzz_pp PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(fuzz_pp PRIVATE odin3_core odin3_flags)
    target_link_options(fuzz_pp PRIVATE -fsanitize=fuzzer)
endif()

# --- Tests ----------------------------------------------------------------------
include(CTest)
# Heavy runs (2E plan point (p), ruling C1): every sweep and sweep-xl CTest goes through
# odin3_add_sweep_test(), which runs it via tools/heavy-run (one heavy job machine-wide, inside
# the odin3-heavy.slice memory cap). Builds go through tools/build-locked.
set(ODIN3_HEAVY_RUN "${PROJECT_SOURCE_DIR}/tools/heavy-run")
include(${PROJECT_SOURCE_DIR}/cmake/Odin3Sweep.cmake)
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
    odin3_add_unit_test(test_util_file)
    odin3_add_unit_test(test_ir_value)
    odin3_add_unit_test(test_ir_celltype)
    odin3_add_unit_test(test_ir_netlist)
    odin3_add_unit_test(test_ir_cells)
    odin3_add_unit_test(test_ir_wire)
    odin3_add_unit_test(test_ir_prov)
    odin3_add_unit_test(test_ir_check)
    odin3_add_unit_test(test_ir_compact)
    odin3_add_unit_test(test_ir_module)
    odin3_add_unit_test(test_ir_proc)
    # 2E: the proc working form. Every later test_proc_* and test_opt_* links proc_build.c too.
    odin3_add_unit_test(test_proc_pw)
    target_sources(test_proc_pw PRIVATE tests/unit/proc_build.c)
    # 2E Task 3: the pass skeleton (fake phases), the shared budget and ffinit.
    odin3_add_unit_test(test_proc_pass)
    target_sources(test_proc_pass PRIVATE tests/unit/proc_build.c)
    target_compile_definitions(test_proc_pass PRIVATE ODIN3_DOCS_DIR="${PROJECT_SOURCE_DIR}/docs")
    odin3_add_unit_test(test_proc_emit)
    target_sources(test_proc_emit PRIVATE tests/unit/proc_build.c)
    odin3_add_unit_test(test_proc_clean)
    target_sources(test_proc_clean PRIVATE tests/unit/proc_build.c)
    odin3_add_unit_test(test_proc_rmdead)
    target_sources(test_proc_rmdead PRIVATE tests/unit/proc_build.c)
    odin3_add_unit_test(test_budget)
    odin3_add_unit_test(test_ffinit)
    odin3_add_unit_test(test_ast_srcman)
    odin3_add_unit_test(test_ast_abi_internal)
    odin3_add_unit_test(test_ast_build)
    odin3_add_unit_test(test_ast_read)
    odin3_add_unit_test(test_ast_number)
    odin3_add_unit_test(test_ast_attr)
    odin3_add_unit_test(test_ast_comment)
    odin3_add_unit_test(test_ast_symtab)
    odin3_add_unit_test(test_ast_check)
    odin3_add_unit_test(test_ast_print)
    odin3_add_unit_test(test_ast_dump)
    odin3_add_unit_test(test_ast_equal)
    # 2C: the Verilog parser. Its tests include the generated parser header (lexer_support.h).
    odin3_add_unit_test(test_verilog_parser)
    target_include_directories(test_verilog_parser PRIVATE ${_gen})
    add_dependencies(test_verilog_parser verilog_generated)
    target_compile_definitions(test_verilog_parser PRIVATE
        ODIN3_VERILOG_DATA="${PROJECT_SOURCE_DIR}/tests/verilog")
    odin3_add_unit_test(test_verilog_lexer)
    target_include_directories(test_verilog_lexer PRIVATE ${_gen})
    add_dependencies(test_verilog_lexer verilog_generated)
    odin3_add_unit_test(test_verilog_parser_decl)
    target_include_directories(test_verilog_parser_decl PRIVATE ${_gen})
    add_dependencies(test_verilog_parser_decl verilog_generated)
    odin3_add_unit_test(test_verilog_parser_expr)
    target_include_directories(test_verilog_parser_expr PRIVATE ${_gen})
    add_dependencies(test_verilog_parser_expr verilog_generated)
    odin3_add_unit_test(test_verilog_parser_stmt)
    target_include_directories(test_verilog_parser_stmt PRIVATE ${_gen})
    add_dependencies(test_verilog_parser_stmt verilog_generated)
    odin3_add_unit_test(test_verilog_parser_inst)
    target_include_directories(test_verilog_parser_inst PRIVATE ${_gen})
    add_dependencies(test_verilog_parser_inst verilog_generated)
    # The coverage rows as acceptance tests (plan point (p)): the rows of grammar tasks up to
    # ODIN3_VR_UPTO; each later grammar task raises it.
    odin3_add_unit_test(test_verilog_rows)
    target_include_directories(test_verilog_rows PRIVATE ${_gen})
    add_dependencies(test_verilog_rows verilog_generated)
    target_compile_definitions(test_verilog_rows PRIVATE ODIN3_VR_UPTO=7
        ODIN3_VERILOG_DATA="${PROJECT_SOURCE_DIR}/tests/verilog")
    odin3_add_unit_test(test_project_words)
    odin3_add_unit_test(test_project_paths)
    odin3_add_unit_test(test_project_record)
    target_compile_definitions(test_project_record PRIVATE
        ODIN3_PROJECT_FIXTURES="${PROJECT_SOURCE_DIR}/tests/golden/projects")
    odin3_add_unit_test(test_project_flist)
    target_compile_definitions(test_project_flist PRIVATE
        ODIN3_PROJECT_FIXTURES="${PROJECT_SOURCE_DIR}/tests/golden/projects"
        ODIN3_FLIST_DATA="${PROJECT_SOURCE_DIR}/tests/unit/data/project/flist")
    odin3_add_unit_test(test_project_tcl)
    odin3_add_unit_test(test_project_quartus)
    target_compile_definitions(test_project_quartus PRIVATE
        ODIN3_QUARTUS_DATA="${PROJECT_SOURCE_DIR}/tests/unit/data/project/quartus")
    odin3_add_unit_test(test_project_o3proj)
    target_compile_definitions(test_project_o3proj PRIVATE
        ODIN3_O3PROJ_DATA="${PROJECT_SOURCE_DIR}/tests/unit/data/project/o3proj")
    odin3_add_unit_test(test_project_xml)
    odin3_add_unit_test(test_project_odin2)
    target_compile_definitions(test_project_odin2 PRIVATE
        ODIN3_ODIN2_DATA="${PROJECT_SOURCE_DIR}/tests/unit/data/project/odin2")
    odin3_add_unit_test(test_project_entry)
    target_compile_definitions(test_project_entry PRIVATE
        ODIN3_PROJECT_FIXTURES="${PROJECT_SOURCE_DIR}/tests/golden/projects"
        ODIN3_O3PROJ_DATA="${PROJECT_SOURCE_DIR}/tests/unit/data/project/o3proj")
    odin3_add_unit_test(test_project_pass)
    target_compile_definitions(test_project_pass PRIVATE
        ODIN3_PROJECT_FIXTURES="${PROJECT_SOURCE_DIR}/tests/golden/projects"
        ODIN3_CLI_PROJECT="${PROJECT_SOURCE_DIR}/tests/cli/project")
    # odin3_add_pp_test(<name>): a preprocessor unit test with the shared fixture (pp_fixture.c).
    function(odin3_add_pp_test name)
        odin3_add_unit_test(${name})
        target_sources(${name} PRIVATE tests/unit/pp_fixture.c)
    endfunction()
    odin3_add_pp_test(test_pp_define)
    target_compile_definitions(test_pp_define PRIVATE
        ODIN3_PP_PROBES="${PROJECT_SOURCE_DIR}/tests/unit/data/pp/probes")
    odin3_add_pp_test(test_pp_directives)
    odin3_add_pp_test(test_pp_cond)
    target_compile_definitions(test_pp_cond PRIVATE
        ODIN3_PP_PROBES="${PROJECT_SOURCE_DIR}/tests/unit/data/pp/probes"
        ODIN3_PP_MICROS="${PROJECT_SOURCE_DIR}/tests/micro/verilog/preprocessor")
    odin3_add_pp_test(test_pp_include)
    target_compile_definitions(test_pp_include PRIVATE
        ODIN3_PP_INCLUDE_DATA="${PROJECT_SOURCE_DIR}/tests/unit/data/pp/include")
    odin3_add_pp_test(test_pp_expand)
    target_compile_definitions(test_pp_expand PRIVATE
        ODIN3_PP_PROBES="${PROJECT_SOURCE_DIR}/tests/unit/data/pp/probes")
    odin3_add_pp_test(test_pp_check)
    target_compile_definitions(test_pp_check PRIVATE
        ODIN3_PP_PROBES="${PROJECT_SOURCE_DIR}/tests/unit/data/pp/probes"
        ODIN3_PP_MICROS="${PROJECT_SOURCE_DIR}/tests/micro/verilog")
    odin3_add_pp_test(test_pp_limits)
    odin3_add_pp_test(test_pp_mixed)
    # 2B §10.5 properties: a file without directives is its own output; re-preprocessing an
    # output gives the same tokens (micros, probes and seeded random files).
    odin3_add_pp_test(test_pp_props)
    target_compile_definitions(test_pp_props PRIVATE
        ODIN3_PP_PROBES="${PROJECT_SOURCE_DIR}/tests/unit/data/pp/probes"
        ODIN3_PP_MICROS="${PROJECT_SOURCE_DIR}/tests/micro/verilog")
    odin3_add_unit_test(test_blif_lexer)
    odin3_add_unit_test(test_blif_reader)
    target_compile_definitions(test_blif_reader PRIVATE
        ODIN3_BLIF_FIXTURES="${PROJECT_SOURCE_DIR}/tests/golden/blif")
    odin3_add_unit_test(test_blif_writer)
    target_compile_definitions(test_blif_writer PRIVATE
        ODIN3_BLIF_FIXTURES="${PROJECT_SOURCE_DIR}/tests/golden/blif")
    # 3E: write_blif --vtr (tests/unit/blif_vtr_test.c: read a BLIF text, run write_blif).
    odin3_add_unit_test(test_blif_vtr)
    target_sources(test_blif_vtr PRIVATE tests/unit/blif_vtr_test.c tests/unit/proc_fixture.c
                                         tests/unit/proc_fixture_core.c)
    target_compile_definitions(test_blif_vtr PRIVATE
        ODIN3_LIB_DIR="${PROJECT_SOURCE_DIR}/lib"
        ODIN3_BLIF_FIXTURES="${PROJECT_SOURCE_DIR}/tests/golden/blif"
        ODIN3_BVT_ROOT="${PROJECT_BINARY_DIR}"
        ODIN3_BVT_DATA="${PROJECT_SOURCE_DIR}/tests/unit/data/blif_vtr")
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
    odin3_add_unit_test(test_dot_writer)
    odin3_add_unit_test(test_json_writer)
    odin3_add_unit_test(test_verilog_writer)
    # 2E Task 1: test_verilog_writer also writes rtlil/ffcells.v (IR change D's eleven cells),
    # beside test_rtlil_writer's ffcells.il, for CTest ffcells_yosys_read (ODIN3_YOSYS055 block).
    target_sources(test_verilog_writer PRIVATE tests/unit/ffcells_fixture.c)
    target_compile_definitions(test_verilog_writer PRIVATE
        ODIN3_RTLIL_OUT="${PROJECT_BINARY_DIR}/rtlil")
    set_tests_properties(test_verilog_writer PROPERTIES FIXTURES_SETUP ffcells)
    # The writers' Yosys checks run when Yosys is found (workspace build first, then PATH) and
    # are ignored otherwise (CI has no Yosys).
    find_program(ODIN3_YOSYS_EXE yosys HINTS $ENV{HOME}/odin3-ws/external/yosys/build)
    # 3D-1 (3D spec §2, AB-2): the $lut cell. test_lut_cell also writes lut/lut.json and
    # lut/lut.il, the inputs of CTests lut_json_yosys (below) and lut_rtlil_yosys (Yosys 0.55).
    file(MAKE_DIRECTORY ${PROJECT_BINARY_DIR}/lut)
    odin3_add_unit_test(test_lut_cell)
    target_compile_definitions(test_lut_cell PRIVATE ODIN3_LUT_OUT="${PROJECT_BINARY_DIR}/lut")
    set_tests_properties(test_lut_cell PROPERTIES FIXTURES_SETUP lut_fixtures)
    if(ODIN3_YOSYS_EXE)
        set_tests_properties(test_json_writer test_verilog_writer PROPERTIES
            ENVIRONMENT "ODIN3_YOSYS=${ODIN3_YOSYS_EXE}")
        # Yosys reads write_json's $lut back and counts one. Its stat line is "$lut  1" up to
        # 0.5x and "1  $lut" later, so both orders match; tee shows stat under -q, and sh makes
        # Yosys's exit status count too.
        add_test(NAME lut_json_yosys
            COMMAND sh -c "out=$(\"$0\" -q -p \"read_json $1; tee -o /dev/stdout stat\") && printf '%s\\n' \"$out\" | grep -Eq '[$]lut +1$|(^|[^0-9])1 +[$]lut$'"
                    ${ODIN3_YOSYS_EXE} ${PROJECT_BINARY_DIR}/lut/lut.json)
        set_tests_properties(lut_json_yosys PROPERTIES
            LABELS differential FIXTURES_REQUIRED lut_fixtures)
    endif()
    odin3_add_unit_test(test_pass_outputs)
    target_sources(test_pass_outputs PRIVATE tests/unit/proc_fixture.c
                                             tests/unit/proc_fixture_core.c)
    target_compile_definitions(test_pass_outputs PRIVATE
        ODIN3_BLIF_FIXTURES="${PROJECT_SOURCE_DIR}/tests/golden/blif")
    odin3_add_unit_test(test_pass_manager)
    target_sources(test_pass_manager PRIVATE tests/unit/proc_fixture.c
                                             tests/unit/proc_fixture_core.c)
    target_compile_definitions(test_pass_manager PRIVATE
        ODIN3_BLIF_FIXTURES="${PROJECT_SOURCE_DIR}/tests/golden/blif"
        ODIN3_LIB_DIR="${PROJECT_SOURCE_DIR}/lib"
        ODIN3_TECHLIB_FIXTURES="${PROJECT_SOURCE_DIR}/tests/golden/techlib")
    # 2D Task 5: write_rtlil. Its designs are also written to rtlil/*.il, the input of the
    # Yosys 0.55 round trip (rtlil_yosys_roundtrip, in the ODIN3_YOSYS055 block below).
    file(MAKE_DIRECTORY ${PROJECT_BINARY_DIR}/rtlil)
    odin3_add_unit_test(test_rtlil_writer)
    target_sources(test_rtlil_writer PRIVATE tests/unit/ffcells_fixture.c)
    target_compile_definitions(test_rtlil_writer PRIVATE
        ODIN3_RTLIL_OUT="${PROJECT_BINARY_DIR}/rtlil")
    set_tests_properties(test_rtlil_writer PROPERTIES FIXTURES_SETUP rtlil_fixtures)
    odin3_add_unit_test(test_elab_cval)
    target_compile_definitions(test_elab_cval PRIVATE
        ODIN3_ELAB_DATA="${PROJECT_SOURCE_DIR}/tests/elab")
    odin3_add_unit_test(test_elab_ctype)
    # 2D Task 9: the evaluator and interpreter, on sources parsed by 2C (verilog_test.h).
    odin3_add_unit_test(test_elab_interp)
    target_include_directories(test_elab_interp PRIVATE ${_gen})
    add_dependencies(test_elab_interp verilog_generated)
    set_tests_properties(test_rtlil_writer PROPERTIES FIXTURES_SETUP "rtlil_fixtures;ffcells")

    add_executable(test_api tests/unit/test_api.c)
    target_link_libraries(test_api PRIVATE libodin3 unity odin3_flags)
    target_compile_definitions(test_api PRIVATE
        EXAMPLE_PLUGIN_PATH="$<TARGET_FILE:example_plugin>")
    add_dependencies(test_api example_plugin)
    add_test(NAME test_api COMMAND test_api)
    set_tests_properties(test_api PROPERTIES LABELS unit)

    # The IR ABI walked like a plugin would: only libodin3.so and odin3.h.
    add_executable(test_abi_ir tests/unit/test_abi_ir.c)
    target_link_libraries(test_abi_ir PRIVATE libodin3 unity odin3_flags)
    target_compile_definitions(test_abi_ir PRIVATE
        ODIN3_BLIF_FIXTURES="${PROJECT_SOURCE_DIR}/tests/golden/blif")
    add_test(NAME test_abi_ir COMMAND test_abi_ir)
    set_tests_properties(test_abi_ir PROPERTIES LABELS unit)
    odin3_add_unit_test(test_abi_values)
    target_sources(test_abi_values PRIVATE tests/unit/proc_fixture.c
                                           tests/unit/proc_fixture_core.c)
    # Test-only, never installed: libodin3's objects plus odin3_test_proc_design (Task 3's P1), so
    # the Python walk reaches a $proc node through the ABI before a front end makes one (2D Task 6).
    add_library(odin3_procfix SHARED $<TARGET_OBJECTS:odin3_core>
                tests/unit/proc_fixture_lib.c tests/unit/proc_fixture_core.c)
    set_target_properties(odin3_procfix PROPERTIES C_VISIBILITY_PRESET hidden)
    target_include_directories(odin3_procfix PRIVATE ${PROJECT_SOURCE_DIR}/src
                                                     ${PROJECT_SOURCE_DIR}/include)
    target_link_libraries(odin3_procfix PRIVATE odin3_flags ${CMAKE_DL_LIBS})
    # The AST and source-manager ABI (v4) built and read like an adapter would: libodin3.so only.
    add_executable(test_ast_abi tests/unit/test_ast_abi.c)
    target_link_libraries(test_ast_abi PRIVATE libodin3 unity odin3_flags)
    add_test(NAME test_ast_abi COMMAND test_ast_abi)
    set_tests_properties(test_ast_abi PROPERTIES LABELS unit)
    # A plugin whose init registers, then fails (it must stay loaded: test_abi_ext).
    add_library(plugin_bad_init MODULE tests/unit/plugin_bad_init.c)
    set_target_properties(plugin_bad_init PROPERTIES PREFIX "")
    target_link_libraries(plugin_bad_init PRIVATE libodin3 odin3_flags)
    # Plugin cell types and passes, and the provenance visitors, through libodin3.so only.
    add_executable(test_abi_ext tests/unit/test_abi_ext.c)
    target_link_libraries(test_abi_ext PRIVATE libodin3 unity odin3_flags)
    target_compile_definitions(test_abi_ext PRIVATE
        ODIN3_BLIF_FIXTURES="${PROJECT_SOURCE_DIR}/tests/golden/blif"
        ODIN3_CLI_FIXTURES="${PROJECT_SOURCE_DIR}/tests/cli"
        ODIN3_TEST_TMP="${PROJECT_BINARY_DIR}/test_abi_ext.blif"
        EXAMPLE_PLUGIN_PATH="$<TARGET_FILE:example_plugin>"
        BAD_INIT_PLUGIN_PATH="$<TARGET_FILE:plugin_bad_init>")
    add_dependencies(test_abi_ext example_plugin plugin_bad_init)
    add_test(NAME test_abi_ext COMMAND test_abi_ext)
    set_tests_properties(test_abi_ext PROPERTIES LABELS unit)
    odin3_add_unit_test(test_abi_core)
    target_compile_definitions(test_abi_core PRIVATE
        ODIN3_BLIF_FIXTURES="${PROJECT_SOURCE_DIR}/tests/golden/blif"
        ODIN3_CLI_FIXTURES="${PROJECT_SOURCE_DIR}/tests/cli")

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
    # 3E §16.2: write_blif --vtr over tests/golden/blif-vtr (and the goldens its sample.txt
    # lists) against the expected files, then blif-vtr-check --strict on every expected file
    # (vtr_fixtures.STRICT_EXCEPTIONS without --strict).
    add_test(NAME blif_vtr_fixtures
             COMMAND ${PROJECT_SOURCE_DIR}/tools/blif-vtr-check/vtr-fixtures
                     --odin3 $<TARGET_FILE:odin3> --dir ${PROJECT_SOURCE_DIR}/tests/golden/blif-vtr
                     --checker ${PROJECT_SOURCE_DIR}/tools/blif-vtr-check/blif-vtr-check)
    set_tests_properties(blif_vtr_fixtures PROPERTIES LABELS unit)
    # 3E §15.2: the committed models JSONs pass the schema check, and lib/vtr.o3lib's hard cells
    # match the k6_frac models (CI; the freshness `check --arch` runs locally and in 3H).
    set(_vtr_models ${PROJECT_SOURCE_DIR}/tools/vtr-models/vtr-models)
    set(_vtr_arch ${PROJECT_SOURCE_DIR}/tests/vtr/arch)
    add_test(NAME vtr_models_check
             COMMAND sh -c "\"$0\" check \"$1\" && \"$0\" check \"$2\" && \"$0\" check-lib \"$3\" --models \"$1\""
                     ${_vtr_models} ${_vtr_arch}/k6_frac_N10_frac_chain_mem32K_40nm.models.json
                     ${_vtr_arch}/EArch.models.json ${PROJECT_SOURCE_DIR}/lib/vtr.o3lib)
    set_tests_properties(vtr_models_check PROPERTIES LABELS unit)
    # The driver's own negative: a deliberately wrong expected file must be reported.
    add_test(NAME blif_vtr_fixtures_detects_corruption
             COMMAND ${PROJECT_SOURCE_DIR}/tools/blif-vtr-check/vtr-fixtures
                     --odin3 $<TARGET_FILE:odin3>
                     --dir ${PROJECT_SOURCE_DIR}/tests/golden/blif-vtr-corrupt)
    set_tests_properties(blif_vtr_fixtures_detects_corruption PROPERTIES LABELS unit
        PASS_REGULAR_EXPRESSION "FAIL corrupt: differs from corrupt.vtr.blif")
    # The goldens' distinct black-box stanzas (tools/golden-blackboxes) against lib/vtr.o3lib.
    add_test(NAME blif_roundtrip_techlib
             COMMAND ${PROJECT_SOURCE_DIR}/tools/blif-roundtrip/blif-roundtrip
                     --rt $<TARGET_FILE:odin3-blif-rt> --techlib ${PROJECT_SOURCE_DIR}/lib/vtr.o3lib
                     --fixtures ${PROJECT_SOURCE_DIR}/tests/golden/techlib)
    set_tests_properties(blif_roundtrip_techlib PROPERTIES LABELS unit)

    # Writer validation over the fixtures (Yosys, Icarus, Graphviz, ABC); exit 77 (skipped)
    # when a tool is missing, as in CI.
    set(_wc_args --write $<TARGET_FILE:odin3-write> --fixtures ${PROJECT_SOURCE_DIR}/tests/golden/blif)
    if(ODIN3_YOSYS_EXE)
        list(PREPEND _wc_args --yosys ${ODIN3_YOSYS_EXE})
    endif()
    set(_wc_env "")
    set(_wc_env_list "")
    if(ODIN3_WITH_ABC)
        set(_wc_env "ODIN3_ABC=$<TARGET_FILE:abc>")
        set(_wc_env_list "ODIN3_ABC=$<TARGET_FILE:abc>;")
    endif()
    add_test(NAME writer_check_fixtures
             COMMAND ${PROJECT_SOURCE_DIR}/tools/writer-check/writer-check -j 2 ${_wc_args})
    set_tests_properties(writer_check_fixtures PROPERTIES LABELS unit SKIP_RETURN_CODE 77
        ENVIRONMENT "${_wc_env}")
    # Negative check: damaged JSON and Verilog must be caught by both gates.
    add_test(NAME writer_check_detects_corruption
             COMMAND ${PROJECT_SOURCE_DIR}/tools/writer-check/writer-check -j 2 ${_wc_args}
                     --write ${PROJECT_SOURCE_DIR}/tools/writer-check/write-corrupt)
    set_tests_properties(writer_check_detects_corruption PROPERTIES LABELS unit
        SKIP_REGULAR_EXPRESSION "writer-check: missing tools"
        PASS_REGULAR_EXPRESSION "json: identical [0-9]+, ref-excluded [0-9]+, refused \\(multi-driver\\) [0-9]+, different [1-9][^\n]*\nverilog iverilog[^\n]*\nverilog equiv: equivalent [0-9]+, excluded [0-9]+, NOT-equivalent [1-9]"
        ENVIRONMENT "${_wc_env_list}WC_REAL_WRITE=$<TARGET_FILE:odin3-write>")
    # Negative check: a black box only the writer's side has is an error, not an exclusion.
    add_test(NAME writer_check_bbox_not_excluded
             COMMAND ${PROJECT_SOURCE_DIR}/tools/writer-check/writer-check -j 2 ${_wc_args}
                     --write ${PROJECT_SOURCE_DIR}/tools/writer-check/write-corrupt)
    set_tests_properties(writer_check_bbox_not_excluded PROPERTIES LABELS unit
        SKIP_REGULAR_EXPRESSION "writer-check: missing tools"
        PASS_REGULAR_EXPRESSION "verilog equiv: equivalent 0, excluded [0-9]+, NOT-equivalent 0, yosys-error 0, error [1-9]"
        ENVIRONMENT "${_wc_env_list}WC_CORRUPT=bbox;WC_REAL_WRITE=$<TARGET_FILE:odin3-write>")

    add_test(NAME symbol_rules
        COMMAND ${PROJECT_SOURCE_DIR}/tools/check-symbols.sh
                $<TARGET_FILE:libodin3> $<TARGET_FILE:example_plugin>)
    set_tests_properties(symbol_rules PROPERTIES LABELS unit)

    add_test(NAME abi_exports
        COMMAND ${PROJECT_SOURCE_DIR}/tools/check-symbols.sh
                --exports ${PROJECT_SOURCE_DIR}/include/odin3/odin3.h $<TARGET_FILE:libodin3>)
    set_tests_properties(abi_exports PROPERTIES LABELS unit)

    # The ABI stays v4 through 2D (ruling I1): 2D's additions are additive.
    add_test(NAME cli_version COMMAND odin3 --version)
    set_tests_properties(cli_version PROPERTIES
        LABELS unit PASS_REGULAR_EXPRESSION "^odin3 ${PROJECT_VERSION} \\(ABI 4\\)")
    add_test(NAME cli_example_plugin COMMAND odin3 --plugin $<TARGET_FILE:example_plugin>)
    set_tests_properties(cli_example_plugin PROPERTIES LABELS unit)
    # tests/cli/test_cli.sh <case> ...: pass scripts end to end (round trip, located errors, --top).
    foreach(_case IN ITEMS script_roundtrip p_roundtrip unknown_pass_p unknown_pass_script
                           unknown_pass_later_script top top_written_first top_unapplied quoted
                           help_runs_nothing check_flag usage techlib plugin writers sim keep_ast
                           project_dump project_top project_revision project_diag_paths
                           project_preprocess)
        add_test(NAME cli_${_case}
            COMMAND ${PROJECT_SOURCE_DIR}/tests/cli/test_cli.sh ${_case} $<TARGET_FILE:odin3>
                    ${PROJECT_SOURCE_DIR}/tools/netlist-compare/netlist-compare
                    ${PROJECT_SOURCE_DIR}/tests/golden/blif)
        set_tests_properties(cli_${_case} PROPERTIES LABELS unit
            ENVIRONMENT "ODIN3_EXAMPLE_PLUGIN=$<TARGET_FILE:example_plugin>;ODIN3_SIM_VECTORS=$<TARGET_FILE:odin3-sim-vectors>")
    endforeach()

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
    # ODIN3_CMAKE: test_heavy_run's configure-time check of odin3_add_sweep_test (cmake -P).
    list(APPEND _tools_env "ODIN3_CMAKE=${CMAKE_COMMAND}")
    # ODIN3_CLI: test_vtr_fixtures' round trips (3E §16.3 step 1 on the --vtr fixtures).
    list(APPEND _tools_env "ODIN3_CLI=$<TARGET_FILE:odin3>")
    set_tests_properties(python_tools PROPERTIES LABELS tools ENVIRONMENT "${_tools_env}")
    # 3E §15.1/§15.2: the VPR negative corpus. Every tests/vtr/blif-bad/*.blif has exactly one
    # violation of its prefix's class; every strict/*.blif passes without --strict and fails with
    # it. (That VPR rejects exactly the top-level files is Task 11's --bad-corpus run.)
    add_test(NAME blif_vtr_check_bad
             COMMAND ${Python3_EXECUTABLE} -m unittest -v tests.tools.test_vtr_models.BlifBadCorpusTest
             WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(blif_vtr_check_bad PROPERTIES LABELS unit)
    # The project-input corpus (tests/golden/projects) parses to its expected records in every
    # format. The oracle half (`project-fixtures oracle`) needs Yosys and GHDL and runs locally.
    add_test(NAME project_fixtures
        COMMAND ${Python3_EXECUTABLE}
                ${PROJECT_SOURCE_DIR}/tools/project-fixtures/project_fixtures.py check)
    set_tests_properties(project_fixtures PROPERTIES LABELS tools)
    # 2B §10.2: odin3's read_project/dump_project (and preprocess for the include negatives)
    # reproduce every case's record or located error in every format whose reader has landed;
    # front-end negatives are skipped until read_verilog exists (--with-frontend).
    add_test(NAME project_corpus
        COMMAND ${Python3_EXECUTABLE}
                ${PROJECT_SOURCE_DIR}/tools/project-fixtures/project_fixtures.py compare
                --odin3 $<TARGET_FILE:odin3>)
    set_tests_properties(project_corpus PROPERTIES LABELS tools)
    # 2B §10.5: model-based fuzzing of the preprocessor through `preprocess --segments`
    # (1,000 seeded cases here; run 100,000 locally with --cases), and the files-read property
    # over the corpus's Phase 2 positive cases.
    add_test(NAME pp_fuzz
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/pp-fuzz/pp_fuzz.py run
                --odin3 $<TARGET_FILE:odin3> --seed 1 --cases 1000)
    set_tests_properties(pp_fuzz PROPERTIES LABELS tools)
    add_test(NAME pp_properties
        COMMAND ${CMAKE_COMMAND} -E env ODIN3_BIN=$<TARGET_FILE:odin3>
                ${Python3_EXECUTABLE} -m unittest -v tests.tools.test_pp_properties
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(pp_properties PROPERTIES LABELS tools)

    if(ODIN3_FUZZ)
        # The seeds (`pp-fuzz seeds`): the preprocessor micros, every micro holding a backtick
        # and the reviewer's probes, smallest first, each after options byte 0x00 (tight caps)
        # and 0xC3 (roomy caps, defines, include directory). fuzz_pp_default_caps runs each seed
        # once at the default §9 caps (no mutation: a mutated bomb needs the lowered caps);
        # fuzz_pp_smoke fuzzes for 10 s; long runs are local. One worker, 2 GB RSS.
        # -print_funcs=0: libFuzzer symbolizes every NEW_FUNC line, and without an unversioned
        # llvm-symbolizer ASan falls back to addr2line, which stalls the smoke ~90 s. Crash
        # reports still symbolize; ASAN_SYMBOLIZER_PATH points at a versioned one when found.
        find_program(ODIN3_LLVM_SYMBOLIZER NAMES llvm-symbolizer llvm-symbolizer-20
                     llvm-symbolizer-19 llvm-symbolizer-18)
        set(_fuzz_env "")
        if(ODIN3_LLVM_SYMBOLIZER)
            set(_fuzz_env "ASAN_SYMBOLIZER_PATH=${ODIN3_LLVM_SYMBOLIZER}")
        endif()
        set(_fuzz_seeds ${PROJECT_BINARY_DIR}/fuzz_pp_seeds)
        set(_fuzz_corpus ${PROJECT_BINARY_DIR}/fuzz_pp_corpus)
        file(MAKE_DIRECTORY ${_fuzz_corpus})
        add_test(NAME fuzz_pp_seeds
            COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/pp-fuzz/pp_fuzz.py seeds
                    ${_fuzz_seeds} ${PROJECT_SOURCE_DIR}/tests/micro/verilog/preprocessor
                    ${PROJECT_SOURCE_DIR}/tests/unit/data/pp/probes
                    --backtick ${PROJECT_SOURCE_DIR}/tests/micro/verilog)
        set_tests_properties(fuzz_pp_seeds PROPERTIES FIXTURES_SETUP fuzz_pp_seeds LABELS fuzz)
        add_test(NAME fuzz_pp_default_caps
            COMMAND ${CMAKE_COMMAND} -E env FUZZ_PP_DEFAULT_CAPS=1
                    $<TARGET_FILE:fuzz_pp> -runs=0 -timeout=60 -rss_limit_mb=2048 ${_fuzz_seeds})
        set_tests_properties(fuzz_pp_default_caps PROPERTIES FIXTURES_REQUIRED fuzz_pp_seeds
                             LABELS fuzz TIMEOUT 300)
        add_test(NAME fuzz_pp_smoke
            COMMAND fuzz_pp -max_total_time=10 -timeout=10 -rss_limit_mb=2048 -print_funcs=0
                    -dict=${PROJECT_SOURCE_DIR}/tests/fuzz/fuzz_pp.dict -print_final_stats=1
                    ${_fuzz_corpus} ${_fuzz_seeds})
        set_tests_properties(fuzz_pp_smoke PROPERTIES FIXTURES_REQUIRED fuzz_pp_seeds
                             LABELS fuzz TIMEOUT 120)
        if(_fuzz_env)
            set_tests_properties(fuzz_pp_default_caps fuzz_pp_smoke PROPERTIES
                                 ENVIRONMENT "${_fuzz_env}")
        endif()
    endif()

    # 2B §10.4: the preprocessor against Icarus and Yosys over the probes, the backtick micros
    # and the VTR set (local only: skipped, exit 77, when iverilog, Yosys or the VTR set is
    # absent, as in CI).
    add_test(NAME pp_diff
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/pp-diff/pp_diff.py
                --odin3 $<TARGET_FILE:odin3>
                --iverilog $<IF:$<BOOL:${ODIN3_IVERILOG}>,${ODIN3_IVERILOG},iverilog>
                --yosys $<IF:$<BOOL:${ODIN3_YOSYS}>,${ODIN3_YOSYS},yosys>
                --suite ${PROJECT_SOURCE_DIR})
    set_tests_properties(pp_diff PROPERTIES LABELS differential SKIP_RETURN_CODE 77)

    # 2C §3.1: the conflicts Bison resolves by precedence are exactly the committed allowlist,
    # and no operator token can follow an expression outside the operator rules (--operand).
    add_test(NAME verilog_grammar_solved
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/parse-check/solved_conflicts.py
                --bison ${BISON_EXECUTABLE}
                --grammar ${PROJECT_SOURCE_DIR}/src/frontends/verilog/verilog.y --operand expr
                --expect ${PROJECT_SOURCE_DIR}/tests/verilog/solved_conflicts.txt)
    set_tests_properties(verilog_grammar_solved PROPERTIES LABELS unit)

    # Python loading a sanitized libodin3 needs the sanitizer runtime preloaded.
    set(_python_env "ODIN3_LIB=$<TARGET_FILE:libodin3>"
                    "ODIN3_PROC_LIB=$<TARGET_FILE:odin3_procfix>")
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
    # The Python plugin tests need cffi: the repo's .venv has it (requirements-dev.txt), so they
    # use its Python when there is one (CI's Python3 has it too); skip (not fail) without cffi.
    find_program(ODIN3_PLUGIN_PYTHON NAMES python3 python
                 PATHS ${PROJECT_SOURCE_DIR}/.venv/bin NO_DEFAULT_PATH)
    if(NOT ODIN3_PLUGIN_PYTHON)
        set(ODIN3_PLUGIN_PYTHON ${Python3_EXECUTABLE})
    endif()
    add_test(NAME python_example_plugin
        COMMAND ${CMAKE_COMMAND} -E env ${_python_env}
                ${ODIN3_PLUGIN_PYTHON} plugins/python/example_plugin.py
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(python_example_plugin PROPERTIES
        LABELS unit SKIP_REGULAR_EXPRESSION "No module named 'cffi'")
    # Phase 1 exit test (1D): Python walks every committed BLIF fixture through the C ABI and
    # gets exactly the `stats` pass's counts and cell-type histogram. Any unittest skip (no cffi;
    # no ODIN3_LIB) is reported as Skipped; with ODIN3_REQUIRE_CFFI=1 (CI) a missing cffi fails.
    add_test(NAME python_walk
        COMMAND ${CMAKE_COMMAND} -E env ${_python_env}
                ${ODIN3_PLUGIN_PYTHON} -m unittest -v tests.tools.test_python_walk
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(python_walk PROPERTIES
        LABELS tools SKIP_REGULAR_EXPRESSION "skipped '")
    # 2A: the AST helpers (Design.ast, Ast, AstNode) walk a module built through the ABI.
    add_test(NAME python_ast
        COMMAND ${CMAKE_COMMAND} -E env ${_python_env}
                ${ODIN3_PLUGIN_PYTHON} -m unittest -v tests.tools.test_python_ast
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(python_ast PROPERTIES
        LABELS tools SKIP_REGULAR_EXPRESSION "skipped '")

    # Yosys 0.55 as VTR pins it (2D plan point (p)): the oracle of every 2D-2F comparison. Found
    # only in the VTR build (never PATH, which may hold another Yosys) and accepted only when
    # `yosys -V` says 0.55; otherwise no 0.55 test is registered (CI has none: they run locally
    # and nightly). Every later Yosys-0.55 CTest goes inside this block. ODIN3_YOSYS (above) is
    # the workspace 0.69 build, used only by the nightly drift job; the two are never mixed.
    find_program(ODIN3_YOSYS055 yosys
        HINTS $ENV{VTR_ROOT}/build/bin
              $ENV{HOME}/odin3-ws/external/vtr-verilog-to-routing/build/bin
        NO_DEFAULT_PATH)
    set(ODIN3_YOSYS055_OK FALSE)
    if(ODIN3_YOSYS055)
        execute_process(COMMAND ${ODIN3_YOSYS055} -V
            OUTPUT_VARIABLE _yosys055_version ERROR_QUIET RESULT_VARIABLE _yosys055_rc
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(_yosys055_rc EQUAL 0 AND _yosys055_version MATCHES "Yosys 0\\.55([^0-9]|$)")
            set(ODIN3_YOSYS055_OK TRUE)
            message(STATUS "Yosys 0.55: ${ODIN3_YOSYS055} (${_yosys055_version})")
        else()
            message(STATUS "Yosys 0.55: ${ODIN3_YOSYS055} is '${_yosys055_version}', not 0.55; "
                           "no Yosys-0.55 tests")
        endif()
    else()
        message(STATUS "Yosys 0.55: not found (VTR build); no Yosys-0.55 tests")
    endif()
    if(ODIN3_YOSYS055_OK)
        # 2D §13/§14.3: Yosys 0.55 reads every write_rtlil fixture of test_rtlil_writer back
        # (read_rtlil; write_rtlil) and lowers it (read_rtlil; proc; opt; write_blif), smallest
        # first.
        add_test(NAME rtlil_yosys_roundtrip
            COMMAND ${PROJECT_SOURCE_DIR}/tools/rtlil-check/yosys-roundtrip
                    ${ODIN3_YOSYS055} ${PROJECT_BINARY_DIR}/rtlil)
        set_tests_properties(rtlil_yosys_roundtrip PROPERTIES
            LABELS differential FIXTURES_REQUIRED rtlil_fixtures)
        # 2E Task 1 (IR change D, plan point (b)): Yosys 0.55 reads the eleven word flip-flops
        # and latches as test_rtlil_writer writes them (ffcells.il; Yosys's cell checker) and as
        # test_verilog_writer writes them (ffcells.v, generic instances; -icells).
        add_test(NAME ffcells_yosys_read
            COMMAND ${ODIN3_YOSYS055} -q -p
                    "read_rtlil ${PROJECT_BINARY_DIR}/rtlil/ffcells.il; check -assert; write_rtlil -; design -reset; read_verilog -icells ${PROJECT_BINARY_DIR}/rtlil/ffcells.v; hierarchy -check; check -assert")
        set_tests_properties(ffcells_yosys_read PROPERTIES
            LABELS differential FIXTURES_REQUIRED ffcells)
        # 2E Task 3 (2D spec §13 point 7, review I1): Yosys 0.55's hierarchy -check accepts
        # write_rtlil's black-box stubs (bbox.il: one parameterised black box at two widths, a
        # declared model with an inout, a hard cell), and no instance port is resized (-e).
        add_test(NAME rtlil_bbox_hierarchy
            COMMAND ${ODIN3_YOSYS055} -q -e Resizing -p
                    "read_rtlil ${PROJECT_BINARY_DIR}/rtlil/bbox.il; hierarchy -check -top top; check -assert")
        set_tests_properties(rtlil_bbox_hierarchy PROPERTIES
            LABELS differential FIXTURES_REQUIRED rtlil_fixtures)
        # 3D-1 (finding 21): Yosys 0.55 reads write_rtlil's $lut (WIDTH 2, LUT 4'0110) and its
        # stat reports one $lut; sh makes the exit status count as well as the line.
        add_test(NAME lut_rtlil_yosys
            COMMAND sh -c "out=$(\"$0\" -q -p \"read_rtlil $1; tee -o /dev/stdout stat\") && printf '%s\\n' \"$out\" | grep -Eq '[$]lut +1$'"
                    ${ODIN3_YOSYS055} ${PROJECT_BINARY_DIR}/lut/lut.il)
        set_tests_properties(lut_rtlil_yosys PROPERTIES
            LABELS differential FIXTURES_REQUIRED lut_fixtures)
        # test_verilog_writer's own -icells check of the eleven runs on this 0.55 too, never on
        # the workspace 0.69 of ODIN3_YOSYS.
        set_property(TEST test_verilog_writer APPEND PROPERTY
            ENVIRONMENT "ODIN3_YOSYS055=${ODIN3_YOSYS055}")
    endif()
endif()
