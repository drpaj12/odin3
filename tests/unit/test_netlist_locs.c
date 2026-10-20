/*
 * test_netlist_locs.c — read_verilog -netlist (spec 5.4 D-3, L-5, L-6, T-2 Verilog legs, T-5):
 * `src` attributes, the trailing `// LOC` comment of an assign, L-5 names, the IMPORTED kind of the
 * reader's own records, and no change without the option. Every case works in its own mkdtemp
 * directory under the build tree.
 */
#include "ir/check.h"
#include "ir/design.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "passes/manager.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef ODIN3_LOCS_ROOT
#error "ODIN3_LOCS_ROOT must name the build tree"
#endif

enum { PATH_BUF = 512, DIR_BUF = 256, CHUNK = 4096, LOG_MAX = 4096, PARENTS = 2 };

static char dir[DIR_BUF];
static odin3_design *design;
static char log_text[LOG_MAX];
static uint32_t warnings;

static void sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    warnings += level == ODIN3_LOG_WARN ? 1U : 0U;
    if (level <= ODIN3_LOG_WARN) {
        size_t used = strlen(log_text);
        (void)snprintf(log_text + used, sizeof log_text - used, "%s\n", msg);
    }
}

static void path_of(const char *name, char *out) {
    (void)snprintf(out, PATH_BUF, "%s/%s", dir, name);
}

void setUp(void) {
    (void)snprintf(dir, sizeof dir, "%s/nlocs.XXXXXX", ODIN3_LOCS_ROOT);
    TEST_ASSERT_NOT_NULL(mkdtemp(dir));
    log_text[0] = '\0';
    warnings = 0;
    odin3_log_set_sink(sink, NULL);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
}

void tearDown(void) {
    DIR *handle = opendir(dir);
    for (struct dirent *entry = handle != NULL ? readdir(handle) : NULL; entry != NULL;
         entry = readdir(handle)) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
            char path[PATH_BUF];
            path_of(entry->d_name, path);
            (void)unlink(path);
        }
    }
    if (handle != NULL) {
        (void)closedir(handle);
    }
    (void)rmdir(dir);
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(design);
    design = NULL;
}

typedef struct file_spec {
    const char *name;
    const char *text;
} file_spec;

static void put_file(const file_spec *spec) {
    char path[PATH_BUF];
    path_of(spec->name, path);
    FILE *file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(strlen(spec->text), fwrite(spec->text, 1, strlen(spec->text), file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static char *slurp(const char *name) {
    char path[PATH_BUF];
    path_of(name, path);
    FILE *file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(file);
    char *text = odin3_util_calloc(1);
    size_t len = 0;
    char chunk[CHUNK];
    for (size_t got = fread(chunk, 1, sizeof chunk, file); got > 0;
         got = fread(chunk, 1, sizeof chunk, file)) {
        text = odin3_util_realloc(text, len + got + 1);
        memcpy(text + len, chunk, got);
        len += got;
        text[len] = '\0';
    }
    (void)fclose(file);
    return text;
}

/* One pass run: `pass options <dir>/file`. */
typedef struct pass_call {
    const char *pass;
    const char *options;
    const char *file;
} pass_call;

static odin3_status run(const pass_call *call) {
    char path[PATH_BUF];
    char args[2 * PATH_BUF];
    path_of(call->file, path);
    (void)snprintf(args, sizeof args, "%s %s", call->options, path);
    return odin3_pass_run(design, call->pass, odin3_bytes_cstr(args));
}

static void run_ok(const pass_call *call) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run(call), log_text);
}

/* The options of a read of net.v. */
typedef enum read_opts { OPTS_NONE, OPTS_ICELLS, OPTS_NETLIST, OPTS_BOTH } read_opts;

static const char *const k_opts[] = {"", "-icells", "-netlist", "-icells -netlist"};

/* Writes net.v and reads it with the options. */
static void read_net(const char *text, read_opts opts) {
    put_file(&(file_spec){"net.v", text});
    run_ok(&(pass_call){"read_verilog", k_opts[opts], "net.v"});
}

/* The dump_locs text of the design. */
static char *dump(void) {
    char path[PATH_BUF];
    path_of("dump.txt", path);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run(design, "dump_locs", odin3_bytes_cstr(path)));
    return slurp("dump.txt");
}

static void expect_in(const char *text, const char *needle) {
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(text, needle), text);
}

static void expect_out(const char *text, const char *needle) {
    TEST_ASSERT_NULL_MESSAGE(strstr(text, needle), text);
}

static void expect_check(void) {
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK,
        odin3_check_design(design, (odin3_check_opts){ODIN3_CHECK_FULL, ODIN3_VIEW_NONE}));
}

/* Whether file names one of the netlist files of these tests. */
static bool is_netlist_file(const char *file) {
    static const char *const k_netlists[] = {"net.v", "back.v"};
    size_t len = strlen(file);
    for (size_t i = 0; i < sizeof k_netlists / sizeof k_netlists[0]; i++) {
        size_t tail = strlen(k_netlists[i]);
        if (len >= tail && strcmp(file + len - tail, k_netlists[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* The record's locations name no netlist file when it is a SOURCE. */
static void expect_record_not_in_net(const odin3_prov_record *rec) {
    TEST_ASSERT_NOT_NULL(rec);
    for (uint32_t k = 0; rec->kind == ODIN3_PROV_SOURCE && k < rec->n_locs; k++) {
        const char *file = odin3_strtab_get(odin3_design_strtab(design), rec->locs[k].file);
        TEST_ASSERT_FALSE_MESSAGE(is_netlist_file(file), file);
    }
}

/* No SOURCE record points at a netlist file (the guard of spec risk 2), by walking every record. */
static void expect_no_source_in_net(void) {
    for (uint32_t id = 1; id < odin3_prov_end(design); id++) {
        expect_record_not_in_net(odin3_prov_get(design, (odin3_prov_id){id}));
    }
}

/* The netlist of the W-6 Verilog rows: module, port, wire, instance, always, three assigns. */
static const char *const NET =
    "(* src = \"a.v:1.1-30.10\" *)\n"
    "module top(clk, a, b, y, z, v, q);\n"
    "  input clk;\n"
    "  input a;\n"
    "  input b;\n"
    "  output y;\n"
    "  output z;\n"
    "  output [1:0] v;\n"
    "  (* src = \"a.v:4.3-4.20\" *)\n"
    "  output q;\n"
    "  (* src = \"a.v:5.3-5.12\" *)\n"
    "  wire w;\n"
    "  reg r;\n"
    "  (* src = \"a.v:6.3-6.30\" *)\n"
    "  \\$and #(.A_SIGNED(0), .B_SIGNED(0), .A_WIDTH(1), .B_WIDTH(1), .Y_WIDTH(1)) u1 (.A(a), "
    ".B(b), .Y(w));\n"
    "  assign y = a & w; // a.v:7\n"
    "  assign z = a | b; // a.v:8\n"
    "  assign v = {a ^ b, a & ~b}; // a.v:9\n"
    "  (* src = \"a.v:10.3-11.9\" *)\n"
    "  always @(posedge clk)\n"
    "    r <= a;\n"
    "  assign q = r;\n"
    "endmodule\n";

static void test_without_option_nothing_changes(void) {
    read_net(NET, OPTS_ICELLS);
    char *text = dump();
    expect_out(text, "a.v:");
    expect_in(text, "net.v:");
    expect_check();
    free(text);
}

static void test_without_option_matches_stripped_file(void) {
    read_net(NET, OPTS_ICELLS);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run(design, "proc", odin3_bytes_cstr("")));
    run_ok(&(pass_call){"write_blif", "", "with.blif"});
    odin3_design_destroy(design);
    design = odin3_design_create();
    /* the same layout with every comment and `src` attribute name changed */
    char *plain = odin3_util_calloc(strlen(NET) + 1);
    strcpy(plain, NET);
    for (char *at = strstr(plain, "// a.v:"); at != NULL; at = strstr(at, "// a.v:")) {
        at[3] = 'x';
        at += 3;
    }
    for (char *at = strstr(plain, "src ="); at != NULL; at = strstr(at, "src =")) {
        at[0] = 'x';
        at += 3;
    }
    read_net(plain, OPTS_ICELLS);
    free(plain);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run(design, "proc", odin3_bytes_cstr("")));
    run_ok(&(pass_call){"write_blif", "", "without.blif"});
    char *with = slurp("with.blif");
    char *without = slurp("without.blif");
    TEST_ASSERT_EQUAL_STRING(without, with);
    free(with);
    free(without);
}

static void test_attributes_restore(void) {
    read_net(NET, OPTS_BOTH);
    char *text = dump();
    expect_in(text, "top|module|top|a.v:1.1-30.10\n");
    expect_in(text, "top|wire|q|a.v:4.3-4.20\n"); /* a port, by its declaration */
    expect_in(text, "top|wire|w|a.v:5.3-5.12\n");
    expect_in(text, "top|node|u1|a.v:6.3-6.30\n"); /* an instance */
    expect_in(text, "top|wire|clk|\n");            /* no attribute, no location */
    expect_out(text, "net.v:");                    /* no netlist line is an HDL location */
    expect_check();
    free(text);
}

static void test_always_restores(void) {
    read_net(NET, OPTS_BOTH);
    char *text = dump();
    expect_in(text, "|a.v:10.3-11.9\n");
    free(text);
}

static void test_assign_comments(void) {
    read_net(NET, OPTS_BOTH);
    char *text = dump();
    expect_in(text, "|a.v:7\n");
    expect_in(text, "|a.v:8\n");
    expect_in(text, "|a.v:9\n"); /* a vector assign of grouped bits: every cell of the assign */
    expect_out(text, "a.v:7.");  /* the line form only (X-2) */
    free(text);
}

static void test_assign_without_comment_gets_none(void) {
    read_net("module top(a, b, y, z);\n  input a;\n  input b;\n  output y;\n  output z;\n"
             "  assign y = a & b; // a.v:7\n  assign z = a | b;\nendmodule\n",
             OPTS_NETLIST);
    char *text = dump();
    expect_in(text, "|a.v:7\n");
    /* the second assign's gate has no location */
    const char *first = strstr(text, "|a.v:7\n");
    TEST_ASSERT_NULL(strstr(first + 1, "|a.v:7\n"));
    expect_out(text, "a.v:8");
    expect_out(text, "net.v:");
    free(text);
}

static void test_user_comment_is_restored_under_netlist(void) {
    read_net("module top(a, b, y);\n  input a;\n  input b;\n  output y;\n"
             "  assign y = a & b; // see a.v:3\nendmodule\n",
             OPTS_NETLIST);
    char *text = dump();
    expect_in(text, "|see a.v:3\n"); /* the file is the text before the last colon (L-2) */
    free(text);
    expect_check();
}

static void test_not_one_loc_comment_is_ignored(void) {
    read_net("module top(a, b, y);\n  input a;\n  input b;\n  output y;\n"
             "  assign y = a & b; // a.v:3 and more\nendmodule\n",
             OPTS_NETLIST);
    char *text = dump();
    expect_out(text, "a.v:");
    free(text);
}

static const char *const NAMES =
    "module top(a, d, y);\n  input a;\n  input d;\n  output y;\n"
    "  wire \\sub/mid@m.v:4 ;\n  wire \\sub2/mid@m.v:5 ;\n  wire \\foo/bar ;\n"
    "  assign \\sub/mid@m.v:4  = a & d;\n  assign \\sub2/mid@m.v:5  = a | d;\n"
    "  assign \\foo/bar  = a ^ d;\n"
    "  assign y = \\sub/mid@m.v:4  & \\sub2/mid@m.v:5  & \\foo/bar ;\nendmodule\n";

static void test_names_are_split(void) {
    read_net(NAMES, OPTS_NETLIST);
    char *text = dump();
    expect_in(text, "top|wire|mid|m.v:4\n");
    expect_in(text, "top|wire|mid$1|m.v:5\n"); /* IR-14: the first free suffix */
    expect_in(text, "top|wire|foo/bar|\n");    /* a name that does not match is untouched */
    expect_check();
    free(text);
}

static void test_names_untouched_without_option(void) {
    read_net(NAMES, OPTS_NONE);
    char *text = dump();
    expect_in(text, "top|wire|sub/mid@m.v:4|");
    expect_out(text, "|m.v:");
    free(text);
}

static void test_malformed_src_stays_an_attribute(void) {
    read_net("module top(a, y);\n  input a;\n  output y;\n  (* src = \"nonsense\" *)\n"
             "  wire w;\n  (* src = \"also bad\" *)\n  wire w2;\n  assign w = a;\n"
             "  assign w2 = a;\n  assign y = w & w2;\nendmodule\n",
             OPTS_NETLIST);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, warnings, log_text); /* one per file (L-6) */
    expect_in(log_text, "2 `src` attribute(s)");
    expect_in(log_text, "first: \"nonsense\"");
    char *text = dump();
    expect_out(text, "nonsense");
    free(text);
    run_ok(&(pass_call){"write_verilog", "", "out.v"});
    char *out = slurp("out.v");
    expect_in(out, "src = \"nonsense\"");
    free(out);
}

static void test_records_are_imported_and_derived(void) {
    read_net(NET, OPTS_BOTH);
    expect_no_source_in_net();
    const odin3_module *top = odin3_module_get(design, odin3_design_top(design));
    const odin3_prov_record *rec = odin3_prov_get(design, odin3_module_prov(top));
    TEST_ASSERT_NOT_NULL(rec);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_DERIVED, rec->kind);
    TEST_ASSERT_EQUAL_UINT32(PARENTS, rec->parents.count);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_SOURCE, odin3_prov_get(design, rec->parents.ids[0])->kind);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_IMPORTED, odin3_prov_get(design, rec->parents.ids[1])->kind);
    /* one operation for the whole read */
    uint32_t ops = 0;
    for (uint32_t id = 1; id < odin3_prov_end(design); id++) {
        const odin3_prov_record *any = odin3_prov_get(design, (odin3_prov_id){id});
        if (any->kind == ODIN3_PROV_DERIVED) {
            ops = ops == 0 ? any->op : ops;
            TEST_ASSERT_EQUAL_UINT32(ops, any->op);
        }
    }
    TEST_ASSERT_NOT_EQUAL_UINT32(0, ops);
}

static void test_object_without_text_is_imported_only(void) {
    read_net("module top(a, y);\n  input a;\n  output y;\n  assign y = ~a;\nendmodule\n",
             OPTS_NETLIST);
    expect_no_source_in_net();
    for (uint32_t id = 1; id < odin3_prov_end(design); id++) {
        TEST_ASSERT_NOT_EQUAL_INT(ODIN3_PROV_DERIVED,
                                  odin3_prov_get(design, (odin3_prov_id){id})->kind);
    }
}

/* T-2: HDL -> write_verilog -> read_verilog -netlist gives the locations back (X-2: an assign's
 * cell keeps its line only). */
static void test_round_trip_through_the_writer(void) {
    put_file(&(file_spec){"a.v",
                          "module top(input clk, input a, input b, output y, output reg q);\n"
                          "  wire w;\n  assign w = a & b;\n  assign y = w | a;\n"
                          "  always @(posedge clk)\n    q <= w;\nendmodule\n"});
    run_ok(&(pass_call){"read_verilog", "", "a.v"});
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run(design, "proc", odin3_bytes_cstr("")));
    run_ok(&(pass_call){"write_verilog", "", "back.v"});
    odin3_design_destroy(design);
    design = odin3_design_create();
    run_ok(&(pass_call){"read_verilog", "-icells -netlist", "back.v"});
    char *text = dump();
    expect_in(text, "top|module|top|");
    expect_in(text, "a.v:1.1-7.10\n");
    expect_in(text, "top|wire|w|");
    expect_in(text, "a.v:2.8-2.9\n");
    expect_in(text, "a.v:1.41-1.49\n"); /* port y */
    expect_in(text, "a.v:5.3-6.12\n");  /* the always */
    expect_in(text, "|node|$c6|");
    expect_in(text, "a.v:3\n");
    expect_in(text, "a.v:4\n");
    expect_out(text, "back.v:");
    expect_check();
    expect_no_source_in_net();
    free(text);
}

static void test_options_are_refused_with_lib_and_parse_only(void) {
    put_file(&(file_spec){"net.v", "module top(a);\n  input a;\nendmodule\n"});
    TEST_ASSERT_NOT_EQUAL_INT(ODIN3_OK,
                              run(&(pass_call){"read_verilog", "--lib -netlist", "net.v"}));
    TEST_ASSERT_NOT_EQUAL_INT(ODIN3_OK,
                              run(&(pass_call){"read_verilog", "--parse-only -netlist", "net.v"}));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run(&(pass_call){"read_verilog", "--netlist", "net.v"}));
}

static void test_unknown_option_stays_an_error(void) {
    put_file(&(file_spec){"net.v", "module top(a);\n  input a;\nendmodule\n"});
    TEST_ASSERT_NOT_EQUAL_INT(ODIN3_OK, run(&(pass_call){"read_verilog", "-netlists", "net.v"}));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_without_option_nothing_changes);
    RUN_TEST(test_without_option_matches_stripped_file);
    RUN_TEST(test_attributes_restore);
    RUN_TEST(test_always_restores);
    RUN_TEST(test_assign_comments);
    RUN_TEST(test_assign_without_comment_gets_none);
    RUN_TEST(test_user_comment_is_restored_under_netlist);
    RUN_TEST(test_not_one_loc_comment_is_ignored);
    RUN_TEST(test_names_are_split);
    RUN_TEST(test_names_untouched_without_option);
    RUN_TEST(test_malformed_src_stays_an_attribute);
    RUN_TEST(test_records_are_imported_and_derived);
    RUN_TEST(test_object_without_text_is_imported_only);
    RUN_TEST(test_round_trip_through_the_writer);
    RUN_TEST(test_options_are_refused_with_lib_and_parse_only);
    RUN_TEST(test_unknown_option_stays_an_error);
    return UNITY_END();
}
