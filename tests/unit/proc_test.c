/*
 * proc_test.c — the helpers the 2D process-generator unit tests share (test_elab_stage2_proc.c,
 * test_elab_stage2_case.c; Task 22 review I3): reading a source through the read_verilog pass,
 * the located-diagnostic assertions, and the text dump of a `$proc` body. See proc_test.h.
 */
#include "proc_test.h"

#include "elab_test.h"

#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/module.h"
#include "ir/proc.h"
#include "odin3/odin3.h"
#include "unity.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { DUMP_MAX = 1 << 13 };

et_result pt_rs;

void pt_read_with(const pass_src *src) {
    et_src file = {"t.v", src->text};
    et_req req = {&file, 1, NULL, NULL, NULL};
    et_setup(&req, &pt_rs);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_set_keep_ast(pt_rs.design, ODIN3_KEEP_AST_NONE));
    pt_rs.status = odin3_pass_run(pt_rs.design, "read_verilog", odin3_bytes_cstr(src->args));
}

void pt_read_files(const et_src *srcs, uint32_t n, const char *args) {
    et_req req = {srcs, n, NULL, NULL, NULL};
    et_setup(&req, &pt_rs);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_set_keep_ast(pt_rs.design, ODIN3_KEEP_AST_NONE));
    pt_rs.status = odin3_pass_run(pt_rs.design, "read_verilog", odin3_bytes_cstr(args));
}

void pt_read_src(const char *text) {
    pass_src src = {"t.v", text};
    pt_read_with(&src);
}

void pt_expect_ir_ok(void) {
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_check_design(pt_rs.design, opts), et_log(&pt_rs));
}

/* Errors that are located diagnostics: not the pass manager's own "failed" line. */
uint32_t pt_diag_errors(void) {
    return pt_rs.errors - et_count_with(&pt_rs, ODIN3_LOG_ERROR, "pass read_verilog: failed");
}

/* The warnings of 2D: not the IR check's. */
uint32_t pt_soft_warnings(void) {
    return pt_rs.warnings - et_count_with(&pt_rs, ODIN3_LOG_WARN, "check: ");
}

void pt_read_clean(const char *text) {
    pt_read_src(text);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, pt_rs.status, et_log(&pt_rs));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, pt_diag_errors(), et_log(&pt_rs));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, pt_soft_warnings(), et_log(&pt_rs));
    pt_expect_ir_ok();
}

/* A failed stage 2 leaves valid, incomplete modules (EL-2). */
void pt_read_failing(const char *text) {
    pt_read_src(text);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_PARSE, pt_rs.status, et_log(&pt_rs));
    pt_expect_ir_ok();
}

static const char *diag_line(at_loc at, const char *level, const char *msg) {
    static char buf[768];
    int got = snprintf(buf, sizeof buf, "t.v:%u:%u: %s: %s", (unsigned)at.line, (unsigned)at.col,
                       level, msg);
    TEST_ASSERT_TRUE(got > 0 && (size_t)got < sizeof buf);
    return buf;
}

void pt_expect_warning(at_loc at, const char *msg) {
    et_expect(&pt_rs, ODIN3_LOG_WARN, diag_line(at, "warning", msg));
}

void pt_expect_error(at_loc at, const char *msg) {
    et_expect(&pt_rs, ODIN3_LOG_ERROR, diag_line(at, "error", msg));
}

uint32_t pt_warnings_with(const char *needle) {
    return et_count_with(&pt_rs, ODIN3_LOG_WARN, needle);
}

/* --- reading the IR back --------------------------------------------------------------------- */

uint32_t pt_id_of(const char *text) {
    uint32_t id = 0;
    TEST_ASSERT_TRUE_MESSAGE(
        odin3_strtab_find(odin3_design_strtab(pt_rs.design), odin3_bytes_cstr(text), &id), text);
    return id;
}

const char *pt_str_of(uint32_t id) {
    return odin3_strtab_get(odin3_design_strtab(pt_rs.design), id);
}

odin3_module *pt_top(void) {
    uint32_t id = pt_id_of("t");
    for (uint32_t index = 1; index < odin3_design_module_end(pt_rs.design); index++) {
        odin3_module *module = odin3_module_get(pt_rs.design, (odin3_module_id){index});
        if (odin3_module_name(module) == id) {
            return module;
        }
    }
    TEST_FAIL_MESSAGE("no module t");
    return NULL;
}

odin3_net_id pt_wnet(const char *name, uint32_t bit) {
    odin3_wire_id wire = odin3_module_find_wire(pt_top(), pt_id_of(name));
    TEST_ASSERT_NOT_EQUAL_UINT32_MESSAGE(0, wire.v, name);
    odin3_net_id net = odin3_wire_net(pt_top(), wire, bit);
    TEST_ASSERT_NOT_EQUAL_UINT32_MESSAGE(0, net.v, name);
    return net;
}

static const odin3_celltype_def *def_of(odin3_node_id node) {
    return odin3_celltype_get(pt_rs.design, odin3_node_type(pt_top(), node));
}

odin3_node_id pt_nth_node(const char *type, uint32_t nth) {
    const odin3_module *module = pt_top();
    uint32_t seen = 0;
    for (uint32_t id = 1; id < odin3_module_node_end(module); id++) {
        odin3_node_id node = {id};
        if (odin3_node_live(module, node) && strcmp(def_of(node)->name, type) == 0 &&
            seen++ == nth) {
            return node;
        }
    }
    return (odin3_node_id){0};
}

uint32_t pt_count_of(const char *type) {
    uint32_t count = 0;
    while (pt_nth_node(type, count).v != 0) {
        count++;
    }
    return count;
}

odin3_node_id pt_proc_node(uint32_t nth) {
    odin3_node_id node = pt_nth_node("$proc", nth);
    TEST_ASSERT_NOT_EQUAL_UINT32(0, node.v);
    return node;
}

/* The net on pin `index` of a port of a $proc node. */
static odin3_net_id slice_net(odin3_pinslice slice, uint32_t index) {
    TEST_ASSERT_TRUE(index < slice.count);
    return odin3_pin_net(pt_top(), (odin3_pin_id){slice.first.v + index});
}

static odin3_net_id in_net(odin3_node_id node, uint32_t index) {
    return slice_net(odin3_node_port(pt_top(), node, ODIN3_PROC_PORT_I), index);
}

odin3_net_id pt_out_net(odin3_node_id node, uint32_t index) {
    return slice_net(odin3_node_port(pt_top(), node, ODIN3_PROC_PORT_O), index);
}

const odin3_proc_body *pt_body_of(odin3_node_id node, odin3_proc_body *storage) {
    const odin3_value *value = odin3_node_param(pt_top(), node, ODIN3_PROC_P_BODY);
    TEST_ASSERT_NOT_NULL(value);
    TEST_ASSERT_TRUE(odin3_proc_body_view(value, storage));
    return storage;
}

/* --- the dump of a body: pins in net-ID order, cases, switches and syncs ---------------------- */

static void put_pbit(odin3_strbuf *out, odin3_pbit pbit) {
    if (pbit == ODIN3_PBIT_ANY) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, odin3_bytes_cstr("-")));
        return;
    }
    char tag = (pbit & ~ODIN3_PBIT_INDEX_MASK) == ODIN3_PBIT_OUT ? 'O' : 'I';
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_strbuf_appendf(out, "%c%u", tag, (unsigned)(pbit & ODIN3_PBIT_INDEX_MASK)));
}

/* A span as a list: consecutive pins of one port as a range (O8..O15). */
static void put_span(odin3_strbuf *out, const odin3_proc_body *body, odin3_pspan span) {
    for (uint32_t i = 0; i < span.len;) {
        odin3_pbit first = body->pbits[span.first + i];
        uint32_t j = i;
        while (first != ODIN3_PBIT_ANY && j + 1 < span.len &&
               body->pbits[span.first + j + 1] == first + (j + 1 - i)) {
            j++;
        }
        if (i > 0) {
            TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, odin3_bytes_cstr(",")));
        }
        put_pbit(out, first);
        if (j > i) {
            TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, odin3_bytes_cstr("..")));
            put_pbit(out, body->pbits[span.first + j]);
        }
        i = j + 1;
    }
}

static void put_actions(odin3_strbuf *out, const odin3_proc_body *body, uint32_t first,
                        uint32_t n) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, odin3_bytes_cstr("[")));
    for (uint32_t i = first; i < first + n; i++) {
        if (i > first) {
            TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, odin3_bytes_cstr(" ")));
        }
        put_span(out, body, body->actions[i].lhs);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, odin3_bytes_cstr("<-")));
        put_span(out, body, body->actions[i].rhs);
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, odin3_bytes_cstr("]")));
}

static void put_case(odin3_strbuf *out, const odin3_proc_body *body, uint32_t index) {
    const odin3_proc_case *cs = &body->cases[index];
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_appendf(out, "C%u", (unsigned)index));
    if (cs->n_compares > 0) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, odin3_bytes_cstr(" cmp(")));
        for (uint32_t k = 0; k < cs->n_compares; k++) {
            if (k > 0) {
                TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, odin3_bytes_cstr("|")));
            }
            put_span(out, body, body->compares[cs->first_compare + k]);
        }
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, odin3_bytes_cstr(")")));
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, odin3_bytes_cstr(" ")));
    put_actions(out, body, cs->first_action, cs->n_actions);
    if (cs->n_switches > 0) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_strbuf_appendf(out, " sw%u+%u", (unsigned)cs->first_switch,
                                                   (unsigned)cs->n_switches));
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, odin3_bytes_cstr("\n")));
}

static void put_sync(odin3_strbuf *out, const odin3_proc_body *body, uint32_t index) {
    static const char *const k_names[] = {"?", "posedge", "negedge", "low", "high", "always"};
    const odin3_proc_sync *sync = &body->syncs[index];
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_appendf(out, "S %s", k_names[sync->type]));
    if (sync->type != ODIN3_SYNC_ALWAYS) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, odin3_bytes_cstr(" ")));
        put_pbit(out, sync->signal);
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, odin3_bytes_cstr(" ")));
    put_actions(out, body, sync->first_action, sync->n_actions);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, odin3_bytes_cstr("\n")));
}

/* "I<w> O<w>", a line per case, switch and sync. */
const char *pt_dump(odin3_node_id node) {
    static char text[DUMP_MAX];
    odin3_proc_body storage;
    const odin3_proc_body *body = pt_body_of(node, &storage);
    odin3_strbuf out;
    odin3_strbuf_init(&out);
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK,
        odin3_strbuf_appendf(&out, "I%u O%u\n",
                             (unsigned)odin3_node_port(pt_top(), node, ODIN3_PROC_PORT_I).count,
                             (unsigned)odin3_node_port(pt_top(), node, ODIN3_PROC_PORT_O).count));
    for (uint32_t i = 0; i < body->n_cases; i++) {
        put_case(&out, body, i);
    }
    for (uint32_t j = 0; j < body->n_switches; j++) {
        const odin3_proc_switch *sw = &body->switches[j];
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_appendf(&out, "W%u (", (unsigned)j));
        put_span(&out, body, sw->signal);
        TEST_ASSERT_EQUAL_INT(
            ODIN3_OK, odin3_strbuf_appendf(&out, ") f%u C%u+%u\n", (unsigned)sw->flags,
                                           (unsigned)sw->first_case, (unsigned)sw->n_cases));
    }
    for (uint32_t k = 0; k < body->n_syncs; k++) {
        put_sync(&out, body, k);
    }
    TEST_ASSERT_TRUE(out.len < sizeof text);
    memcpy(text, out.data, out.len);
    text[out.len] = '\0';
    odin3_strbuf_free(&out);
    return text;
}

void pt_expect_dump(uint32_t nth, const char *want) {
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want, pt_dump(pt_proc_node(nth)), et_log(&pt_rs));
}

void pt_expect_in(odin3_node_id node, uint32_t index, odin3_net_id want) {
    TEST_ASSERT_EQUAL_UINT32(want.v, in_net(node, index).v);
}

void pt_expect_out(odin3_node_id node, uint32_t index, odin3_net_id want) {
    TEST_ASSERT_EQUAL_UINT32(want.v, pt_out_net(node, index).v);
}

/* The constant net of bit (the shared $_CONST*_ node's Y). */
odin3_net_id pt_const_net_of(odin3_bit bit) {
    static const char *const k_names[4] = {"$_CONST0_", "$_CONST1_", "$_CONSTX_", "$_CONSTZ_"};
    odin3_node_id node = pt_nth_node(k_names[bit], 0);
    TEST_ASSERT_NOT_EQUAL_UINT32(0, node.v);
    odin3_pinslice slice = odin3_node_port(pt_top(), node, 0);
    return odin3_pin_net(pt_top(), slice.first);
}
