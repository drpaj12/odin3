/*
 * test_verilog_rows.c — the coverage rows as acceptance tests on plain streams (2C spec §7.2,
 * plan point (p)): every tests/verilog/coverage/rows.txt line whose task is at most
 * ODIN3_VR_UPTO parses to a UNIT that passes odin3_ast_check FULL; the UNIT prints, the printed
 * text re-parses, and the two UNITs' children are pairwise odin3_ast_equal; where the row has a
 * hand-written or generated .ast golden, the children's dumps equal it byte for byte.
 *
 * Each row runs as its own Unity test, named after the row.
 */
#include "ast/ast.h"
#include "ast/check.h"
#include "ast/print.h"
#include "frontends/verilog/parser.h"
#include "ir/check.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "unity.h"
#include "util/file.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"
#include "verilog_test.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ODIN3_VR_UPTO
#error "ODIN3_VR_UPTO (the last grammar task whose rows run) must be defined"
#endif

enum { MAX_FILE = 1 << 20, MAX_ROWS = 256, NAME_BYTES = 64, PATH_BYTES = 512 };

static const char HAND_WRITTEN[] = "# hand-written\n";
/* The first line of a golden odin3-parse-check --write made (Task 9), compared the same way. */
static const char GENERATED[] = "# generated: odin3-parse-check --write\n";

/* One rows.txt line. */
typedef struct row {
    char id[NAME_BYTES];
    char file[NAME_BYTES * 2];
    int task;
} row;

static row rows[MAX_ROWS];
static size_t row_count;
static const row *current;
static uint32_t goldens; /* hand-written goldens compared */

void setUp(void) {
}

void tearDown(void) {
    odin3_log_set_sink(NULL, NULL);
}

static void coverage_path(const char *file, char *out) {
    int len = snprintf(out, PATH_BYTES, "%s/coverage/%s", ODIN3_VERILOG_DATA, file);
    TEST_ASSERT_TRUE(len > 0 && len < PATH_BYTES);
}

/* The file's bytes as a NUL-terminated string in out (freed by the caller); false if missing. */
static bool read_text(const char *path, odin3_strbuf *out) {
    odin3_file_bytes bytes = {0};
    odin3_status st = odin3_file_read_all(path, MAX_FILE, &bytes);
    if (st == ODIN3_OK) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_strbuf_append(out, (odin3_bytes){bytes.data, bytes.len}));
    }
    odin3_file_bytes_free(&bytes);
    return st == ODIN3_OK;
}

/* Parses text as name in the store res->ast (a fresh run): the UNIT, or a failed assertion
 * quoting what was logged. */
static odin3_ast_id parse_into(vt_result *res, const char *name, const char *text) {
    odin3_vp_stream stream;
    vt_stream(res->design, name, odin3_bytes_cstr(text), &stream);
    odin3_vl_run run;
    odin3_vl_run_init(&run, res->design);
    odin3_strbuf_clear(&res->log);
    odin3_log_set_sink(vt_sink, res);
    odin3_ast_id unit = {0};
    odin3_status st = odin3_verilog_parse_stream(&run, res->ast, &stream, &unit);
    odin3_log_set_sink(NULL, NULL);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, st, res->log.data != NULL ? res->log.data : "");
    return unit;
}

/* The UNIT's children dumped as a .ast file holds them (each dump followed by '\n'). */
static void dump_children(const odin3_ast *ast, odin3_ast_id unit, odin3_strbuf *out) {
    for (uint32_t i = 0; i < odin3_ast_nchild(ast, unit); i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_dump(ast, odin3_ast_child(ast, unit, i), out));
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, odin3_bytes_cstr("\n")));
    }
}

/* The golden's text after its first line, which marks it hand-written (*hand) or generated. */
static const char *golden_body(const char *text, bool *hand) {
    *hand = strncmp(text, HAND_WRITTEN, strlen(HAND_WRITTEN)) == 0;
    if (*hand) {
        return text + strlen(HAND_WRITTEN);
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, strncmp(text, GENERATED, strlen(GENERATED)), current->file);
    return text + strlen(GENERATED);
}

/* The row's golden, when it has one, against the parse's dump. */
static void compare_golden(const vt_result *res, odin3_ast_id unit) {
    char path[PATH_BYTES];
    char golden[NAME_BYTES * 2];
    int stem = (int)strlen(current->file) - (int)strlen(".v");
    int len = snprintf(golden, sizeof golden, "%.*s.ast", stem, current->file);
    TEST_ASSERT_TRUE(stem > 0 && len > 0 && (size_t)len < sizeof golden);
    coverage_path(golden, path);
    odin3_strbuf want;
    odin3_strbuf_init(&want);
    if (read_text(path, &want)) {
        bool hand = false;
        const char *body = golden_body(want.data, &hand);
        odin3_strbuf got;
        odin3_strbuf_init(&got);
        dump_children(res->ast, unit, &got);
        TEST_ASSERT_EQUAL_STRING(body, got.data != NULL ? got.data : "");
        odin3_strbuf_free(&got);
        goldens += hand ? 1U : 0U;
    }
    odin3_strbuf_free(&want);
}

/* The printed UNIT re-parses (into the same store) to children pairwise equal to unit's. */
static void check_round_trip(vt_result *res, odin3_ast_id unit) {
    odin3_strbuf printed;
    odin3_strbuf_init(&printed);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_print(res->ast, unit, &printed));
    const char *text = printed.data != NULL ? printed.data : "";
    odin3_ast_id again = parse_into(res, "printed.v", text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_check(res->ast, ODIN3_CHECK_FULL));
    uint32_t count = odin3_ast_nchild(res->ast, unit);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(count, odin3_ast_nchild(res->ast, again), text);
    for (uint32_t i = 0; i < count; i++) {
        bool equal = false;
        TEST_ASSERT_EQUAL_INT(
            ODIN3_OK, odin3_ast_equal(res->ast, odin3_ast_child(res->ast, unit, i), res->ast,
                                      odin3_ast_child(res->ast, again, i), &equal));
        TEST_ASSERT_TRUE_MESSAGE(equal, text);
    }
    odin3_strbuf_free(&printed);
}

static void test_row(void) {
    char path[PATH_BYTES];
    coverage_path(current->file, path);
    odin3_strbuf source;
    odin3_strbuf_init(&source);
    TEST_ASSERT_TRUE_MESSAGE(read_text(path, &source), path);
    vt_result res;
    memset(&res, 0, sizeof res);
    odin3_strbuf_init(&res.dump);
    odin3_strbuf_init(&res.log);
    res.design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(res.design);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_create(res.design, ODIN3_AST_FORM_PARSED,
                                                     (odin3_passrun_id){1}, &res.ast));
    odin3_ast_id unit = parse_into(&res, current->file, source.data);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_check(res.ast, ODIN3_CHECK_FULL));
    check_round_trip(&res, unit);
    compare_golden(&res, unit);
    odin3_strbuf_free(&source);
    vt_free(&res);
}

/* One rows.txt line, `<row> <file> <task>`, into *out. */
static void parse_row(const char *line, row *out) {
    TEST_ASSERT_EQUAL_INT(2, sscanf(line, "%63s %127s", out->id, out->file));
    const char *task = strstr(line, out->file) + strlen(out->file);
    char *end = NULL;
    long value = strtol(task, &end, 10);
    TEST_ASSERT_TRUE(end != task && value > 0);
    out->task = (int)value;
}

/* The rows of text (rows.txt) whose task is at most ODIN3_VR_UPTO, into rows[]. */
static void keep_rows(const char *text) {
    for (const char *line = text; line != NULL && *line != '\0';) {
        TEST_ASSERT_TRUE(row_count < MAX_ROWS);
        parse_row(line, &rows[row_count]);
        row_count += rows[row_count].task <= ODIN3_VR_UPTO ? 1U : 0U;
        line = strchr(line, '\n');
        line = line != NULL ? line + 1 : NULL;
    }
}

/* rows.txt: `<row> <file> <task>` per line; the rows of tasks up to ODIN3_VR_UPTO are kept. */
static void test_rows_file(void) {
    char path[PATH_BYTES];
    coverage_path("rows.txt", path);
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    TEST_ASSERT_TRUE_MESSAGE(read_text(path, &text), path);
    keep_rows(text.data);
    odin3_strbuf_free(&text);
    TEST_ASSERT_TRUE(row_count > 0);
}

/* The hand-written goldens of the rows up to each grammar task (rows.txt and the .ast files of
 * tests/verilog/coverage): a golden that goes missing fails here instead of being skipped. */
static const uint32_t GOLDENS_UPTO[] = {[4] = 4, [5] = 25, [6] = 39, [7] = 46};

/* Every golden of the rows run was compared, and the count is reported. */
static void test_goldens_compared(void) {
    char text[NAME_BYTES];
    (void)snprintf(text, sizeof text, "%u hand-written goldens equal", (unsigned)goldens);
    TEST_MESSAGE(text);
    TEST_ASSERT_TRUE(ODIN3_VR_UPTO < sizeof GOLDENS_UPTO / sizeof GOLDENS_UPTO[0]);
    TEST_ASSERT_EQUAL_UINT32(GOLDENS_UPTO[ODIN3_VR_UPTO], goldens);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_rows_file);
    for (size_t i = 0; i < row_count; i++) {
        current = &rows[i];
        UnityDefaultTestRun(test_row, current->id, (int)i + 1);
    }
    RUN_TEST(test_goldens_compared);
    return UNITY_END();
}
