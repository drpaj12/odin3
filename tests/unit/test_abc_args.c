/*
 * test_abc_args.c — the abc pass's arguments, script precedence and start-up checks (3D plan Task
 * 14; spec §7.1, §7.2, AB-19, AB-20): every argument error exact and the IR unchanged, IO errors,
 * the precedence of the script, no engine, the views error, the first INFO line, a refused script,
 * the byte budget, and the pass table entry. The fake engine stands in for ABC.
 */
#include "abc_fake_engine.h"
#include "abc_fixture.h"
#include "frontends/project/project.h"
#include "ir/prov.h"
#include "passes/abc/abc.h"
#include "passes/abc/abc_test.h"
#include "passes/abc/script.h"
#include "passes/budget.h"
#include "passes/manager.h"
#include "proc_fixture.h"
#include "util/alloc.h"

#include <regex.h>
#include <unistd.h>

#if !defined(ODIN3_LIB_DIR) || !defined(ODIN3_GENLIB_DIR)
#error "ODIN3_LIB_DIR and ODIN3_GENLIB_DIR must be defined"
#endif

static odin3_design *design;
static odin3_arena *arena;

void setUp(void) {
    afx_log_reset();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_log_set_level(ODIN3_LOG_DEBUG));
    odin3_abc_fake_reset();
    odin3_abc_test_set_engine(odin3_abc_fake_engine());
    odin3_pass_set_options((odin3_pass_options){0});
    char tmp[AFX_PATH];
    afx_path(tmp, "tmp");
    afx_mkdir(tmp);
    TEST_ASSERT_EQUAL_INT(0, setenv("TMPDIR", tmp, 1));
    TEST_ASSERT_EQUAL_INT(0, unsetenv("ODIN3_ABC_EXCHANGE"));
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    arena = odin3_arena_create(0);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_budget_test_set_limit(0);
    odin3_abc_test_set_engine(NULL);
    odin3_abc_test_set_route(ODIN3_ABC_TEST_ROUTE_AUTO);
    odin3_pass_set_options((odin3_pass_options){0});
    odin3_log_set_sink(NULL, NULL);
    odin3_arena_destroy(arena);
    odin3_design_destroy(design);
}

/* Module m: inputs a b, output y = a & b. */
static void build_and(void) {
    odin3_module_id mid = {0};
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_module_create(design, afx_intern(design, "m"), (odin3_prov_id){0}, &mid));
    odin3_module *mod = odin3_module_get(design, mid);
    odin3_net_id in_a = afx_port(design, mod, "a", ODIN3_DIR_IN);
    odin3_net_id in_b = afx_port(design, mod, "b", ODIN3_DIR_IN);
    odin3_net_id out_y = afx_port(design, mod, "y", ODIN3_DIR_OUT);
    odin3_net_id gate[3] = {in_a, in_b, out_y};
    (void)AFX_CELL(design, mod, "$_AND_", "g", gate, 3);
    afx_check(design);
}

static odin3_status run_abc(const char *args) {
    return odin3_pass_run(design, "abc", odin3_bytes_cstr(args));
}

/* The pass fails with st, logs exactly one `abc:` error equal to line, and the IR is unchanged. */
static void expect_refused(const char *args, odin3_status st, const char *line) {
    char *before = afx_blif_text(design);
    afx_log_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(st, run_abc(args), args);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, afx_count(ODIN3_LOG_ERROR, "abc:"), afx_errors());
    TEST_ASSERT_TRUE_MESSAGE(afx_has(ODIN3_LOG_ERROR, line), afx_errors());
    char *after = afx_blif_text(design);
    TEST_ASSERT_EQUAL_STRING(before, after);
    free(before);
    free(after);
    afx_check(design);
}

/* --- arguments ------------------------------------------------------------------------------ */

static void test_argument_errors(void) {
    build_and();
    expect_refused("--frob", ODIN3_ERR_INVALID_ARG, "abc: unknown argument '--frob'");
    expect_refused("--lut 6 --lut 4", ODIN3_ERR_INVALID_ARG, "abc: --lut given twice");
    expect_refused("--script", ODIN3_ERR_INVALID_ARG, "abc: --script needs a value");
    expect_refused("--lut 1", ODIN3_ERR_INVALID_ARG,
                   "abc: --lut must be an integer from 2 to 16, not '1'");
    expect_refused("--lut 17", ODIN3_ERR_INVALID_ARG,
                   "abc: --lut must be an integer from 2 to 16, not '17'");
    expect_refused("--lut x", ODIN3_ERR_INVALID_ARG,
                   "abc: --lut must be an integer from 2 to 16, not 'x'");
    expect_refused("--prov fast", ODIN3_ERR_INVALID_ARG,
                   "abc: --prov must be fine or cone, not 'fast'");
    TEST_ASSERT_EQUAL_UINT32(0, odin3_abc_fake_open_count());
}

static void test_via_blif_bytes(void) {
    build_and();
    static const char *const bad[] = {"a;b", "a\tb", "a'b", "a#b", "a\"b"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        char dir[AFX_PATH];
        char name[64];
        char args[2 * AFX_PATH];
        char line[2 * AFX_PATH];
        (void)snprintf(name, sizeof name, "%s", bad[i]);
        afx_path(dir, name);
        if (strchr(bad[i], '"') != NULL) {
            continue; /* a double quote cannot be written in an argument */
        }
        (void)snprintf(args, sizeof args, "--via-blif \"%s\"", dir);
        (void)snprintf(line, sizeof line,
                       "abc: --via-blif directory '%s' contains a control character, a quote, ';' "
                       "or '#'; ABC cannot read it",
                       dir);
        expect_refused(args, ODIN3_ERR_INVALID_ARG, line);
    }
}

/* --- IO ------------------------------------------------------------------------------------- */

static void test_io_errors(void) {
    build_and();
    expect_refused("--script nope.abc", ODIN3_ERR_IO,
                   "abc: nope.abc: cannot read the script: No such file or directory");
    expect_refused("--lut 6 --log /nonexistent/x", ODIN3_ERR_IO,
                   "abc: /nonexistent/x: cannot open the log: No such file or directory");
    if (geteuid() == 0) {
        TEST_IGNORE_MESSAGE("a read-only directory does not stop root");
    }
    char ro[AFX_PATH];
    afx_path(ro, "readonly");
    afx_mkdir(ro);
    TEST_ASSERT_EQUAL_INT(0, chmod(ro, 0555));
    char args[2 * AFX_PATH];
    char line[2 * AFX_PATH];
    (void)snprintf(args, sizeof args, "--lut 6 --via-blif %s/sub", ro);
    (void)snprintf(line, sizeof line, "abc: %s/sub: cannot create the directory: Permission denied",
                   ro);
    expect_refused(args, ODIN3_ERR_IO, line);
    TEST_ASSERT_EQUAL_INT(0, chmod(ro, 0755));
}

/* --- precedence ----------------------------------------------------------------------------- */

static void choose(const char *args, odin3_abc_script_choice *out) {
    odin3_abc_opts opts;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_parse_args(odin3_bytes_cstr(args), arena, &opts));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_choose_script(design, &opts, arena, out));
}

static void set_cli_script(const char *path) {
    odin3_pass_options opts = odin3_pass_get_options();
    opts.abc_script = path;
    odin3_pass_set_options(opts);
}

static void set_project_script(const char *given, const char *resolved) {
    odin3_project *proj = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_project(design, &proj));
    proj->abc_script.given = afx_intern(design, given);
    proj->abc_script.resolved = afx_intern(design, resolved);
}

static void test_precedence(void) {
    odin3_abc_script_choice choice;
    choose("", &choice);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_ABC_SRC_BUILTIN, choice.source);
    TEST_ASSERT_EQUAL_STRING("builtin:lib/abc/vtr.abc", choice.given);
    TEST_ASSERT_EQUAL_STRING("embedded", choice.resolved);
    set_project_script("abc/map.abc", "/proj/abc/map.abc");
    choose("", &choice);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_ABC_SRC_PROJECT, choice.source);
    TEST_ASSERT_EQUAL_STRING("abc/map.abc", choice.given);
    TEST_ASSERT_EQUAL_STRING("/proj/abc/map.abc", choice.resolved);
    /* the CLI replaces the record's script (read_project does that); without a project it is CLI */
    set_project_script("cli.abc", "/work/cli.abc");
    set_cli_script("cli.abc");
    choose("", &choice);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_ABC_SRC_PROJECT, choice.source);
    TEST_ASSERT_EQUAL_STRING("cli.abc", choice.given);
    odin3_design *plain = odin3_design_create();
    odin3_abc_opts none = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_choose_script(plain, &none, arena, &choice));
    TEST_ASSERT_EQUAL_UINT8(ODIN3_ABC_SRC_CLI, choice.source);
    TEST_ASSERT_EQUAL_STRING("cli.abc", choice.given);
    TEST_ASSERT_TRUE(choice.resolved[0] == '/');
    odin3_design_destroy(plain);
    choose("--script s.abc", &choice);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_ABC_SRC_PASS, choice.source);
    TEST_ASSERT_EQUAL_STRING("s.abc", choice.given);
}

static void test_exchange_choice(void) {
    odin3_abc_opts opts = {0};
    uint32_t exchange = 99;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_exchange_choice(&opts, &exchange));
    TEST_ASSERT_EQUAL_UINT32(ODIN3_ABC_EXCHANGE_BLIF, exchange);
    TEST_ASSERT_EQUAL_INT(0, setenv("ODIN3_ABC_EXCHANGE", "aig", 1));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_exchange_choice(&opts, &exchange));
    TEST_ASSERT_EQUAL_UINT32(ODIN3_ABC_EXCHANGE_AIG, exchange);
    opts.via_blif = "d"; /* --via-blif wins over the variable */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_exchange_choice(&opts, &exchange));
    TEST_ASSERT_EQUAL_UINT32(ODIN3_ABC_EXCHANGE_BLIF, exchange);
    opts.via_blif = NULL;
    TEST_ASSERT_EQUAL_INT(0, setenv("ODIN3_ABC_EXCHANGE", "xml", 1));
    afx_log_reset();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_abc_exchange_choice(&opts, &exchange));
    TEST_ASSERT_TRUE(
        afx_has(ODIN3_LOG_ERROR, "abc: ODIN3_ABC_EXCHANGE must be aig or blif, not 'xml'"));
}

static void test_hex_digest(void) {
    unsigned char digest[ODIN3_SHA256_DIGEST_BYTES];
    char hex[ODIN3_SHA256_HEX_CHARS];
    uint8_t want[ODIN3_SHA256_DIGEST_BYTES];
    odin3_abc_default_sha256(want);
    odin3_sha256_hex(want, hex);
    TEST_ASSERT_TRUE(odin3_abc_hex_digest(hex, digest));
    TEST_ASSERT_EQUAL_MEMORY(want, digest, sizeof digest);
    TEST_ASSERT_EQUAL_INT(ODIN3_ABC_ROUTE_LINKED, odin3_abc_route(digest));
}

static void test_hex_digest_refusals(void) {
    unsigned char digest[ODIN3_SHA256_DIGEST_BYTES];
    char hex[ODIN3_SHA256_HEX_CHARS];
    uint8_t want[ODIN3_SHA256_DIGEST_BYTES];
    odin3_abc_default_sha256(want);
    odin3_sha256_hex(want, hex);
    TEST_ASSERT_FALSE(odin3_abc_hex_digest("abc", digest));
    hex[3] = 'G';
    TEST_ASSERT_FALSE(odin3_abc_hex_digest(hex, digest));
    hex[3] = 'A'; /* upper case is refused */
    TEST_ASSERT_FALSE(odin3_abc_hex_digest(hex, digest));
}

/* --- start-up checks ---------------------------------------------------------------------------
 */

static void test_no_engine(void) {
    build_and();
    odin3_abc_test_set_engine(NULL);
    expect_refused("--lut 6", ODIN3_ERR_ENGINE,
                   "abc: linked ABC is not available (no engine registered; build with "
                   "ODIN3_WITH_ABC=ON)");
}

static void test_views_error(void) {
    odin3_module_id mid = {0};
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_module_create(design, afx_intern(design, "m"), (odin3_prov_id){0}, &mid));
    odin3_module *mod = odin3_module_get(design, mid);
    odin3_node_spec spec = {afx_type(design, "$add"), afx_intern(design, "$c3"), (odin3_prov_id){0},
                            NULL, 5};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create(mod, &spec, NULL));
    afx_log_reset();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_CHECK, run_abc("--lut 6"));
    TEST_ASSERT_TRUE_MESSAGE(
        afx_has(ODIN3_LOG_ERROR,
                "abc: module 'm' is not in the netlist view: $add '$c3'; run lower first"),
        afx_errors());
    TEST_ASSERT_EQUAL_UINT32(0, odin3_abc_fake_open_count());
}

static void test_first_info_line(void) {
    build_and();
    afx_log_reset();
    (void)run_abc("--lut 6");
    const char *line = afx_only(ODIN3_LOG_INFO, "abc: script ");
    regex_t re;
    TEST_ASSERT_EQUAL_INT(0, regcomp(&re,
                                     "^abc: script builtin:lib/abc/vtr.abc \\(embedded\\), sha256 "
                                     "[0-9a-f]{16}\xE2\x80\xA6, K=6, exchange blif, ABC "
                                     "[0-9a-f]{12}$",
                                     REG_EXTENDED | REG_NOSUB));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, regexec(&re, line, 0, NULL, 0), line);
    regfree(&re);
    uint8_t digest[ODIN3_SHA256_DIGEST_BYTES];
    char hex[ODIN3_SHA256_HEX_CHARS];
    odin3_abc_default_sha256(digest);
    odin3_sha256_hex(digest, hex);
    char prefix[32];
    (void)snprintf(prefix, sizeof prefix, "sha256 %.16s", hex);
    TEST_ASSERT_NOT_NULL(strstr(line, prefix));
    TEST_ASSERT_NOT_NULL(strstr(line, "ABC fa4e00000000"));
}

static void test_refused_script(void) {
    build_and();
    char path[AFX_PATH];
    afx_path(path, "bad.abc");
    AFX_WRITE(path, "write_blif x.blif\n");
    char args[2 * AFX_PATH];
    char line[2 * AFX_PATH];
    (void)snprintf(args, sizeof args, "--lut 6 --script %s", path);
    (void)snprintf(line, sizeof line,
                   "abc: %s:1: abc command 'write_blif' is not allowed: abc scripts cannot read or "
                   "write files",
                   path);
    expect_refused(args, ODIN3_ERR_PARSE, line);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_abc_fake_open_count());
}

static void test_budget(void) {
    build_and();
    odin3_budget_test_set_limit(4096);
    char *before = afx_blif_text(design);
    afx_log_reset();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, run_abc("--lut 6"));
    TEST_ASSERT_TRUE_MESSAGE(
        afx_has(ODIN3_LOG_ERROR,
                "abc needs more than 4096 bytes (the design's budget is 4096 bytes: the smaller of "
                "60% of physical memory and the cgroup's memory.max)"),
        afx_errors());
    TEST_ASSERT_EQUAL_UINT32(0, odin3_abc_fake_open_count());
    char *after = afx_blif_text(design);
    TEST_ASSERT_EQUAL_STRING(before, after);
    free(before);
    free(after);
}

static void test_pass_table_entry(void) {
    const odin3_pass_def *def = odin3_pass_find(odin3_bytes_cstr("abc"));
    TEST_ASSERT_NOT_NULL(def);
    TEST_ASSERT_FALSE(def->wants_ast);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_PASS_PROCESSES_REFUSE, def->processes);
    TEST_ASSERT_EQUAL_STRING(
        "abc [--script FILE] [--lut K] [--gates LIB] [--via-blif DIR] [--log FILE] [--prov "
        "fine|cone]: map each module's soft logic to K-input LUTs (--lut) or to library LIB's "
        "gates (--gates) with linked ABC (one run per clock domain)",
        def->help);
}

static void test_process_is_refused(void) {
    (void)proc_fixture_design(design);
    afx_log_reset();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run_abc("--lut 6"));
    TEST_ASSERT_TRUE(
        afx_has(ODIN3_LOG_ERROR, "pass 'abc': module 'm' has processes; run proc first"));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_abc_fake_open_count());
}

/* --- abc --gates: option and start-up (gatemap plan Task 10) ---------------------------------- */

static void read_generic(void) {
    char lib[AFX_PATH];
    (void)snprintf(lib, sizeof lib, "%s/generic_gates.o3lib", ODIN3_LIB_DIR);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run(design, "read_techlib", odin3_bytes_cstr(lib)));
}

/* The run information of the last pass run. */
static const char *last_info(void) {
    return odin3_strtab_get(
        odin3_design_strtab(design),
        odin3_passrun_info(design, (odin3_passrun_id){odin3_passrun_end(design) - 1U}));
}

static void test_gates_needs_library(void) {
    build_and();
    expect_refused("--gates generic_gates", ODIN3_ERR_INVALID_ARG,
                   "abc: --gates: library 'generic_gates' is not loaded; read it with read_techlib "
                   "first");
    expect_refused("--gates", ODIN3_ERR_INVALID_ARG, "abc: --gates needs a value");
    TEST_ASSERT_EQUAL_UINT32(0, odin3_abc_fake_open_count());
}

static void test_gates_unknown_library(void) {
    read_generic();
    build_and();
    expect_refused("--gates nosuch", ODIN3_ERR_INVALID_ARG,
                   "abc: --gates: library 'nosuch' is not loaded; read it with read_techlib first");
}

static void test_gates_with_lut_refused(void) {
    read_generic();
    build_and();
    expect_refused("--gates generic_gates --lut 6", ODIN3_ERR_INVALID_ARG,
                   "abc: --lut does not apply with --gates");
}

static void test_gates_with_via_blif_refused(void) {
    read_generic();
    build_and();
    char dir[AFX_PATH];
    afx_path(dir, "gates_via");
    char args[2 * AFX_PATH];
    (void)snprintf(args, sizeof args, "--gates generic_gates --via-blif %s", dir);
    expect_refused(args, ODIN3_ERR_INVALID_ARG,
                   "abc: --gates runs ABC in memory; --via-blif cannot be used with it");
    TEST_ASSERT_FALSE(afx_exists(dir));
}

static void test_gates_lut_script_refused(void) {
    read_generic();
    build_and();
    char path[AFX_PATH];
    afx_path(path, "lut_end.abc");
    AFX_WRITE(path, "strash\nif -K 4\n");
    char args[2 * AFX_PATH];
    char line[3 * AFX_PATH];
    (void)snprintf(args, sizeof args, "--gates generic_gates --script %s", path);
    (void)snprintf(line, sizeof line,
                   "abc: %s: the script does not end gate-mapped; with --gates end it with a gate "
                   "mapper ('map' or '&nf')",
                   path);
    expect_refused(args, ODIN3_ERR_PARSE, line);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_abc_fake_open_count());
}

static void test_gates_default_script_is_gates_abc(void) {
    read_generic();
    build_and();
    afx_log_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_abc("--gates generic_gates"), afx_errors());
    const char *info = last_info();
    TEST_ASSERT_NOT_NULL(info);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(info, "script=builtin:lib/abc/gates.abc "), info);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(info, " K=none exchange=aig gates=generic_gates genlib="),
                                 info);
    const char *line = afx_only(ODIN3_LOG_INFO, "abc: script ");
    regex_t re;
    TEST_ASSERT_EQUAL_INT(0,
                          regcomp(&re,
                                  "^abc: script builtin:lib/abc/gates.abc \\(embedded\\), sha256 "
                                  "[0-9a-f]{16}\xE2\x80\xA6, K=none, exchange aig, gates "
                                  "generic_gates, ABC [0-9a-f]{12}$",
                                  REG_EXTENDED | REG_NOSUB));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, regexec(&re, line, 0, NULL, 0), line);
    regfree(&re);
    const char *lib_line = afx_only(ODIN3_LOG_INFO, "abc: --gates library ");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0,
                                  regcomp(&re,
                                          "^abc: --gates library 'generic_gates': 13 gates, "
                                          "genlib sha256 [0-9a-f]{16}\xE2\x80\xA6$",
                                          REG_EXTENDED | REG_NOSUB),
                                  lib_line);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, regexec(&re, lib_line, 0, NULL, 0), lib_line);
    regfree(&re);
}

static void test_gates_ignores_project_and_cli_script(void) {
    read_generic();
    build_and();
    set_cli_script("cli.abc");
    afx_log_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_abc("--gates generic_gates"), afx_errors());
    TEST_ASSERT_TRUE(afx_has(ODIN3_LOG_INFO,
                             "abc: note: --gates does not use the project's or --abc-script's "
                             "script; give --script to choose one"));
    TEST_ASSERT_NOT_NULL(strstr(last_info(), "script=builtin:lib/abc/gates.abc "));
}

static void test_gates_prov_fine_note(void) {
    static const char k_note[] = "abc: note: --prov fine is not available with --gates; "
                                 "provenance uses cut cones (gatemap spec G-2)";
    read_generic();
    build_and();
    afx_log_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_abc("--gates generic_gates"), afx_errors());
    TEST_ASSERT_FALSE(afx_has(ODIN3_LOG_INFO, k_note));
    afx_log_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_abc("--gates generic_gates --prov fine"),
                                  afx_errors());
    TEST_ASSERT_TRUE(afx_has(ODIN3_LOG_INFO, k_note));
    afx_log_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_abc("--gates generic_gates --prov cone"),
                                  afx_errors());
    TEST_ASSERT_FALSE(afx_has(ODIN3_LOG_INFO, k_note));
}

static void test_gates_logs_skipped_cells(void) {
    read_generic();
    build_and();
    afx_log_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_abc("--gates generic_gates"), afx_errors());
    TEST_ASSERT_EQUAL_UINT32(
        6, afx_count(ODIN3_LOG_INFO, "abc: note: library 'generic_gates': cell "));
    TEST_ASSERT_TRUE(afx_has(ODIN3_LOG_INFO, "abc: note: library 'generic_gates': cell 'DFFP' is "
                                             "not used for gate mapping: a flip-flop or latch"));
    TEST_ASSERT_TRUE(afx_has(ODIN3_LOG_INFO,
                             "abc: note: library 'generic_gates': cell 'TIE1' is not used for gate "
                             "mapping: a constant cell (used for constants)"));
}

/* Reads an .o3lib text (written to inline.o3lib first). */
static void read_inline_library(const char *text) {
    char path[AFX_PATH];
    afx_path(path, "inline.o3lib");
    AFX_WRITE(path, text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run(design, "read_techlib", odin3_bytes_cstr(path)));
}

static void test_gates_no_inverter_refused(void) {
    read_inline_library("library noinv\n"
                        "cell AND2 gate area 1\n"
                        "  in A 1 ; in B 1 ; out Y 1\n"
                        "  fn Y = A & B\n"
                        "end\n");
    build_and();
    expect_refused("--gates noinv", ODIN3_ERR_INVALID_ARG,
                   "abc: --gates: library 'noinv' has no inverter and 2-input AND, NAND, OR or NOR "
                   "gate; ABC cannot map to it");
}

static void test_gates_note_texts_with_ports(void) {
    read_inline_library("library ports\n"
                        "cell INV gate area 1\n"
                        "  in A 1 ; out Y 1\n"
                        "  fn Y = ~A\n"
                        "end\n"
                        "cell AND2 gate area 1\n"
                        "  in A 1 ; in B 1 ; out Y 1\n"
                        "  fn Y = A & B\n"
                        "end\n"
                        "cell IGN gate area 1\n"
                        "  in A 1 ; in B 1 ; out Y 1\n"
                        "  fn Y = ~A\n"
                        "end\n"
                        "cell AND2.X1 gate area 1\n"
                        "  in A 1 ; in B 1 ; out Y 1\n"
                        "  fn Y = A & B\n"
                        "end\n"
                        "cell CPIN gate area 1\n"
                        "  in CONST0 1 ; out Y 1\n"
                        "  fn Y = ~CONST0\n"
                        "end\n");
    build_and();
    afx_log_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_abc("--gates ports"), afx_errors());
    TEST_ASSERT_TRUE(afx_has(ODIN3_LOG_INFO, "abc: note: library 'ports': cell 'IGN' is not used "
                                             "for gate mapping: its function ignores input 'B'"));
    TEST_ASSERT_TRUE(afx_has(ODIN3_LOG_INFO,
                             "abc: note: library 'ports': cell 'CPIN' is not used "
                             "for gate mapping: port 'CONST0' is not a genlib name"));
    TEST_ASSERT_TRUE(afx_has(ODIN3_LOG_INFO,
                             "abc: note: library 'ports': cell 'AND2.X1' is not "
                             "used for gate mapping: its name is not a genlib name"));
}

static void test_gates_genlib_loaded(void) {
    read_generic();
    build_and();
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_abc("--gates generic_gates"), afx_errors());
    char path[AFX_PATH];
    (void)snprintf(path, sizeof path, "%s/generic_gates.genlib", ODIN3_GENLIB_DIR);
    char want[8192];
    (void)snprintf(want, sizeof want, "%s", afx_slurp(path));
    TEST_ASSERT_TRUE(want[0] != '\0');
    TEST_ASSERT_EQUAL_STRING(want, odin3_abc_fake_genlib());
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_argument_errors);
    RUN_TEST(test_via_blif_bytes);
    RUN_TEST(test_io_errors);
    RUN_TEST(test_precedence);
    RUN_TEST(test_exchange_choice);
    RUN_TEST(test_hex_digest);
    RUN_TEST(test_hex_digest_refusals);
    RUN_TEST(test_no_engine);
    RUN_TEST(test_views_error);
    RUN_TEST(test_first_info_line);
    RUN_TEST(test_refused_script);
    RUN_TEST(test_budget);
    RUN_TEST(test_pass_table_entry);
    RUN_TEST(test_process_is_refused);
    RUN_TEST(test_gates_needs_library);
    RUN_TEST(test_gates_unknown_library);
    RUN_TEST(test_gates_with_lut_refused);
    RUN_TEST(test_gates_with_via_blif_refused);
    RUN_TEST(test_gates_lut_script_refused);
    RUN_TEST(test_gates_default_script_is_gates_abc);
    RUN_TEST(test_gates_ignores_project_and_cli_script);
    RUN_TEST(test_gates_prov_fine_note);
    RUN_TEST(test_gates_logs_skipped_cells);
    RUN_TEST(test_gates_no_inverter_refused);
    RUN_TEST(test_gates_note_texts_with_ports);
    RUN_TEST(test_gates_genlib_loaded);
    return UNITY_END();
}
