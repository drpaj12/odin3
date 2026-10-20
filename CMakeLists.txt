cmake_minimum_required(VERSION 3.25)
project(odin3 VERSION 0.0.0 LANGUAGES C)

# --- Options -----------------------------------------------------------------
option(ODIN3_WITH_ABC "Build third_party/abc (libabc-pic, the abc tool; the adapter from 3D-2)"
       ON)
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
    src/api/elab_abi.c
    src/api/srcman_abi.c
    src/api/module.c
    src/api/node.c
    src/api/net.c
    src/api/attr.c
    src/api/log.c
    src/api/abc.c
    src/util/alloc.c
    src/util/arena.c
    src/util/hash.c
    src/util/vec.c
    src/util/pagevec.c
    src/util/u64map.c
    src/util/idindex.c
    src/util/str.c
    src/util/clock.c
    src/util/log.c
    src/util/file.c
    src/util/sha256.c
    src/frontends/blif/lexer.c
    src/frontends/blif/param_text.c
    src/frontends/blif/reader.c
    src/frontends/json/json_lex.c
    src/frontends/json/json_parse.c
    src/backends/blif/writer.c
    src/backends/blif/vtr.c
    src/backends/blif/isop.c
    src/passes/manager.c
    src/passes/builtin.c
    src/passes/write_formats.c
    src/passes/sim_pass.c
    src/passes/project_pass.c
    src/passes/arch_pass.c
    src/passes/proc/pw.c
    src/passes/proc/pw_sig.c
    src/passes/proc/pw_encode.c
    src/passes/proc/proc_emit.c
    src/passes/proc/proc_diag.c
    src/passes/proc/proc_clean.c
    src/passes/proc/proc_rmdead.c
    src/passes/proc/proc_prune.c
    src/passes/proc/proc_arst.c
    src/passes/proc/proc_clock.c
    src/passes/proc/proc_snip.c
    src/passes/proc/proc_mux.c
    src/passes/proc/proc_latch.c
    src/passes/proc/proc_ff.c
    src/passes/proc/ppool.c
    src/passes/proc/proc_pass.c
    src/passes/budget.c
    src/passes/ffinit.c
    src/passes/opt/kit.c
    src/passes/opt/dirty.c
    src/passes/opt/opt_loop.c
    src/passes/opt/ff.c
    src/passes/opt/fold.c
    src/passes/opt/expr.c
    src/passes/opt/expr_util.c
    src/passes/opt/expr_logic.c
    src/passes/opt/expr_cmp.c
    src/passes/opt/expr_arith.c
    src/passes/opt/expr_mux.c
    src/passes/opt/expr_undriven.c
    src/passes/opt/expr_ffpol.c
    src/passes/opt/expr_fine.c
    src/passes/opt/clean.c
    src/passes/opt/merge.c
    src/passes/opt/ffbits.c
    src/passes/opt/muxtree.c
    src/passes/opt/wreduce.c
    src/passes/opt/wreduce_op.c
    src/passes/opt/wreduce_ff.c
    src/passes/opt/wreduce_order.c
    src/passes/opt/reduce.c
    src/passes/opt/share.c
    src/passes/opt/dff.c
    src/passes/opt/dff_state.c
    src/passes/opt/dff_dirty.c
    # 4.8: the fsm pass.
    src/passes/fsm/fsmdata.c
    src/passes/fsm/index.c
    src/passes/fsm/ceval.c
    src/passes/fsm/common.c
    src/passes/fsm/sigcache.c
    src/passes/fsm/detect.c
    src/passes/fsm/extract.c
    src/passes/fsm/extract_trans.c
    src/passes/fsm/info.c
    src/passes/fsm/opt.c
    src/passes/fsm/recode.c
    src/passes/fsm/map.c
    src/passes/fsm/map_pattern.c
    src/passes/fsm/unused.c
    src/passes/fsm/fsm.c
    src/passes/fsm/register.c
    src/passes/opt/dff_gates.c
    src/passes/opt/dff_rules.c
    src/passes/opt/dff_constbits.c
    src/passes/opt/dff_merge.c
    src/passes/opt/dff_muxes.c
    src/passes/opt/register.c
    src/passes/opt/simcheck.c
    src/passes/flatten/graph.c
    src/passes/flatten/register.c
    src/passes/flatten/expand.c
    src/passes/flatten/copy.c
    src/passes/flatten/names.c
    src/passes/flatten/prov.c
    src/passes/lower/register.c
    src/passes/lower/lower.c
    src/passes/lower/kit.c
    src/passes/lower/connect.c
    src/passes/lower/size.c
    src/passes/lower/dispatch.c
    src/passes/lower/report.c
    src/passes/lower/bitwise.c
    src/passes/lower/pmux_shift.c
    src/passes/lower/alu.c
    src/passes/lower/cmp.c
    src/passes/lower/mul.c
    src/passes/lower/div.c
    src/passes/lower/ff.c
    src/passes/lower/latch.c
    src/passes/lower/mem.c
    src/passes/lower/snapshot.c
    src/passes/partial_map/register.c
    src/passes/partial_map/hardmap.c
    src/passes/partial_map/generic.c
    src/passes/partial_map/hm_kit.c
    src/passes/partial_map/mul.c
    src/passes/partial_map/adder.c
    src/passes/partial_map/ram.c
    src/passes/partial_map/ram_split.c
    src/passes/partial_map/mem.c
    src/passes/partial_map/mem_merge.c
    src/passes/partial_map/memory_merge.c
    src/passes/partial_map/ramstyle.c
    src/passes/partial_map/register_merge.c
    src/passes/partial_map/mem_plan.c
    src/passes/partial_map/rules.c
    src/passes/lpmmap/register.c
    src/passes/lpmmap/lpmmap.c
    src/passes/lpmmap/lm_kit.c
    src/passes/lpmmap/addsub.c
    src/passes/lpmmap/mult.c
    src/passes/lpmmap/compare.c
    src/passes/lpmmap/mem.c
    src/passes/abc/alias.c
    src/passes/abc/allow.c
    src/passes/abc/script.c
    src/passes/abc/engine.c
    src/passes/abc/child.c
    src/passes/abc/args.c
    src/passes/abc/region.c
    src/passes/abc/result.c
    src/passes/abc/xblif.c
    src/passes/abc/xblif_read.c
    src/passes/abc/aig.c
    src/passes/abc/aligned.c
    src/passes/abc/exchange.c
    src/passes/abc/splice.c
    src/passes/abc/splice_a.c
    src/passes/abc/splice_bc.c
    src/passes/abc/splice_d.c
    src/passes/abc/restore.c
    src/passes/abc/prov.c
    src/passes/abc/depth.c
    src/passes/abc/run.c
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
    src/frontends/arch/arch.c
    src/frontends/arch/arch_xml.c
    src/frontends/arch/arch_models.c
    src/frontends/arch/arch_pb.c
    src/frontends/arch/arch_lib.c
    src/frontends/arch/arch_templates.c
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
    src/frontends/elab/bind.c
    src/frontends/verilog/read_verilog.c
    src/frontends/verilog/read_verilog_lib.c
    src/frontends/verilog/elab1.c
    src/frontends/verilog/elab1_lookup.c
    src/frontends/verilog/elab1_unit.c
    src/frontends/verilog/elab1_top.c
    src/frontends/verilog/elab1_params.c
    src/frontends/verilog/elab1_spec.c
    src/frontends/verilog/elab1_expand.c
    src/frontends/verilog/elab1_write.c
    src/frontends/verilog/elab1_inst.c
    src/frontends/verilog/elab1_bbox.c
    src/frontends/verilog/elab1_gen.c
    src/frontends/verilog/elab1_decl.c
    src/frontends/verilog/elab1_diag.c
    src/frontends/elab/elab_pending.c
    src/frontends/elab/elab2.c
    src/frontends/elab/elab2_wires.c
    src/frontends/elab/elab2_expr.c
    src/frontends/elab/elab2_select.c
    src/frontends/elab/elab2_varsel.c
    src/frontends/elab/elab2_assign.c
    src/frontends/elab/elab2_claim.c
    src/frontends/elab/elab2_wired.c
    src/frontends/elab/elab2_prescan.c
    src/frontends/elab/elab2_warn.c
    src/frontends/elab/elab2_tri.c
    src/frontends/elab/elab2_trilogic.c
    src/frontends/elab/elab2_trivtr.c
    src/frontends/elab/elab2_trivtr_out.c
    src/frontends/elab/elab2_mem.c
    src/frontends/elab/elab2_memport.c
    src/frontends/elab/elab2_memscan.c
    src/frontends/elab/elab2_bbox.c
    src/frontends/elab/elab2_words.c
    src/frontends/elab/pg.c
    src/frontends/elab/elab2_call.c
    src/frontends/elab/pg_always.c
    src/frontends/elab/pg_call.c
    src/frontends/elab/pg_case.c
    src/frontends/elab/pg_emit.c
    src/frontends/elab/pg_loop.c
    src/frontends/elab/pg_scan.c
    src/frontends/elab/pg_memw.c
    src/frontends/elab/pg_init.c
    src/frontends/elab/pg_init_vtr.c
    src/frontends/elab/pg_readmem.c
    src/frontends/elab/pg_select.c
    src/frontends/elab/elab2_inst.c
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
    src/backends/prov_name.c
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

# --- The abc pass's default script, embedded (3D spec §7.7, AB-23) ----------------------------
# lib/abc/vtr.abc becomes the byte array odin3_abc_default_script in
# generated/abc/default_script.c; editing the script re-runs the configure step.
set(_abc_default_script ${PROJECT_SOURCE_DIR}/lib/abc/vtr.abc)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_abc_default_script})
file(READ ${_abc_default_script} _abc_default_hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," ODIN3_ABC_DEFAULT_SCRIPT_BYTES
       "${_abc_default_hex}")
configure_file(cmake/abc_default_script.c.in
               ${PROJECT_BINARY_DIR}/generated/abc/default_script.c @ONLY)
target_sources(odin3_core PRIVATE ${PROJECT_BINARY_DIR}/generated/abc/default_script.c)
# The child runner (3D spec §7.9, AB-33) uses glibc's posix_spawn_file_actions_addchdir_np and
# _addclosefrom_np, which need _DEFAULT_SOURCE on top of odin3_flags' _POSIX_C_SOURCE.
set_source_files_properties(src/passes/abc/child.c PROPERTIES COMPILE_DEFINITIONS _DEFAULT_SOURCE)

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
# dangling-else precedence, with Task 7's instantiation, gates and generate, 941 states, and
# with Task 10's binary ~& and ~|, 947 states (gcc 13): still none but the one below). One flag,
# for the parser only:
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
    # ABC (3D spec AB-5). cmake/abc_pin.cmake stops configure unless third_party/abc is at the
    # commit docs/ORACLES.md pins, and sets ODIN3_ABC_COMMIT. ABC's own CMake project enables C++
    # and drives its Makefile for flags; we build only its `libabc-pic` (a PIC static archive,
    # linked into the adapter from 3D-2) and `odin3_abc_exe`, the `abc` tool linked to it, so
    # ABC compiles once per build directory. ABC's own `abc` and `libabc` targets are never built.
    include(${PROJECT_SOURCE_DIR}/cmake/abc_pin.cmake)
    # ABC has C++ sources, so whatever links libabc-pic links through the C++ driver.
    enable_language(CXX)
    # The function scope keeps our C17 settings out of ABC, which needs GNU extensions; neither
    # odin3_flags nor the sanitisers (both on odin3_flags only) reach ABC's targets.
    function(odin3_add_abc)
        unset(CMAKE_C_STANDARD)
        unset(CMAKE_C_EXTENSIONS)
        # ABC is always optimised: unoptimised, it would stretch the large runs several-fold.
        # Debug keeps its assertions; Release drops them.
        set(CMAKE_C_FLAGS_DEBUG "-g -O2")
        set(CMAKE_CXX_FLAGS_DEBUG "-g -O2")
        set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")
        set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
        set(READLINE_FOUND FALSE)
        set(ABC_SKIP_TESTS ON CACHE BOOL "Skip ABC googletest suite (needs network at configure)")
        add_subdirectory(third_party/abc EXCLUDE_FROM_ALL)
        # No abc.history in the working directory (ABC's cmd/cmdHist.c).
        set_property(TARGET libabc-pic APPEND PROPERTY COMPILE_DEFINITIONS ABC_NO_HISTORY)
        # ABC's main(), plus the commit line of the pin query, in adapters/abc/child_main.c;
        # created here so it keeps ABC's GNU C settings, not C17 (finding 19).
        add_executable(odin3_abc_exe adapters/abc/child_main.c)
        target_link_libraries(odin3_abc_exe PRIVATE libabc-pic)
        set_target_properties(odin3_abc_exe PROPERTIES
            OUTPUT_NAME abc
            LINKER_LANGUAGE CXX
            RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/third_party/abc)
        # ABC's own (never built) `abc` would claim the same path: move it aside.
        set_target_properties(abc PROPERTIES OUTPUT_NAME abc-upstream)
    endfunction()
    odin3_add_abc()
    add_custom_target(abc_tool ALL DEPENDS odin3_abc_exe)
    target_compile_definitions(odin3 PRIVATE ODIN3_HAVE_ABC)

    # --- libodin3_abc.so: the linked-ABC adapter (3D spec §4.1-§4.2, AB-5/AB-6) -------------
    # C17 with odin3_flags (and the sanitisers, which never reach ABC's objects). libabc-pic is
    # linked as $<LINK_ONLY:...>: its objects and libraries, but none of ABC's compile flags or
    # include directories, which only bridge.c gets (below).
    set(ODIN3_ABC_ADAPTER_SOURCES
        adapters/abc/adapter.c adapters/abc/bridge.c adapters/abc/capture.c
        adapters/abc/guard.c adapters/abc/session.c)
    # bridge.c alone includes ABC's headers: as SYSTEM headers, with ABC's own -D flags (they fix
    # the sizes of the types those headers declare), and without -Wstrict-prototypes (ABC
    # declares Abc_FrameCreate() and others without a prototype).
    get_target_property(_abc_iface_opts libabc-pic INTERFACE_COMPILE_OPTIONS)
    set(_abc_defs "")
    foreach(_opt IN LISTS _abc_iface_opts)
        if(_opt MATCHES "^-D")
            list(APPEND _abc_defs "${_opt}")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES _abc_defs)  # ABC lists its -D flags once for C and once for C++
    set_source_files_properties(adapters/abc/bridge.c PROPERTIES COMPILE_OPTIONS
        "-Wno-strict-prototypes;-isystem${PROJECT_SOURCE_DIR}/third_party/abc/src;${_abc_defs}")
    # The adapter's __FILE__ is repository-relative ("adapters/abc/guard.c"), so
    # ODIN3_ADAPTER_ASSERT's text does not depend on where the tree is checked out.
    set(_abc_adapter_copts -fmacro-prefix-map=${PROJECT_SOURCE_DIR}/=)
    set(_abc_adapter_defs ODIN3_ABC_COMMIT_STR="${ODIN3_ABC_COMMIT}")
    target_compile_definitions(odin3_abc_exe PRIVATE ${_abc_adapter_defs})
    # The core's pin check (src/passes/abc/child.c) compares the program's commit with this one.
    set_property(SOURCE src/passes/abc/child.c APPEND PROPERTY COMPILE_DEFINITIONS
                 ${_abc_adapter_defs})
    # AB-33: the subprocess path runs `odin3-abc` beside the `odin3` executable: a copy next to
    # it in the build tree, an install rule beside it in an installed tree.
    add_custom_command(TARGET odin3_abc_exe POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:odin3_abc_exe>
                ${PROJECT_BINARY_DIR}/odin3-abc
        COMMENT "odin3-abc beside odin3")
    install(PROGRAMS $<TARGET_FILE:odin3_abc_exe> DESTINATION bin RENAME odin3-abc)
    # §4.2's wraps: exit, _exit, abort and __assert_fail escape through the guard; the process
    # control and code loading calls are refused (adapters/abc/guard.c): execl (base/sn's child)
    # and dlopen (Libs_Init loads every libabc_*.so of the cwd or $ABC_LIB_PATH at frame creation;
    # Task 5 review C1). The test variant links with them too.
    set(ODIN3_ABC_WRAPS
        -Wl,--wrap=exit -Wl,--wrap=_exit -Wl,--wrap=abort -Wl,--wrap=__assert_fail
        -Wl,--wrap=system -Wl,--wrap=popen -Wl,--wrap=fork -Wl,--wrap=execv -Wl,--wrap=execvp
        -Wl,--wrap=execl -Wl,--wrap=signal -Wl,--wrap=dlopen)
    # §4.2's link options. clang links no sanitiser runtime into a shared object (the executable
    # carries it), so -z defs would refuse every __asan_/__ubsan_ reference: it is dropped for
    # sanitised clang builds only.
    set(_abc_zdefs -Wl,-z,defs)
    if(ODIN3_SANITIZE AND CMAKE_C_COMPILER_ID MATCHES "Clang")
        set(_abc_zdefs "")
    endif()
    # One adapter library with §4.1-§4.2's properties and link options: odin3_abc, and the CLI's
    # test variant (cli_abc_unavailable_warning). Extra arguments are compile definitions.
    function(odin3_add_abc_adapter name)
        add_library(${name} SHARED ${ODIN3_ABC_ADAPTER_SOURCES})
        set_target_properties(${name} PROPERTIES
            OUTPUT_NAME ${name}
            C_VISIBILITY_PRESET hidden
            LINKER_LANGUAGE CXX
            LINK_DEPENDS ${PROJECT_SOURCE_DIR}/adapters/abc/exports.map)
        target_compile_definitions(${name} PRIVATE ${_abc_adapter_defs} ${ARGN})
        target_compile_options(${name} PRIVATE ${_abc_adapter_copts})
        target_link_libraries(${name} PRIVATE libodin3 odin3_flags $<LINK_ONLY:libabc-pic>)
        target_link_options(${name} PRIVATE
            -Wl,--exclude-libs,ALL
            -Wl,--version-script=${PROJECT_SOURCE_DIR}/adapters/abc/exports.map
            ${_abc_zdefs} ${ODIN3_ABC_WRAPS})
    endfunction()
    odin3_add_abc_adapter(odin3_abc)
    # The CLI links the adapter (P3-8) and registers its engine at start-up, before any --plugin
    # (3D spec §4.3); --version names the commit (plan point (u)). libodin3 never links ABC.
    target_link_libraries(odin3 PRIVATE odin3_abc)
    target_include_directories(odin3 PRIVATE ${PROJECT_SOURCE_DIR}/adapters/abc)
    target_compile_definitions(odin3 PRIVATE ${_abc_adapter_defs})
endif()

# --- odin3-write: writer driver (used by tools/writer-check/writer-check) -------------------
add_executable(odin3-write tools/writer-check/odin3-write.c)
target_include_directories(odin3-write PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(odin3-write PRIVATE odin3_core odin3_flags)

# --- odin3-blif-rt: BLIF round-trip driver (used by tools/blif-roundtrip/blif-roundtrip) --------
add_executable(odin3-blif-rt tools/blif-roundtrip/odin3-blif-rt.c)
target_include_directories(odin3-blif-rt PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(odin3-blif-rt PRIVATE odin3_core odin3_flags)

# --- odin3-parse-check: the Verilog coverage-row driver and location oracle (2C §7.2) ----------
add_executable(odin3-parse-check tools/parse-check/odin3-parse-check.c
                                 tools/parse-check/parse_check.c)
target_include_directories(odin3-parse-check PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(odin3-parse-check PRIVATE odin3_core odin3_flags)

# --- elab-check: the elaboration coverage-row driver (2D §14.2, plan Task 25) -------------------
add_executable(elab-check tools/elab-check/elab-check.c)
target_include_directories(elab-check PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(elab-check PRIVATE odin3_core odin3_flags)

# 2D Task 28 (spec 14.5): const-replay, the tiny driver of const-diff (a constant table through
# odin3_cval_op).
add_executable(const-replay tools/elab-check/const-replay.c)
target_include_directories(const-replay PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(const-replay PRIVATE odin3_core odin3_flags)

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

# 2D Task 27 (spec 15): one file per run, `bench_elab --one FILE`, for `elab-sweep --bench`.
add_executable(bench_elab tests/bench/bench_elab.c)
target_include_directories(bench_elab PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(bench_elab PRIVATE odin3_core odin3_flags)
# tests/bench/bench_guard.h refuses a Debug or sanitized binary (review I1).
if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT ODIN3_SANITIZE)
    target_compile_definitions(bench_elab PRIVATE ODIN3_BENCH_RELEASE)
else()
    add_test(NAME bench_elab_refuses_debug COMMAND bench_elab --one /dev/null)
    set_tests_properties(bench_elab_refuses_debug PROPERTIES
        PASS_REGULAR_EXPRESSION "needs a Release build")
endif()

# 2E Task 13 (spec 19): one file per run, `bench_proc --one FILE`, for `elab-sweep --bench`.
add_executable(bench_proc tests/bench/bench_proc.c)
target_include_directories(bench_proc PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(bench_proc PRIVATE odin3_core odin3_flags)
# tests/bench/bench_guard.h (2D's, byte-identical) refuses a Debug or sanitized binary.
if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT ODIN3_SANITIZE)
    target_compile_definitions(bench_proc PRIVATE ODIN3_BENCH_RELEASE)
endif()
# 3D Task 22: bench_abc (Release; `bench_abc --one FILE [--lut K] [--via-blif DIR] [--exe ABC]`):
# one BLIF through the abc pass, the columns of spec 12. It registers the linked engine first
# (3D plan finding 20) and reads the per-run report of abc_test.h.
if(ODIN3_WITH_ABC)
    add_executable(bench_abc tests/bench/bench_abc.c)
    # wait4 (WNOHANG) and nanosleep: the executable's own peak RSS (its VmHWM, polled).
    set_source_files_properties(tests/bench/bench_abc.c PROPERTIES COMPILE_DEFINITIONS
        _DEFAULT_SOURCE)
    target_include_directories(bench_abc PRIVATE ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/tests/bench ${PROJECT_SOURCE_DIR}/adapters/abc)
    target_link_libraries(bench_abc PRIVATE odin3_core odin3_flags odin3_abc)
    add_dependencies(bench_abc odin3_abc_exe)
    # tests/bench/bench_guard.h refuses a Debug or sanitized binary.
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT ODIN3_SANITIZE)
        target_compile_definitions(bench_abc PRIVATE ODIN3_BENCH_RELEASE)
    else()
        add_test(NAME bench_abc_refuses_debug COMMAND bench_abc --one /dev/null)
        set_tests_properties(bench_abc_refuses_debug PROPERTIES
            PASS_REGULAR_EXPRESSION "needs a Release build")
    endif()
endif()
# 2F Task 15: bench_opt (Release; `bench_opt --one FILE`, the interface of elab-sweep --bench). Its
# measuring core (bench_opt.c without main) is also linked into the gate test
# test_bench_opt_columns below; bench_opt_smoke is registered with the Python tools (Task 19).
add_executable(bench_opt tests/bench/bench_opt.c)
target_include_directories(bench_opt PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/tests/bench)
target_link_libraries(bench_opt PRIVATE odin3_core odin3_flags)
# tests/bench/bench_guard.h (shared with bench_elab, 2D) refuses a Debug or sanitized binary.
if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT ODIN3_SANITIZE)
    target_compile_definitions(bench_opt PRIVATE ODIN3_BENCH_RELEASE)
else()
    add_test(NAME bench_opt_refuses_debug COMMAND bench_opt --one /dev/null)
    set_tests_properties(bench_opt_refuses_debug PROPERTIES
        PASS_REGULAR_EXPRESSION "needs a Release build")
endif()

# --- Fuzzers (ODIN3_FUZZ) ---------------------------------------------------------------
if(ODIN3_FUZZ)
    # tests/fuzz/fuzz_pp.c: the preprocessor's crash fuzzer (2B §10.5).
    add_executable(fuzz_pp tests/fuzz/fuzz_pp.c)
    target_include_directories(fuzz_pp PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(fuzz_pp PRIVATE odin3_core odin3_flags)
    target_link_options(fuzz_pp PRIVATE -fsanitize=fuzzer)
    # tests/fuzz/fuzz_verilog.c: the Verilog parser's crash fuzzer (2C §7.5).
    add_executable(fuzz_verilog tests/fuzz/fuzz_verilog.c)
    target_include_directories(fuzz_verilog PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(fuzz_verilog PRIVATE odin3_core odin3_flags)
    target_link_options(fuzz_verilog PRIVATE -fsanitize=fuzzer)
    # tests/fuzz/fuzz_abc_script.c: the ABC script checker's fuzzer (3D §13.7, plan Task 11).
    add_executable(fuzz_abc_script tests/fuzz/fuzz_abc_script.c)
    target_include_directories(fuzz_abc_script PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(fuzz_abc_script PRIVATE odin3_core odin3_flags)
    target_link_options(fuzz_abc_script PRIVATE -fsanitize=fuzzer)
    # tests/fuzz/fuzz_abc_splice.c: the abc pass's splice, on the fake engine (3D §13.7, Task 19).
    add_executable(fuzz_abc_splice tests/fuzz/fuzz_abc_splice.c tests/unit/abc_fake_engine.c)
    target_include_directories(fuzz_abc_splice PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(fuzz_abc_splice PRIVATE odin3_core odin3_flags)
    target_link_options(fuzz_abc_splice PRIVATE -fsanitize=fuzzer)
    # tests/fuzz/fuzz_opt.c: the opt family's crash fuzzer (2F §20.4, plan Task 18). It builds
    # word-cell designs through the unit tests' RTLIL reader (proc_build.c).
    add_executable(fuzz_opt tests/fuzz/fuzz_opt.c tests/unit/proc_build.c)
    target_include_directories(fuzz_opt PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(fuzz_opt PRIVATE odin3_core odin3_flags)
    target_link_options(fuzz_opt PRIVATE -fsanitize=fuzzer)
    # tests/fuzz/fuzz_lower.c: lower's crash fuzzer (3B spec 14.5, Task 9).
    add_executable(fuzz_lower tests/fuzz/fuzz_lower.c tests/unit/lower_fixture.c)
    target_include_directories(fuzz_lower PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(fuzz_lower PRIVATE odin3_core unity odin3_flags)
    target_link_options(fuzz_lower PRIVATE -fsanitize=fuzzer)
    # tests/fuzz/fuzz_elab.c: the elaborator's crash fuzzer (2D §14.5, plan Task 28).
    add_executable(fuzz_elab tests/fuzz/fuzz_elab.c)
    target_include_directories(fuzz_elab PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(fuzz_elab PRIVATE odin3_core odin3_flags)
    target_link_options(fuzz_elab PRIVATE -fsanitize=fuzzer)
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
    odin3_add_unit_test(test_json_parse)
    odin3_add_unit_test(test_util_log)
    odin3_add_unit_test(test_util_file)
    odin3_add_unit_test(test_sha256)
    odin3_add_unit_test(test_ir_value)
    odin3_add_unit_test(test_ir_celltype)
    odin3_add_unit_test(test_ir_netlist)
    odin3_add_unit_test(test_ir_cells)
    # 2E plan point (o): proc-diff's INT-parameter list must equal the registry's.
    target_compile_definitions(test_ir_cells PRIVATE
        ODIN3_INT_PARAMS_FILE="${PROJECT_SOURCE_DIR}/tools/proc-check/int_params.txt")
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
    odin3_add_unit_test(test_proc_prune)
    target_sources(test_proc_prune PRIVATE tests/unit/proc_build.c)
    odin3_add_unit_test(test_proc_arst)
    target_sources(test_proc_arst PRIVATE tests/unit/proc_build.c)
    odin3_add_unit_test(test_proc_mux)
    target_sources(test_proc_mux PRIVATE tests/unit/proc_build.c)
    odin3_add_unit_test(test_proc_latch)
    target_sources(test_proc_latch PRIVATE tests/unit/proc_build.c)
    odin3_add_unit_test(test_proc_ff)
    target_sources(test_proc_ff PRIVATE tests/unit/proc_build.c)
    odin3_add_unit_test(test_budget)
    target_compile_definitions(test_budget PRIVATE
        ODIN3_BUDGET_OUT="${PROJECT_BINARY_DIR}/budget_tmp")
    odin3_add_unit_test(test_ffinit)
    # 2F Task 1: the opt context and the rewrite kit (every test_opt_* links proc_build.c).
    odin3_add_unit_test(test_opt_kit)
    target_sources(test_opt_kit PRIVATE tests/unit/proc_build.c)
    # 2F Task 2: the FF model; Tasks 11-12: opt_dff's rules (test_opt_dff).
    odin3_add_unit_test(test_opt_dff)
    target_sources(test_opt_dff PRIVATE tests/unit/proc_build.c)
    target_compile_definitions(test_opt_dff PRIVATE
        ODIN3_DFF_DATA="${PROJECT_SOURCE_DIR}/tests/unit/data/opt/dff")
    # 2F Task 3: opt_clean.
    odin3_add_unit_test(test_opt_clean)
    target_sources(test_opt_clean PRIVATE tests/unit/proc_build.c)
    # 2F Task 4: the opt pass registrations, options, the call wrapper; Task 13: the opt driver.
    odin3_add_unit_test(test_opt_loop)
    target_sources(test_opt_loop PRIVATE tests/unit/proc_build.c)
    target_compile_definitions(test_opt_loop PRIVATE
        ODIN3_LOOP_DATA="${PROJECT_SOURCE_DIR}/tests/unit/data/opt/loop")
    # 2F Task 15: bench_opt prints every column (its measuring core, built without main).
    odin3_add_unit_test(test_bench_opt_columns)
    target_sources(test_bench_opt_columns PRIVATE tests/bench/bench_opt.c tests/unit/proc_build.c)
    target_include_directories(test_bench_opt_columns PRIVATE ${PROJECT_SOURCE_DIR}/tests/bench)
    target_compile_definitions(test_bench_opt_columns PRIVATE BENCH_OPT_NO_MAIN
        ODIN3_BENCH_DATA="${PROJECT_SOURCE_DIR}/tests/unit/data/opt/bench")
    # 2F Task 8: opt_merge.
    odin3_add_unit_test(test_opt_cse)
    target_sources(test_opt_cse PRIVATE tests/unit/proc_build.c)
    target_compile_definitions(test_opt_cse PRIVATE
        ODIN3_MERGE_DATA="${PROJECT_SOURCE_DIR}/tests/unit/data/opt/merge")
    # PHASE3 #49: opt_merge -ffbits.
    odin3_add_unit_test(test_opt_ffbits)
    target_sources(test_opt_ffbits PRIVATE tests/unit/proc_build.c)
    # 2F Task 9: opt_reduce.
    odin3_add_unit_test(test_opt_reduce)
    target_sources(test_opt_reduce PRIVATE tests/unit/proc_build.c)
    target_compile_definitions(test_opt_reduce PRIVATE
        ODIN3_REDUCE_DATA="${PROJECT_SOURCE_DIR}/tests/unit/data/opt/reduce")
    # 2F Task 14: opt_share.
    odin3_add_unit_test(test_opt_share)
    target_sources(test_opt_share PRIVATE tests/unit/proc_build.c)
    target_compile_definitions(test_opt_share PRIVATE
        ODIN3_SHARE_DATA="${PROJECT_SOURCE_DIR}/tests/unit/data/opt/share")
    # 3G: wreduce.
    odin3_add_unit_test(test_wreduce)
    target_sources(test_wreduce PRIVATE tests/unit/proc_build.c)
    # WR-7: tools/opt-check/il_celltypes.txt (il_order.py's cell table) against the registry.
    odin3_add_unit_test(test_il_celltypes)
    target_compile_definitions(test_il_celltypes PRIVATE
        IL_CELLTYPES_FILE="${PROJECT_SOURCE_DIR}/tools/opt-check/il_celltypes.txt")
    # 2F Task 10: opt_muxtree.
    odin3_add_unit_test(test_opt_muxtree)
    target_sources(test_opt_muxtree PRIVATE tests/unit/proc_build.c)
    target_compile_definitions(test_opt_muxtree PRIVATE
        ODIN3_MUXTREE_DATA="${PROJECT_SOURCE_DIR}/tests/unit/data/opt/muxtree")
    # 4.8 Task 3: the fsm pass's table and net index.
    odin3_add_unit_test(test_fsm_data)
    target_sources(test_fsm_data PRIVATE tests/unit/proc_build.c)
    odin3_add_unit_test(test_fsm_detect)
    target_sources(test_fsm_detect PRIVATE tests/unit/proc_build.c)
    target_compile_definitions(test_fsm_detect PRIVATE
        ODIN3_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    odin3_add_unit_test(test_fsm_extract)
    target_sources(test_fsm_extract PRIVATE tests/unit/proc_build.c)
    target_compile_definitions(test_fsm_extract PRIVATE
        ODIN3_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    odin3_add_unit_test(test_fsm_opt)
    target_compile_definitions(test_fsm_opt PRIVATE
        ODIN3_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    odin3_add_unit_test(test_fsm_recode)
    target_sources(test_fsm_recode PRIVATE tests/unit/proc_build.c)
    odin3_add_unit_test(test_fsm_map)
    target_sources(test_fsm_map PRIVATE tests/unit/proc_build.c)
    target_compile_definitions(test_fsm_map PRIVATE
        ODIN3_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    odin3_add_unit_test(test_fsm_driver)
    target_sources(test_fsm_driver PRIVATE tests/unit/proc_build.c)
    target_compile_definitions(test_fsm_driver PRIVATE
        ODIN3_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    odin3_add_unit_test(test_fsm_ceval)
    target_sources(test_fsm_ceval PRIVATE tests/unit/proc_build.c)
    target_compile_definitions(test_fsm_ceval PRIVATE
        ODIN3_OPT_DATA="${PROJECT_SOURCE_DIR}/tests/opt")
    # 2F Task 5: folding on 2D's evaluator, replayed against the Yosys 0.55 fold table.
    odin3_add_unit_test(test_opt_fold)
    target_sources(test_opt_fold PRIVATE tests/unit/proc_build.c)
    target_compile_definitions(test_opt_fold PRIVATE
        ODIN3_OPT_DATA="${PROJECT_SOURCE_DIR}/tests/opt")
    # 2F Task 6: opt_expr (the driver, the rules of spec 4.2-4.3, replace_undriven).
    odin3_add_unit_test(test_opt_expr)
    target_sources(test_opt_expr PRIVATE tests/unit/proc_build.c)
    # 3C Task 2: hardmap's $mul rule on hand-built flat IR (lib/vtr.o3lib read by pb_build_lib).
    odin3_add_unit_test(test_hardmap_mul)
    target_sources(test_hardmap_mul PRIVATE tests/unit/hardmap_fixture.c tests/unit/proc_build.c)
    target_compile_definitions(test_hardmap_mul PRIVATE
        ODIN3_LIB_DIR="${PROJECT_SOURCE_DIR}/lib"
        ODIN3_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    add_test(NAME cli_blif_vtr_prov_names
        COMMAND ${PROJECT_SOURCE_DIR}/tests/cli/test_cli.sh blif_vtr_prov_names $<TARGET_FILE:odin3>
                ${PROJECT_SOURCE_DIR}/tools/netlist-compare/netlist-compare
                ${PROJECT_SOURCE_DIR}/tests/golden/blif)
    set_tests_properties(cli_blif_vtr_prov_names PROPERTIES LABELS unit)
    add_test(NAME cli_hardmap_mul
        COMMAND ${PROJECT_SOURCE_DIR}/tests/cli/test_cli.sh hardmap_mul $<TARGET_FILE:odin3>
                ${PROJECT_SOURCE_DIR}/tools/netlist-compare/netlist-compare
                ${PROJECT_SOURCE_DIR}/tests/golden/blif)
    set_tests_properties(cli_hardmap_mul PROPERTIES LABELS unit)
    add_test(NAME cli_hardmap_add
        COMMAND ${PROJECT_SOURCE_DIR}/tests/cli/test_cli.sh hardmap_add $<TARGET_FILE:odin3>
                ${PROJECT_SOURCE_DIR}/tools/netlist-compare/netlist-compare
                ${PROJECT_SOURCE_DIR}/tests/golden/blif)
    set_tests_properties(cli_hardmap_add PROPERTIES LABELS unit)
    add_test(NAME cli_pm_alias
        COMMAND ${PROJECT_SOURCE_DIR}/tests/cli/test_cli.sh pm_alias $<TARGET_FILE:odin3>
                ${PROJECT_SOURCE_DIR}/tools/netlist-compare/netlist-compare
                ${PROJECT_SOURCE_DIR}/tests/golden/blif)
    set_tests_properties(cli_pm_alias PROPERTIES LABELS unit)
    add_test(NAME cli_memory_merge
        COMMAND ${PROJECT_SOURCE_DIR}/tests/cli/test_cli.sh memory_merge $<TARGET_FILE:odin3>
                ${PROJECT_SOURCE_DIR}/tools/netlist-compare/netlist-compare
                ${PROJECT_SOURCE_DIR}/tests/golden/blif)
    set_tests_properties(cli_memory_merge PROPERTIES LABELS unit)
    add_test(NAME cli_memory_merge_soft
        COMMAND ${PROJECT_SOURCE_DIR}/tests/cli/test_cli.sh memory_merge_soft $<TARGET_FILE:odin3>
                ${PROJECT_SOURCE_DIR}/tools/netlist-compare/netlist-compare
                ${PROJECT_SOURCE_DIR}/tests/golden/blif)
    set_tests_properties(cli_memory_merge_soft PROPERTIES LABELS unit)
    add_test(NAME cli_pm_ramstyle
        COMMAND ${PROJECT_SOURCE_DIR}/tests/cli/test_cli.sh pm_ramstyle $<TARGET_FILE:odin3>
                ${PROJECT_SOURCE_DIR}/tools/netlist-compare/netlist-compare
                ${PROJECT_SOURCE_DIR}/tests/golden/blif)
    set_tests_properties(cli_pm_ramstyle PROPERTIES LABELS unit)
    # 4.2 Task 3: pm_rules (k6 values, absent models, no 3C number outside rules.c).
    odin3_add_unit_test(test_pm_rules)
    target_sources(test_pm_rules PRIVATE tests/unit/hardmap_fixture.c tests/unit/proc_build.c)
    target_compile_definitions(test_pm_rules PRIVATE
        ODIN3_ARCH_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/unit/data/arch"
        ODIN3_LIB_DIR="${PROJECT_SOURCE_DIR}/lib"
        ODIN3_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    # 4.2 Task 7: generic hard blocks (B-1..B-7): the bind check and report of partial_map.
    odin3_add_unit_test(test_pm_generic)
    target_compile_definitions(test_pm_generic PRIVATE
        ODIN3_LIB_DIR="${PROJECT_SOURCE_DIR}/lib"
        ODIN3_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    # 4.2 Task 8: every registered generic model of the real archs binds and maps (skips 77
    # without VTR_ROOT).
    odin3_add_unit_test(test_pm_generic_vtr)
    set_tests_properties(test_pm_generic_vtr PROPERTIES SKIP_RETURN_CODE 77)
    # 3C Task 3: hardmap's $add/$sub rule (adder chains) on hand-built flat IR.
    odin3_add_unit_test(test_hardmap_adder)
    target_sources(test_hardmap_adder PRIVATE tests/unit/hardmap_fixture.c tests/unit/proc_build.c)
    target_compile_definitions(test_hardmap_adder PRIVATE
        ODIN3_LIB_DIR="${PROJECT_SOURCE_DIR}/lib"
        ODIN3_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    # 3C Task 5: hardmap's explicit RAM instances (depth and width split) on hand-built flat IR.
    odin3_add_unit_test(test_hardmap_ram)
    target_sources(test_hardmap_ram PRIVATE tests/unit/hardmap_fixture.c tests/unit/proc_build.c
        tests/unit/hardmap_ramsim.c)
    target_compile_definitions(test_hardmap_ram PRIVATE
        ODIN3_LIB_DIR="${PROJECT_SOURCE_DIR}/lib"
        ODIN3_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    # 3C Task 6: hardmap's inferred `$mem` (FF merge, port patterns) on hand-built flat IR.
    odin3_add_unit_test(test_hardmap_mem)
    target_sources(test_hardmap_mem PRIVATE tests/unit/hardmap_fixture.c tests/unit/proc_build.c
        tests/unit/hardmap_ramsim.c)
    target_compile_definitions(test_hardmap_mem PRIVATE
        ODIN3_LIB_DIR="${PROJECT_SOURCE_DIR}/lib"
        ODIN3_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    # 4.7 (plan Task 5): the `memory_merge` pass on hand-built flat IR.
    odin3_add_unit_test(test_memory_merge)
    target_sources(test_memory_merge PRIVATE tests/unit/hardmap_fixture.c tests/unit/proc_build.c
        tests/unit/hardmap_ramsim.c)
    target_compile_definitions(test_memory_merge PRIVATE
        ODIN3_LIB_DIR="${PROJECT_SOURCE_DIR}/lib"
        ODIN3_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    # 4.7 (plan Task 6): `ram_style` and its kin are ignored with a warning (G-3).
    odin3_add_unit_test(test_pm_ramstyle)
    target_sources(test_pm_ramstyle PRIVATE tests/unit/hardmap_fixture.c tests/unit/proc_build.c)
    target_compile_definitions(test_pm_ramstyle PRIVATE
        ODIN3_LIB_DIR="${PROJECT_SOURCE_DIR}/lib"
        ODIN3_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    # 3C Task 4 (spec 9.3): the arithmetic and memory micros through the Phase 3 script up to
    # `lower`, against the Parmys goldens (tools/hardmap-check/hardmap-diff). Exit 77 (skipped)
    # when the golden tree is absent (CI). Its Python tests run in python_tools.
    file(STRINGS ${PROJECT_SOURCE_DIR}/tests/hardmap/micros.txt _hm_micro_names REGEX "^[^#]")
    set(_hm_micros "")
    foreach(_hm_micro IN LISTS _hm_micro_names)
        list(APPEND _hm_micros ${PROJECT_SOURCE_DIR}/tests/micro/verilog/${_hm_micro})
    endforeach()
    add_test(NAME hardmap_diff_micro
        COMMAND ${PROJECT_SOURCE_DIR}/tools/hardmap-check/hardmap-diff
                --odin3 $<TARGET_FILE:odin3>
                --expect ${PROJECT_SOURCE_DIR}/tests/hardmap/diff_expect.txt ${_hm_micros})
    set_tests_properties(hardmap_diff_micro PROPERTIES LABELS unit SKIP_RETURN_CODE 77
                         TIMEOUT 1800)
    # 4.2 Tasks 3 and 4 (spec T-2): the k6 identity gate. Tier 1 (CI): `hardmap` against
    # `partial_map`, same library. This is a wiring/alias test, not a regression test: both names
    # share one entry point in one binary, so it cannot see a behaviour change. Tier 2 (needs
    # VTR_ROOT, else exit 77): the tech library against `read_arch` of the k6 XML. Tier 3 (two
    # binaries, main against the head; heavy; run by the controller) is the merge gate.
    add_test(NAME partial_map_identity
        COMMAND ${PROJECT_SOURCE_DIR}/tools/partial-map-check/identity --tier 1
                --odin3 $<TARGET_FILE:odin3> --min-ok 25)
    set_tests_properties(partial_map_identity PROPERTIES LABELS unit TIMEOUT 1800)
    add_test(NAME partial_map_identity_arch
        COMMAND ${PROJECT_SOURCE_DIR}/tools/partial-map-check/identity --tier 2
                --odin3 $<TARGET_FILE:odin3> --min-ok 25)
    set_tests_properties(partial_map_identity_arch PROPERTIES LABELS unit SKIP_RETURN_CODE 77
                         TIMEOUT 1800)
    # 3A Task 2: flatten's pre-flight graph; every test_flatten_* links the fixture helper.
    odin3_add_unit_test(test_flatten_graph)
    target_sources(test_flatten_graph PRIVATE tests/unit/flatten_fixture.c tests/unit/proc_build.c)
    target_compile_definitions(test_flatten_graph PRIVATE
        ODIN3_BLIF_DIR="${PROJECT_SOURCE_DIR}/tests/golden/blif")
    # 3A Task 3: the expansion.
    odin3_add_unit_test(test_flatten_expand)
    target_sources(test_flatten_expand PRIVATE tests/unit/flatten_fixture.c)
    target_compile_definitions(test_flatten_expand PRIVATE
        ODIN3_BLIF_DIR="${PROJECT_SOURCE_DIR}/tests/golden/blif"
        ODIN3_FLATTEN_DIR="${PROJECT_SOURCE_DIR}/tests/flatten")
    # 3A Task 4: flatten-sim (no Yosys), flatten-golden and flatten-diff (Yosys 0.55, in the block
    # below) over the fixtures of tests/flatten; exit 77 skips when an ASan child cannot be capped.
    # The sim leg leaves out mem.v (1E has no $mem) and bb.v (a black box has no semantics); the
    # golden leg compares the flat top module's write_rtlil, write_blif and write_verilog text
    # with tests/flatten/expected (spec 14.2) and checks two runs byte-identical.
    set(_flat_dir ${PROJECT_SOURCE_DIR}/tests/flatten)
    set(_flat_sim ${_flat_dir}/hier.v ${_flat_dir}/params.v ${_flat_dir}/gen.v ${_flat_dir}/feed.v
                  ${_flat_dir}/keep.v ${_flat_dir}/nested.blif)
    add_test(NAME flatten_sim_fixtures
        COMMAND ${PROJECT_SOURCE_DIR}/tools/flatten-check/flatten-sim
                --odin3 $<TARGET_FILE:odin3> ${_flat_sim})
    add_test(NAME flatten_sim_roundtrip
        COMMAND ${PROJECT_SOURCE_DIR}/tools/flatten-check/flatten-sim --roundtrip
                --odin3 $<TARGET_FILE:odin3> ${_flat_sim})
    add_test(NAME flatten_golden
        COMMAND ${PROJECT_SOURCE_DIR}/tools/flatten-check/flatten-golden
                --odin3 $<TARGET_FILE:odin3>
                tests/flatten/hier.v tests/flatten/params.v tests/flatten/gen.v
                tests/flatten/feed.v tests/flatten/keep.v tests/flatten/bb.v tests/flatten/mem.v
                tests/flatten/nested.blif)
    set_tests_properties(flatten_sim_fixtures flatten_sim_roundtrip flatten_golden PROPERTIES
                         LABELS flatten SKIP_RETURN_CODE 77 TIMEOUT 600)
    # cli_flatten: the CLI runs read_blif; flatten; check on a hierarchical BLIF (hand_body: sub is
    # expanded, bb is a black box and stays).
    add_test(NAME cli_flatten
        COMMAND $<TARGET_FILE:odin3> -p
                "read_blif ${PROJECT_SOURCE_DIR}/tests/golden/blif/hand_body.blif; flatten; check")
    set_tests_properties(cli_flatten PROPERTIES LABELS flatten
        PASS_REGULAR_EXPRESSION "flatten: 1 instances expanded \\(0 kept\\) in 1 roots")
    # cli_flatten_multidriver: two instances of sub drive z. No legal read can reach flatten with
    # that net (read_blif refuses it, PHASE3 #36; so does read_verilog), so the CLI test pins the
    # located read error, in every build type. What `flatten` itself does with a net it joins to a
    # second driver is covered by test_flatten_expand (test_flat_multidriver_*, hand IR; and
    # test_flat_multidriver_from_blif, two legal reads plus a second instance added by hand).
    add_test(NAME cli_flatten_multidriver
        COMMAND $<TARGET_FILE:odin3> -p
                "read_blif ${PROJECT_SOURCE_DIR}/tests/flatten/multidriver.blif; flatten")
    set_tests_properties(cli_flatten_multidriver PROPERTIES LABELS flatten
        PASS_REGULAR_EXPRESSION
        "multidriver\\.blif:8: .*net 'z' is driven by 2 cell outputs \\(one allowed\\)")
    # 3A Task 5 (spec 14.5): 2,000 random hierarchies of `verilog-gen --flatten` through
    # flatten + check (Debug runs check around the pass) and sim before = after (a design the
    # simulator cannot run is counted skipped, never a pass). A failing case is written to
    # ${PROJECT_BINARY_DIR}/fuzz_flatten as a reproducer.
    # Labelled `flatten`, not `fuzz`: the CI fuzz-smoke build has no odin3 binary to drive.
    add_test(NAME fuzz_flatten_quick
        COMMAND ${PROJECT_SOURCE_DIR}/tools/flatten-check/fuzz_flatten --cases 2000 --seed 1
                --odin3 $<TARGET_FILE:odin3> --repro-dir ${PROJECT_BINARY_DIR}/fuzz_flatten)
    set_tests_properties(fuzz_flatten_quick PROPERTIES LABELS flatten SKIP_RETURN_CODE 77
                         TIMEOUT 3600)
    # hand_ports has no instance: flatten expands nothing and leaves the design as read.
    add_test(NAME cli_flatten_hand_ports
        COMMAND $<TARGET_FILE:odin3> -p
                "read_blif ${PROJECT_SOURCE_DIR}/tests/golden/blif/hand_ports.blif; flatten; check")
    set_tests_properties(cli_flatten_hand_ports PROPERTIES LABELS flatten
        PASS_REGULAR_EXPRESSION "flatten: 0 instances expanded \\(0 kept\\) in 1 roots")
    # nested.blif: three levels, 7 instances (two of them unnamed), all expanded.
    add_test(NAME cli_flatten_nested
        COMMAND $<TARGET_FILE:odin3> -p
                "read_blif ${PROJECT_SOURCE_DIR}/tests/flatten/nested.blif; flatten; check")
    set_tests_properties(cli_flatten_nested PROPERTIES LABELS flatten
        PASS_REGULAR_EXPRESSION "flatten: 7 instances expanded \\(0 kept\\) in 1 roots")
    # 3B Task 1: lower's toolkit and pass; every test_lower_* links the fixture helper.
    odin3_add_unit_test(test_lower_kit)
    target_sources(test_lower_kit PRIVATE tests/unit/lower_fixture.c)
    odin3_add_unit_test(test_lower_bitwise)
    target_sources(test_lower_bitwise PRIVATE tests/unit/lower_fixture.c)
    odin3_add_unit_test(test_lower_pmux_shift)
    target_sources(test_lower_pmux_shift PRIVATE tests/unit/lower_fixture.c)
    odin3_add_unit_test(test_lower_alu)
    target_sources(test_lower_alu PRIVATE tests/unit/lower_fixture.c)
    odin3_add_unit_test(test_lower_mul)
    target_sources(test_lower_mul PRIVATE tests/unit/lower_fixture.c)
    odin3_add_unit_test(test_lower_div)
    target_sources(test_lower_div PRIVATE tests/unit/lower_fixture.c)
    odin3_add_unit_test(test_lower_ff)
    target_sources(test_lower_ff PRIVATE tests/unit/lower_fixture.c tests/unit/lower_ffref.c)
    odin3_add_unit_test(test_lower_latch)
    target_sources(test_lower_latch PRIVATE tests/unit/lower_fixture.c tests/unit/lower_latchref.c)
    odin3_add_unit_test(test_lower_mem)
    target_sources(test_lower_mem PRIVATE tests/unit/lower_fixture.c tests/unit/lower_memref.c)
    # 3B Task 9: bench_lower (Release; nodes created per second and peak RSS of `lower` on N mixed word
    # cells). Built with the unit tests' fixture helper, so only when tests are built.
    add_executable(bench_lower tests/bench/bench_lower.c tests/unit/lower_fixture.c)
    target_include_directories(bench_lower PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/tests/bench)
    target_link_libraries(bench_lower PRIVATE odin3_core unity odin3_flags)
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT ODIN3_SANITIZE)
        target_compile_definitions(bench_lower PRIVATE ODIN3_BENCH_RELEASE)
        add_test(NAME bench_lower_smoke COMMAND bench_lower --cells 5000)
        set_tests_properties(bench_lower_smoke PROPERTIES LABELS lower
            PASS_REGULAR_EXPRESSION "cells=5000 .* nodes_per_s=[0-9]+ ")
    else()
        add_test(NAME bench_lower_refuses_debug COMMAND bench_lower --cells 10)
        set_tests_properties(bench_lower_refuses_debug PROPERTIES LABELS lower
            WILL_FAIL FALSE PASS_REGULAR_EXPRESSION "needs a Release build")
    endif()
    odin3_add_unit_test(test_lower_corpus)
    target_sources(test_lower_corpus PRIVATE tests/unit/lower_fixture.c)
    # 3B Task 9 (spec 12.1): the module built in reverse order compares identical (netlist-compare).
    add_test(NAME lower_reverse_netlist_compare
        COMMAND ${PROJECT_SOURCE_DIR}/tools/netlist-compare/netlist-compare
                --implicit-models lower_reverse_a.blif lower_reverse_b.blif)
    set_tests_properties(test_lower_corpus PROPERTIES FIXTURES_SETUP lower_reverse)
    set_tests_properties(lower_reverse_netlist_compare PROPERTIES FIXTURES_REQUIRED lower_reverse
        LABELS lower)
    odin3_add_unit_test(test_lower_pass)
    target_sources(test_lower_pass PRIVATE tests/unit/lower_fixture.c tests/unit/proc_fixture.c
                   tests/unit/proc_fixture_core.c)
    target_compile_definitions(test_lower_pass PRIVATE
        ODIN3_BLIF_DIR="${PROJECT_SOURCE_DIR}/tests/golden/blif")
    set_tests_properties(test_lower_kit test_lower_bitwise test_lower_pmux_shift test_lower_alu test_lower_mul test_lower_div test_lower_ff test_lower_mem test_lower_corpus test_lower_pass PROPERTIES LABELS "unit;lower")
    # 3B Task 2/3: lower-sim on the hand-built designs (the 1E simulator before = after; a design
    # with a $shiftx is reported skipped), the per-cell goldens (lower-cells) and the CLI line.
    # Labelled `lower`, never `fuzz`: the CI fuzz-smoke build has no odin3 binary.
    add_test(NAME lower_sim_hand
        COMMAND ${PROJECT_SOURCE_DIR}/tools/lower-check/lower-sim --odin3 $<TARGET_FILE:odin3>
                --each ${PROJECT_SOURCE_DIR}/tests/lower/hand/and_mux.v
                ${PROJECT_SOURCE_DIR}/tests/lower/hand/shiftx.v)
    set_tests_properties(lower_sim_hand PROPERTIES LABELS lower SKIP_RETURN_CODE 77 TIMEOUT 600
        PASS_REGULAR_EXPRESSION "and_mux.v: .and: pass.*shiftx.v: .shiftx: skipped")
    add_test(NAME lower_cells
        COMMAND ${PROJECT_SOURCE_DIR}/tools/lower-check/lower-cells --odin3 $<TARGET_FILE:odin3>
                ${PROJECT_SOURCE_DIR}/tests/lower/cells)
    set_tests_properties(lower_cells PROPERTIES LABELS lower SKIP_RETURN_CODE 77 TIMEOUT 1800)
    # 3B Task 5 (spec 5.7): a $pow that survives to lower is the located error, nothing built.
    add_test(NAME cli_lower_pow
        COMMAND $<TARGET_FILE:odin3> -p
                "read_verilog --nolatches ${PROJECT_SOURCE_DIR}/tests/lower/hand/pow.v; hierarchy; proc; lower")
    set_tests_properties(cli_lower_pow PROPERTIES LABELS lower
        PASS_REGULAR_EXPRESSION "lower: module 'm': '[$]pow' '[^']*' has no bit-level form \\(run opt first; Yosys leaves it unmapped\\)")
    add_test(NAME cli_lower_hand
        COMMAND $<TARGET_FILE:odin3> -p
                "read_verilog --nolatches ${PROJECT_SOURCE_DIR}/tests/lower/hand/and_mux.v; hierarchy; proc; opt; lower; check; stats")
    set_tests_properties(cli_lower_hand PROPERTIES LABELS lower
        PASS_REGULAR_EXPRESSION "lower: module 'm': lowered [0-9]+ word cells to [0-9]+ bit cells")
    odin3_add_unit_test(test_abc_alias_table)
    target_compile_definitions(test_abc_alias_table PRIVATE
        ODIN3_ABC_RC="${PROJECT_SOURCE_DIR}/third_party/abc/abc.rc")
    # 3D Task 11: the script checker (its accepted cases are shared with 3D-2's adapter test,
    # point (p)) and the embedded default script, compared with lib/abc/vtr.abc.
    odin3_add_unit_test(test_abc_script)
    target_sources(test_abc_script PRIVATE tests/unit/abc_script_cases.c)
    odin3_add_unit_test(test_abc_default_script)
    target_compile_definitions(test_abc_default_script PRIVATE
        ODIN3_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    # 3D Task 9a: the child runner (AB-33), on shell fixtures that stand in for odin3-abc.
    odin3_add_unit_test(test_abc_child)
    target_compile_definitions(test_abc_child PRIVATE
        ODIN3_CHILD_DATA="${PROJECT_SOURCE_DIR}/tests/unit/data/abc/child")
    # 3D Task 9b: the route, the program lookup, the pin check and the six texts. With ABC the
    # build's own odin3-abc must exist beside the test (it is copied there after odin3_abc_exe).
    odin3_add_unit_test(test_abc_route)
    target_compile_definitions(test_abc_route PRIVATE
        ODIN3_CHILD_DATA="${PROJECT_SOURCE_DIR}/tests/unit/data/abc/child")
    if(ODIN3_WITH_ABC)
        target_compile_definitions(test_abc_route PRIVATE ODIN3_HAVE_ABC_EXE)
        add_dependencies(test_abc_route odin3_abc_exe)
    endif()
    # 3D Tasks 14 to 16: the abc pass up to the exchange, on the fake engine (tests/unit/
    # abc_fake_engine.c, which never links ABC). Each test writes its files under abc_tests/<name>.
    file(MAKE_DIRECTORY ${PROJECT_BINARY_DIR}/abc_tests)
    foreach(_abc_test test_abc_args test_abc_region test_abc_result test_abc_xblif
                      test_abc_exchange test_abc_splice test_abc_prov)
        odin3_add_unit_test(${_abc_test})
        target_compile_definitions(${_abc_test} PRIVATE
            ODIN3_ABC_TEST_DIR="${PROJECT_BINARY_DIR}/abc_tests/${_abc_test}"
            ODIN3_LIB_DIR="${PROJECT_SOURCE_DIR}/lib")
        if(NOT _abc_test STREQUAL "test_abc_result")
            target_sources(${_abc_test} PRIVATE tests/unit/abc_fake_engine.c)
        endif()
    endforeach()
    target_sources(test_abc_args PRIVATE tests/unit/proc_fixture.c tests/unit/proc_fixture_core.c)
    # 3D Task 22: the per-run report (phase times, counts) bench_abc reads, on the fake engine.
    odin3_add_unit_test(test_abc_report)
    target_compile_definitions(test_abc_report PRIVATE
        ODIN3_ABC_TEST_DIR="${PROJECT_BINARY_DIR}/abc_tests/test_abc_report"
        ODIN3_LIB_DIR="${PROJECT_SOURCE_DIR}/lib"
        ODIN3_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    target_sources(test_abc_report PRIVATE tests/unit/abc_fake_engine.c)
    # 3D Task 20: the AIG builder and the in-memory protocol, on the fake engine.
    odin3_add_unit_test(test_abc_aig)
    target_compile_definitions(test_abc_aig PRIVATE
        ODIN3_ABC_TEST_DIR="${PROJECT_BINARY_DIR}/abc_tests/test_abc_aig"
        ODIN3_LIB_DIR="${PROJECT_SOURCE_DIR}/lib"
        ODIN3_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    target_sources(test_abc_aig PRIVATE tests/unit/abc_fake_engine.c)
    # 3D Task 19: the splice's end-to-end checks. test_abc_unconn and test_abc_pass run the fake
    # engine in process; with ABC, test_abc_pass and test_abc_init1 also drive the odin3 binary
    # (real ABC) and tools/equiv-check, whose ABC is the build's own.
    foreach(_abc_test test_abc_unconn test_abc_init1 test_abc_pass)
        odin3_add_unit_test(${_abc_test})
        target_compile_definitions(${_abc_test} PRIVATE
            ODIN3_ABC_TEST_DIR="${PROJECT_BINARY_DIR}/abc_tests/${_abc_test}"
            ODIN3_LIB_DIR="${PROJECT_SOURCE_DIR}/lib"
            ODIN3_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
        if(NOT _abc_test STREQUAL "test_abc_init1")
            target_sources(${_abc_test} PRIVATE tests/unit/abc_fake_engine.c)
        endif()
        if(ODIN3_WITH_ABC AND _abc_test STREQUAL "test_abc_unconn")
            target_compile_definitions(${_abc_test} PRIVATE
                ODIN3_ABC_EXE_PATH="$<TARGET_FILE:odin3_abc_exe>")
            add_dependencies(${_abc_test} odin3_abc_exe)
        endif()
        if(ODIN3_WITH_ABC AND NOT _abc_test STREQUAL "test_abc_unconn")
            target_compile_definitions(${_abc_test} PRIVATE ODIN3_HAVE_REAL_ABC
                ODIN3_BIN="$<TARGET_FILE:odin3>"
                ODIN3_EQUIV_CHECK="${PROJECT_SOURCE_DIR}/tools/equiv-check/equiv-check")
            add_dependencies(${_abc_test} odin3 odin3_abc_exe)
            set_tests_properties(${_abc_test} PROPERTIES
                ENVIRONMENT "ODIN3_ABC=$<TARGET_FILE:odin3_abc_exe>;ODIN3_REQUIRE_ABC=1")
        endif()
        set_tests_properties(${_abc_test} PROPERTIES TIMEOUT 1800)
    endforeach()
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
    # 2C Task 8: the read_verilog pass on temporary and committed trees, and the lexer cases
    # that need the preprocessor (macros, includes).
    odin3_add_unit_test(test_read_verilog)
    target_compile_definitions(test_read_verilog PRIVATE
        ODIN3_RV_DATA="${PROJECT_SOURCE_DIR}/tests/unit/data/verilog/read"
        ODIN3_PROJECT_FIXTURES="${PROJECT_SOURCE_DIR}/tests/golden/projects")
    odin3_add_pp_test(test_verilog_lexer_pp)
    target_include_directories(test_verilog_lexer_pp PRIVATE ${_gen})
    add_dependencies(test_verilog_lexer_pp verilog_generated)
    # 2C Task 9: the location oracle of odin3-parse-check on seeded faults.
    odin3_add_unit_test(test_parse_check)
    target_sources(test_parse_check PRIVATE tools/parse-check/parse_check.c)
    target_include_directories(test_parse_check PRIVATE ${PROJECT_SOURCE_DIR}/tools/parse-check)
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
    # 3E-5: odin3_blif_isop, the irredundant cover write_blif writes a $lut as.
    odin3_add_unit_test(test_blif_isop)
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
    odin3_add_unit_test(test_techlib_string_param)
    foreach(_t IN ITEMS test_arch_record test_arch_models test_arch_pb test_arch_lib test_arch_pass test_arch_vtr test_arch_access)
        odin3_add_unit_test(${_t})
        target_compile_definitions(${_t} PRIVATE
            ODIN3_ARCH_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/unit/data/arch"
            ODIN3_LIB_DIR="${PROJECT_SOURCE_DIR}/lib")
    endforeach()
    add_test(NAME cli_arch
        COMMAND ${PROJECT_SOURCE_DIR}/tests/cli/arch_cli.sh $<TARGET_FILE:odin3>
                ${PROJECT_SOURCE_DIR}/tests/unit/data/arch/pb_ok.xml)
    set_tests_properties(cli_arch PROPERTIES LABELS unit)
    set_tests_properties(test_arch_vtr PROPERTIES SKIP_RETURN_CODE 77)
    add_test(NAME arch_corpus
        COMMAND ${PROJECT_SOURCE_DIR}/tests/arch/corpus.sh $<TARGET_FILE:odin3>)
    set_tests_properties(arch_corpus PROPERTIES LABELS unit SKIP_RETURN_CODE 77 TIMEOUT 1200)
    # 5.1 Task 3: lib/lpm.o3lib and LPM-structured Verilog read back as registered cells.
    odin3_add_unit_test(test_lpm_lib)
    # BLIF `.param` on a tech-library `.subckt` (1C amendment 2026-10-19, PHASE5 decision 8).
    odin3_add_unit_test(test_blif_lib_params)
    target_sources(test_blif_lib_params PRIVATE tests/unit/lpmmap_fixture.c tests/unit/proc_build.c)
    target_compile_definitions(test_blif_lib_params PRIVATE
        ODIN3_LIB_DIR="${PROJECT_SOURCE_DIR}/lib")
    odin3_add_unit_test(test_lpmmap_scaffold)
    # 5.1 Task 9 (spec T-1..T-4): one micro per LPM cell through the LPM script, the Verilog round
    # trip and the lint (tools/lpm-check). Exit 77 (skipped) when iverilog is missing.
    foreach(_lpm_micro IN ITEMS lpm_add_sub lpm_mult lpm_compare lpm_mixed altsyncram_sp altsyncram_sdp
                                 altsyncram_soft)
        add_test(NAME lpm_check_${_lpm_micro}
            COMMAND ${PROJECT_SOURCE_DIR}/tools/lpm-check/lpm-check
                    --odin3 $<TARGET_FILE:odin3>
                    ${PROJECT_SOURCE_DIR}/tests/lpm/${_lpm_micro}.v)
        set_tests_properties(lpm_check_${_lpm_micro} PROPERTIES LABELS "unit;micro"
                             SKIP_RETURN_CODE 77 TIMEOUT 300)
    endforeach()
    add_test(NAME lpm_check_family
        COMMAND ${PROJECT_SOURCE_DIR}/tools/lpm-check/lpm-check
                --odin3 $<TARGET_FILE:odin3> --family "Stratix IV"
                ${PROJECT_SOURCE_DIR}/tests/lpm/altsyncram_sp.v)
    set_tests_properties(lpm_check_family PROPERTIES LABELS "unit;micro" SKIP_RETURN_CODE 77
                         TIMEOUT 300)
    odin3_add_unit_test(test_lpmmap_ram)
    target_sources(test_lpmmap_ram PRIVATE tests/unit/lpmmap_fixture.c tests/unit/proc_build.c)
    target_compile_definitions(test_lpmmap_ram PRIVATE
        ODIN3_LIB_DIR="${PROJECT_SOURCE_DIR}/lib")
    odin3_add_unit_test(test_lpmmap_compare)
    target_sources(test_lpmmap_compare PRIVATE tests/unit/lpmmap_fixture.c tests/unit/proc_build.c)
    target_compile_definitions(test_lpmmap_compare PRIVATE
        ODIN3_LIB_DIR="${PROJECT_SOURCE_DIR}/lib")
    odin3_add_unit_test(test_lpmmap_mult)
    target_sources(test_lpmmap_mult PRIVATE tests/unit/lpmmap_fixture.c tests/unit/proc_build.c)
    target_compile_definitions(test_lpmmap_mult PRIVATE
        ODIN3_LIB_DIR="${PROJECT_SOURCE_DIR}/lib")
    odin3_add_unit_test(test_lpmmap_addsub)
    target_sources(test_lpmmap_addsub PRIVATE tests/unit/lpmmap_fixture.c tests/unit/proc_build.c)
    target_compile_definitions(test_lpmmap_addsub PRIVATE
        ODIN3_LIB_DIR="${PROJECT_SOURCE_DIR}/lib")
    target_sources(test_lpmmap_scaffold PRIVATE tests/unit/lpmmap_fixture.c tests/unit/proc_build.c)
    target_compile_definitions(test_lpmmap_scaffold PRIVATE
        ODIN3_LIB_DIR="${PROJECT_SOURCE_DIR}/lib")
    target_sources(test_lpm_lib PRIVATE tests/unit/lpmmap_fixture.c tests/unit/proc_build.c)
    target_compile_definitions(test_lpm_lib PRIVATE
        ODIN3_LIB_DIR="${PROJECT_SOURCE_DIR}/lib"
        ODIN3_LPM_DIR="${PROJECT_SOURCE_DIR}/tests/lpm")
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
    # 3D Task 9: write_verilog's $lut form. test_lut_verilog writes lut/luts.v and luts_tb.v (16
    # random tables per width 1..6); CTest lut_verilog_icarus simulates them when Icarus is found.
    odin3_add_unit_test(test_lut_verilog)
    target_compile_definitions(test_lut_verilog PRIVATE ODIN3_LUT_OUT="${PROJECT_BINARY_DIR}/lut")
    set_tests_properties(test_lut_verilog PROPERTIES FIXTURES_SETUP lut_verilog_fixtures)
    find_program(ODIN3_IVERILOG iverilog)
    find_program(ODIN3_VVP vvp)
    if(ODIN3_IVERILOG AND ODIN3_VVP)
        add_test(NAME lut_verilog_icarus
            COMMAND sh -c "\"$0\" -g2005 -o \"$2/luts.vvp\" \"$2/luts.v\" \"$2/luts_tb.v\" && \"$1\" \"$2/luts.vvp\" | grep -qx PASS"
                    ${ODIN3_IVERILOG} ${ODIN3_VVP} ${PROJECT_BINARY_DIR}/lut)
        set_tests_properties(lut_verilog_icarus PROPERTIES
            LABELS differential FIXTURES_REQUIRED lut_verilog_fixtures)
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
    # 3D Task 8: the pass-run table's arguments and run information (IR-12).
    odin3_add_unit_test(test_passrun)
    # 2D Task 5: write_rtlil. Its designs are also written to rtlil/*.il, the input of the
    # Yosys 0.55 round trip (rtlil_yosys_roundtrip, in the ODIN3_YOSYS055 block below).
    file(MAKE_DIRECTORY ${PROJECT_BINARY_DIR}/rtlil)
    odin3_add_unit_test(test_rtlil_writer)
    target_sources(test_rtlil_writer PRIVATE tests/unit/ffcells_fixture.c)
    target_compile_definitions(test_rtlil_writer PRIVATE
        ODIN3_RTLIL_OUT="${PROJECT_BINARY_DIR}/rtlil")
    set_tests_properties(test_rtlil_writer PROPERTIES FIXTURES_SETUP rtlil_fixtures)
    # 2D Task 28 (review M18): odin3_cval_op against the word cells' simulate hooks.
    odin3_add_unit_test(test_elab_cellsim)
    odin3_add_unit_test(test_elab_cval)
    target_compile_definitions(test_elab_cval PRIVATE
        ODIN3_ELAB_DATA="${PROJECT_SOURCE_DIR}/tests/elab")
    odin3_add_unit_test(test_elab_ctype)
    # 2D Task 9: the evaluator and interpreter, on sources parsed by 2C (verilog_test.h).
    odin3_add_unit_test(test_elab_interp)
    target_include_directories(test_elab_interp PRIVATE ${_gen})
    add_dependencies(test_elab_interp verilog_generated)
    # 2D Task 10: stage 1's context, lookup, top, pending kinds, ownership (tests/unit/elab_test.h
    # writes each case's files into a fresh directory under elab_tmp).
    file(MAKE_DIRECTORY ${PROJECT_BINARY_DIR}/elab_tmp)
    odin3_add_unit_test(test_elab_stage1_lookup)
    target_include_directories(test_elab_stage1_lookup PRIVATE ${_gen})
    add_dependencies(test_elab_stage1_lookup verilog_generated)
    target_compile_definitions(test_elab_stage1_lookup PRIVATE
        ODIN3_ELAB_TMP="${PROJECT_BINARY_DIR}/elab_tmp"
        ODIN3_LIB_DIR="${PROJECT_SOURCE_DIR}/lib"
        ODIN3_PROJECT_FIXTURES="${PROJECT_SOURCE_DIR}/tests/golden/projects")
    # 2D Task 11: parameters, specializations, passes E/W, defparam and project push-down.
    odin3_add_unit_test(test_elab_stage1_params)
    target_include_directories(test_elab_stage1_params PRIVATE ${_gen})
    add_dependencies(test_elab_stage1_params verilog_generated)
    target_compile_definitions(test_elab_stage1_params PRIVATE
        ODIN3_ELAB_TMP="${PROJECT_BINARY_DIR}/elab_tmp"
        ODIN3_LIB_DIR="${PROJECT_SOURCE_DIR}/lib"
        ODIN3_PROJECT_FIXTURES="${PROJECT_SOURCE_DIR}/tests/golden/projects")
    # 2D Task 12: instances, connections, arrays, black boxes and cell types.
    odin3_add_unit_test(test_elab_stage1_inst)
    target_include_directories(test_elab_stage1_inst PRIVATE ${_gen})
    add_dependencies(test_elab_stage1_inst verilog_generated)
    target_compile_definitions(test_elab_stage1_inst PRIVATE
        ODIN3_ELAB_TMP="${PROJECT_BINARY_DIR}/elab_tmp"
        ODIN3_LIB_DIR="${PROJECT_SOURCE_DIR}/lib")
    # 2D Task 14: generate, scope names and references.
    odin3_add_unit_test(test_elab_stage1_gen)
    target_include_directories(test_elab_stage1_gen PRIVATE ${_gen})
    add_dependencies(test_elab_stage1_gen verilog_generated)
    target_compile_definitions(test_elab_stage1_gen PRIVATE
        ODIN3_ELAB_TMP="${PROJECT_BINARY_DIR}/elab_tmp"
        ODIN3_LIB_DIR="${PROJECT_SOURCE_DIR}/lib")
    # 2D Task 15: declarations, ports, implicit nets, blocks and subroutines.
    odin3_add_unit_test(test_elab_stage1_decl)
    target_include_directories(test_elab_stage1_decl PRIVATE ${_gen})
    add_dependencies(test_elab_stage1_decl verilog_generated)
    target_compile_definitions(test_elab_stage1_decl PRIVATE
        ODIN3_ELAB_TMP="${PROJECT_BINARY_DIR}/elab_tmp"
        ODIN3_LIB_DIR="${PROJECT_SOURCE_DIR}/lib")
    # 2D Task 16: the REJECT and IGNORE rows, the error cap, the empty stage-1 tables.
    odin3_add_unit_test(test_elab_stage1_diag)
    target_include_directories(test_elab_stage1_diag PRIVATE ${_gen})
    add_dependencies(test_elab_stage1_diag verilog_generated)
    target_compile_definitions(test_elab_stage1_diag PRIVATE
        ODIN3_ELAB_TMP="${PROJECT_BINARY_DIR}/elab_tmp"
        ODIN3_COVERAGE_DIR="${PROJECT_SOURCE_DIR}/tests/verilog/coverage"
        ODIN3_LIB_DIR="${PROJECT_SOURCE_DIR}/lib")
    # 2D Task 17: stage 2's modules, ports, wires, provenance, attributes, keep levels, budget and
    # the ABI entry odin3_ast_elaborate (tests/unit/elab_test.h's et_read runs the whole pass).
    odin3_add_unit_test(test_elab_stage2_wires)
    target_include_directories(test_elab_stage2_wires PRIVATE ${_gen})
    add_dependencies(test_elab_stage2_wires verilog_generated)
    target_compile_definitions(test_elab_stage2_wires PRIVATE
        ODIN3_ELAB_TMP="${PROJECT_BINARY_DIR}/elab_tmp")
    # 2D Task 18: expressions and selects to word cells, continuous and net declaration
    # assignments (EL-28, EL-30, EL-31, EL-32 reads).
    odin3_add_unit_test(test_elab_stage2_expr)
    target_include_directories(test_elab_stage2_expr PRIVATE ${_gen})
    add_dependencies(test_elab_stage2_expr verilog_generated)
    target_compile_definitions(test_elab_stage2_expr PRIVATE
        ODIN3_ELAB_TMP="${PROJECT_BINARY_DIR}/elab_tmp")
    # 2D Task 19: drivers (A4, uwire, supply, wired nets, tri-state), instances, gates, the error
    # cap and the summary line.
    odin3_add_unit_test(test_elab_stage2_drive)
    target_include_directories(test_elab_stage2_drive PRIVATE ${_gen})
    add_dependencies(test_elab_stage2_drive verilog_generated)
    target_compile_definitions(test_elab_stage2_drive PRIVATE
        ODIN3_ELAB_TMP="${PROJECT_BINARY_DIR}/elab_tmp"
        ODIN3_LIB_DIR="${PROJECT_SOURCE_DIR}/lib")
    # 2D-7b: the library-unit read (read_verilog --lib; spec 3.1a, 17 #80).
    odin3_add_unit_test(test_read_verilog_lib)
    target_include_directories(test_read_verilog_lib PRIVATE ${_gen})
    add_dependencies(test_read_verilog_lib verilog_generated)
    target_compile_definitions(test_read_verilog_lib PRIVATE
        ODIN3_ELAB_TMP="${PROJECT_BINARY_DIR}/elab_tmp"
        ODIN3_LIB_DIR="${PROJECT_SOURCE_DIR}/lib")
    # 5.2 Task 1/2: read_verilog -icells (spec I, N, V rules).
    odin3_add_unit_test(test_icells)
    target_include_directories(test_icells PRIVATE ${_gen})
    add_dependencies(test_icells verilog_generated)
    target_compile_definitions(test_icells PRIVATE
        ODIN3_ELAB_TMP="${PROJECT_BINARY_DIR}/elab_tmp")
    # 5.2 Task 3 (spec T-3): every cell type the writer writes as an instance reads back.
    odin3_add_unit_test(test_icells_roundtrip)
    # 2D Task 20: memories (the mem2reg decision, registers per word, $mem and its read ports).
    odin3_add_unit_test(test_elab_stage2_mem)
    target_include_directories(test_elab_stage2_mem PRIVATE ${_gen})
    add_dependencies(test_elab_stage2_mem verilog_generated)
    target_compile_definitions(test_elab_stage2_mem PRIVATE
        ODIN3_ELAB_TMP="${PROJECT_BINARY_DIR}/elab_tmp")
    # 2D Task 21: the process generator (always blocks to $proc nodes; EL-33 to EL-35).
    odin3_add_unit_test(test_elab_stage2_proc)
    target_sources(test_elab_stage2_proc PRIVATE tests/unit/proc_test.c)
    target_include_directories(test_elab_stage2_proc PRIVATE ${_gen})
    add_dependencies(test_elab_stage2_proc verilog_generated)
    target_compile_definitions(test_elab_stage2_proc PRIVATE
        ODIN3_ELAB_TMP="${PROJECT_BINARY_DIR}/elab_tmp")
    # 2D Task 22: if, case, case temporaries, flags and the case method for variable selects.
    odin3_add_unit_test(test_elab_stage2_case)
    target_sources(test_elab_stage2_case PRIVATE tests/unit/proc_test.c)
    target_include_directories(test_elab_stage2_case PRIVATE ${_gen})
    add_dependencies(test_elab_stage2_case verilog_generated)
    target_compile_definitions(test_elab_stage2_case PRIVATE
        ODIN3_ELAB_TMP="${PROJECT_BINARY_DIR}/elab_tmp")
    # 2D Task 23: loops, function and task calls, calls in continuous contexts, register-array words.
    odin3_add_unit_test(test_elab_stage2_call)
    target_sources(test_elab_stage2_call PRIVATE tests/unit/proc_test.c)
    target_include_directories(test_elab_stage2_call PRIVATE ${_gen})
    add_dependencies(test_elab_stage2_call verilog_generated)
    target_compile_definitions(test_elab_stage2_call PRIVATE
        ODIN3_ELAB_TMP="${PROJECT_BINARY_DIR}/elab_tmp")
    # 2D Task 24: memory writes (the write ports of $mem), initial values, $readmemh/b, the
    # per-bit rules, the empty pending tables and the sink module (OOM, determinism).
    odin3_add_unit_test(test_elab_stage2_init)
    target_sources(test_elab_stage2_init PRIVATE tests/unit/proc_test.c)
    target_include_directories(test_elab_stage2_init PRIVATE ${_gen})
    add_dependencies(test_elab_stage2_init verilog_generated)
    target_compile_definitions(test_elab_stage2_init PRIVATE
        ODIN3_ELAB_TMP="${PROJECT_BINARY_DIR}/elab_tmp"
        ODIN3_READMEM_DATA="${PROJECT_SOURCE_DIR}/tests/unit/data/elab/readmem"
        ODIN3_SINK_V="${PROJECT_SOURCE_DIR}/tests/elab/sink/sink.v")
    # 2D-VI: `initial x = <non-constant>` under read_verilog --vtr-compat (Peter 2026-10-13).
    odin3_add_unit_test(test_elab_vtr_initial)
    target_sources(test_elab_vtr_initial PRIVATE tests/unit/proc_test.c)
    target_include_directories(test_elab_vtr_initial PRIVATE ${_gen})
    add_dependencies(test_elab_vtr_initial verilog_generated)
    target_compile_definitions(test_elab_vtr_initial PRIVATE
        ODIN3_ELAB_TMP="${PROJECT_BINARY_DIR}/elab_tmp")
    # 2D-VI2: several tri-state drivers of an internal net under read_verilog --vtr-compat
    # (Peter 2026-10-14; PHASE2 decision 27).
    odin3_add_unit_test(test_elab_vtr_multidriver)
    target_sources(test_elab_vtr_multidriver PRIVATE tests/unit/proc_test.c)
    target_include_directories(test_elab_vtr_multidriver PRIVATE ${_gen})
    add_dependencies(test_elab_vtr_multidriver verilog_generated)
    target_compile_definitions(test_elab_vtr_multidriver PRIVATE
        ODIN3_ELAB_TMP="${PROJECT_BINARY_DIR}/elab_tmp")
    # 2D Task 13: the binder and the EL-4 store-shape rows (hand-built elaborated stores).
    odin3_add_unit_test(test_elab_bind)
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
    # the Python walk reaches a $proc node through the ABI before a front end makes one (2D Task 6),
    # and odin3_test_passrun_info (3D Task 8), so it checks qor_netstats' `# abc:` parser against the
    # run information the core formats.
    add_library(odin3_procfix SHARED $<TARGET_OBJECTS:odin3_core>
                tests/unit/proc_fixture_lib.c tests/unit/proc_fixture_core.c
                tests/unit/passrun_fixture_lib.c)
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
    # 3D Task 4: the ABC engine ABI and registry (cases in order: registration is final).
    odin3_add_unit_test(test_abc_engine_abi)
    if(ODIN3_WITH_ABC)
        # 3D Task 5: the linked engine's session, capture and guard on the real ABC. The adapter's
        # sources again, with the test hooks (ODIN3_ABC_TEST_HOOKS), linked with the same wraps.
        # The escapes and the adapter assertion run in child processes (detect_leaks=0: each
        # leaks its frame by design); the parent leaks nothing, and LeakSanitizer may suppress
        # only ABC's own functions (adapters/abc/lsan.supp).
        add_library(odin3_abc_testlib OBJECT ${ODIN3_ABC_ADAPTER_SOURCES})
        target_compile_definitions(odin3_abc_testlib PRIVATE ${_abc_adapter_defs}
                                                             ODIN3_ABC_TEST_HOOKS=1)
        target_compile_options(odin3_abc_testlib PRIVATE ${_abc_adapter_copts})
        target_link_libraries(odin3_abc_testlib PRIVATE libodin3 odin3_flags)
        add_executable(test_abc_adapter tests/unit/test_abc_adapter.c)
        target_include_directories(test_abc_adapter PRIVATE ${PROJECT_SOURCE_DIR}/adapters/abc)
        target_compile_definitions(test_abc_adapter PRIVATE ODIN3_ABC_TEST_HOOKS=1)
        target_link_libraries(test_abc_adapter PRIVATE odin3_abc_testlib libodin3 unity odin3_flags
                                                       $<LINK_ONLY:libabc-pic>)
        # 3D Task 6: pthread_create is wrapped with a counter here only (bridge.c's test hook;
        # §9: the default script starts no thread). The 40-AND networks use odin3_prng.
        target_link_options(test_abc_adapter PRIVATE ${ODIN3_ABC_WRAPS}
                                                     -Wl,--wrap=pthread_create)
        target_sources(test_abc_adapter PRIVATE src/sim/prng.c)
        target_include_directories(test_abc_adapter PRIVATE ${PROJECT_SOURCE_DIR}/src)
        set_target_properties(test_abc_adapter PROPERTIES LINKER_LANGUAGE CXX)
        # A planted libabc_o3test.so, alone in its directory: ABC would dlopen it from the cwd or
        # $ABC_LIB_PATH at frame creation, and its constructor exits 9 (review finding C1).
        add_library(abc_planted_lib MODULE tests/unit/abc_planted_lib.c)
        set_target_properties(abc_planted_lib PROPERTIES
            PREFIX ""
            OUTPUT_NAME libabc_o3test
            LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/abc_planted)
        target_link_libraries(abc_planted_lib PRIVATE odin3_flags)
        add_dependencies(test_abc_adapter abc_planted_lib)
        target_compile_definitions(test_abc_adapter PRIVATE
            ODIN3_ABC_PLANTED_DIR="${PROJECT_BINARY_DIR}/abc_planted")
        add_test(NAME test_abc_adapter COMMAND test_abc_adapter)
        set_tests_properties(test_abc_adapter PROPERTIES LABELS unit
            ENVIRONMENT "LSAN_OPTIONS=suppressions=${PROJECT_SOURCE_DIR}/adapters/abc/lsan.supp")
        # 3D Task 9c: the real odin3-abc as the child, beside the linked engine in this process
        # (the adapter's sources again, as above; the child runs under prlimit --as=2 GiB through
        # a wrapper script the test writes, because this ASan process cannot).
        add_executable(test_abc_child_real tests/unit/test_abc_child_real.c)
        target_include_directories(test_abc_child_real PRIVATE ${PROJECT_SOURCE_DIR}/adapters/abc
                                                               ${PROJECT_SOURCE_DIR}/src)
        target_compile_definitions(test_abc_child_real PRIVATE ODIN3_ABC_TEST_HOOKS=1
            ODIN3_CHILD_DATA="${PROJECT_SOURCE_DIR}/tests/unit/data/abc/child")
        target_link_libraries(test_abc_child_real PRIVATE odin3_abc_testlib odin3_core unity
                                                          odin3_flags $<LINK_ONLY:libabc-pic>)
        # The test build of bridge.c counts pthread_create (as test_abc_adapter's link does).
        target_link_options(test_abc_child_real PRIVATE ${ODIN3_ABC_WRAPS}
                                                         -Wl,--wrap=pthread_create)
        set_target_properties(test_abc_child_real PROPERTIES LINKER_LANGUAGE CXX)
        add_dependencies(test_abc_child_real odin3_abc_exe)
        add_test(NAME abc_child_real COMMAND test_abc_child_real)
        set_tests_properties(abc_child_real PROPERTIES LABELS unit
            ENVIRONMENT "LSAN_OPTIONS=suppressions=${PROJECT_SOURCE_DIR}/adapters/abc/lsan.supp")
        # 3D Task 21: test_abc_prov's fine-mode cases run the real linked engine in process (the
        # name map), beside its fake-engine cases (the adapter's sources, as test_abc_child_real).
        target_include_directories(test_abc_prov PRIVATE ${PROJECT_SOURCE_DIR}/adapters/abc)
        target_compile_definitions(test_abc_prov PRIVATE ODIN3_HAVE_REAL_ABC)
        target_link_libraries(test_abc_prov PRIVATE odin3_abc_testlib $<LINK_ONLY:libabc-pic>)
        target_link_options(test_abc_prov PRIVATE ${ODIN3_ABC_WRAPS} -Wl,--wrap=pthread_create)
        set_target_properties(test_abc_prov PROPERTIES LINKER_LANGUAGE CXX)
        set_tests_properties(test_abc_prov PROPERTIES
            ENVIRONMENT "LSAN_OPTIONS=suppressions=${PROJECT_SOURCE_DIR}/adapters/abc/lsan.supp")
    endif()
    odin3_add_unit_test(test_abi_core)
    target_compile_definitions(test_abi_core PRIVATE
        ODIN3_BLIF_FIXTURES="${PROJECT_SOURCE_DIR}/tests/golden/blif"
        ODIN3_CLI_FIXTURES="${PROJECT_SOURCE_DIR}/tests/cli")

    add_test(NAME blif_roundtrip_fixtures
             COMMAND ${PROJECT_SOURCE_DIR}/tools/blif-roundtrip/blif-roundtrip
                     --rt $<TARGET_FILE:odin3-blif-rt> --fixtures
                     ${PROJECT_SOURCE_DIR}/tests/golden/blif)
    set_tests_properties(blif_roundtrip_fixtures PROPERTIES LABELS unit)
    # 3E §16.3 step 5 (CI): write_blif --vtr over the fixtures: W1 == W2 (or the VB-10 decode),
    # the Parmys samples identical to their W1, dffsre and pow refused (E1b, E5).
    add_test(NAME blif_roundtrip_vtr_fixtures
             COMMAND ${PROJECT_SOURCE_DIR}/tools/blif-roundtrip/blif-roundtrip --vtr
                     --rt $<TARGET_FILE:odin3-blif-rt> --techlib ${PROJECT_SOURCE_DIR}/lib/vtr.o3lib
                     --fixtures ${PROJECT_SOURCE_DIR}/tests/golden/blif)
    set_tests_properties(blif_roundtrip_vtr_fixtures PROPERTIES LABELS unit)
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
        set(_wc_env "ODIN3_ABC=$<TARGET_FILE:odin3_abc_exe>")
        set(_wc_env_list "ODIN3_ABC=$<TARGET_FILE:odin3_abc_exe>;")
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

    set(_symbol_libs $<TARGET_FILE:libodin3> $<TARGET_FILE:example_plugin>)
    if(ODIN3_WITH_ABC)
        list(APPEND _symbol_libs $<TARGET_FILE:odin3_abc>)  # 3D §4.2: the wraps leave no exit
    endif()
    add_test(NAME symbol_rules
        COMMAND ${PROJECT_SOURCE_DIR}/tools/check-symbols.sh ${_symbol_libs})
    set_tests_properties(symbol_rules PROPERTIES LABELS unit)

    add_test(NAME abi_exports
        COMMAND ${PROJECT_SOURCE_DIR}/tools/check-symbols.sh
                --exports ${PROJECT_SOURCE_DIR}/include/odin3/odin3.h $<TARGET_FILE:libodin3>)
    set_tests_properties(abi_exports PROPERTIES LABELS unit)

    if(ODIN3_WITH_ABC)
        # 3D §4.2: the adapter exports exactly its two functions and imports none of the
        # process-ending, process-control or code-loading calls (the wraps took those ABC makes;
        # the rest of the exec, spawn and dl families must not appear with a pin bump).
        set(_abc_exports_args
            --only odin3_plugin_init,odin3_abc_adapter_init
            --forbid exit,_exit,_Exit,quick_exit,abort,system,popen,fork,vfork,execv,execvp,execl,execle,execlp,execve,execvpe,fexecve,posix_spawn,posix_spawnp,signal,dlopen,dlmopen)
        add_test(NAME abc_adapter_exports
            COMMAND ${PROJECT_SOURCE_DIR}/tools/check-symbols.sh ${_abc_exports_args}
                    $<TARGET_FILE:odin3_abc>)
        set_tests_properties(abc_adapter_exports PROPERTIES LABELS unit)
        # The same check on libodin3.so must fail: exit 1, with the "not in --only" line.
        add_test(NAME abc_adapter_exports_refuses_libodin3
            COMMAND sh -c "out=$(\"$0\" \"$@\" 2>&1); rc=$?; printf '%s\\n' \"$out\"; \
test \"$rc\" -eq 1 && printf '%s\\n' \"$out\" | grep -q ' exports odin3_abi_version, which is not in --only$'"
                    ${PROJECT_SOURCE_DIR}/tools/check-symbols.sh ${_abc_exports_args}
                    $<TARGET_FILE:libodin3>)
        set_tests_properties(abc_adapter_exports_refuses_libodin3 PROPERTIES LABELS unit)
    endif()

    # The ABI stays v4 through 2D (ruling I1) and 3D (spec Q4): their additions are additive.
    add_test(NAME cli_version COMMAND odin3 --version)
    set_tests_properties(cli_version PROPERTIES
        LABELS unit PASS_REGULAR_EXPRESSION "^odin3 ${PROJECT_VERSION} \\(ABI 4\\)")
    add_test(NAME cli_example_plugin COMMAND odin3 --plugin $<TARGET_FILE:example_plugin>)
    set_tests_properties(cli_example_plugin PROPERTIES LABELS unit)
    if(ODIN3_WITH_ABC)
        # 3D Task 6: --version names the linked ABC; a CLI whose adapter cannot register (the
        # test variant's abc_commit is refused) warns and runs on without it.
        odin3_add_abc_adapter(odin3_abc_unavailable ODIN3_ABC_TEST_UNREGISTRABLE=1)
        add_executable(odin3_abc_unavailable_cli src/cli/main.c)
        target_link_libraries(odin3_abc_unavailable_cli PRIVATE libodin3 odin3_abc_unavailable
                                                                odin3_flags)
        target_include_directories(odin3_abc_unavailable_cli PRIVATE
            ${PROJECT_SOURCE_DIR}/adapters/abc)
        target_compile_definitions(odin3_abc_unavailable_cli PRIVATE ODIN3_HAVE_ABC
                                                                     ${_abc_adapter_defs})
        foreach(_case IN ITEMS version_abc abc_unavailable_warning)
            set(_cli $<TARGET_FILE:odin3>)
            if(_case STREQUAL "abc_unavailable_warning")
                set(_cli $<TARGET_FILE:odin3_abc_unavailable_cli>)
            endif()
            add_test(NAME cli_${_case}
                COMMAND ${PROJECT_SOURCE_DIR}/tests/cli/test_cli.sh ${_case} ${_cli}
                        ${PROJECT_SOURCE_DIR}/tools/netlist-compare/netlist-compare
                        ${PROJECT_SOURCE_DIR}/tests/golden/blif)
            set_tests_properties(cli_${_case} PROPERTIES LABELS unit)
        endforeach()
    endif()
    if(ODIN3_WITH_ABC)
        # 3D Task 19: the abc pass from the command line (real ABC), the unconn gate and the
        # equivalence gate (abc-equiv over the goldens and the abc data BLIFs).
        foreach(_case IN ITEMS abc abc_bad_script abc_script_precedence abc_script_crash
                               abc_unconn_ch_intrinsics)
            set(_name cli_${_case})
            if(_case STREQUAL "abc_unconn_ch_intrinsics")
                set(_name ${_case})
            endif()
            add_test(NAME ${_name}
                COMMAND ${PROJECT_SOURCE_DIR}/tests/cli/test_cli.sh ${_case} $<TARGET_FILE:odin3>
                        ${PROJECT_SOURCE_DIR}/tools/netlist-compare/netlist-compare
                        ${PROJECT_SOURCE_DIR}/tests/golden/blif)
            set_tests_properties(${_name} PROPERTIES LABELS unit SKIP_RETURN_CODE 77
                TIMEOUT 900)
        endforeach()
        find_package(Python3 3.10 COMPONENTS Interpreter REQUIRED)
        add_test(NAME abc_equiv_micro
            COMMAND ${CMAKE_COMMAND} -E env ODIN3_ABC=$<TARGET_FILE:odin3_abc_exe>
                    ODIN3_REQUIRE_ABC=1
                    ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/abc-check/abc_equiv.py
                    --odin3 $<TARGET_FILE:odin3>
                    --equiv-check ${PROJECT_SOURCE_DIR}/tools/equiv-check/equiv-check
                    --lut 6 --expect-fail ${PROJECT_SOURCE_DIR}/tests/abc/equiv_expect_fail.txt
                    ${PROJECT_SOURCE_DIR}/tests/golden/blif
                    ${PROJECT_SOURCE_DIR}/tests/unit/data/abc
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
        set_tests_properties(abc_equiv_micro PROPERTIES LABELS tools TIMEOUT 3600)
        # 3D Task 21: the same micros through both exchanges (abc-diff): each result equivalent to
        # the input and to the other, the LUT, FF and depth counts printed.
        add_test(NAME abc_diff_micro
            COMMAND ${CMAKE_COMMAND} -E env ODIN3_ABC=$<TARGET_FILE:odin3_abc_exe>
                    ODIN3_REQUIRE_ABC=1
                    ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/abc-check/abc_diff.py
                    --odin3 $<TARGET_FILE:odin3>
                    --equiv-check ${PROJECT_SOURCE_DIR}/tools/equiv-check/equiv-check
                    --lut 6 --expect-fail ${PROJECT_SOURCE_DIR}/tests/abc/equiv_expect_fail.txt
                    --keep ${PROJECT_BINARY_DIR}/abc_tests/abc_diff_micro
                    ${PROJECT_SOURCE_DIR}/tests/golden/blif
                    ${PROJECT_SOURCE_DIR}/tests/unit/data/abc
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
        set_tests_properties(abc_diff_micro PROPERTIES LABELS tools TIMEOUT 3600)
        # 3D Task 22: determinism (spec 13.8) on the micros, in CI: (a) two processes and (b) three
        # sessions in one process, on the default exchange, and (a) through the BLIF exchange with
        # its files compared. The designs abc-equiv lists as failing are skipped.
        add_test(NAME abc_determinism_ci
            COMMAND ${CMAKE_COMMAND} -E env ODIN3_ABC=$<TARGET_FILE:odin3_abc_exe>
                    ODIN3_REQUIRE_ABC=1
                    ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/abc-check/abc_determinism.py
                    --odin3 $<TARGET_FILE:odin3> --mode a --mode b --lut 6
                    --expect-fail ${PROJECT_SOURCE_DIR}/tests/abc/equiv_expect_fail.txt
                    --keep ${PROJECT_BINARY_DIR}/abc_tests/abc_determinism_ci
                    ${PROJECT_SOURCE_DIR}/tests/golden/blif
                    ${PROJECT_SOURCE_DIR}/tests/unit/data/abc
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
        set_tests_properties(abc_determinism_ci PROPERTIES LABELS tools TIMEOUT 3600)
        add_test(NAME abc_determinism_ci_blif
            COMMAND ${CMAKE_COMMAND} -E env ODIN3_ABC=$<TARGET_FILE:odin3_abc_exe>
                    ODIN3_REQUIRE_ABC=1
                    ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/abc-check/abc_determinism.py
                    --odin3 $<TARGET_FILE:odin3> --via-blif --mode a --lut 6
                    --expect-fail ${PROJECT_SOURCE_DIR}/tests/abc/equiv_expect_fail.txt
                    --keep ${PROJECT_BINARY_DIR}/abc_tests/abc_determinism_ci_blif
                    ${PROJECT_SOURCE_DIR}/tests/golden/blif
                    ${PROJECT_SOURCE_DIR}/tests/unit/data/abc
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
        set_tests_properties(abc_determinism_ci_blif PROPERTIES LABELS tools TIMEOUT 3600)
        # bench_abc prints every column of spec 12's table (Release only; the Python test skips
        # without ODIN3_BENCH_ABC).
        if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT ODIN3_SANITIZE)
            add_test(NAME bench_abc_smoke
                COMMAND ${CMAKE_COMMAND} -E env ODIN3_ABC=$<TARGET_FILE:odin3_abc_exe>
                        ODIN3_REQUIRE_ABC=1 ODIN3_BENCH_ABC=$<TARGET_FILE:bench_abc>
                        ${Python3_EXECUTABLE} -m unittest -v tests.tools.test_bench_abc
                WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
            set_tests_properties(bench_abc_smoke PROPERTIES LABELS tools TIMEOUT 900)
        endif()
        # 3D Task 19 (deferred to the controller, point (m)): every k6 Parmys golden of the VTR set
        # except mcml and LU32PEEng, smallest first (abc-equiv sorts), stereovision3 listed as
        # multiclock; the run log's summary is quoted in the PR.
        set(_k6_golden $ENV{HOME}/odin3-ws/golden/k6_frac_N10_frac_chain_mem32K_40nm)
        if(EXISTS ${_k6_golden}/vtr AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
            file(GLOB _abc_goldens ${_k6_golden}/vtr/*/*.parmys.blif)
            list(FILTER _abc_goldens EXCLUDE REGEX "/(mcml|LU32PEEng)/")
            odin3_add_sweep_test(abc_equiv_golden LABEL sweep
                COMMAND ${CMAKE_COMMAND} -E env ODIN3_ABC=$<TARGET_FILE:odin3_abc_exe>
                        ODIN3_REQUIRE_ABC=1
                        ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/abc-check/abc_equiv.py
                        --odin3 $<TARGET_FILE:odin3>
                        --equiv-check ${PROJECT_SOURCE_DIR}/tools/equiv-check/equiv-check
                        --lut 6 --timeout 3000
                        --expect-fail ${PROJECT_SOURCE_DIR}/tests/abc/equiv_expect_fail.txt
                        --keep $ENV{HOME}/odin3-ws/work/abc-equiv-golden ${_abc_goldens})
            # 3D Task 21 (deferred to the controller, point (m); the exchange differential, I8):
            # the same goldens through the in-memory exchange against equiv-check, and both
            # exchanges against each other (abc-diff: LUT, FF and depth ratios, smallest first).
            odin3_add_sweep_test(abc_equiv_golden_aig LABEL sweep
                COMMAND ${CMAKE_COMMAND} -E env ODIN3_ABC=$<TARGET_FILE:odin3_abc_exe>
                        ODIN3_REQUIRE_ABC=1
                        ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/abc-check/abc_equiv.py
                        --odin3 $<TARGET_FILE:odin3>
                        --equiv-check ${PROJECT_SOURCE_DIR}/tools/equiv-check/equiv-check
                        --lut 6 --timeout 3000 --exchange aig
                        --expect-fail ${PROJECT_SOURCE_DIR}/tests/abc/equiv_expect_fail.txt
                        --keep $ENV{HOME}/odin3-ws/work/abc-equiv-golden-aig ${_abc_goldens})
            # 3D Task 22 (the controller's, point (m)): determinism (a) on the goldens, with the
            # BLIF exchange's files compared, and bench_abc on them, smallest first (the table
            # goes verbatim into PHASE3); the linked ABC against the executable on the same BLIF.
            odin3_add_sweep_test(abc_determinism_golden LABEL sweep
                COMMAND ${CMAKE_COMMAND} -E env ODIN3_ABC=$<TARGET_FILE:odin3_abc_exe>
                        ODIN3_REQUIRE_ABC=1
                        ${Python3_EXECUTABLE} -I
                        ${PROJECT_SOURCE_DIR}/tools/abc-check/abc_determinism.py
                        --odin3 $<TARGET_FILE:odin3> --via-blif --mode a --lut 6 --timeout 3000
                        --expect-fail ${PROJECT_SOURCE_DIR}/tests/abc/equiv_expect_fail.txt
                        --keep $ENV{HOME}/odin3-ws/work/abc-determinism-golden ${_abc_goldens})
            if(TARGET bench_abc)
                odin3_add_sweep_test(bench_abc_set LABEL sweep
                    COMMAND ${CMAKE_COMMAND} -E env ODIN3_ABC=$<TARGET_FILE:odin3_abc_exe>
                            ${PROJECT_SOURCE_DIR}/tools/abc-check/bench-abc-set
                            --bench $<TARGET_FILE:bench_abc> --exe $<TARGET_FILE:odin3_abc_exe>
                            --keep $ENV{HOME}/odin3-ws/work/abc-bench-set ${_abc_goldens})
            endif()
            odin3_add_sweep_test(abc_diff_golden LABEL sweep
                COMMAND ${CMAKE_COMMAND} -E env ODIN3_ABC=$<TARGET_FILE:odin3_abc_exe>
                        ODIN3_REQUIRE_ABC=1
                        ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/abc-check/abc_diff.py
                        --odin3 $<TARGET_FILE:odin3>
                        --equiv-check ${PROJECT_SOURCE_DIR}/tools/equiv-check/equiv-check
                        --lut 6 --timeout 3000
                        --expect-fail ${PROJECT_SOURCE_DIR}/tests/abc/equiv_expect_fail.txt
                        --keep $ENV{HOME}/odin3-ws/work/abc-diff-golden ${_abc_goldens})
        endif()
    endif()
    # tests/cli/test_cli.sh <case> ...: pass scripts end to end (round trip, located errors, --top).
    foreach(_case IN ITEMS script_roundtrip p_roundtrip unknown_pass_p unknown_pass_script
                           unknown_pass_later_script top top_written_first top_unapplied quoted
                           help_runs_nothing check_flag usage techlib plugin writers sim keep_ast
                           project_dump project_top project_revision project_diag_paths
                           project_preprocess parse_only parse_only_with_script elab_only
                           elab_only_with_script abc_script_missing abc_script_project opt_snapshot opt_determinism)
        add_test(NAME cli_${_case}
            COMMAND ${PROJECT_SOURCE_DIR}/tests/cli/test_cli.sh ${_case} $<TARGET_FILE:odin3>
                    ${PROJECT_SOURCE_DIR}/tools/netlist-compare/netlist-compare
                    ${PROJECT_SOURCE_DIR}/tests/golden/blif)
        set_tests_properties(cli_${_case} PROPERTIES LABELS unit
            ENVIRONMENT "ODIN3_EXAMPLE_PLUGIN=$<TARGET_FILE:example_plugin>;ODIN3_SIM_VECTORS=$<TARGET_FILE:odin3-sim-vectors>")
    endforeach()

    find_package(Python3 3.10 COMPONENTS Interpreter REQUIRED)
    # 3D §4.1: no assert( in the adapter (ODIN3_ADAPTER_ASSERT instead), and the other §15.1 rules.
    add_test(NAME abc_adapter_no_assert
        COMMAND sh -c "\"$0\" tools/check_rules.py adapters/abc/*.c adapters/abc/*.h"
                ${Python3_EXECUTABLE}
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(abc_adapter_no_assert PROPERTIES LABELS unit)
    set(_tools_env "ODIN3_SIM_VECTORS=$<TARGET_FILE:odin3-sim-vectors>"
                   "ODIN3_PARSE_CHECK=$<TARGET_FILE:odin3-parse-check>"
                   "ODIN3_ELAB_CHECK=$<TARGET_FILE:elab-check>")
    if(ODIN3_WITH_ABC)
        # REQUIRE makes the ABC-backed tests fail, not skip, if this ABC is unusable.
        list(APPEND _tools_env "ODIN3_ABC=$<TARGET_FILE:odin3_abc_exe>" "ODIN3_REQUIRE_ABC=1")
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
                         --driver $<TARGET_FILE:odin3-sim-vectors>
                         --abc $<TARGET_FILE:odin3_abc_exe>
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
    # ODIN3_BENCH_PROC: test_bench_proc_report's check that a Debug bench_proc refuses to run.
    list(APPEND _tools_env "ODIN3_BENCH_PROC=$<TARGET_FILE:bench_proc>")
    set_tests_properties(python_tools PROPERTIES LABELS tools ENVIRONMENT "${_tools_env}")
    # 2F Task 16 (spec 20.1, plan points (f), (g)): one tiny source per opt rule (tests/opt/rules/
    # <sub>/<rule>.v or .blif, with its golden .il and the row in rules.txt). Odin III runs
    # `read_verilog --nolatches; proc --noopt; <sub-pass> --opt-stats` on each, smallest first: the
    # rule's counter must be at least 1 and out.il must equal the golden. Needs no Yosys; the
    # Yosys 0.55 half is opt_rules_yosys (below). Exit 77 (skip) when an ASan child cannot be
    # capped.
    add_test(NAME opt_rules
        COMMAND ${PROJECT_SOURCE_DIR}/tools/opt-check/opt-rules
                --odin3 $<TARGET_FILE:odin3> ${PROJECT_SOURCE_DIR}/tests/opt/rules)
    set_tests_properties(opt_rules PROPERTIES LABELS opt SKIP_RETURN_CODE 77 TIMEOUT 600)
    # opt_ffbits (PHASE3 #49): `opt_merge -ffbits` on the tests/opt/ffbits micros: the flip-flop
    # bits left, against tests/opt/ffbits/expect.txt. Needs no Yosys; opt_ffbits_yosys is the
    # parity check, opt_equiv_ffbits the refinement.
    add_test(NAME opt_ffbits
        COMMAND ${PROJECT_SOURCE_DIR}/tools/opt-check/ffbits-check --odin3 $<TARGET_FILE:odin3>)
    set_tests_properties(opt_ffbits PROPERTIES LABELS opt TIMEOUT 600)
    # opt_simcheck_rules (review I2): the same 141 rows with --opt-simcheck (Debug builds), the
    # in-process 1E simulation check around every sub-pass call; it fails unless at least 90
    # modules were judged (95 at the time of writing), so a silent no-op cannot pass.
    if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR ODIN3_SANITIZE)
        add_test(NAME opt_simcheck_rules
            COMMAND ${PROJECT_SOURCE_DIR}/tools/opt-check/opt-rules
                    --odin3 $<TARGET_FILE:odin3> --simcheck-min 90
                    ${PROJECT_SOURCE_DIR}/tests/opt/rules)
        set_tests_properties(opt_simcheck_rules PROPERTIES LABELS opt
                             SKIP_RETURN_CODE 77 TIMEOUT 600)
    endif()
    # 2E Task 13 (spec 19): bench_proc prints the columns the targets are judged from. A Debug or
    # sanitized build refuses to run (tests/bench/bench_guard.h, exit 2: its times are not the
    # targets'); a Release build's smoke runs `--one` on two micros (a case under a combinational
    # block; a memory under a clocked process) and checks every column. The full bench is
    # `elab-sweep --bench build/release/bench_proc --mem-cap 6G` (tools/proc-check/
    # bench-proc-report report LOG prints the targets); tests/proc/bench_expect_fail.txt lists the
    # three macro-only include files.
    if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR ODIN3_SANITIZE)
        add_test(NAME bench_proc_refuses_debug COMMAND bench_proc --one /dev/null)
        set_tests_properties(bench_proc_refuses_debug PROPERTIES LABELS tools
            PASS_REGULAR_EXPRESSION "needs a Release build")
    else()
        odin3_add_sweep_test(bench_proc_smoke LABEL sweep
            COMMAND ${PROJECT_SOURCE_DIR}/tools/proc-check/bench-proc-report smoke
                    --bench $<TARGET_FILE:bench_proc>
                    --techlib ${PROJECT_SOURCE_DIR}/lib/vtr.o3lib
                    ${PROJECT_SOURCE_DIR}/tests/micro/verilog/keywords/always/case_default.v
                    ${PROJECT_SOURCE_DIR}/tests/micro/verilog/syntax/inferred_SPram.v)
    endif()
    # 2F Task 19 (spec 19): bench_opt prints the columns the targets are judged from. A Debug or
    # sanitized build refuses to run (bench_opt_refuses_debug); a Release build's smoke runs `--one`
    # on two micros (a case under a combinational block; a memory under a clocked process) and
    # checks every column. The full bench is `elab-sweep --bench build/release/bench_opt`
    # (tools/opt-check/bench-opt-report report --expect-fail tests/opt/bench_expect_fail.txt LOG
    # prints the targets).
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT ODIN3_SANITIZE)
        odin3_add_sweep_test(bench_opt_smoke LABEL sweep
            COMMAND ${PROJECT_SOURCE_DIR}/tools/opt-check/bench-opt-report smoke
                    --bench $<TARGET_FILE:bench_opt>
                    --techlib ${PROJECT_SOURCE_DIR}/lib/vtr.o3lib
                    ${PROJECT_SOURCE_DIR}/tests/micro/verilog/keywords/always/case_default.v
                    ${PROJECT_SOURCE_DIR}/tests/micro/verilog/syntax/inferred_SPram.v)
    endif()
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
        # DEBUGINFOD_URLS empty: without a network llvm-symbolizer blocks on debuginfod, and a
        # crash report then ends in the CTest timeout instead of being printed.
        set(_fuzz_env "DEBUGINFOD_URLS=")
        if(ODIN3_LLVM_SYMBOLIZER)
            list(APPEND _fuzz_env "ASAN_SYMBOLIZER_PATH=${ODIN3_LLVM_SYMBOLIZER}")
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

        # 2C §7.5: the parser's seeds (`fuzz_seeds.py`): two BOM-prefixed files (Review Focus 5),
        # then the coverage rows and the syntax micros, smallest first, each as a raw input and
        # as a token stream, twenty generated modules and tests/fuzz/fuzz_verilog_regress (the
        # inputs of earlier findings). fuzz_verilog_seed_inputs runs each
        # seed once; fuzz_verilog_smoke fuzzes 10 s per mode (raw, token stream). Long runs are
        # local (one worker, 2 GB RSS).
        set(_fv_seeds ${PROJECT_BINARY_DIR}/fuzz_verilog_seeds)
        set(_fv_corpus ${PROJECT_BINARY_DIR}/fuzz_verilog_corpus)
        file(MAKE_DIRECTORY ${_fv_corpus})
        add_test(NAME fuzz_verilog_seeds
            COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/verilog-gen/fuzz_seeds.py
                    ${_fv_seeds} ${PROJECT_SOURCE_DIR}/tests/verilog/coverage
                    ${PROJECT_SOURCE_DIR}/tests/micro/verilog/syntax --generated 20
                    --regress ${PROJECT_SOURCE_DIR}/tests/fuzz/fuzz_verilog_regress)
        set_tests_properties(fuzz_verilog_seeds PROPERTIES FIXTURES_SETUP fuzz_verilog_seeds
                             LABELS fuzz)
        add_test(NAME fuzz_verilog_seed_inputs
            COMMAND $<TARGET_FILE:fuzz_verilog> -runs=0 -timeout=60 -rss_limit_mb=2048
                    ${_fv_seeds})
        set(_fv_smoke -max_total_time=10 -timeout=10 -rss_limit_mb=2048 -print_funcs=0
                      -print_final_stats=1 ${_fv_corpus} ${_fv_seeds})
        add_test(NAME fuzz_verilog_smoke
            COMMAND sh -c "FUZZ_VERILOG_MODE=raw \"$0\" \"$@\" && FUZZ_VERILOG_MODE=tokens \"$0\" \"$@\""
                    $<TARGET_FILE:fuzz_verilog> ${_fv_smoke})
        set_tests_properties(fuzz_verilog_seed_inputs fuzz_verilog_smoke PROPERTIES
                             FIXTURES_REQUIRED fuzz_verilog_seeds LABELS fuzz TIMEOUT 300)
        if(_fuzz_env)
            set_tests_properties(fuzz_verilog_seed_inputs fuzz_verilog_smoke PROPERTIES
                                 ENVIRONMENT "${_fuzz_env}")
        endif()

        # 2D §14.5 (plan Task 28): the elaborator's seeds (`elab_fuzz_seeds.py`): every
        # tests/elab/rows file and the micros' common/ directory, smallest first, and ten
        # generated modules (verilog_gen.py --elab), each after a mode byte (--nolatches on).
        # fuzz_elab_seed_inputs runs each once; fuzz_elab_smoke fuzzes 10 s (-max_len 4096, the
        # caps lowered by the harness). One worker, 2 GB RSS; long runs are local.
        set(_fe_seeds ${PROJECT_BINARY_DIR}/fuzz_elab_seeds)
        set(_fe_corpus ${PROJECT_BINARY_DIR}/fuzz_elab_corpus)
        file(MAKE_DIRECTORY ${_fe_corpus})
        add_test(NAME fuzz_elab_seeds
            COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/elab-check/elab_fuzz_seeds.py
                    ${_fe_seeds} ${PROJECT_SOURCE_DIR}/tests/elab/rows
                    ${PROJECT_SOURCE_DIR}/tests/micro/verilog/common --generated 10)
        set_tests_properties(fuzz_elab_seeds PROPERTIES FIXTURES_SETUP fuzz_elab_seeds
                             LABELS fuzz)
        add_test(NAME fuzz_elab_seed_inputs
            COMMAND $<TARGET_FILE:fuzz_elab> -runs=0 -timeout=60 -rss_limit_mb=2048
                    -print_funcs=0 ${_fe_seeds})
        add_test(NAME fuzz_elab_smoke
            COMMAND $<TARGET_FILE:fuzz_elab> -seed=1 -max_total_time=10 -timeout=10
                    -rss_limit_mb=2048 -max_len=4096 -print_funcs=0 -print_final_stats=1
                    ${_fe_corpus} ${_fe_seeds})
        set_tests_properties(fuzz_elab_seed_inputs fuzz_elab_smoke PROPERTIES
                             FIXTURES_REQUIRED fuzz_elab_seeds LABELS fuzz TIMEOUT 600)
        if(_fuzz_env)
            set_tests_properties(fuzz_elab_seed_inputs fuzz_elab_smoke PROPERTIES
                                 ENVIRONMENT "${_fuzz_env}")
        endif()

        # 3D §13.7 (plan Task 11): the ABC script checker (no ABC runs). The seeds
        # (tests/fuzz/abc_script_seeds: a K byte, then lib/abc/vtr.abc, VTR's whole stage, one
        # script per abc.rc alias and one per refused example of test_abc_script) run once;
        # fuzz_abc_script_smoke fuzzes 60 s with inputs up to 8 KiB; fuzz_abc_script_long fuzzes
        # an hour, up to past the 64 KiB cap, as a heavy sweep (tools/heavy-run, label sweep).
        set(_fa_seeds ${PROJECT_SOURCE_DIR}/tests/fuzz/abc_script_seeds)
        set(_fa_corpus ${PROJECT_BINARY_DIR}/fuzz_abc_script_corpus)
        file(MAKE_DIRECTORY ${_fa_corpus})
        add_test(NAME fuzz_abc_script_seed_inputs
            COMMAND $<TARGET_FILE:fuzz_abc_script> -runs=0 -timeout=60 -rss_limit_mb=2048
                    ${_fa_seeds})
        # -seed=1: every CI run explores the same inputs, so a finding reproduces from its log.
        add_test(NAME fuzz_abc_script_smoke
            COMMAND $<TARGET_FILE:fuzz_abc_script> -seed=1 -max_total_time=60 -timeout=10
                    -rss_limit_mb=2048 -max_len=8192 -print_funcs=0 -print_final_stats=1
                    ${_fa_corpus} ${_fa_seeds})
        set_tests_properties(fuzz_abc_script_seed_inputs fuzz_abc_script_smoke PROPERTIES
                             LABELS fuzz TIMEOUT 300)
        if(_fuzz_env)
            set_tests_properties(fuzz_abc_script_seed_inputs fuzz_abc_script_smoke PROPERTIES
                                 ENVIRONMENT "${_fuzz_env}")
        endif()
        # 2F §20.4 (plan Task 18): the opt family. The seeds (opt_fuzz_seeds.py: builder records
        # of every cell type and the small golden BLIFs, each after a mode byte) run once;
        # fuzz_opt_smoke fuzzes 10 s with inputs up to 512 bytes (a gate test, not a sweep).
        set(_fo_seeds ${PROJECT_BINARY_DIR}/fuzz_opt_seeds)
        set(_fo_corpus ${PROJECT_BINARY_DIR}/fuzz_opt_corpus)
        file(MAKE_DIRECTORY ${_fo_corpus})
        add_test(NAME fuzz_opt_seeds
            COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/opt-check/opt_fuzz_seeds.py
                    ${_fo_seeds} --regress ${PROJECT_SOURCE_DIR}/tests/fuzz/fuzz_opt_regress
                    ${PROJECT_SOURCE_DIR}/tests/golden/blif)
        set_tests_properties(fuzz_opt_seeds PROPERTIES FIXTURES_SETUP fuzz_opt_seeds LABELS fuzz)
        add_test(NAME fuzz_opt_seed_inputs
            COMMAND $<TARGET_FILE:fuzz_opt> -runs=0 -timeout=60 -rss_limit_mb=2048
                    -print_funcs=0 ${_fo_seeds})
        add_test(NAME fuzz_opt_smoke
            COMMAND $<TARGET_FILE:fuzz_opt> -seed=1 -max_total_time=10 -timeout=10
                    -rss_limit_mb=2048 -max_len=512 -print_funcs=0 -print_final_stats=1
                    ${_fo_corpus} ${_fo_seeds})
        set_tests_properties(fuzz_opt_seed_inputs fuzz_opt_smoke PROPERTIES
                             FIXTURES_REQUIRED fuzz_opt_seeds LABELS fuzz TIMEOUT 600)
        if(_fuzz_env)
            set_tests_properties(fuzz_opt_seed_inputs fuzz_opt_smoke PROPERTIES
                                 ENVIRONMENT "${_fuzz_env}")
        endif()

        # 3B §14.5 (Task 9): lower's fuzzer, 60 s (a gate test; the long run is a heavy-run).
        set(_fl_corpus ${PROJECT_BINARY_DIR}/fuzz_lower_corpus)
        file(MAKE_DIRECTORY ${_fl_corpus})
        add_test(NAME fuzz_lower_smoke
            COMMAND $<TARGET_FILE:fuzz_lower> -seed=1 -max_total_time=60 -timeout=30
                    -rss_limit_mb=2048 -max_len=64 -print_funcs=0 -print_final_stats=1
                    ${_fl_corpus})
        set_tests_properties(fuzz_lower_smoke PROPERTIES LABELS fuzz TIMEOUT 600)
        if(_fuzz_env)
            set_tests_properties(fuzz_lower_smoke PROPERTIES ENVIRONMENT "${_fuzz_env}")
        endif()
        # The long run, capped at 1 h (PHASE3 decision 54: shipped out to the bigger machine,
        # tools/vtr-qor/offload/run-offload; `timeout 90m` is only a backstop).
        odin3_add_sweep_test(fuzz_lower_long LABEL sweep
            COMMAND timeout 90m $<TARGET_FILE:fuzz_lower> -max_total_time=3600 -timeout=30
                    -rss_limit_mb=2048 -max_len=64 -print_funcs=0 -print_final_stats=1
                    ${_fl_corpus})
        if(_fuzz_env)
            set_tests_properties(fuzz_lower_long PROPERTIES ENVIRONMENT "${_fuzz_env}")
        endif()

        # 3D §13.7 (Task 19): 2,000 runs of the splice fuzzer in the smoke (no odin3 binary needed,
        # so the fuzz label is right); the long run is a sweep.
        set(_fs_corpus ${PROJECT_BINARY_DIR}/fuzz_abc_splice_corpus)
        file(MAKE_DIRECTORY ${_fs_corpus})
        add_test(NAME fuzz_abc_splice_smoke
            COMMAND $<TARGET_FILE:fuzz_abc_splice> -runs=2000 -seed=1 -timeout=60
                    -rss_limit_mb=2048 -max_len=128 -print_funcs=0 -print_final_stats=1
                    ${_fs_corpus})
        set_tests_properties(fuzz_abc_splice_smoke PROPERTIES LABELS fuzz TIMEOUT 600)
        if(_fuzz_env)
            set_tests_properties(fuzz_abc_splice_smoke PROPERTIES ENVIRONMENT "${_fuzz_env}")
        endif()
        odin3_add_sweep_test(fuzz_abc_splice_long LABEL sweep
            COMMAND timeout 3h $<TARGET_FILE:fuzz_abc_splice> -max_total_time=3600 -timeout=60
                    -rss_limit_mb=2048 -max_len=128 -print_funcs=0 -print_final_stats=1
                    ${_fs_corpus})
        if(_fuzz_env)
            set_tests_properties(fuzz_abc_splice_long PROPERTIES ENVIRONMENT "${_fuzz_env}")
        endif()

        odin3_add_sweep_test(fuzz_abc_script_long LABEL sweep
            COMMAND timeout 3h $<TARGET_FILE:fuzz_abc_script> -max_total_time=3600 -timeout=10
                    -rss_limit_mb=2048 -max_len=70000 -print_funcs=0 -print_final_stats=1
                    ${_fa_corpus} ${_fa_seeds})
        if(_fuzz_env)
            set_tests_properties(fuzz_abc_script_long PROPERTIES ENVIRONMENT "${_fuzz_env}")
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

    # 2C §7.2: every coverage row through read → check → print → re-read → equal, its .ast golden
    # and the location oracle, smallest file first.
    file(GLOB _coverage_rows CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/tests/verilog/coverage/*.v)
    add_test(NAME verilog_coverage
        COMMAND $<TARGET_FILE:odin3-parse-check> --smallest-first ${_coverage_rows})
    set_tests_properties(verilog_coverage PROPERTIES LABELS verilog)

    # 2D Task 25 (spec 14.2): the 125 coverage rows of tests/elab/rows elaborate, pass check FULL
    # and match their goldens, smallest first, in both latch modes (the nolatches goldens exist for
    # every row with a combinational always).
    file(STRINGS ${PROJECT_SOURCE_DIR}/tests/elab/rows/rows.txt _elab_row_lines)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 ${PROJECT_SOURCE_DIR}/tests/elab/rows/rows.txt)
    set(_elab_rows_all "")
    set(_elab_rows_comb "")
    foreach(_line IN LISTS _elab_row_lines)
        if(_line MATCHES "^[A-Z][0-9]+ ([^ ]+) comb=([01]) clocks=[0-9]+$")
            list(APPEND _elab_rows_all ${PROJECT_SOURCE_DIR}/tests/elab/rows/${CMAKE_MATCH_1})
            if(CMAKE_MATCH_2 STREQUAL "1")
                list(APPEND _elab_rows_comb ${PROJECT_SOURCE_DIR}/tests/elab/rows/${CMAKE_MATCH_1})
            endif()
        else()
            message(FATAL_ERROR "tests/elab/rows/rows.txt: bad line '${_line}'")
        endif()
    endforeach()
    add_test(NAME elab_coverage
        COMMAND elab-check --smallest-first ${_elab_rows_all})
    add_test(NAME elab_coverage_nolatches
        COMMAND elab-check --nolatches --smallest-first ${_elab_rows_comb})
    set_tests_properties(elab_coverage elab_coverage_nolatches PROPERTIES LABELS elab)
    # 2C §7.3: the negative cases (odin3 --parse-only in the case directory), and the 2D handover
    # cases (read_verilog without --parse-only; pending, exit 77, while 2D's entry is the stub).
    add_test(NAME verilog_reject
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/parse-check/run_reject.py
                $<TARGET_FILE:odin3> ${PROJECT_SOURCE_DIR}/tests/verilog/reject)
    set_tests_properties(verilog_reject PROPERTIES LABELS verilog)
    # The handover cases must parse (2C §7.3: 2D reports them, not the parser): read, round
    # trip and location oracle, with no golden.
    file(GLOB _handover_cases CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/tests/verilog/reject-2d/*.v
                                                ${PROJECT_SOURCE_DIR}/tests/verilog/ignore-2d/*.v)
    add_test(NAME verilog_handover_parse
        COMMAND $<TARGET_FILE:odin3-parse-check> --smallest-first ${_handover_cases})
    set_tests_properties(verilog_handover_parse PROPERTIES LABELS verilog)
    # Task 24 (point (l)): no coverage row and not the sink module hits a not-yet-built construct.
    file(GLOB _no_pending_rows CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/tests/verilog/coverage/*.v
                                                 ${PROJECT_SOURCE_DIR}/tests/elab/sink/sink.v)
    add_test(NAME elab_no_pending
        COMMAND sh -c "for f in \"$@\"; do if \"$0\" -p \"read_verilog $f\" 2>&1 | grep -q 'elaboration is not available yet (2D)'; then echo \"pending: $f\"; exit 1; fi; done"
                $<TARGET_FILE:odin3> ${_no_pending_rows})
    set_tests_properties(elab_no_pending PROPERTIES LABELS verilog)
    # Task 16: stage 1 reports every row; A4 is Task 19's (the second driver of a net) and T9 and
    # T10 Task 21's (the process generator's per-bit rules, EL-41); none of the ignore cases is
    # pending.
    set(_handover_pending_reject-2d "")
    set(_handover_pending_ignore-2d "")
    foreach(_handover reject-2d ignore-2d)
        string(REPLACE "-" "_" _handover_test "verilog_${_handover}")
        add_test(NAME ${_handover_test}
            COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/parse-check/run_reject.py
                    --elaborate "--pending=${_handover_pending_${_handover}}"
                    $<TARGET_FILE:odin3> ${PROJECT_SOURCE_DIR}/tests/verilog/${_handover})
        set_tests_properties(${_handover_test} PROPERTIES LABELS verilog SKIP_RETURN_CODE 77)
    endforeach()

    # 2C §7.4 (plan Task 10): the corpus sweeps through parse-corpus, one file at a time, smallest
    # first: every micro against tests/verilog/micro_expect_fail.txt; every VTR and Koios
    # benchmark together when VTR_ROOT is set at configure time (koios/lenet.v last, point (n));
    # every committed BLIF through odin3-write --verilog (Review Focus 1). Heavy sweeps (label
    # `sweep`) through odin3_add_sweep_test(): tools/heavy-run alone takes the lock and the slice
    # (skip 77 without ODIN3_HEAVY_OK=1). Run them as `ODIN3_HEAVY_OK=1 ctest --preset release
    # -L sweep`.
    set(_parse_corpus ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/parse-check/parse_corpus.py
                      --check $<TARGET_FILE:odin3-parse-check>)
    odin3_add_sweep_test(verilog_micro_parse LABEL sweep
        COMMAND ${_parse_corpus}
                --expect-fail ${PROJECT_SOURCE_DIR}/tests/verilog/micro_expect_fail.txt
                ${PROJECT_SOURCE_DIR}/tests/micro/verilog)
    # 2D Task 26 (spec 14.5): every micro under `odin3 --elab-only`, smallest first, with
    # --top from its Parmys golden (micro-top); failures only as listed in
    # tests/elab/micro_expect_fail.txt. Run as `ODIN3_HEAVY_OK=1 ctest --preset sweep -R
    # verilog_micro_elab`. Two 3G jobs fit the heavy slice's 6G (two 4G jobs do not).
    odin3_add_sweep_test(verilog_micro_elab LABEL sweep
        COMMAND ${PROJECT_SOURCE_DIR}/tools/elab-check/elab-sweep
                --odin3 $<TARGET_FILE:odin3>
                --techlib ${PROJECT_SOURCE_DIR}/lib/vtr.o3lib
                --lib ${PROJECT_SOURCE_DIR}/tests/elab/lib/vtr_primitives.v --jobs 2 --mem-cap 3G
                --expect-fail ${PROJECT_SOURCE_DIR}/tests/elab/micro_expect_fail.txt
                --root ${PROJECT_SOURCE_DIR}/tests/micro/verilog
                ${PROJECT_SOURCE_DIR}/tests/micro/verilog)
    odin3_add_sweep_test(verilog_writer_parse LABEL sweep
        COMMAND ${_parse_corpus} --writer $<TARGET_FILE:odin3-write>
                ${PROJECT_SOURCE_DIR}/tests/golden/blif)
    if(DEFINED ENV{VTR_ROOT})
        set(_vtr_verilog $ENV{VTR_ROOT}/vtr_flow/benchmarks/verilog)
        file(GLOB _vtr_benchmarks CONFIGURE_DEPENDS ${_vtr_verilog}/*.v ${_vtr_verilog}/koios/*.v)
    endif()
    if(_vtr_benchmarks)
        odin3_add_sweep_test(verilog_vtr_parse LABEL sweep
            COMMAND ${_parse_corpus} --root ${_vtr_verilog} ${_vtr_benchmarks})
    elseif(DEFINED ENV{VTR_ROOT})
        message(STATUS "verilog_vtr_parse not registered: no benchmarks under $ENV{VTR_ROOT}")
    endif()
    # 2D Task 27 (spec 14.5, 15): the 64 VTR and Koios files (31 + 33) as ONE set under
    # `odin3 --elab-only`, smallest first, koios/lenet.v last (it gets 5G, alone, in the 6G slice);
    # failures only as listed in tests/elab/vtr_expect_fail.txt. Run as `ODIN3_HEAVY_OK=1 ctest
    # --preset sweep -R verilog_vtr_elab` from a Release build. The benchmarks' hard blocks
    # (adder, multiply, *_port_ram) come from lib/vtr.o3lib.
    if(_vtr_benchmarks)
        odin3_add_sweep_test(verilog_vtr_elab LABEL sweep
            COMMAND ${PROJECT_SOURCE_DIR}/tools/elab-check/elab-sweep
                    --odin3 $<TARGET_FILE:odin3>
                    --techlib ${PROJECT_SOURCE_DIR}/lib/vtr.o3lib --jobs 2 --mem-cap 3G
                    --cap-for koios/lenet.v=5G
                    --expect-fail ${PROJECT_SOURCE_DIR}/tests/elab/vtr_expect_fail.txt
                    --root ${_vtr_verilog} ${_vtr_benchmarks})
    endif()
    # 3E-4 (spec 3E §16.3, §16.4): write_blif --vtr over every status=ok k6_frac golden, in two
    # sweeps, each one file at a time, smallest first, under tools/heavy-run (skip 77 without
    # ODIN3_HEAVY_OK=1). Run as `ODIN3_HEAVY_OK=1 ctest --preset sweep -R
    # 'blif_roundtrip_vtr_k6|vtr_read_check_k6'` from a Release build; the first run records the
    # times. Registered only when VTR_ROOT is set and the golden tree exists.
    #   blif_roundtrip_vtr_k6: W1 == W2 (or the VB-10 decode), Parmys netlist-compare identical,
    #     Odin II identical under --vtr-normalize, equiv-check where it applies.
    #   vtr_read_check_k6: VPR `--exit_before_pack on` reads every W; refusals pair with VPR's
    #     verdict on G; blif-vtr-check agrees with VPR on every G and W.
    set(_k6_golden $ENV{HOME}/odin3-ws/golden/k6_frac_N10_frac_chain_mem32K_40nm)
    if(DEFINED ENV{VTR_ROOT} AND EXISTS ${_k6_golden}
       AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT ODIN3_SANITIZE)
        odin3_add_sweep_test(blif_roundtrip_vtr_k6 LABEL sweep
            COMMAND ${PROJECT_SOURCE_DIR}/tools/blif-roundtrip/blif-roundtrip --vtr
                    --rt $<TARGET_FILE:odin3-blif-rt>
                    --techlib ${PROJECT_SOURCE_DIR}/lib/vtr.o3lib
                    --out $ENV{HOME}/odin3-ws/work/blif-roundtrip-vtr ${_k6_golden})
        odin3_add_sweep_test(vtr_read_check_k6 LABEL sweep
            COMMAND ${PROJECT_SOURCE_DIR}/tools/vtr-read-check/vtr-read-check
                    --odin3 $<TARGET_FILE:odin3> --golden-root ${_k6_golden}
                    --arch $ENV{VTR_ROOT}/vtr_flow/arch/timing/k6_frac_N10_frac_chain_mem32K_40nm.xml
                    --vpr $ENV{VTR_ROOT}/vpr/vpr
                    --work $ENV{HOME}/odin3-ws/work/vtr-read-check)
    endif()
    # The negative corpus against VPR itself (18 tiny files, not a heavy run): VPR rejects exactly
    # the files the checker does, with the message in each file's `# vpr:` line. Registered when
    # VTR_ROOT points at a built VTR.
    if(DEFINED ENV{VTR_ROOT} AND EXISTS $ENV{VTR_ROOT}/vpr/vpr)
        add_test(NAME vtr_read_check_bad_corpus
                 COMMAND ${PROJECT_SOURCE_DIR}/tools/vtr-read-check/vtr-read-check
                         --bad-corpus ${PROJECT_SOURCE_DIR}/tests/vtr/blif-bad
                         --arch $ENV{VTR_ROOT}/vtr_flow/arch/timing/k6_frac_N10_frac_chain_mem32K_40nm.xml
                         --vpr $ENV{VTR_ROOT}/vpr/vpr
                         --work $ENV{HOME}/odin3-ws/work/vtr-read-check)
        set_tests_properties(vtr_read_check_bad_corpus PROPERTIES LABELS tools)
    endif()
    # 2D Task 27 (spec 15): bench_elab prints the columns the targets are judged from. The smoke
    # runs `--one` on two micros (a plain assign; a memory under a clocked process) and checks
    # every column.
    odin3_add_sweep_test(bench_elab_smoke LABEL sweep
        COMMAND ${PROJECT_SOURCE_DIR}/tools/elab-check/bench-report smoke
                --bench $<TARGET_FILE:bench_elab>
                --techlib ${PROJECT_SOURCE_DIR}/lib/vtr.o3lib
                ${PROJECT_SOURCE_DIR}/tests/micro/verilog/keywords/assign/assign_wide.v
                ${PROJECT_SOURCE_DIR}/tests/micro/verilog/syntax/inferred_SPram.v)

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

    # 2E Task 11 (spec 18.2): the 48 construct rows. Odin III's `read_verilog; proc --noopt;
    # write_rtlil` equals each golden (and the --nolatches one of a combinational row), the five
    # error rows give exactly their .err lines with ODIN3_ERR_PARSE, and case_default's golden
    # holds one $pmux of WIDTH 8. 48 tiny files: not a sweep.
    add_test(NAME proc_rows
        COMMAND ${PROJECT_SOURCE_DIR}/tools/proc-check/proc-rows goldens
                --odin3 $<TARGET_FILE:odin3>)
    set_tests_properties(proc_rows PROPERTIES
        LABELS proc SKIP_RETURN_CODE 77 TIMEOUT 300)

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
        # 3A Task 4 (spec 14.3): flatten against Yosys 0.55 on the fixtures; a difference not
        # listed in tests/flatten/diff_expect.txt (with a reason) fails.
        # 3B Task 3 (spec 14.2): lower against Yosys 0.55 on the per-cell fixtures, tier S (the
        # simplemap family): netlist-compare exit 0 and equal cell-type histograms for every file.
        # 3B Task 5 adds the mul and div families ($mul; $div and $mod).
        # 3B Task 4: one test per family; tier T (equiv-check and equal histograms after opt -fast)
        # for the techmap.v family. Fixtures are told apart by their file-name prefix.
        file(GLOB _lower_cells CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/tests/lower/cells/*.v)
        set(_lower_prefixes pmux shl shr sshr shiftx neg add sub lt le gt ge mul div mod
            dff dffe sdff sdffe sdffce adff adffe aldff aldffe dffsr dffsre dlatch mem)
        set(_lower_family_bitwise "")
        set(_lower_family_pmux "pmux")
        set(_lower_family_shift "shl;shr;sshr;shiftx")
        set(_lower_family_arith "neg;add;sub;lt;le;gt;ge")
        set(_lower_family_mul "mul")
        set(_lower_family_div "div;mod")
        set(_lower_bitwise_files ${_lower_cells})
        foreach(_prefix IN LISTS _lower_prefixes)
            list(FILTER _lower_bitwise_files EXCLUDE REGEX "/cells/${_prefix}_[^/]*$")
        endforeach()
        add_test(NAME lower_cells_yosys
            COMMAND ${PROJECT_SOURCE_DIR}/tools/lower-check/lower-diff
                    --odin3 $<TARGET_FILE:odin3> --yosys ${ODIN3_YOSYS055}
                    --family bitwise --pre proc --each ${_lower_bitwise_files})
        set_tests_properties(lower_cells_yosys PROPERTIES LABELS lower SKIP_RETURN_CODE 77
                             TIMEOUT 1800)
        foreach(_family IN ITEMS pmux shift arith mul div)
            set(_family_files "")
            foreach(_prefix IN LISTS _lower_family_${_family})
                set(_prefix_files ${_lower_cells})
                list(FILTER _prefix_files INCLUDE REGEX "/cells/${_prefix}_[^/]*$")
                list(APPEND _family_files ${_prefix_files})
            endforeach()
            if(_family_files)
                add_test(NAME lower_cells_yosys_${_family}
                    COMMAND ${PROJECT_SOURCE_DIR}/tools/lower-check/lower-diff
                            --odin3 $<TARGET_FILE:odin3> --yosys ${ODIN3_YOSYS055}
                            --family ${_family} --pre proc
                            --expect ${PROJECT_SOURCE_DIR}/tests/lower/diff_expect.txt
                            --each ${_family_files})
                set_tests_properties(lower_cells_yosys_${_family} PROPERTIES LABELS lower
                                     SKIP_RETURN_CODE 77 TIMEOUT 1800)
            endif()
        endforeach()
        # 3B Task 6 (spec 14.2, PHASE3 #19): the flip-flops. Tier S (netlist-compare exit 0) for the
        # synchronous types; tier D (equiv-check) for the asynchronous ones against Yosys's lowering
        # with tools/lower-check/maps/; the falling-edge fixtures run --neg-edge keep (the default
        # makes an inverter, which equiv-check does not accept as a clock); and Parmys's unmodified
        # enable-over-reset maps, which differ exactly on $adffe and $aldffe (spec 16 row 4).
        set(_ff_sync dff dffe sdff sdffe sdffce)
        set(_ff_async adff adffe aldff aldffe dffsr dffsre)
        set(_ff_sync_pos "")
        set(_ff_async_pos "")
        set(_ff_neg "")
        set(_ff_parmys "")
        foreach(_prefix IN LISTS _ff_sync _ff_async)
            set(_prefix_files ${_lower_cells})
            list(FILTER _prefix_files INCLUDE REGEX "/cells/${_prefix}_[^/]*$")
            set(_prefix_pos ${_prefix_files})
            list(FILTER _prefix_pos EXCLUDE REGEX "_neg")
            list(FILTER _prefix_files INCLUDE REGEX "_neg")
            list(APPEND _ff_neg ${_prefix_files})
            if(_prefix IN_LIST _ff_sync)
                list(APPEND _ff_sync_pos ${_prefix_pos})
            else()
                list(APPEND _ff_async_pos ${_prefix_pos})
            endif()
            if(_prefix MATCHES "^a(dff|ldff)e?$")
                list(APPEND _ff_parmys ${_prefix_pos})
            endif()
        endforeach()
        if(_ff_sync_pos AND _ff_async_pos AND _ff_neg AND _ff_parmys)
            set(_lower_diff ${PROJECT_SOURCE_DIR}/tools/lower-check/lower-diff
                --odin3 $<TARGET_FILE:odin3> --yosys ${ODIN3_YOSYS055} --family ff)
            add_test(NAME lower_cells_yosys_ff_sync COMMAND ${_lower_diff} --tier S --each ${_ff_sync_pos})
            add_test(NAME lower_cells_yosys_ff_async COMMAND ${_lower_diff} --tier D --each ${_ff_async_pos})
            add_test(NAME lower_cells_yosys_ff_neg
                COMMAND ${_lower_diff} --lower-args "--neg-edge keep" --each ${_ff_neg})
            add_test(NAME lower_cells_yosys_ff_parmys
                COMMAND ${_lower_diff} --parmys-exact
                        --expect ${PROJECT_SOURCE_DIR}/tests/lower/diff_expect_parmys.txt
                        --each ${_ff_parmys})
            set_tests_properties(lower_cells_yosys_ff_sync lower_cells_yosys_ff_async
                lower_cells_yosys_ff_neg lower_cells_yosys_ff_parmys
                PROPERTIES LABELS lower SKIP_RETURN_CODE 77 TIMEOUT 1800)
        endif()
        # 3B Task 7 (spec 6.2, 14.2): the latches. $dlatch is tier S (netlist-compare exit 0). The
        # fixtures cover $dlatch only: $adlatch, $dlatchsr and $sr have no Verilog form (proc and
        # opt infer one $dlatch per bit, in Yosys as in Odin III), so they are covered by
        # test_lower_latch.
        set(_latch_files ${_lower_cells})
        list(FILTER _latch_files INCLUDE REGEX "/cells/dlatch_[^/]*$")
        if(_latch_files)
            add_test(NAME lower_cells_yosys_latch
                COMMAND ${PROJECT_SOURCE_DIR}/tools/lower-check/lower-diff
                        --odin3 $<TARGET_FILE:odin3> --yosys ${ODIN3_YOSYS055}
                        --family latch --tier S --each ${_latch_files})
            set_tests_properties(lower_cells_yosys_latch PROPERTIES LABELS lower
                                 SKIP_RETURN_CODE 77 TIMEOUT 1800)
        endif()
        # 3B Task 8 (spec 7, 14.2): $mem against Yosys's `memory -nomap; memory_map`, tier D
        # (equiv-check; SIZE <= 16, WIDTH <= 4 so it decides). The falling-edge fixture runs
        # --neg-edge keep (equiv-check does not accept an inverter as a clock). mem_w4_d4_off1 is
        # the listed difference: ABITS above clog2(SIZE) (tests/lower/diff_expect.txt).
        set(_mem_files ${_lower_cells})
        list(FILTER _mem_files INCLUDE REGEX "/cells/mem_[^/]*$")
        set(_mem_neg ${_mem_files})
        list(FILTER _mem_neg INCLUDE REGEX "_neg")
        list(FILTER _mem_files EXCLUDE REGEX "_neg")
        if(_mem_files AND _mem_neg)
            set(_lower_mem_diff ${PROJECT_SOURCE_DIR}/tools/lower-check/lower-diff
                --odin3 $<TARGET_FILE:odin3> --yosys ${ODIN3_YOSYS055} --family mem)
            add_test(NAME lower_cells_yosys_mem
                COMMAND ${_lower_mem_diff}
                        --expect ${PROJECT_SOURCE_DIR}/tests/lower/diff_expect.txt
                        --each ${_mem_files})
            add_test(NAME lower_cells_yosys_mem_neg
                COMMAND ${_lower_mem_diff} --lower-args "--neg-edge keep" --each ${_mem_neg})
            set_tests_properties(lower_cells_yosys_mem lower_cells_yosys_mem_neg
                PROPERTIES LABELS lower SKIP_RETURN_CODE 77 TIMEOUT 1800)
        endif()
        add_test(NAME flatten_diff_fixtures
            COMMAND ${PROJECT_SOURCE_DIR}/tools/flatten-check/flatten-diff
                    --odin3 $<TARGET_FILE:odin3> --yosys ${ODIN3_YOSYS055}
                    --expect ${PROJECT_SOURCE_DIR}/tests/flatten/diff_expect.txt
                    ${_flat_dir}/hier.v ${_flat_dir}/params.v ${_flat_dir}/gen.v
                    ${_flat_dir}/feed.v ${_flat_dir}/keep.v ${_flat_dir}/bb.v ${_flat_dir}/mem.v)
        set_tests_properties(flatten_diff_fixtures PROPERTIES LABELS flatten SKIP_RETURN_CODE 77
                             TIMEOUT 600)
        # The same generator against Yosys 0.55: names, widths and cell types (the equivalence
        # proof is the heavy leg: fuzz_flatten --yosys --equiv, 3A Task 5).
        add_test(NAME fuzz_flatten_yosys_quick
            COMMAND ${PROJECT_SOURCE_DIR}/tools/flatten-check/fuzz_flatten --cases 300 --seed 5001
                    --odin3 $<TARGET_FILE:odin3> --yosys ${ODIN3_YOSYS055}
                    --repro-dir ${PROJECT_BINARY_DIR}/fuzz_flatten)
        set_tests_properties(fuzz_flatten_yosys_quick PROPERTIES LABELS flatten
                             SKIP_RETURN_CODE 77 TIMEOUT 3600)
        # 3C Task 1 (spec 9.2): the Parmys probes. probe-leg runs Parmys on a micro of
        # tests/hardmap/probes into a scratch golden root under the build tree, then parmys-probe
        # against the source (iverilog); the leg passes when the probe exits as the README records
        # (1 = the Parmys defect of R-7 / A-8 / R-8 reproduces, 0 = the control matches). Needs
        # VTR_ROOT and iverilog; each leg is a few seconds, so it is a sweep only by label.
        if(DEFINED ENV{VTR_ROOT} AND ODIN3_IVERILOG)
            set(_probe_legs
                "ramsplit_defect|ramsplit|1|--cycles,2000,--warm,16,--mask,addr=0x000f"
                "ramsplit_control|ramsplit|0|--cycles,2000,--warm,16,--mask,addr=0x000e"
                "subeq_control|subeq|0|--cycles,3000"
                "subwide_defect|subwide|1|--cycles,3000"
                "subprobe2_defect|subprobe2|1|--cycles,3000"
                "addrmerge_defect|addrmerge|1|--cycles,2000,--warm,16"
                "addrmerge_control|addrmerge|0|--cycles,2000,--warm,16,--mask,we=0x0")
            foreach(_leg IN LISTS _probe_legs)
                string(REPLACE "|" ";" _f "${_leg}")
                list(GET _f 0 _leg_name)
                list(GET _f 1 _leg_micro)
                list(GET _f 2 _leg_expect)
                list(GET _f 3 _leg_opts)
                string(REPLACE "," ";" _leg_opts "${_leg_opts}")
                odin3_add_sweep_test(parmys_probe_${_leg_name} LABEL sweep
                    COMMAND ${PROJECT_SOURCE_DIR}/tools/hardmap-check/probe-leg
                            ${PROJECT_BINARY_DIR}/parmys_probe/${_leg_micro}
                            ${_leg_micro} ${_leg_expect} ${_leg_opts})
            endforeach()
        endif()
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
        # 2D Task 21: the round trip of the process generator's write_rtlil through Yosys 0.55
        # (read_rtlil; proc exits 0).
        set_property(TEST test_elab_stage2_proc APPEND PROPERTY
            ENVIRONMENT "ODIN3_YOSYS055=${ODIN3_YOSYS055}")
        # 2D Task 21 (spec 14.3): elab-equiv on the T7, T8 and `q + 1` sources: Odin III's
        # processes and Yosys's own, through one Yosys back end, are equivalent. Exit 77 (skip)
        # when an ASan child cannot be capped (no systemd-run).
        add_test(NAME elab_proc_bridge_smoke
            COMMAND sh -c "for f in t7 t8 qplus1; do \"$0\" --odin3 \"$1\" --yosys \"$2\" --equiv-check \"$3\" --top t \"$4/$f.v\" || exit $?; done"
                    ${PROJECT_SOURCE_DIR}/tools/elab-check/elab-equiv
                    $<TARGET_FILE:odin3> ${ODIN3_YOSYS055}
                    ${PROJECT_SOURCE_DIR}/tools/equiv-check/equiv-check
                    ${PROJECT_SOURCE_DIR}/tests/verilog/proc-bridge)
        set_tests_properties(elab_proc_bridge_smoke PROPERTIES
            LABELS differential SKIP_RETURN_CODE 77)
        # 2D Task 22 (spec 14.3): elab-equiv on every source of tests/verilog/case-bridge: if,
        # case, casez/casex wildcards, nesting with blocking temporaries, full_case, folding, item
        # expressions and the case method for variable selects (--unmap-enables: a held register is an
        # enabled flip-flop on the Yosys side). Exit 77 (skip) as above.
        add_test(NAME elab_case_bridge
            COMMAND sh -c "for f in \"$4\"/*.v; do \"$0\" --odin3 \"$1\" --yosys \"$2\" --equiv-check \"$3\" --top t --unmap-enables \"$f\" || exit $?; done"
                    ${PROJECT_SOURCE_DIR}/tools/elab-check/elab-equiv
                    $<TARGET_FILE:odin3> ${ODIN3_YOSYS055}
                    ${PROJECT_SOURCE_DIR}/tools/equiv-check/equiv-check
                    ${PROJECT_SOURCE_DIR}/tests/verilog/case-bridge)
        set_tests_properties(elab_case_bridge PROPERTIES
            LABELS differential SKIP_RETURN_CODE 77)
        # 2H-triage (PHASE2 #27, review M-I3): the default (IEEE) reading of several tri-state
        # drivers on an internal net, without --vtr-compat, against Yosys 0.55 `tribuf -logic`: the
        # micro multi_assignment and the D3 accept files. The --vtr-compat reading of the micro
        # differs by design (micro_equiv_expect_fail.txt). Exit 77 (skip) as above.
        add_test(NAME elab_tribuf_default_equiv
            COMMAND sh -c "for f in \"$4\"/syntax/multi_assignment.v \"$5\"/D3_*.v; do \"$0\" --odin3 \"$1\" --yosys \"$2\" --equiv-check \"$3\" --yosys-tribuf \"$f\" || exit $?; done"
                    ${PROJECT_SOURCE_DIR}/tools/elab-check/elab-equiv
                    $<TARGET_FILE:odin3> ${ODIN3_YOSYS055}
                    ${PROJECT_SOURCE_DIR}/tools/equiv-check/equiv-check
                    ${PROJECT_SOURCE_DIR}/tests/micro/verilog
                    ${PROJECT_SOURCE_DIR}/tests/elab/accept)
        set_tests_properties(elab_tribuf_default_equiv PROPERTIES
            LABELS differential SKIP_RETURN_CODE 77)
        # PHASE2 decision 28: a lone tri-state driver of a top-level output loses its enable
        # under --vtr-compat as in Parmys: the WARNING, and the sp-splice netlist identical to the
        # Parmys golden (tests/golden/vtr_tristate). Exit 77 (skip) as above.
        if(DEFINED ENV{VTR_ROOT})
            add_test(NAME vtr_tristate_out
                COMMAND ${PROJECT_SOURCE_DIR}/tests/golden/vtr_tristate/check.sh
                        $<TARGET_FILE:odin3> ${ODIN3_YOSYS055}
                        ${PROJECT_SOURCE_DIR}/tools/opt-check/sp-splice
                        ${PROJECT_SOURCE_DIR}/tools/netlist-compare/netlist-compare
                        $ENV{VTR_ROOT}/vtr_flow/arch/timing/k6_frac_N10_frac_chain_mem32K_40nm.xml
                        ${PROJECT_SOURCE_DIR}/tests/golden/vtr_tristate)
            set_tests_properties(vtr_tristate_out PROPERTIES
                LABELS differential SKIP_RETURN_CODE 77)
        endif()
        # 2D Task 23 (spec 14.3): elab-equiv on every source of tests/verilog/call-bridge: unrolled
        # loops, functions and tasks inlined (in processes and in continuous contexts), nested calls
        # and variable-index reads of net arrays. Exit 77 (skip) as above.
        add_test(NAME elab_call_bridge
            COMMAND sh -c "for f in \"$4\"/*.v; do \"$0\" --odin3 \"$1\" --yosys \"$2\" --equiv-check \"$3\" --top t \"$f\" || exit $?; done"
                    ${PROJECT_SOURCE_DIR}/tools/elab-check/elab-equiv
                    $<TARGET_FILE:odin3> ${ODIN3_YOSYS055}
                    ${PROJECT_SOURCE_DIR}/tools/equiv-check/equiv-check
                    ${PROJECT_SOURCE_DIR}/tests/verilog/call-bridge)
        set_tests_properties(elab_call_bridge PROPERTIES
            LABELS differential SKIP_RETURN_CODE 77)
        # 2D Task 24 (spec 14.3): elab-equiv on every source of tests/verilog/mem-bridge: the write
        # ports of $mem (all three ranges, a write then a read at the same index and a cycle with
        # the enable off; conditional, nested, in loops, tasks and functions; partial and
        # concatenated targets). memory_map makes enabled flip-flops, which equiv-check cannot
        # read: --unmap-enables. Exit 77 (skip) as above.
        add_test(NAME elab_mem_bridge
            COMMAND sh -c "for f in \"$4\"/*.v; do \"$0\" --odin3 \"$1\" --yosys \"$2\" --equiv-check \"$3\" --top t --unmap-enables \"$f\" || exit $?; done"
                    ${PROJECT_SOURCE_DIR}/tools/elab-check/elab-equiv
                    $<TARGET_FILE:odin3> ${ODIN3_YOSYS055}
                    ${PROJECT_SOURCE_DIR}/tools/equiv-check/equiv-check
                    ${PROJECT_SOURCE_DIR}/tests/verilog/mem-bridge)
        set_tests_properties(elab_mem_bridge PROPERTIES
            LABELS differential SKIP_RETURN_CODE 77)
        # 2E Task 11 (spec 18.3, 18.5): the 43 lowering rows against Yosys 0.55. proc_diff_rows
        # runs proc-diff per row in both latch modes (and with the pre-proc trio once `opt`
        # exists), rerunning a failing row with --bisect; proc_bridge_rows runs elab-equiv --pass
        # proc per row (tests/proc/bridge_expect.txt lists what equiv-check cannot read). Exit 77
        # (skip) when an ASan child cannot be capped.
        add_test(NAME proc_diff_rows
            COMMAND ${PROJECT_SOURCE_DIR}/tools/proc-check/proc-rows diff
                    --odin3 $<TARGET_FILE:odin3> --yosys ${ODIN3_YOSYS055})
        add_test(NAME proc_bridge_rows
            COMMAND ${PROJECT_SOURCE_DIR}/tools/proc-check/proc-rows bridge
                    --odin3 $<TARGET_FILE:odin3> --yosys ${ODIN3_YOSYS055}
                    --equiv-check ${PROJECT_SOURCE_DIR}/tools/equiv-check/equiv-check)
        set_tests_properties(proc_diff_rows proc_bridge_rows PROPERTIES
            LABELS proc SKIP_RETURN_CODE 77 TIMEOUT 600)
        # 2D Task 25 (spec 14.2): elab-equiv on every coverage row, smallest first, except the
        # rows with several clocks (goldens only) and those in tests/elab/equiv_expect_fail.txt.
        odin3_add_sweep_test(elab_coverage_equiv LABEL sweep
            COMMAND ${PROJECT_SOURCE_DIR}/tools/elab-check/run-rows-equiv
                    --elab-equiv ${PROJECT_SOURCE_DIR}/tools/elab-check/elab-equiv
                    --odin3 $<TARGET_FILE:odin3> --yosys ${ODIN3_YOSYS055}
                    --equiv-check ${PROJECT_SOURCE_DIR}/tools/equiv-check/equiv-check
                    --rows ${PROJECT_SOURCE_DIR}/tests/elab/rows
                    --expect-fail ${PROJECT_SOURCE_DIR}/tests/elab/equiv_expect_fail.txt)
        # 2D Task 26 (spec 14.5): the micros through the bridge (elab-equiv per file, Yosys
        # 0.55), smallest first. The micros that fail before the bridge are in
        # micro_expect_fail.txt; the ones the bridge finds not equivalent in
        # micro_equiv_expect_fail.txt. A Yosys crash is an `oracle crash`, not a failure.
        # large/mkDelayWorker32B.v runs alone at a 6G cap (Yosys side B peaks at 4.8 GB, 2H).
        # --equiv-budget 1800: the Yosys and equiv-check runs of one file get 1800 s together, then
        # it is `unproven` (elab-equiv exit 4), listable, where the sweep's hour is a hard timeout (2H,
        # large/arm_core.v).
        odin3_add_sweep_test(elab_equiv_micro LABEL sweep
            COMMAND ${PROJECT_SOURCE_DIR}/tools/elab-check/elab-sweep
                    --odin3 $<TARGET_FILE:odin3> --equiv --yosys ${ODIN3_YOSYS055}
                    --equiv-check ${PROJECT_SOURCE_DIR}/tools/equiv-check/equiv-check
                    --techlib ${PROJECT_SOURCE_DIR}/lib/vtr.o3lib --vtr-compat
                    --lib ${PROJECT_SOURCE_DIR}/tests/elab/lib/vtr_primitives.v
                    --expect-fail ${PROJECT_SOURCE_DIR}/tests/elab/micro_expect_fail.txt
                    --expect-fail ${PROJECT_SOURCE_DIR}/tests/elab/micro_equiv_expect_fail.txt
                    --root ${PROJECT_SOURCE_DIR}/tests/micro/verilog
                    --cap-for large/mkDelayWorker32B.v=6G
                    --equiv-budget 1800
                    ${PROJECT_SOURCE_DIR}/tests/micro/verilog)
        # 2D Task 27 (spec 14.5): the 64 VTR and Koios files through the bridge (Yosys 0.55), one
        # set, smallest first; koios/lenet.v's Yosys side (~7 GB) is an `xl` entry of
        # tests/elab/vtr_equiv_expect_fail.txt, run by elab_equiv_vtr_xl. A Yosys crash is an
        # `oracle crash`. Labels sweep and nightly. Run as `ODIN3_HEAVY_OK=1 ctest --preset sweep
        # -R elab_equiv_vtr$`.
        if(_vtr_benchmarks)
            odin3_add_sweep_test(elab_equiv_vtr LABEL sweep
                COMMAND ${PROJECT_SOURCE_DIR}/tools/elab-check/elab-sweep
                        --odin3 $<TARGET_FILE:odin3> --equiv --yosys ${ODIN3_YOSYS055}
                        --equiv-check ${PROJECT_SOURCE_DIR}/tools/equiv-check/equiv-check
                        --techlib ${PROJECT_SOURCE_DIR}/lib/vtr.o3lib
                        --jobs 2 --mem-cap 3G
                        --expect-fail ${PROJECT_SOURCE_DIR}/tests/elab/vtr_expect_fail.txt
                        --expect-fail ${PROJECT_SOURCE_DIR}/tests/elab/vtr_equiv_expect_fail.txt
                        --root ${_vtr_verilog} ${_vtr_benchmarks})
            set_tests_properties(elab_equiv_vtr PROPERTIES LABELS "sweep;nightly")
            # lenet.v alone: both sides in an 8G child cap, in the 9G slice (the controller sets
            # ODIN3_HEAVY_XL_OK=1 by hand, with no agent running). Its outcome (the peak RSS, or
            # `out of memory (cap 8G)`) is reported, never listed as expected.
            odin3_add_sweep_test(elab_equiv_vtr_xl LABEL sweep-xl
                COMMAND ${PROJECT_SOURCE_DIR}/tools/elab-check/elab-sweep
                        --odin3 $<TARGET_FILE:odin3> --equiv --yosys ${ODIN3_YOSYS055}
                        --equiv-check ${PROJECT_SOURCE_DIR}/tools/equiv-check/equiv-check
                        --techlib ${PROJECT_SOURCE_DIR}/lib/vtr.o3lib
                        --xl --mem-cap 8G --partial
                        --expect-fail ${PROJECT_SOURCE_DIR}/tests/elab/vtr_equiv_expect_fail.txt
                        --root ${_vtr_verilog} ${_vtr_verilog}/koios/lenet.v)
        endif()
        # 2D Task 26 (spec 14.4): the project corpus's positive phase-2 Verilog cases through the
        # bridge against their ref.blif (oracle drift against the recorded 0.69 is reported, not
        # failed), and the negative front-end cases' kind, location and names. In the gate.
        add_test(NAME project_corpus_frontend
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/tools/project-fixtures/project_fixtures.py compare
                    --odin3 $<TARGET_FILE:odin3> --with-frontend --yosys ${ODIN3_YOSYS055}
                    --equiv-check ${PROJECT_SOURCE_DIR}/tools/equiv-check/equiv-check)
        set_tests_properties(project_corpus_frontend PROPERTIES LABELS tools)
        # 2F Task 16 (spec 20.1): opt_rules plus Yosys 0.55 on the same in.il: its cover point (or
        # log line) for the rule must fire and its per-module cell histogram must equal Odin III's.
        # 141 small files, not a sweep. Rows marked dev=N are the documented spec 17 divergences.
        add_test(NAME opt_rules_yosys
            COMMAND ${PROJECT_SOURCE_DIR}/tools/opt-check/opt-rules
                    --odin3 $<TARGET_FILE:odin3> --yosys ${ODIN3_YOSYS055}
                    ${PROJECT_SOURCE_DIR}/tests/opt/rules)
        set_tests_properties(opt_rules_yosys PROPERTIES LABELS opt SKIP_RETURN_CODE 77 TIMEOUT 900)
        # 4.8 Task 2 (spec T-2): fsm-equiv proves Yosys 0.55's own `fsm` on every FSM micro of
        # tests/fsm/micros.txt equivalent to the RTL from reset, so each micro is a sound test;
        # fsm_facts_current: tests/fsm/yosys_facts.json is what this Yosys reports for the corpus.
        add_test(NAME fsm_equiv_selftest
            COMMAND ${PROJECT_SOURCE_DIR}/tools/fsm-check/fsm-equiv
                    --yosys ${ODIN3_YOSYS055} --selftest ${PROJECT_SOURCE_DIR}/tests/fsm/micros.txt)
        # 4.8 Task 10 (spec T-2, T-3): Odin III's `fsm; opt` is equivalent to its `proc` from reset
        # (a bounded miter in Yosys, tests/fsm/micros.txt), and fsm-diff finds the same FSMs,
        # states and encodings as Yosys 0.55 on the same RTLIL for every micro of the corpus.
        add_test(NAME fsm_equiv_micro
            COMMAND ${PROJECT_SOURCE_DIR}/tools/fsm-check/fsm-equiv --yosys ${ODIN3_YOSYS055}
                    --odin $<TARGET_FILE:odin3>
                    --selftest ${PROJECT_SOURCE_DIR}/tests/fsm/micros.txt)
        add_test(NAME fsm_diff_micro
            COMMAND ${PROJECT_SOURCE_DIR}/tools/fsm-check/fsm-diff --yosys ${ODIN3_YOSYS055}
                    --odin $<TARGET_FILE:odin3> --micros)
        set_tests_properties(fsm_equiv_micro fsm_diff_micro PROPERTIES LABELS "unit;micro"
                             TIMEOUT 900)
        add_test(NAME fsm_facts_current
            COMMAND ${PROJECT_SOURCE_DIR}/tools/fsm-check/fsm-facts --check
                    --yosys ${ODIN3_YOSYS055})
        set_tests_properties(fsm_equiv_selftest fsm_facts_current PROPERTIES LABELS "unit;micro"
                             TIMEOUT 300)
        # 2E Task 12: test_proc_gen_diff parses the --proc profile with this Yosys; 2D-7b:
        # test_hard_cell_widths runs write_rtlil's black-box stubs through it and the bridge.
        set_property(TEST python_tools APPEND PROPERTY ENVIRONMENT
                     "ODIN3_YOSYS055=${ODIN3_YOSYS055}")
        # 2D Task 28: test_elab_diff runs const-diff against this Yosys and the replay driver.
        set_property(TEST python_tools APPEND PROPERTY ENVIRONMENT
                     "ODIN3_CONST_REPLAY=$<TARGET_FILE:const-replay>")

        # 2E Task 12 (spec 18.6, plan point (p)): the proc sweeps. Every one is an elab-sweep run
        # (smallest first, one child per file under memcap, expected failures with needles) whose
        # child is proc-diff (--proc-diff) or the bridge (--equiv --pass proc). Two 3G jobs fit the
        # heavy slice's 6G; --trio auto feeds the input after the pre-proc passes once `opt` runs
        # (2F-5) and --no-trio before that. Run: ODIN3_HEAVY_OK=1 ctest --preset sweep -R '^proc_'.
        set(_proc_sweep ${PROJECT_SOURCE_DIR}/tools/elab-check/elab-sweep
            --odin3 $<TARGET_FILE:odin3> --yosys ${ODIN3_YOSYS055}
            --techlib ${PROJECT_SOURCE_DIR}/lib/vtr.o3lib --trio auto --jobs 2 --mem-cap 3G)
        set(_proc_bridge --equiv --pass proc
            --equiv-check ${PROJECT_SOURCE_DIR}/tools/equiv-check/equiv-check)
        set(_micro_dir ${PROJECT_SOURCE_DIR}/tests/micro/verilog)
        set(_micro_expect --expect-fail ${PROJECT_SOURCE_DIR}/tests/proc/micro_expect_fail.txt
            --root ${_micro_dir})
        odin3_add_sweep_test(proc_diff_micro LABEL sweep
            COMMAND ${_proc_sweep} --proc-diff ${_micro_expect} ${_micro_dir})
        odin3_add_sweep_test(proc_bridge_micro LABEL sweep
            COMMAND ${_proc_sweep} ${_proc_bridge} ${_micro_expect}
                    --expect-fail ${PROJECT_SOURCE_DIR}/tests/proc/micro_bridge_expect_fail.txt
                    ${_micro_dir})
        # The three Parmys rejects that 2E lowers, and the typo-fixed boundtop standing in for
        # the 2D reject (tests/proc/paj_boundtop_typo_fixed.v), lower and are identical in both
        # input modes (--also-no-trio).
        odin3_add_sweep_test(proc_diff_micro_lowers LABEL sweep
            COMMAND ${_proc_sweep} --proc-diff --also-no-trio
                    ${_micro_dir}/large/paj_framebuftop_hierarchy_no_mem.v
                    ${_micro_dir}/large/paj_raygentop_hierarchy_no_mem.v
                    ${_micro_dir}/syntax/diffeq_f_systemC.v
                    ${PROJECT_SOURCE_DIR}/tests/proc/paj_boundtop_typo_fixed.v)
        # The generator (200 seeds; nightly runs 5,000): proc-diff in both latch modes and the
        # bridge in --nolatches. A difference fails with its seed and file kept under the build
        # directory.
        odin3_add_sweep_test(proc_gen_vs_yosys LABEL sweep
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/tools/proc-check/proc_gen_diff.py
                    --odin3 $<TARGET_FILE:odin3> --yosys ${ODIN3_YOSYS055}
                    --equiv-check ${PROJECT_SOURCE_DIR}/tools/equiv-check/equiv-check
                    --seeds 1..200 --mem-cap 3G --work ${PROJECT_BINARY_DIR}/proc_gen_work)
        # 2F Task 17 (spec 20.2, 20.3, point (m), (o)): the opt evidence tools, through
        # elab-sweep's --opt-equiv / --opt-diff modes (one tool child per file, smallest first,
        # under memcap; the micros that 2D or proc reject are tests/proc/micro_expect_fail.txt).
        # opt_equiv_rules: opt-equiv on every per-rule source (the refinement of each sub-pass call
        # of the Phase 2 script, not a sweep). Every source is built to make a rule fire, so
        # `--min-changed 1` fails a file in which no call changed anything (a broken snapshot);
        # the 19 sources whose rule the Phase 2 script does not run are in
        # tests/opt/rules_equiv_expect_fail.txt. The sweep reads `.v` sources only: the 5 `.blif`
        # sources (expr/mux_to_inv, gate_and, gate_or, gate_mux, gate_not) are out of scope,
        # since opt-equiv's front is `read_verilog`; opt-rules' per-row refinement checks them
        # (it runs `read_blif`). Run: ODIN3_HEAVY_OK=1 ctest --preset sweep -R
        # '^opt_(equiv|diff)_micro$'.
        set(_opt_sweep ${PROJECT_SOURCE_DIR}/tools/elab-check/elab-sweep
            --odin3 $<TARGET_FILE:odin3> --yosys ${ODIN3_YOSYS055}
            --techlib ${PROJECT_SOURCE_DIR}/lib/vtr.o3lib --jobs 2 --mem-cap 2G)
        # --jobs 1: outside tools/heavy-run, fd 9 of a threaded elab-sweep is Python's own.
        set(_opt_rules_dir ${PROJECT_SOURCE_DIR}/tests/opt/rules)
        add_test(NAME opt_equiv_rules
            COMMAND ${_opt_sweep} --jobs 1 --opt-equiv --opt-arg=--min-changed=1
                    --root ${_opt_rules_dir}
                    --expect-fail ${PROJECT_SOURCE_DIR}/tests/opt/rules_equiv_expect_fail.txt
                    ${_opt_rules_dir}/clean ${_opt_rules_dir}/dff ${_opt_rules_dir}/expr
                    ${_opt_rules_dir}/merge ${_opt_rules_dir}/muxtree ${_opt_rules_dir}/reduce
                    ${_opt_rules_dir}/share)
        set_tests_properties(opt_equiv_rules PROPERTIES LABELS opt SKIP_RETURN_CODE 77
                             TIMEOUT 1800)
        # 3G (plan Task 3 step 3): the same on the wreduce sources with the Phase 3 script, which
        # runs wreduce; every call that changes something is checked (clock-aware) in turn.
        # PHASE3 #49: opt_merge -ffbits against Yosys (techmap; opt -fast leaves the same bits) and
        # its refinement (opt-equiv on the Phase 3 script plus the step) on tests/opt/ffbits.
        add_test(NAME opt_ffbits_yosys
            COMMAND ${PROJECT_SOURCE_DIR}/tools/opt-check/ffbits-check
                    --odin3 $<TARGET_FILE:odin3> --yosys ${ODIN3_YOSYS055})
        set_tests_properties(opt_ffbits_yosys PROPERTIES LABELS opt TIMEOUT 600)
        add_test(NAME opt_equiv_ffbits
            COMMAND ${_opt_sweep} --jobs 1 --opt-equiv --opt-arg=--min-changed=1
                    --opt-arg=--script=${PROJECT_SOURCE_DIR}/tools/opt-check/ffbits.script
                    --root ${PROJECT_SOURCE_DIR}/tests/opt/ffbits
                    --expect-fail ${PROJECT_SOURCE_DIR}/tests/opt/ffbits_equiv_expect_fail.txt
                    ${PROJECT_SOURCE_DIR}/tests/opt/ffbits)
        set_tests_properties(opt_equiv_ffbits PROPERTIES LABELS opt SKIP_RETURN_CODE 77
                             TIMEOUT 1800)
        add_test(NAME opt_equiv_wreduce_rules
            COMMAND ${_opt_sweep} --jobs 1 --opt-equiv --opt-arg=--min-changed=1
                    --opt-arg=--script=${PROJECT_SOURCE_DIR}/tools/opt-check/phase3.script
                    --root ${_opt_rules_dir}
                    --expect-fail ${PROJECT_SOURCE_DIR}/tests/opt/wreduce_equiv_expect_fail.txt
                    ${_opt_rules_dir}/wreduce)
        set_tests_properties(opt_equiv_wreduce_rules PROPERTIES LABELS opt SKIP_RETURN_CODE 77
                             TIMEOUT 1800)
        # PHASE3 #52 (opt_share parity): the opt_share sources with the Phase 3 script, whose
        # `opt -full` runs opt_share; every call that changes something is checked in turn, and
        # opt_share must change something in every source (--require-sub: --min-changed counts
        # calls of any sub-pass, and wreduce or opt_expr alone would satisfy it).
        add_test(NAME opt_equiv_share_rules
            COMMAND ${_opt_sweep} --jobs 1 --opt-equiv --opt-arg=--min-changed=1
                    --opt-arg=--require-sub=opt_share
                    --opt-arg=--script=${PROJECT_SOURCE_DIR}/tools/opt-check/phase3.script
                    --root ${_opt_rules_dir} ${_opt_rules_dir}/share)
        set_tests_properties(opt_equiv_share_rules PROPERTIES LABELS opt SKIP_RETURN_CODE 77
                             TIMEOUT 1800)
        # 3G (plan Task 3 step 4): Odin III's wreduce and Phase 3 script against Yosys's on the
        # same input: cells, widths and flags of every $add $sub $mul, refinement, identity.
        add_test(NAME opt_diff_wreduce_rules
            COMMAND ${_opt_sweep} --jobs 1 --opt-diff
                    --opt-arg=--script=wreduce --opt-arg=--script=phase3
                    --opt-arg=--expect=${PROJECT_SOURCE_DIR}/tests/opt/diff_expect.txt
                    --opt-arg=--root=${_opt_rules_dir} --root ${_opt_rules_dir}
                    ${_opt_rules_dir}/wreduce)
        set_tests_properties(opt_diff_wreduce_rules PROPERTIES LABELS opt SKIP_RETURN_CODE 77
                             TIMEOUT 1800)
        # WR-7 amended (PHASE3 #51): the visiting order. tests/opt/order holds Verilog whose result
        # depends on the order of the cells. CIRCULAR on the order: Yosys reads Odin III's file
        # through il_order.py, which ranks cells with the same walk as wreduce_order.c, so this
        # proves that the two agree and that wreduce refines Yosys's result for that order, not that
        # the walk is Yosys's front-end order. That is measured by hardmap-diff on LU8PEEng,
        # LU32PEEng and mcml against the Parmys goldens (PHASE3 #51). Both scripts run; the one
        # difference of `wreduce` alone (cell names, two buffers) is listed in diff_expect.txt with
        # its cause.
        add_test(NAME opt_diff_wreduce_order
            COMMAND ${_opt_sweep} --jobs 1 --opt-diff
                    --opt-arg=--script=wreduce --opt-arg=--script=phase3
                    --opt-arg=--expect=${PROJECT_SOURCE_DIR}/tests/opt/diff_expect.txt
                    --opt-arg=--root=${PROJECT_SOURCE_DIR}/tests/opt
                    --root ${PROJECT_SOURCE_DIR}/tests/opt
                    ${PROJECT_SOURCE_DIR}/tests/opt/order)
        set_tests_properties(opt_diff_wreduce_order PROPERTIES LABELS opt SKIP_RETURN_CODE 77
                             TIMEOUT 600)
        # A forward reference (a net with a wire is not followed): Odin III keeps node-ID order and,
        # like Yosys's front end, leaves the cells wide. Script wreduce alone: after `proc` and
        # compact the Phase 3 names differ from Yosys's for a reason that is not wreduce's.
        add_test(NAME opt_diff_wreduce_forward
            COMMAND ${_opt_sweep} --jobs 1 --opt-diff
                    --opt-arg=--script=wreduce
                    --opt-arg=--expect=${PROJECT_SOURCE_DIR}/tests/opt/diff_expect.txt
                    --opt-arg=--root=${PROJECT_SOURCE_DIR}/tests/opt
                    --root ${PROJECT_SOURCE_DIR}/tests/opt
                    ${PROJECT_SOURCE_DIR}/tests/opt/order_forward)
        set_tests_properties(opt_diff_wreduce_forward PROPERTIES LABELS opt SKIP_RETURN_CODE 77
                             TIMEOUT 600)
        # --cap-for: syntax/inferred_ram_w_clog2.v (1479 B, a 3776-latch RAM) needs 2.3 GB for its base
        # case from the initial state, over the 2G child cap; it proves at 4G under both scripts (2H).
        odin3_add_sweep_test(opt_equiv_micro LABEL sweep
            COMMAND ${_opt_sweep} --opt-equiv --min-changed-total 200 ${_micro_expect}
                    --cap-for syntax/inferred_ram_w_clog2.v=4G
                    --expect-fail ${PROJECT_SOURCE_DIR}/tests/opt/micro_expect_fail.txt
                    --expect-fail ${PROJECT_SOURCE_DIR}/tests/opt/equiv_expect_fail.txt
                    --expect-fail ${PROJECT_SOURCE_DIR}/tests/opt/equiv_phase2_expect_fail.txt
                    ${_micro_dir})
        # --min-changed-total 200: the 441 micros under 1100 B alone have 266 changing calls, so
        # a do-nothing odin3 (0 on every design) fails (2F-6 re-review N3).
        set_tests_properties(opt_equiv_micro PROPERTIES LABELS "sweep;opt_equiv")
        # 3B Task 9 (spec 14.4): lower-equiv. The mutants of spec 14.4 as unittest cases; the
        # per-cell fixtures (the mul/div/mod fixtures above 3 bits are SAT-heavy: their proofs
        # run past the 60 s limit and are left to lower-diff's tier T); the falling-edge memory
        # with --neg-edge keep (equiv-check does not take an inverted clock, as in lower-diff).
        add_test(NAME lower_equiv_python
            COMMAND ${CMAKE_COMMAND} -E env ODIN3_CLI=$<TARGET_FILE:odin3>
                    ODIN3_YOSYS055=${ODIN3_YOSYS055} VTR_ROOT=$ENV{VTR_ROOT}
                    ${Python3_EXECUTABLE} -m unittest tests.tools.test_lower_equiv
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
        set_tests_properties(lower_equiv_python PROPERTIES LABELS lower TIMEOUT 900)
        set(_lower_equiv_files ${_lower_cells})
        list(FILTER _lower_equiv_files EXCLUDE REGEX "/cells/(mul|div|mod)_w([4-9]|1[0-9])")
        list(FILTER _lower_equiv_files EXCLUDE REGEX "/cells/mem_w4_d4_neg[.]v$")
        set(_lower_equiv_neg ${PROJECT_SOURCE_DIR}/tests/lower/cells/mem_w4_d4_neg.v)
        add_test(NAME lower_cells_equiv
            COMMAND ${PROJECT_SOURCE_DIR}/tools/lower-check/lower-equiv
                    --odin3 $<TARGET_FILE:odin3> --yosys ${ODIN3_YOSYS055}
                    --pre proc --expect ${PROJECT_SOURCE_DIR}/tests/lower/equiv_expect.txt
                    --root ${PROJECT_SOURCE_DIR}/tests/lower/cells --each ${_lower_equiv_files})
        add_test(NAME lower_cells_equiv_neg_keep
            COMMAND ${PROJECT_SOURCE_DIR}/tools/lower-check/lower-equiv
                    --odin3 $<TARGET_FILE:odin3> --yosys ${ODIN3_YOSYS055}
                    --pre proc --lower-args "--neg-edge keep" ${_lower_equiv_neg})
        set_tests_properties(lower_cells_equiv lower_cells_equiv_neg_keep
                             PROPERTIES LABELS lower SKIP_RETURN_CODE 77 TIMEOUT 1800)
        # 5.2 (spec sections 7 to 9, tools/icells-check). What each test proves:
        #  - icells_inst, icells_insttext: hand-written netlists of `$` instances, one file per cell
        #    type the writer emits as an instance (16 types), read with -icells, written, read
        #    again; every file must hold its type (--require-dollar and the file name).
        #  - icells_cells_word, icells_cells_lpm: Verilog designs whose word-level netlist has
        #    `$` instances (memory, $shiftx, $pow, set/reset flip-flops, latches); the written
        #    Verilog reads back with -icells, equivalent (Yosys 0.55 refinement).
        #  - icells_lpm_cells: the 5.1 LPM micros; the lpm_*/altsyncram library instances read back.
        #  - icells_plain_gate, icells_plain_lut: NO `$` instance occurs at the bit levels (every
        #    gate is an assign or an always block), so these prove the plain Verilog read-back of
        #    the writer's output (BLIF equiv-check), not -icells.
        #  - icells_micros_<level> (label sweep): the same over the 68 micros of
        #    tests/micro/verilog/micro, a few minutes per level in Debug; run with
        #    `ctest --test-dir build/debug -R icells_micros`.
        # They need Yosys 0.55 (the CI image has none), so they are local-only; the unit tests
        # (test_icells, test_icells_roundtrip) and tests/tools (python_tools) run in CI.
        # tests/tools/test_icells_check.py runs a mutated read-back end to end when Yosys 0.55 is known.
        set_property(TEST python_tools APPEND PROPERTY ENVIRONMENT "ODIN3_YOSYS055=${ODIN3_YOSYS055}")
        file(GLOB _icells_cells ${PROJECT_SOURCE_DIR}/tests/icells/cell_*.v)
        file(GLOB _icells_plain ${PROJECT_SOURCE_DIR}/tests/icells/plain_*.v)
        file(GLOB _icells_inst ${PROJECT_SOURCE_DIR}/tests/icells/inst/inst_*.v)
        file(GLOB _icells_insttext ${PROJECT_SOURCE_DIR}/tests/icells/inst_text/inst_*.v)
        file(GLOB _icells_micros ${PROJECT_SOURCE_DIR}/tests/micro/verilog/micro/*.v)
        set(_icells_lpm_cells ${PROJECT_SOURCE_DIR}/tests/lpm/lpm_add_sub.v
            ${PROJECT_SOURCE_DIR}/tests/lpm/lpm_compare.v ${PROJECT_SOURCE_DIR}/tests/lpm/lpm_mult.v
            ${PROJECT_SOURCE_DIR}/tests/lpm/lpm_mixed.v ${PROJECT_SOURCE_DIR}/tests/lpm/altsyncram_sp.v
            ${PROJECT_SOURCE_DIR}/tests/lpm/altsyncram_sdp.v)
        list(SORT _icells_cells)
        list(SORT _icells_plain)
        list(SORT _icells_inst)
        list(SORT _icells_insttext)
        list(SORT _icells_micros)
        file(MAKE_DIRECTORY ${PROJECT_BINARY_DIR}/icells_tmp)
        set(_icells_run ${CMAKE_COMMAND} -E env TMPDIR=${PROJECT_BINARY_DIR}/icells_tmp
            ODIN3_ABC=$<TARGET_FILE:odin3_abc_exe>
            ${PROJECT_SOURCE_DIR}/tools/icells-check/icells-check
            --odin3 $<TARGET_FILE:odin3> --yosys ${ODIN3_YOSYS055}
            --expect ${PROJECT_SOURCE_DIR}/tests/icells/expect.txt)
        function(odin3_add_icells_test name)
            cmake_parse_arguments(ARG "" "LEVEL;LABELS" "FLAGS;FILES" ${ARGN})
            add_test(NAME ${name}
                COMMAND ${_icells_run} --level ${ARG_LEVEL} ${ARG_FLAGS} ${ARG_FILES})
            set_tests_properties(${name} PROPERTIES LABELS "${ARG_LABELS}" SKIP_RETURN_CODE 77
                                 TIMEOUT 1800)
        endfunction()
        odin3_add_icells_test(icells_inst LEVEL inst LABELS "icells;unit"
            FLAGS --require-dollar FILES ${_icells_inst})
        odin3_add_icells_test(icells_insttext LEVEL insttext LABELS "icells;unit"
            FLAGS --require-dollar FILES ${_icells_insttext})
        odin3_add_icells_test(icells_cells_word LEVEL word LABELS "icells;unit"
            FLAGS --require-dollar FILES ${_icells_cells})
        odin3_add_icells_test(icells_cells_lpm LEVEL lpm LABELS "icells;unit"
            FLAGS --require-dollar FILES ${_icells_cells})
        odin3_add_icells_test(icells_lpm_cells LEVEL lpm LABELS "icells;unit"
            FLAGS --require-lib FILES ${_icells_lpm_cells})
        odin3_add_icells_test(icells_lpm_soft LEVEL lpm LABELS "icells;unit"
            FLAGS --require-dollar FILES ${PROJECT_SOURCE_DIR}/tests/lpm/altsyncram_soft.v)
        foreach(_lvl IN ITEMS word lpm)
            odin3_add_icells_test(icells_micros_${_lvl} LEVEL ${_lvl} LABELS "sweep;icells"
                FILES ${_icells_micros})
        endforeach()
        if(ODIN3_WITH_ABC)
            foreach(_lvl IN ITEMS gate lut)
                odin3_add_icells_test(icells_plain_${_lvl} LEVEL ${_lvl} LABELS "icells;unit"
                    FILES ${_icells_plain} ${_icells_cells})
                odin3_add_icells_test(icells_micros_${_lvl} LEVEL ${_lvl} LABELS "sweep;icells"
                    FILES ${_icells_micros})
            endforeach()
        endif()
        # 3B Task 9 (spec 14.4, 14.5): the lower corpus over the micros, smallest first, through
        # heavy-run: lower_run_micro (ODIN3_OK, check FULL, the netlist view written as BLIF),
        # lower_sim_micro (the simulator before and after; a type without a hook is `skipped`,
        # never `pass`) and lower_equiv_micro (lower refines its input, in Yosys 0.55). A micro
        # `opt` folds to no word cell compares nothing (--allow-empty). The micros that 2D, proc or
        # opt reject are in the other lists; tests/lower/micro_expect_fail.txt lists lower's own.
        set(_lower_micro_expect ${_micro_expect}
            --expect-fail ${PROJECT_SOURCE_DIR}/tests/lower/micro_expect_fail.txt
            --expect-fail ${PROJECT_SOURCE_DIR}/tests/opt/micro_expect_fail.txt)
        # --min-compared-total (3B-4 review H3): every tool prints `compared N of M designs`; a
        # skipped, folded or unrun design adds 0, and a sweep below its floor fails, so an odin3
        # that lowers nothing (or a tool that skips everything) cannot pass. The floors are a
        # third to a half of the rate measured on the 58 bm_* micros by hand (2026-10-18:
        # lower-run 56 of 58, lower-sim 24 of 58 (the rest hold a type without a simulate hook,
        # which --allow-skipped lets through), lower-equiv 38 of 40 ok) applied to the about 450
        # micros the other lists leave; the controller raises them after the first full sweep.
        # First full sweep (release build, 2026-10-18, head 5cf3d1f5; 539 micros): lower-run
        # compared 330, lower-sim 96, lower-equiv 265 (of which 18 were reported "not a
        # refinement" by the checker limits fixed in 3B-heavy-triage, so 247 were proved). Floors
        # of that round: 300, 90 and 240. 3B close (main 12473742 after the soundness fixes,
        # 2026-10-18): lower-run 330 compared, lower-sim 96, lower-equiv 261. Expected after 3B close:
        # 261 + 4 (base-case false differences now ok) + 4 (--timeout-for) - 1 (multi_edge, now
        # unproven) = 268; the 3 "not a refinement" fixed were already counted. Floors: the measured
        # counts for lower-run and lower-sim (330, 96: no margin, a new expect entry trips them) and
        # 260 for lower-equiv (the 268 expected less a margin, until the controller's rerun gives the number).
        odin3_add_sweep_test(lower_run_micro LABEL sweep
            COMMAND ${_opt_sweep} --lower-run --min-compared-total 330 ${_lower_micro_expect}
                    --expect-fail ${PROJECT_SOURCE_DIR}/tests/lower/run_micro_expect_fail.txt
                    ${_micro_dir})
        odin3_add_sweep_test(lower_sim_micro LABEL sweep
            COMMAND ${_opt_sweep} --lower-sim --opt-arg=--allow-empty --opt-arg=--allow-skipped
                    --min-compared-total 96 ${_lower_micro_expect} ${_micro_dir})
        # equiv_micro_expect_fail.txt: the micros lower-equiv cannot decide (a SAT size limit)
        # --timeout-for: the files that prove in 100 to 140 s alone (3B close; the default SAT
        # limit is 60 s); a file that needs more is listed in equiv_micro_expect_fail.txt.
        odin3_add_sweep_test(lower_equiv_micro LABEL sweep
            COMMAND ${_opt_sweep} --lower-equiv --opt-arg=--allow-empty
                    --timeout-for micro/bm_match3_str_arch.v=300
                    --timeout-for full/cf_fir_3_8_8.v=300
                    --timeout-for syntax/rs_decoder_1.v=300
                    --timeout-for full/cf_cordic_v_18_18_18.v=300
                    --min-compared-total 260 ${_lower_micro_expect}
                    --expect-fail ${PROJECT_SOURCE_DIR}/tests/lower/equiv_micro_expect_fail.txt
                    ${_micro_dir})
        set_tests_properties(lower_run_micro lower_sim_micro lower_equiv_micro
                             PROPERTIES LABELS "sweep;lower")
        # opt_diff_micro: every micro with the Phase 2 script and `opt -full`; a difference fails
        # unless tests/opt/diff_expect.txt lists it with a reason.
        odin3_add_sweep_test(opt_diff_micro LABEL sweep
            COMMAND ${_opt_sweep} --opt-diff ${_micro_expect}
                    --expect-fail ${PROJECT_SOURCE_DIR}/tests/opt/micro_expect_fail.txt
                    --expect-fail ${PROJECT_SOURCE_DIR}/tests/opt/diff_micro_expect_fail.txt
                    --expect-fail ${PROJECT_SOURCE_DIR}/tests/opt/diff_micro_phase2_expect_fail.txt
                    --opt-arg=--script=phase2 "--opt-arg=--script=opt -full"
                    --opt-arg=--expect=${PROJECT_SOURCE_DIR}/tests/opt/diff_expect.txt
                    --opt-arg=--root=${_micro_dir} ${_micro_dir})
        # 3G: every micro with the Phase 3 script (opt-equiv: every call that changes something is
        # checked, wreduce included) and with `wreduce` and the Phase 3 script against Yosys.
        odin3_add_sweep_test(opt_equiv_micro_phase3 LABEL sweep
            COMMAND ${_opt_sweep} --opt-equiv --min-changed-total 200 ${_micro_expect}
                    --cap-for syntax/inferred_ram_w_clog2.v=4G
                    # WR-7 (PHASE3 #51): the wreduce call of des_area refines but takes 60 to 600 s
                    # of SAT with the new visiting order (main: under 60 s); proved at 600 s.
                    --timeout-for large/des_area.v=600
                    --opt-arg=--script=${PROJECT_SOURCE_DIR}/tools/opt-check/phase3.script
                    --expect-fail ${PROJECT_SOURCE_DIR}/tests/opt/micro_expect_fail.txt
                    --expect-fail ${PROJECT_SOURCE_DIR}/tests/opt/equiv_expect_fail.txt
                    --expect-fail ${PROJECT_SOURCE_DIR}/tests/opt/equiv_phase3_expect_fail.txt
                    ${_micro_dir})
        set_tests_properties(opt_equiv_micro_phase3 PROPERTIES LABELS "sweep;opt_equiv")
        odin3_add_sweep_test(opt_diff_micro_wreduce LABEL sweep
            COMMAND ${_opt_sweep} --opt-diff ${_micro_expect}
                    --expect-fail ${PROJECT_SOURCE_DIR}/tests/opt/micro_expect_fail.txt
                    --expect-fail ${PROJECT_SOURCE_DIR}/tests/opt/diff_micro_expect_fail.txt
                    --expect-fail ${PROJECT_SOURCE_DIR}/tests/opt/diff_micro_wreduce_expect_fail.txt
                    # WR-7 (PHASE3 #51): these refine, but not within 60 s any more (proved at 600 s)
                    --timeout-for full/iir1.v=600 --timeout-for large/spree.v=600
                    --timeout-for full/cf_fft_256_8.v=600
                    --opt-arg=--script=wreduce --opt-arg=--script=phase3
                    --opt-arg=--expect=${PROJECT_SOURCE_DIR}/tests/opt/diff_expect.txt
                    --opt-arg=--root=${_micro_dir} ${_micro_dir})
        # opt_order_probe: spec 3's pass-order evidence (point (o)); evidence only, always exit 0.
        odin3_add_sweep_test(opt_order_probe LABEL sweep
            COMMAND ${PROJECT_SOURCE_DIR}/tools/opt-check/order-probe.sh --yosys ${ODIN3_YOSYS055}
                    --limit 60 ${_micro_dir})
        set_tests_properties(opt_order_probe PROPERTIES LABELS "sweep;nightly")
        # 2F Task 18 (spec 20.4): the corpus sweeps. opt_micro: the Phase 2 script on every micro,
        # Odin III only (ODIN3_OK, check FULL, iterations below the cap; the largest count is
        # printed). The micros that 2D or proc reject are tests/proc/micro_expect_fail.txt. One
        # child at a time under 3G. Run: ODIN3_HEAVY_OK=1 ctest --preset sweep -R '^opt_micro$'.
        set(_opt_run_sweep ${PROJECT_SOURCE_DIR}/tools/elab-check/elab-sweep
            --odin3 $<TARGET_FILE:odin3> --opt-run
            --techlib ${PROJECT_SOURCE_DIR}/lib/vtr.o3lib --jobs 1 --mem-cap 3G)
        odin3_add_sweep_test(opt_micro LABEL sweep
            COMMAND ${_opt_run_sweep} ${_micro_expect} ${_micro_dir})
        # opt_gen_vs_yosys: 200 seeds of the --opt generator through opt-diff's steps 3-5 (the
        # Phase 2 script and `opt -full`); nightly runs 5,000 and the same files against Yosys
        # 0.69 (--yosys-069: a disagreement is `oracle drift`, reported, never a failure).
        odin3_add_sweep_test(opt_gen_vs_yosys LABEL sweep
            COMMAND ${PROJECT_SOURCE_DIR}/tools/opt-check/opt-gen-diff
                    --odin3 $<TARGET_FILE:odin3> --yosys ${ODIN3_YOSYS055}
                    --seeds 1..200 --jobs 1 --mem-cap 3G
                    --expect-structural ${PROJECT_SOURCE_DIR}/tests/opt/gen_structural_expect.txt
                    --work ${PROJECT_BINARY_DIR}/opt_gen_work)
        # 3B Task 9 (spec 14.5, decision 47): lower_gen_vs_yosys, 200 seeds of the --lower generator
        # through lower-diff (tier by kind) and lower-equiv. Strict: a NOT A REFINEMENT is a bug in
        # lower and is never listed; an unproven seed fails (--strict-unproven; 0 measured); the
        # tier-T histogram differences of seeds 1..200 (equivalent, a few gates apart after
        # `opt -fast`) are the list tests/lower/gen_diff_expect.txt, checked by step and gate delta.
        # lower_gen_vs_yosys_long (5,000 seeds; shipped out to the bigger machine, PHASE3 decision
        # 54, tools/vtr-qor/offload/run-offload; not run locally) keeps equivalence
        # (lower-equiv, and lower-diff's equiv step) the pass criterion and only reports the
        # histogram differences (--tier-t report): about 4% of the comb seeds, not listed.
        set(_lower_gen ${PROJECT_SOURCE_DIR}/tools/lower-check/lower-gen-diff
            --odin3 $<TARGET_FILE:odin3> --yosys ${ODIN3_YOSYS055} --mem-cap 3G
            --expect ${PROJECT_SOURCE_DIR}/tests/lower/gen_diff_expect.txt)
        odin3_add_sweep_test(lower_gen_vs_yosys LABEL sweep
            COMMAND ${_lower_gen} --seeds 1..200 --min-compared 190 --strict-unproven
                    --work ${PROJECT_BINARY_DIR}/lower_gen_work)
        odin3_add_sweep_test(lower_gen_vs_yosys_long LABEL sweep
            COMMAND ${_lower_gen} --seeds 1..5000 --min-compared 4500 --tier-t report
                    --work ${PROJECT_BINARY_DIR}/lower_gen_long_work)
        set_tests_properties(lower_gen_vs_yosys PROPERTIES LABELS "sweep;lower")
        set_tests_properties(lower_gen_vs_yosys_long PROPERTIES LABELS "sweep;lower;nightly")
        # 2D Task 28 (spec 14.5, review M24): the --elab generator's modules through the bridge
        # (elab-diff, 200 seeds) and 500 random constant expressions, odin3_cval_op against Yosys
        # 0.55's folds (const-diff). The nightly runs are the *_long tests: 5,000 seeds, 10,000
        # expressions, and the 0.69 drift (a file where 0.55 and 0.69 disagree is `oracle drift`,
        # not a failure). A failing seed keeps its file under the build directory.
        odin3_add_sweep_test(elab_gen_vs_yosys LABEL sweep
            COMMAND ${PROJECT_SOURCE_DIR}/tools/elab-check/elab-diff
                    --odin3 $<TARGET_FILE:odin3> --yosys ${ODIN3_YOSYS055}
                    --equiv-check ${PROJECT_SOURCE_DIR}/tools/equiv-check/equiv-check
                    --seeds 1..200 --mem-cap 2G
                    --expect-fail ${PROJECT_SOURCE_DIR}/tests/elab/gen_expect_fail.txt
                    --work ${PROJECT_BINARY_DIR}/elab_gen_work)
        odin3_add_sweep_test(elab_const_diff LABEL sweep
            COMMAND ${PROJECT_SOURCE_DIR}/tools/elab-check/const-diff
                    --yosys ${ODIN3_YOSYS055} --replay $<TARGET_FILE:const-replay>
                    --count 500 --seed 1)
        odin3_add_sweep_test(elab_gen_vs_yosys_long LABEL sweep
            COMMAND ${PROJECT_SOURCE_DIR}/tools/elab-check/elab-diff
                    --odin3 $<TARGET_FILE:odin3> --yosys ${ODIN3_YOSYS055}
                    --equiv-check ${PROJECT_SOURCE_DIR}/tools/equiv-check/equiv-check
                    --seeds 1..5000 --mem-cap 2G
                    --expect-fail ${PROJECT_SOURCE_DIR}/tests/elab/gen_expect_fail.txt
                    --work ${PROJECT_BINARY_DIR}/elab_gen_long_work)
        odin3_add_sweep_test(elab_const_diff_long LABEL sweep
            COMMAND ${PROJECT_SOURCE_DIR}/tools/elab-check/const-diff
                    --yosys ${ODIN3_YOSYS055} --replay $<TARGET_FILE:const-replay>
                    --count 10000 --seed 2)
        set_tests_properties(elab_gen_vs_yosys_long elab_const_diff_long
                             PROPERTIES LABELS "sweep;nightly")
        if(ODIN3_YOSYS)
            odin3_add_sweep_test(elab_gen_drift LABEL sweep
                COMMAND ${PROJECT_SOURCE_DIR}/tools/elab-check/elab-diff
                        --odin3 $<TARGET_FILE:odin3> --yosys ${ODIN3_YOSYS055}
                        --equiv-check ${PROJECT_SOURCE_DIR}/tools/equiv-check/equiv-check
                        --seeds 1..200 --mem-cap 2G --yosys-069 ${ODIN3_YOSYS}
                        --work ${PROJECT_BINARY_DIR}/elab_gen_drift_work)
            set_tests_properties(elab_gen_drift PROPERTIES LABELS "sweep;nightly")
            # 2D Task 28 follow-up (spec 17 #82): the sources of tests/verilog/oracle-bug lose a
            # select write in Yosys 0.55's opt_muxtree (2F OP-13) though 2D's RTLIL is right:
            # elab-equiv is equivalent on Yosys 0.69 and NOT equivalent (exit 1) on 0.55. A 0.55
            # that passes makes the test fail (stale: move the sources to case-bridge). Exit 77
            # (skip) as above.
            add_test(NAME elab_oracle_muxtree_bug
                COMMAND sh -c "for f in \"$5\"/*.v; do \"$0\" --odin3 \"$1\" --yosys \"$2\" --allow-version 0.69 --equiv-check \"$3\" --top gtop \"$f\" || exit $?; \"$0\" --odin3 \"$1\" --yosys \"$4\" --equiv-check \"$3\" --top gtop \"$f\"; rc=$?; [ $rc -eq 77 ] && exit 77; [ $rc -eq 1 ] || { echo \"$f: Yosys 0.55 no longer loses the write (exit $rc)\"; exit 1; }; done"
                        ${PROJECT_SOURCE_DIR}/tools/elab-check/elab-equiv
                        $<TARGET_FILE:odin3> ${ODIN3_YOSYS}
                        ${PROJECT_SOURCE_DIR}/tools/equiv-check/equiv-check
                        ${ODIN3_YOSYS055}
                        ${PROJECT_SOURCE_DIR}/tests/verilog/oracle-bug)
            set_tests_properties(elab_oracle_muxtree_bug PROPERTIES
                LABELS differential SKIP_RETURN_CODE 77)
        endif()
        # The 31 VTR and 33 Koios benchmarks as one set by size, when VTR_ROOT is set at
        # configure time (also label nightly). lenet.v is listed `xl` in vtr_expect_fail.txt: the
        # 6G sweeps skip it, and the sweep-xl CTests run exactly it, alone, 8 GiB per child.
        if(_vtr_benchmarks)
            set(_vtr_expect --expect-fail ${PROJECT_SOURCE_DIR}/tests/proc/vtr_expect_fail.txt
                --root ${_vtr_verilog})
            set(_vtr_xl --xl --cap-for koios/lenet.v=8G --jobs 1)
            odin3_add_sweep_test(proc_diff_vtr LABEL sweep
                COMMAND ${_proc_sweep} --proc-diff ${_vtr_expect} ${_vtr_benchmarks})
            set(_vtr_bridge_expect
                --expect-fail ${PROJECT_SOURCE_DIR}/tests/proc/vtr_bridge_expect_fail.txt)
            odin3_add_sweep_test(proc_bridge_vtr LABEL sweep
                COMMAND ${_proc_sweep} ${_proc_bridge} ${_vtr_expect} ${_vtr_bridge_expect}
                        ${_vtr_benchmarks})
            odin3_add_sweep_test(proc_diff_vtr_xl LABEL sweep-xl
                COMMAND ${_proc_sweep} --proc-diff ${_vtr_xl} ${_vtr_expect} ${_vtr_benchmarks})
            odin3_add_sweep_test(proc_bridge_vtr_xl LABEL sweep-xl
                COMMAND ${_proc_sweep} ${_proc_bridge} ${_vtr_xl} ${_vtr_expect}
                        ${_vtr_bridge_expect} ${_vtr_benchmarks})
            set_tests_properties(proc_diff_vtr proc_bridge_vtr PROPERTIES LABELS "sweep;nightly")
            # 2F Task 18: the Phase 2 script on the 64 files as one set by size, Odin III only
            # (also nightly). lenet.v runs last, in the 6G slice, under a 5G child cap: unlike the
            # proc differentials there is no Yosys side to push it to sweep-xl.
            odin3_add_sweep_test(opt_vtr LABEL sweep
                COMMAND ${_opt_run_sweep}
                        --expect-fail ${PROJECT_SOURCE_DIR}/tests/opt/vtr_expect_fail.txt
                        --root ${_vtr_verilog} --cap-for koios/lenet.v=5G ${_vtr_benchmarks})
            set_tests_properties(opt_vtr PROPERTIES LABELS "sweep;nightly")
            # 3A Task 5 (spec 14.6): the 19 VTR designs of the exit test through flatten, smallest
            # first, one child at a time: wall, peak RSS, flat sizes, kept (must be 0), the node
            # counts against the instance tree, bytes per object, and flatten-diff against Yosys
            # 0.55 (names, widths, cell types by module; tests/flatten/vtr_expect.txt lists known
            # differences with reasons). The Markdown table is written to
            # ${PROJECT_BINARY_DIR}/flatten_vtr_table.md. Run: ODIN3_HEAVY_OK=1 ctest --preset
            # sweep -R '^flatten_vtr_sweep$' from a Release build (the peak RSS needs one).
            set(_flatten_vtr ${PROJECT_SOURCE_DIR}/tools/flatten-check/flatten-vtr
                --odin3 $<TARGET_FILE:odin3> --root ${_vtr_verilog}
                --techlib ${PROJECT_SOURCE_DIR}/lib/vtr.o3lib --mem-cap 3G
                --cap-for mcml.v=5G --cap-for LU32PEEng.v=5G --cap-for bgm.v=5G
                --expect ${PROJECT_SOURCE_DIR}/tests/flatten/vtr_expect.txt
                --table ${PROJECT_BINARY_DIR}/flatten_vtr_table.md)
            if(ODIN3_YOSYS055_OK)
                list(APPEND _flatten_vtr --yosys ${ODIN3_YOSYS055})
            endif()
            odin3_add_sweep_test(flatten_vtr_sweep LABEL sweep COMMAND ${_flatten_vtr})
            # 2F Task 20 (spec 20.2, 20.3, ruling I9): the nightly VTR legs of opt-equiv and
            # opt-diff, run by hand (Peter runs no nightly): tools/opt-check/opt-nightly, one
            # tool run at a time under memcap, smallest file first. opt_equiv_vtr and
            # opt_diff_vtr (opt-diff with all five scripts) run the 31 verilog/*.v files, mcml.v
            # last. The _xl pair runs koios/lenet.v alone (tests/opt/vtr_nightly_xl.txt; fails
            # when the xl selection is empty), 8G per child in the 9G slice (ODIN3_HEAVY_XL_OK=1,
            # an idle machine only). Run: ODIN3_HEAVY_OK=1 ctest --preset sweep -R
            # '^opt_(equiv|diff)_vtr$'. Not in CI.
            set(_opt_nightly ${PROJECT_SOURCE_DIR}/tools/opt-check/opt-nightly
                --odin3 $<TARGET_FILE:odin3> --yosys ${ODIN3_YOSYS055}
                --techlib ${PROJECT_SOURCE_DIR}/lib/vtr.o3lib --root ${_vtr_verilog}
                --expect-fail ${PROJECT_SOURCE_DIR}/tests/opt/vtr_expect_fail.txt
                --expect-fail ${PROJECT_SOURCE_DIR}/tests/opt/vtr_nightly_xl.txt
                --diff-expect ${PROJECT_SOURCE_DIR}/tests/opt/diff_expect.txt)
            odin3_add_sweep_test(opt_equiv_vtr LABEL sweep
                COMMAND ${_opt_nightly} --set vtr31 --leg equiv --min-changed-total 1 --mem-cap 3G)
            odin3_add_sweep_test(opt_diff_vtr LABEL sweep
                COMMAND ${_opt_nightly} --set vtr31 --leg diff --mem-cap 3G)
            set_tests_properties(opt_equiv_vtr opt_diff_vtr PROPERTIES LABELS "sweep;nightly")
            odin3_add_sweep_test(opt_equiv_vtr_xl LABEL sweep-xl
                COMMAND ${_opt_nightly} --set all64 --xl --leg equiv --min-changed-total 1 --mem-cap 8G)
            odin3_add_sweep_test(opt_diff_vtr_xl LABEL sweep-xl
                COMMAND ${_opt_nightly} --set all64 --xl --leg diff --mem-cap 8G)
        endif()

        # 2H (PHASE2 #4, decision 13; tools/exit-check): every micro, and the VTR benchmarks that
        # have a Parmys golden, through sp-splice (Odin III's front end through opt and proc, then
        # Yosys 0.55 with Parmys) against the Parmys golden of the k6_frac arch: the golden's
        # provenance (vtr_commit, arch, source and BLIF hashes) is checked first, then
        # netlist-compare, then equiv-check --x-report. Classes: identical, equivalent (pass); eq-x
        # (passes only with a reviewed record, tests/exit/*_eqx.txt); listed-reject (Parmys itself
        # rejects it, tests/exit/micro_expect.txt); unproven (equiv-check's tool limits,
        # tests/exit/{micro,vtr}_unproven.txt: their own bucket, never a pass); no-golden (no
        # golden, an oracle harness gap or an oracle killed at its memory cap,
        # tests/exit/micro_nogolden.txt: never counted); differs, error, hard
        # (fail; hard exits 3). Smallest first, one design at a time, every child, sp-splice
        # included, under memcap (3G; the four biggest files 5G, alone, in the 6G slice). The run
        # fails when fewer than --min-checked designs were proven, so a wrong golden tree cannot
        # pass. exit_vtr runs the 31 files directly under benchmarks/verilog (the Koios designs have
        # no k6_frac golden; exit-check counts such a design as no-golden, and CMake adds koios/
        # only when goldens exist). Logs go to ${CMAKE_BINARY_DIR}/exit-work/{micro,vtr}. Run as
        # `ODIN3_HEAVY_OK=1 ctest --preset sweep -R '^exit_'` from a Release build.
        set(_exit_arch $ENV{VTR_ROOT}/vtr_flow/arch/timing/k6_frac_N10_frac_chain_mem32K_40nm.xml)
        if(DEFINED ENV{VTR_ROOT} AND EXISTS ${_exit_arch})
            set(_exit_check ${PROJECT_SOURCE_DIR}/tools/exit-check/exit-check
                --odin3 $<TARGET_FILE:odin3> --yosys ${ODIN3_YOSYS055} --arch ${_exit_arch}
                --vtr-root $ENV{VTR_ROOT} --jobs 1 --mem-cap 3G
                --cap-for mcml.v=5G --cap-for arm_core.v=5G
                --cap-for mkDelayWorker32B.v=5G --cap-for LU64PEEng.v=5G)
            odin3_add_sweep_test(exit_micro LABEL sweep
                COMMAND ${_exit_check}
                        --expect-fail ${PROJECT_SOURCE_DIR}/tests/exit/micro_expect.txt
                        --expect-no-golden ${PROJECT_SOURCE_DIR}/tests/exit/micro_nogolden.txt
                        --expect-unproven ${PROJECT_SOURCE_DIR}/tests/exit/micro_unproven.txt
                        --eqx-record ${PROJECT_SOURCE_DIR}/tests/exit/micro_eqx.txt
                        --equiv-box-budget 900
                        --min-checked 400 --work ${CMAKE_BINARY_DIR}/exit-work/micro
                        --root ${PROJECT_SOURCE_DIR}/tests/micro/verilog
                        ${PROJECT_SOURCE_DIR}/tests/micro/verilog)
            file(GLOB _exit_vtr_files CONFIGURE_DEPENDS $ENV{VTR_ROOT}/vtr_flow/benchmarks/verilog/*.v)
            # The Koios designs (41) have no k6_frac Parmys golden: they join the sweep only when
            # the golden tree has a vtr/koios directory (else exit-check would skip them anyway).
            set(_exit_golden $ENV{HOME}/odin3-ws/golden/k6_frac_N10_frac_chain_mem32K_40nm)
            if(IS_DIRECTORY ${_exit_golden}/vtr/koios)
                file(GLOB _exit_koios_files CONFIGURE_DEPENDS
                     $ENV{VTR_ROOT}/vtr_flow/benchmarks/verilog/koios/*.v)
                list(APPEND _exit_vtr_files ${_exit_koios_files})
            endif()
            if(_exit_vtr_files)
                odin3_add_sweep_test(exit_vtr LABEL sweep
                    COMMAND ${_exit_check}
                            --expect-fail ${PROJECT_SOURCE_DIR}/tests/exit/vtr_expect.txt
                            --expect-unproven ${PROJECT_SOURCE_DIR}/tests/exit/vtr_unproven.txt
                            --eqx-record ${PROJECT_SOURCE_DIR}/tests/exit/vtr_eqx.txt
                            --min-checked 20 --work ${CMAKE_BINARY_DIR}/exit-work/vtr
                            --root $ENV{VTR_ROOT}/vtr_flow/benchmarks/verilog
                            ${_exit_vtr_files})
            endif()
            # 3C Task 7 (spec 9.4): the 19 through hardmap-diff, in the order of vtr_designs.txt.
            file(STRINGS ${PROJECT_SOURCE_DIR}/tests/hardmap/vtr_designs.txt _hm_vtr_names
                 REGEX "^[^#]")
            set(_hm_vtr "")
            foreach(_hm_name IN LISTS _hm_vtr_names)
                list(APPEND _hm_vtr $ENV{VTR_ROOT}/vtr_flow/benchmarks/verilog/${_hm_name}.v)
            endforeach()
            odin3_add_sweep_test(hardmap_vtr_sweep LABEL sweep
                COMMAND ${PROJECT_SOURCE_DIR}/tools/hardmap-check/hardmap-diff
                        --odin3 $<TARGET_FILE:odin3> --yosys $ENV{VTR_ROOT}/build/bin/yosys
                        --arch $ENV{VTR_ROOT}/vtr_flow/arch/timing/k6_frac_N10_frac_chain_mem32K_40nm.xml
                        --expect ${PROJECT_SOURCE_DIR}/tests/hardmap/diff_expect.txt
                        --expect ${PROJECT_SOURCE_DIR}/tests/hardmap/vtr_expect.txt
                        ${_hm_vtr})
        else()
            message(STATUS "exit_micro, exit_vtr not registered: needs VTR_ROOT with the k6_frac arch")
        endif()
    endif()

    # 2C §7.5: generated modules with their own expected trees, through odin3 and Yosys
    # (tools/verilog-gen/compare.py; 200 seeds here, 5,000 locally). The oracle is VTR's Yosys
    # (0.55, docs/ORACLES.md) when the workspace has it, else ODIN3_YOSYS (CI's).
    find_program(ODIN3_YOSYS_VTR yosys
                 PATHS $ENV{HOME}/odin3-ws/external/vtr-verilog-to-routing/build/bin
                 NO_DEFAULT_PATH)
    set(_gen_yosys ${ODIN3_YOSYS_VTR})
    if(NOT _gen_yosys)
        set(_gen_yosys ${ODIN3_YOSYS})
    endif()
    if(_gen_yosys)
        add_test(NAME verilog_gen_vs_yosys
            COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/verilog-gen/compare.py
                    --odin3 $<TARGET_FILE:odin3> --parse-check $<TARGET_FILE:odin3-parse-check>
                    --yosys ${_gen_yosys} --seeds 1..200
                    --work ${PROJECT_BINARY_DIR}/verilog_gen_work)
        set_tests_properties(verilog_gen_vs_yosys PROPERTIES LABELS verilog TIMEOUT 900)
    else()
        message(STATUS "verilog_gen_vs_yosys not registered: needs yosys")
    endif()
endif()

# Every test runs with its own TMPDIR, one directory per build tree (outside the repo: a test
# checks that a temporary directory is not a git checkout). A tool killed by a ctest TIMEOUT
# (SIGKILL) cannot remove the temporary directory it made; the cleanup fixture below does, after
# the last test that needs it, pass or fail. Tools remove their own directories on every other exit.
if(BUILD_TESTING)
    string(MD5 _bin_hash "${PROJECT_BINARY_DIR}")
    string(SUBSTRING ${_bin_hash} 0 12 _bin_hash)
    set(_test_tmp /tmp/odin3-ctest-${_bin_hash})
    add_test(NAME test_tmp_setup COMMAND ${CMAKE_COMMAND} -E make_directory ${_test_tmp})
    add_test(NAME test_tmp_cleanup COMMAND ${CMAKE_COMMAND} -E rm -rf ${_test_tmp})
    set_tests_properties(test_tmp_setup PROPERTIES FIXTURES_SETUP test_tmp)
    set_tests_properties(test_tmp_cleanup PROPERTIES FIXTURES_CLEANUP test_tmp)
    get_property(_all_tests DIRECTORY PROPERTY TESTS)
    foreach(_t IN LISTS _all_tests)
        if(NOT _t MATCHES "^test_tmp_")
            set_property(TEST ${_t} APPEND PROPERTY ENVIRONMENT "TMPDIR=${_test_tmp}")
            set_property(TEST ${_t} APPEND PROPERTY FIXTURES_REQUIRED test_tmp)
        endif()
    endforeach()
endif()
