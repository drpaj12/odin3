/*
 * verilog_test.h — helpers shared by the 2C unit tests (static inline, so a test uses what it
 * needs): a hand-built stream over the 2A source manager alone (no preprocessor), and a parse of
 * one text with a capturing log sink, the store checked and dumped.
 *
 * A test using vt_parse resets the log sink in its tearDown (odin3_log_set_sink(NULL, NULL)), so
 * a failed assertion inside vt_parse never leaves the sink pointing at a dead result.
 */
#ifndef ODIN3_TESTS_UNIT_VERILOG_TEST_H
#define ODIN3_TESTS_UNIT_VERILOG_TEST_H

#include "ast/ast.h"
#include "ast/check.h"
#include "ast/print.h"
#include "ast/srcman.h"
#include "frontends/verilog/parser.h"
#include "ir/check.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "unity.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* One parse: the design and store (owned until vt_free), the driver's status and UNIT, the
 * UNIT's dump (on success), and what was logged during the call. */
typedef struct vt_result {
    odin3_design *design;
    odin3_ast *ast;
    odin3_status status;
    odin3_ast_id unit;
    odin3_strbuf dump;
    odin3_strbuf log;      /* every message logged during the call, each followed by '\n' */
    uint32_t errors;       /* messages at ODIN3_LOG_ERROR */
    uint32_t invalid_args; /* errors logged as "odin3_<function>: …" (builder or API misuse) */
} vt_result;

/* The capturing sink: user is the vt_result. */
static inline void vt_sink(odin3_log_level level, const char *msg, void *user) {
    vt_result *res = user;
    (void)odin3_strbuf_appendf(&res->log, "%s\n", msg);
    if (level != ODIN3_LOG_ERROR) {
        return;
    }
    res->errors++;
    size_t word = strspn(msg, "abcdefghijklmnopqrstuvwxyz0123456789_");
    if (strncmp(msg, "odin3_", strlen("odin3_")) == 0 && strncmp(msg + word, ": ", 2) == 0) {
        res->invalid_args++;
    }
}

/*
 * A stream over text as 2B would hand it over with no macro or include: a FILE buffer named name
 * holding the text, its line starts, and one segment mapping the whole stream onto it.
 */
static inline void vt_stream(odin3_design *design, const char *name, odin3_bytes text,
                             odin3_vp_stream *out) {
    odin3_srcman *sm = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_srcman(design, &sm));
    uint32_t name_id = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr(name), &name_id));
    odin3_srcfile_spec spec = {.name = name_id, .len = (uint32_t)text.len};
    odin3_srcbuf_id buf = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_add_file(sm, &spec, &buf));
    const char *bytes = text.ptr;
    for (size_t i = 0; i < text.len; i++) {
        if (bytes[i] == '\n') {
            TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_add_line(sm, buf, (uint32_t)i + 1));
        }
    }
    odin3_segment seg = {buf.v, 0, odin3_srcman_loc(sm, buf, 0).v, {0}};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_add_segment(sm, &seg));
    *out = (odin3_vp_stream){text, buf, name_id};
}

/*
 * Parses text as the file name through odin3_verilog_parse_stream in a fresh design, run and
 * parsed store, with the capturing sink installed for the call; on success the store passes
 * odin3_ast_check FULL and the UNIT is dumped into out->dump. Free with vt_free.
 */
static inline void vt_parse(const char *name, const char *text, vt_result *out) {
    memset(out, 0, sizeof *out);
    odin3_strbuf_init(&out->dump);
    odin3_strbuf_init(&out->log);
    out->design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(out->design);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_create(out->design, ODIN3_AST_FORM_PARSED,
                                                     (odin3_passrun_id){1}, &out->ast));
    odin3_vp_stream stream;
    vt_stream(out->design, name, odin3_bytes_cstr(text), &stream);
    odin3_vl_run run;
    odin3_vl_run_init(&run, out->design);
    odin3_log_set_sink(vt_sink, out);
    out->status = odin3_verilog_parse_stream(&run, out->ast, &stream, &out->unit);
    if (out->status == ODIN3_OK) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_check(out->ast, ODIN3_CHECK_FULL));
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_dump(out->ast, out->unit, &out->dump));
    }
    odin3_log_set_sink(NULL, NULL);
}

/* Frees what vt_parse made and resets the log sink. */
static inline void vt_free(vt_result *res) {
    odin3_log_set_sink(NULL, NULL);
    odin3_ast_destroy(res->ast);
    odin3_design_destroy(res->design);
    odin3_strbuf_free(&res->dump);
    odin3_strbuf_free(&res->log);
    memset(res, 0, sizeof *res);
}

#endif
