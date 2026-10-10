/*
 * test_ast_srcman.c — unit tests for the source manager: buffers, line maps, locations, macro and
 * include chains, segment cursors, ranges, srcloc, printing, located diagnostics, limits, OOM.
 */
#include "ast/srcman.h"
#include "ast/srcman_test.h"
#include "ir/design.h"
#include "ir/prov.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    CAPTURE = 4096,
    EXP = ODIN3_SRCBUF_EXPANSION,
    ARG = ODIN3_SRCBUF_MACRO_ARG,
    SCRATCH = ODIN3_SRCBUF_SCRATCH,
    OOM_LIMIT = 64,
    DEEP_ARGS = 40,
    CHAIN_MAX = 64,
};

static odin3_design *design;
static odin3_srcman *sm;
static char captured[CAPTURE];
static int delivered;
static size_t errors_logged;

static void capture_sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    delivered++;
    if (level == ODIN3_LOG_ERROR) {
        errors_logged++;
    }
    strncpy(captured, msg, sizeof captured - 1);
}

void setUp(void) {
    odin3_log_set_sink(capture_sink, NULL);
    captured[0] = '\0';
    delivered = 0;
    errors_logged = 0;
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    sm = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_srcman(design, &sm));
    TEST_ASSERT_NOT_NULL(sm);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(design);
    design = NULL;
    sm = NULL;
}

/* --- helpers ------------------------------------------------------------------------------- */

typedef struct src_file {
    odin3_srcbuf_id id;
    const char *text;
} src_file;

static uint32_t intern(const char *name) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr(name), &str));
    return str;
}

/* Adds every line start of text to buf (after each '\n'). */
static void add_lines(odin3_srcbuf_id buf, const char *text) {
    uint32_t len = (uint32_t)strlen(text);
    for (uint32_t i = 0; i < len; i++) {
        if (text[i] == '\n') {
            TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_add_line(sm, buf, i + 1));
        }
    }
}

static src_file add_spec(odin3_srcfile_spec spec, const char *text) {
    src_file file = {{0}, text};
    spec.len = (uint32_t)strlen(text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_add_file(sm, &spec, &file.id));
    TEST_ASSERT_NOT_EQUAL(0, file.id.v);
    add_lines(file.id, text);
    return file;
}

/* A file of library work, resolved to /proj/<name>, opened from parent (0: a project file). */
static src_file add_text(const char *name, uint32_t parent, const char *text) {
    char resolved[CAPTURE];
    strcpy(resolved, "/proj/");
    strncat(resolved, name, sizeof resolved - strlen(resolved) - 1);
    odin3_srcfile_spec spec = {.name = intern(name),
                               .resolved = intern(resolved),
                               .library = intern("work"),
                               .parent = parent};
    return add_spec(spec, text);
}

/* Line and column of "line:col" (1-based, bytes). */
static void parse_pos(const char *pos, uint32_t out[2]) {
    char *colon = NULL;
    out[0] = (uint32_t)strtoul(pos, &colon, 10);
    TEST_ASSERT_EQUAL_CHAR(':', *colon);
    out[1] = (uint32_t)strtoul(colon + 1, NULL, 10);
}

/* The raw loc of "line:col" of a file. */
static uint32_t at(src_file file, const char *pos) {
    uint32_t want[2];
    parse_pos(pos, want);
    uint32_t off = 0;
    for (uint32_t cur = 1; cur < want[0]; off++) {
        if (file.text[off] == '\n') {
            cur++;
        }
    }
    odin3_loc loc = odin3_srcman_loc(sm, file.id, off + want[1] - 1);
    TEST_ASSERT_NOT_EQUAL(0, loc.v);
    return loc.v;
}

/* The raw loc of offset off in buffer buf. */
static uint32_t in(odin3_srcbuf_id buf, uint32_t off) {
    odin3_loc loc = odin3_srcman_loc(sm, buf, off);
    TEST_ASSERT_NOT_EQUAL(0, loc.v);
    return loc.v;
}

static odin3_srcbuf_id expand(odin3_expansion_spec spec) {
    odin3_srcbuf_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_add_expansion(sm, &spec, &id));
    TEST_ASSERT_NOT_EQUAL(0, id.v);
    return id;
}

static odin3_loc lc(uint32_t raw) {
    return (odin3_loc){raw};
}

static uint32_t spelling(uint32_t raw) {
    return odin3_srcman_spelling(sm, lc(raw)).v;
}

static uint32_t exp_loc(uint32_t raw) {
    return odin3_srcman_expansion_loc(sm, lc(raw)).v;
}

static uint32_t file_loc(uint32_t raw) {
    return odin3_srcman_file_loc(sm, lc(raw)).v;
}

/* "file:line:col" of raw (given style), in a static buffer. */
static const char *fmt(uint32_t raw, odin3_srcfmt style) {
    static char text[CAPTURE];
    odin3_strbuf buf;
    odin3_strbuf_init(&buf);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_format(sm, lc(raw), &buf, style));
    strncpy(text, buf.data, sizeof text - 1);
    odin3_strbuf_free(&buf);
    return text;
}

/* The one message odin3_diag delivers for an error about `what` at raw. */
static const char *diag_at(uint32_t raw, const char *what) {
    delivered = 0;
    captured[0] = '\0';
    odin3_diag(design, ODIN3_LOG_ERROR, lc(raw), "bad '%s'", what);
    TEST_ASSERT_EQUAL_INT(1, delivered);
    return captured;
}

static odin3_range span(uint32_t loc, uint32_t end) {
    return (odin3_range){lc(loc), lc(end)};
}

/* A range given to expansion_range and the range it must return. */
typedef struct range_case {
    odin3_range want;
    odin3_range given;
} range_case;

static void assert_range(range_case rc) {
    odin3_range got = odin3_srcman_expansion_range(sm, rc.given);
    TEST_ASSERT_EQUAL_UINT32(rc.want.loc.v, got.loc.v);
    TEST_ASSERT_EQUAL_UINT32(rc.want.end.v, got.end.v);
}

/* raw decodes to "line:col". */
static void assert_pos(uint32_t raw, const char *pos) {
    uint32_t want[2];
    parse_pos(pos, want);
    odin3_srcpos got = {0};
    TEST_ASSERT_TRUE(odin3_srcman_decode(sm, lc(raw), &got));
    TEST_ASSERT_EQUAL_UINT32(want[0], got.line);
    TEST_ASSERT_EQUAL_UINT32(want[1], got.col);
}

/* --- the §3.1 example (f.v) ---------------------------------------------------------------- */

static const char F_V[] = "`define INNER(p) (p + 1)\n"
                          "`define OUTER(q) `INNER(q * 2)\n"
                          "assign y = `OUTER(xx);\n";

typedef struct fv {
    src_file file;
    odin3_srcbuf_id e1, a1, e2, a2, a3;
} fv;

static fv build_fv(void) {
    fv ex;
    ex.file = add_text("f.v", 0, F_V);
    src_file sf = ex.file;
    ex.e1 = expand((odin3_expansion_spec){.kind = EXP,
                                          .name = intern("OUTER"),
                                          .parent = at(sf, "3:12"),
                                          .parent_end = at(sf, "3:22"),
                                          .def = at(sf, "2:18"),
                                          .len = 13});
    ex.a1 = expand((odin3_expansion_spec){.kind = ARG,
                                          .name = intern("OUTER"),
                                          .parent = in(ex.e1, 7),
                                          .parent_end = in(ex.e1, 8),
                                          .def = at(sf, "3:19"),
                                          .len = 2});
    ex.e2 = expand((odin3_expansion_spec){.kind = EXP,
                                          .name = intern("INNER"),
                                          .parent = in(ex.e1, 0),
                                          .parent_end = in(ex.e1, 13),
                                          .def = at(sf, "1:18"),
                                          .len = 7});
    ex.a2 = expand((odin3_expansion_spec){.kind = ARG,
                                          .name = intern("INNER"),
                                          .parent = in(ex.e2, 1),
                                          .parent_end = in(ex.e2, 2),
                                          .def = in(ex.a1, 0),
                                          .len = 2});
    ex.a3 = expand((odin3_expansion_spec){.kind = ARG,
                                          .name = intern("INNER"),
                                          .parent = in(ex.e2, 1),
                                          .parent_end = in(ex.e2, 2),
                                          .def = in(ex.e1, 8),
                                          .len = 4});
    return ex;
}

static void test_fv_buffer_info(void) {
    fv ex = build_fv();
    odin3_srcbuf_info info;
    TEST_ASSERT_TRUE(odin3_srcman_buffer_info(sm, ex.file.id, &info));
    TEST_ASSERT_EQUAL_UINT32(ODIN3_SRCBUF_FILE, info.kind);
    TEST_ASSERT_EQUAL_UINT32(intern("f.v"), info.name);
    TEST_ASSERT_EQUAL_UINT32(intern("/proj/f.v"), info.resolved);
    TEST_ASSERT_EQUAL_UINT32(intern("work"), info.library);
    TEST_ASSERT_EQUAL_UINT32(1, info.start);
    TEST_ASSERT_EQUAL_UINT32(strlen(F_V), info.len);
    TEST_ASSERT_EQUAL_UINT32(4, info.lines); /* three lines and the empty one after the last \n */
    uint32_t file_end = info.start + info.len;
    TEST_ASSERT_TRUE(odin3_srcman_buffer_info(sm, ex.e1, &info));
    TEST_ASSERT_EQUAL_UINT32(EXP, info.kind);
    TEST_ASSERT_EQUAL_UINT32(file_end + 1, info.start); /* len + 1 reserved: no two touch */
    TEST_ASSERT_EQUAL_UINT32(13, info.len);
    TEST_ASSERT_EQUAL_UINT32(at(ex.file, "2:18"), info.def);
    TEST_ASSERT_EQUAL_UINT32(at(ex.file, "3:12"), info.parent);
    TEST_ASSERT_EQUAL_UINT32(at(ex.file, "3:22"), info.parent_end);
    TEST_ASSERT_EQUAL_UINT32(0, info.resolved);
    TEST_ASSERT_EQUAL_UINT32(0, info.lines);
    TEST_ASSERT_TRUE(odin3_srcman_buffer_info(sm, ex.a3, &info));
    TEST_ASSERT_EQUAL_UINT32(ARG, info.kind);
    TEST_ASSERT_EQUAL_UINT32(intern("INNER"), info.name);
    TEST_ASSERT_EQUAL_UINT32(in(ex.e1, 8), info.def);
    TEST_ASSERT_EQUAL_size_t(0, errors_logged);
}

static void test_fv_buffer_of_and_bounds(void) {
    fv ex = build_fv();
    odin3_srcbuf_info info;
    TEST_ASSERT_TRUE(odin3_srcman_buffer_info(sm, ex.file.id, &info));
    uint32_t file_end = info.start + info.len;
    TEST_ASSERT_FALSE(odin3_srcman_buffer_info(sm, (odin3_srcbuf_id){0}, &info));
    TEST_ASSERT_FALSE(odin3_srcman_buffer_info(sm, (odin3_srcbuf_id){ex.a3.v + 1}, &info));
    /* buffer_of and loc bounds: one-past-the-end belongs to its own buffer */
    TEST_ASSERT_EQUAL_UINT32(ex.file.id.v, odin3_srcman_buffer_of(sm, lc(file_end)).v);
    TEST_ASSERT_EQUAL_UINT32(ex.e1.v, odin3_srcman_buffer_of(sm, lc(file_end + 1)).v);
    TEST_ASSERT_EQUAL_UINT32(ex.e2.v, odin3_srcman_buffer_of(sm, lc(in(ex.e2, 7))).v);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_srcman_buffer_of(sm, lc(0)).v);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_srcman_buffer_of(sm, lc(in(ex.a3, 4) + 1)).v);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_srcman_loc(sm, ex.e1, 14).v);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_srcman_loc(sm, (odin3_srcbuf_id){99}, 0).v);
    TEST_ASSERT_EQUAL_size_t(0, errors_logged);
}

static void test_fv_spelling_expansion_file_locs(void) {
    fv ex = build_fv();
    src_file sf = ex.file;
    uint32_t xx = in(ex.a2, 0);
    uint32_t star = in(ex.a3, 1);
    uint32_t two = in(ex.a3, 3);
    uint32_t paren = in(ex.e2, 0);
    TEST_ASSERT_EQUAL_UINT32(in(ex.a1, 0), spelling(xx)); /* one step: run-in-run */
    TEST_ASSERT_EQUAL_UINT32(at(sf, "3:19"), spelling(spelling(xx)));
    TEST_ASSERT_EQUAL_UINT32(in(ex.e1, 9), spelling(star));
    TEST_ASSERT_EQUAL_UINT32(at(sf, "2:27"), spelling(spelling(star)));
    assert_pos(spelling(spelling(star)), "2:27");
    TEST_ASSERT_EQUAL_UINT32(at(sf, "2:29"), spelling(spelling(two)));
    TEST_ASSERT_EQUAL_UINT32(at(sf, "1:18"), spelling(paren));
    TEST_ASSERT_EQUAL_UINT32(at(sf, "3:1"), spelling(at(sf, "3:1"))); /* a FILE loc is itself */
    TEST_ASSERT_EQUAL_UINT32(at(sf, "3:12"), exp_loc(xx));
    TEST_ASSERT_EQUAL_UINT32(at(sf, "3:12"), exp_loc(star));
    TEST_ASSERT_EQUAL_UINT32(at(sf, "3:12"), exp_loc(two));
    TEST_ASSERT_EQUAL_UINT32(at(sf, "3:12"), exp_loc(paren));
    TEST_ASSERT_EQUAL_UINT32(at(sf, "3:19"), file_loc(xx));
    TEST_ASSERT_EQUAL_UINT32(at(sf, "3:12"), file_loc(star));
    TEST_ASSERT_EQUAL_UINT32(at(sf, "3:12"), file_loc(two));
    TEST_ASSERT_EQUAL_UINT32(at(sf, "3:12"), file_loc(paren));
    TEST_ASSERT_EQUAL_UINT32(at(sf, "3:12"), file_loc(in(ex.e2, 3))); /* '+' */
    assert_pos(xx, "3:19");
    assert_pos(star, "3:12");
    TEST_ASSERT_EQUAL_UINT32(0, spelling(0));
    TEST_ASSERT_EQUAL_UINT32(0, exp_loc(0));
    TEST_ASSERT_EQUAL_UINT32(0, file_loc(0));
    TEST_ASSERT_EQUAL_UINT32(0, spelling(in(ex.a3, 4) + 1)); /* past the space */
    odin3_srcpos pos = {0};
    TEST_ASSERT_FALSE(odin3_srcman_decode(sm, lc(0), &pos));
}

/* Output "assign y = (xx * 2 + 1);": the segment map and the loc of every byte. */
static void test_fv_segment_cursor(void) {
    fv ex = build_fv();
    src_file sf = ex.file;
    odin3_segment segs[] = {
        {ex.file.id.v, 0, at(sf, "3:1"), {0}}, {ex.file.id.v, 11, in(ex.e2, 0), {0}},
        {ex.file.id.v, 12, in(ex.a2, 0), {0}}, {ex.file.id.v, 14, in(ex.a3, 0), {0}},
        {ex.file.id.v, 18, in(ex.e2, 2), {0}}, {ex.file.id.v, 23, at(sf, "3:22"), {0}}};
    for (size_t i = 0; i < sizeof segs / sizeof segs[0]; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_add_segment(sm, &segs[i]));
    }
    uint32_t want[24];
    for (uint32_t i = 0; i < 11; i++) {
        want[i] = at(sf, "3:1") + i;
    }
    want[11] = in(ex.e2, 0);
    want[12] = in(ex.a2, 0);
    want[13] = in(ex.a2, 1);
    for (uint32_t i = 0; i < 4; i++) {
        want[14 + i] = in(ex.a3, i);
    }
    for (uint32_t i = 0; i < 5; i++) {
        want[18 + i] = in(ex.e2, 2 + i);
    }
    want[23] = at(sf, "3:22");
    odin3_srcman_cursor cur;
    odin3_srcman_cursor_init(&cur, sm, ex.file.id.v);
    for (uint32_t i = 0; i < 24; i++) {
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(want[i], odin3_srcman_cursor_loc(&cur, i).v, "forward");
    }
    for (uint32_t i = 24; i-- > 0;) {
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(want[i], odin3_srcman_cursor_loc(&cur, i).v, "backward");
    }
    TEST_ASSERT_EQUAL_UINT32(want[23], odin3_srcman_cursor_loc(&cur, 23).v); /* jump forward */
    TEST_ASSERT_EQUAL_UINT32(want[3], odin3_srcman_cursor_loc(&cur, 3).v);   /* jump back */
    odin3_srcman_cursor none;
    odin3_srcman_cursor_init(&none, sm, ex.e1.v); /* not a stream */
    TEST_ASSERT_EQUAL_UINT32(0, odin3_srcman_cursor_loc(&none, 0).v);
    TEST_ASSERT_EQUAL_size_t(0, errors_logged);
}

static void test_fv_chains_through_diag(void) {
    fv ex = build_fv();
    TEST_ASSERT_EQUAL_STRING("f.v:3:19: error: bad 'xx'\n"
                             "  in argument of macro 'INNER' at f.v:1:19\n"
                             "  in argument of macro 'OUTER' at f.v:2:25",
                             diag_at(in(ex.a2, 0), "xx"));
    TEST_ASSERT_EQUAL_STRING("f.v:3:12: error: bad '*'\n"
                             "  in argument of macro 'INNER' at f.v:1:19\n"
                             "  expanded from macro 'OUTER' at f.v:2:27",
                             diag_at(in(ex.a3, 1), "*"));
    TEST_ASSERT_EQUAL_STRING("f.v:3:12: error: bad '2'\n"
                             "  in argument of macro 'INNER' at f.v:1:19\n"
                             "  expanded from macro 'OUTER' at f.v:2:29",
                             diag_at(in(ex.a3, 3), "2"));
    TEST_ASSERT_EQUAL_STRING("f.v:3:12: error: bad '+'\n"
                             "  expanded from macro 'INNER' at f.v:1:21\n"
                             "  expanded from macro 'OUTER' at f.v:2:18",
                             diag_at(in(ex.e2, 3), "+"));
    TEST_ASSERT_EQUAL_STRING("f.v:3:1: error: bad 'assign'", diag_at(at(ex.file, "3:1"), "assign"));
    delivered = 0;
    odin3_diag(design, ODIN3_LOG_WARN, lc(at(ex.file, "1:1")), "w %d", 1);
    TEST_ASSERT_EQUAL_STRING("f.v:1:1: warning: w 1", captured);
    odin3_diag(design, ODIN3_LOG_INFO, lc(0), "i");
    TEST_ASSERT_EQUAL_STRING("<unknown>:0:0: info: i", captured);
    TEST_ASSERT_EQUAL_INT(2, delivered);
}

typedef struct chain_log {
    odin3_srcchain_entry entries[CHAIN_MAX];
    uint32_t count;
} chain_log;

static void record_entry(void *user, const odin3_srcchain_entry *entry) {
    chain_log *log = user;
    TEST_ASSERT_LESS_THAN_UINT32(CHAIN_MAX, log->count);
    log->entries[log->count++] = *entry;
}

static chain_log chain_of(uint32_t raw) {
    chain_log log;
    memset(&log, 0, sizeof log);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_chain(sm, lc(raw), record_entry, &log));
    return log;
}

static void assert_entry(const odin3_srcchain_entry *entry, uint32_t kind, const char *name,
                         uint32_t where) {
    TEST_ASSERT_EQUAL_UINT32(kind, entry->kind);
    TEST_ASSERT_EQUAL_UINT32(intern(name), entry->name);
    TEST_ASSERT_EQUAL_UINT32(where, entry->at);
}

static void test_chain_entries(void) {
    fv ex = build_fv();
    chain_log log = chain_of(in(ex.a3, 1));
    TEST_ASSERT_EQUAL_UINT32(2, log.count);
    assert_entry(&log.entries[0], ODIN3_SRCCHAIN_ARG, "INNER", at(ex.file, "1:19"));
    assert_entry(&log.entries[1], ODIN3_SRCCHAIN_EXPANSION, "OUTER", at(ex.file, "2:27"));
    TEST_ASSERT_EQUAL_UINT32(0, chain_of(at(ex.file, "3:1")).count);
    TEST_ASSERT_EQUAL_UINT32(0, chain_of(0).count);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_srcman_chain(sm, lc(1), NULL, NULL));
    odin3_strbuf buf;
    odin3_strbuf_init(&buf);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_format_chain(sm, lc(in(ex.e2, 3)), &buf));
    TEST_ASSERT_EQUAL_STRING("\n  expanded from macro 'INNER' at f.v:1:21"
                             "\n  expanded from macro 'OUTER' at f.v:2:18",
                             buf.data);
    odin3_strbuf_free(&buf);
}

/* --- ranges and srcloc (spec §3.2) ---------------------------------------------------------- */

static void assert_srcloc(odin3_range given, const char *file, const uint32_t want[4]) {
    odin3_srcloc out;
    memset(&out, 0xff, sizeof out);
    odin3_srcman_srcloc(sm, given, &out);
    TEST_ASSERT_EQUAL_UINT32(given.loc.v, out.loc); /* raw, as stored on the node */
    TEST_ASSERT_EQUAL_UINT32(file != NULL ? intern(file) : 0, out.file);
    TEST_ASSERT_EQUAL_UINT32(want[0], out.line);
    TEST_ASSERT_EQUAL_UINT32(want[1], out.col);
    TEST_ASSERT_EQUAL_UINT32(want[2], out.end_line);
    TEST_ASSERT_EQUAL_UINT32(want[3], out.end_col);
}

static void test_out_ranges(void) {
    src_file sf = add_text("o.v", 0, "`define OUT y\nassign `OUT = a;\n");
    odin3_srcbuf_id em = expand((odin3_expansion_spec){.kind = EXP,
                                                       .name = intern("OUT"),
                                                       .parent = at(sf, "2:8"),
                                                       .parent_end = at(sf, "2:12"),
                                                       .def = at(sf, "1:13"),
                                                       .len = 1});
    odin3_range ident = span(in(em, 0), in(em, 1));
    odin3_range net_assign = span(in(em, 0), at(sf, "2:16"));
    odin3_range cont_assign = span(at(sf, "2:1"), at(sf, "2:17"));
    assert_range((range_case){.want = span(at(sf, "2:8"), at(sf, "2:12")), .given = ident});
    assert_range((range_case){.want = span(at(sf, "2:8"), at(sf, "2:16")), .given = net_assign});
    assert_range((range_case){.want = cont_assign, .given = cont_assign});
    TEST_ASSERT_EQUAL_UINT32(at(sf, "2:12"), odin3_srcman_expansion_end(sm, lc(in(em, 1))).v);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_srcman_expansion_end(sm, lc(0)).v);
    assert_srcloc(ident, "o.v", (const uint32_t[]){2, 8, 2, 12});
    assert_srcloc(net_assign, "o.v", (const uint32_t[]){2, 8, 2, 16});
    assert_srcloc(cont_assign, "o.v", (const uint32_t[]){2, 1, 2, 17});
    /* not in one file, out of order, or end unknown: the start alone */
    src_file other = add_text("p.v", 0, "x\n");
    assert_range((range_case){.want = span(at(sf, "2:1"), 0),
                              .given = span(at(sf, "2:1"), at(other, "1:1"))});
    assert_range((range_case){.want = span(at(sf, "2:16"), 0),
                              .given = span(at(sf, "2:16"), at(sf, "2:1"))});
    assert_range((range_case){.want = span(at(sf, "2:1"), 0), .given = span(at(sf, "2:1"), 0)});
    assert_srcloc(span(at(sf, "2:1"), at(other, "1:1")), "o.v", (const uint32_t[]){2, 1, 0, 0});
    assert_srcloc(span(0, 0), NULL, (const uint32_t[]){0, 0, 0, 0});
}

/* `define W 8; `assign x = `W + 1;` and `assign z = `W;`: ends map through parent_end. */
static void test_w_ranges(void) {
    src_file sf = add_text("w.v", 0, "`define W 8\nassign x = `W + 1;\nassign z = `W;\n");
    odin3_srcbuf_id em = expand((odin3_expansion_spec){.kind = EXP,
                                                       .name = intern("W"),
                                                       .parent = at(sf, "2:12"),
                                                       .parent_end = at(sf, "2:14"),
                                                       .def = at(sf, "1:11"),
                                                       .len = 1});
    odin3_srcbuf_id e3 = expand((odin3_expansion_spec){.kind = EXP,
                                                       .name = intern("W"),
                                                       .parent = at(sf, "3:12"),
                                                       .parent_end = at(sf, "3:14"),
                                                       .def = at(sf, "1:11"),
                                                       .len = 1});
    assert_range((range_case){.want = span(at(sf, "2:12"), at(sf, "2:14")),
                              .given = span(in(em, 0), in(em, 1))}); /* NUMBER */
    assert_range((range_case){.want = span(at(sf, "2:12"), at(sf, "2:18")),
                              .given = span(in(em, 0), at(sf, "2:18"))}); /* BINARY */
    assert_range((range_case){.want = span(at(sf, "3:8"), at(sf, "3:14")),
                              .given = span(at(sf, "3:8"), in(e3, 1))});
    assert_srcloc(span(at(sf, "3:8"), in(e3, 1)), "w.v", (const uint32_t[]){3, 8, 3, 14});
    assert_srcloc(span(in(em, 0), in(em, 1)), "w.v", (const uint32_t[]){2, 12, 2, 14});
    TEST_ASSERT_EQUAL_STRING("w.v:2:12: error: bad '8'\n  expanded from macro 'W' at w.v:1:11",
                             diag_at(in(em, 0), "8"));
}

static void add_segments(uint32_t stream, const uint32_t (*pairs)[2], size_t count) {
    for (size_t i = 0; i < count; i++) {
        odin3_segment seg = {stream, pairs[i][0], pairs[i][1], {0}};
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_add_segment(sm, &seg));
    }
}

/* x`S with `define S _y lexes as one identifier x_y spanning two segments. */
static void test_token_spanning_segments(void) {
    src_file sf = add_text("s.v", 0, "`define S _y\nwire x`S;\n");
    odin3_srcbuf_id em = expand((odin3_expansion_spec){.kind = EXP,
                                                       .name = intern("S"),
                                                       .parent = at(sf, "2:7"),
                                                       .parent_end = at(sf, "2:9"),
                                                       .def = at(sf, "1:11"),
                                                       .len = 2});
    const uint32_t pairs[][2] = {{0, at(sf, "2:1")}, {6, in(em, 0)}, {8, at(sf, "2:9")}};
    add_segments(sf.id.v, pairs, 3); /* output "wire x_y;" */
    odin3_srcman_cursor cur;
    odin3_srcman_cursor_init(&cur, sm, sf.id.v);
    uint32_t loc = odin3_srcman_cursor_loc(&cur, 5).v;
    uint32_t end = odin3_srcman_cursor_loc(&cur, 7).v + 1;
    TEST_ASSERT_EQUAL_UINT32(at(sf, "2:6"), loc);
    TEST_ASSERT_EQUAL_UINT32(in(em, 2), end); /* one past the expansion, not loc + 3 */
    TEST_ASSERT_EQUAL_UINT32(em.v, odin3_srcman_buffer_of(sm, lc(end)).v);
    assert_range((range_case){.want = span(at(sf, "2:6"), at(sf, "2:9")),
                              .given = span(loc, end)}); /* x through the `S use */
    assert_srcloc(span(loc, end), "s.v", (const uint32_t[]){2, 6, 2, 9});
}

/* 8'h`HI`LO with HI = f and LO = 0: the digits come from two adjacent macros. */
static void test_hex_digits_from_two_macros(void) {
    src_file sf = add_text("h.v", 0, "`define HI f\n`define LO 0\nassign q = 8'h`HI`LO;\n");
    odin3_srcbuf_id hi = expand((odin3_expansion_spec){.kind = EXP,
                                                       .name = intern("HI"),
                                                       .parent = at(sf, "3:15"),
                                                       .parent_end = at(sf, "3:18"),
                                                       .def = at(sf, "1:12"),
                                                       .len = 1});
    odin3_srcbuf_id lo = expand((odin3_expansion_spec){.kind = EXP,
                                                       .name = intern("LO"),
                                                       .parent = at(sf, "3:18"),
                                                       .parent_end = at(sf, "3:21"),
                                                       .def = at(sf, "2:12"),
                                                       .len = 1});
    const uint32_t pairs[][2] = {
        {0, at(sf, "3:1")}, {14, in(hi, 0)}, {15, in(lo, 0)}, {16, at(sf, "3:21")}};
    add_segments(sf.id.v, pairs, 4); /* output "assign q = 8'hf0;" */
    odin3_srcman_cursor cur;
    odin3_srcman_cursor_init(&cur, sm, sf.id.v);
    uint32_t loc = odin3_srcman_cursor_loc(&cur, 11).v;
    TEST_ASSERT_EQUAL_UINT32(in(hi, 0), odin3_srcman_cursor_loc(&cur, 14).v);
    uint32_t end = odin3_srcman_cursor_loc(&cur, 15).v + 1;
    TEST_ASSERT_EQUAL_UINT32(in(lo, 1), end);
    assert_range(
        (range_case){.want = span(at(sf, "3:12"), at(sf, "3:21")), .given = span(loc, end)});
    assert_range((range_case){.want = span(at(sf, "3:15"), at(sf, "3:21")),
                              .given = span(in(hi, 0), end)}); /* digits */
    TEST_ASSERT_EQUAL_STRING("h.v:3:18: error: bad '0'\n  expanded from macro 'LO' at h.v:2:12",
                             diag_at(in(lo, 0), "0"));
}

/* "a, empty comment, b": the stripped comment is an inserted space (a loc-0 segment). */
static void test_comment_space_segment(void) {
    src_file sf = add_text("c.v", 0, "a/**/b\n");
    const uint32_t pairs[][2] = {{0, at(sf, "1:1")}, {1, 0}, {2, at(sf, "1:6")}};
    add_segments(sf.id.v, pairs, 3); /* output "a b" */
    odin3_srcman_cursor cur;
    odin3_srcman_cursor_init(&cur, sm, sf.id.v);
    TEST_ASSERT_EQUAL_UINT32(at(sf, "1:1"), odin3_srcman_cursor_loc(&cur, 0).v);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_srcman_cursor_loc(&cur, 1).v);
    TEST_ASSERT_EQUAL_UINT32(at(sf, "1:6"), odin3_srcman_cursor_loc(&cur, 2).v);
    TEST_ASSERT_EQUAL_UINT32(at(sf, "1:7"), odin3_srcman_cursor_loc(&cur, 3).v);
}

static void test_command_line_buffer(void) {
    odin3_srcfile_spec spec = {.name = intern("<command line>")};
    src_file cmd = add_spec(spec, "`define A 1\n`define B(x) x\n`define N 4\n");
    src_file sf = add_text("n.v", 0, "assign n = `N;\n");
    odin3_srcbuf_id em = expand((odin3_expansion_spec){.kind = EXP,
                                                       .name = intern("N"),
                                                       .parent = at(sf, "1:12"),
                                                       .parent_end = at(sf, "1:14"),
                                                       .def = at(cmd, "3:11"),
                                                       .len = 1});
    TEST_ASSERT_EQUAL_STRING("<command line>:3:9", fmt(at(cmd, "3:9"), ODIN3_SRCFMT_GIVEN));
    TEST_ASSERT_EQUAL_STRING("<command line>:3:9", fmt(at(cmd, "3:9"), ODIN3_SRCFMT_RESOLVED));
    TEST_ASSERT_EQUAL_STRING("n.v:1:12", fmt(at(sf, "1:12"), ODIN3_SRCFMT_GIVEN));
    TEST_ASSERT_EQUAL_STRING("/proj/n.v:1:12", fmt(at(sf, "1:12"), ODIN3_SRCFMT_RESOLVED));
    TEST_ASSERT_EQUAL_STRING("<unknown>:0:0", fmt(0, ODIN3_SRCFMT_GIVEN));
    TEST_ASSERT_EQUAL_STRING("<unknown>:0:0", fmt(0, ODIN3_SRCFMT_RESOLVED));
    TEST_ASSERT_EQUAL_UINT32(at(cmd, "3:11"), spelling(in(em, 0))); /* the body's def */
    TEST_ASSERT_EQUAL_STRING("n.v:1:12: error: bad '4'\n"
                             "  expanded from macro 'N' at <command line>:3:11",
                             diag_at(in(em, 0), "4"));
    odin3_srcbuf_info info;
    TEST_ASSERT_TRUE(odin3_srcman_buffer_info(sm, cmd.id, &info));
    TEST_ASSERT_EQUAL_UINT32(0, info.resolved);
    TEST_ASSERT_EQUAL_UINT32(0, info.library);
}

/* Review Focus 1 of the spec (§12): `define F(x) (x + `W) used inside an included file. */
static void test_nested_expansion_in_include(void) {
    src_file top = add_text("top.v", 0, "module top;\n`include \"inc.v\"\nendmodule\n");
    src_file inc = add_text("inc.v", at(top, "2:1"),
                            "`define F(x) (x + `W)\n`define W 8\nassign y = `F(a);\n");
    odin3_srcbuf_id ef = expand((odin3_expansion_spec){.kind = EXP,
                                                       .name = intern("F"),
                                                       .parent = at(inc, "3:12"),
                                                       .parent_end = at(inc, "3:17"),
                                                       .def = at(inc, "1:14"),
                                                       .len = 8});
    odin3_srcbuf_id ax = expand((odin3_expansion_spec){.kind = ARG,
                                                       .name = intern("F"),
                                                       .parent = in(ef, 1),
                                                       .parent_end = in(ef, 2),
                                                       .def = at(inc, "3:15"),
                                                       .len = 1});
    odin3_srcbuf_id ew = expand((odin3_expansion_spec){.kind = EXP,
                                                       .name = intern("W"),
                                                       .parent = in(ef, 5),
                                                       .parent_end = in(ef, 7),
                                                       .def = at(inc, "2:11"),
                                                       .len = 1});
    TEST_ASSERT_EQUAL_STRING("inc.v:3:15: error: bad 'a'\n"
                             "  in argument of macro 'F' at inc.v:1:15\n"
                             "  included from top.v:2",
                             diag_at(in(ax, 0), "a"));
    TEST_ASSERT_EQUAL_STRING("inc.v:3:12: error: bad '8'\n"
                             "  expanded from macro 'W' at inc.v:2:11\n"
                             "  expanded from macro 'F' at inc.v:1:19\n"
                             "  included from top.v:2",
                             diag_at(in(ew, 0), "8"));
    TEST_ASSERT_EQUAL_STRING("inc.v:3:12: error: bad '+'\n"
                             "  expanded from macro 'F' at inc.v:1:17\n"
                             "  included from top.v:2",
                             diag_at(in(ef, 3), "+"));
    TEST_ASSERT_EQUAL_UINT32(at(inc, "3:12"), exp_loc(in(ew, 0))); /* not the include's parent */
    assert_range((range_case){.want = span(at(inc, "3:12"), at(inc, "3:17")),
                              .given = span(in(ef, 0), in(ef, 8))});
    /* an argument token's end maps through its formal to the use: 3:17, not 3:16 */
    assert_srcloc(span(in(ax, 0), in(ax, 1)), "inc.v", (const uint32_t[]){3, 15, 3, 17});
    assert_srcloc(span(in(ew, 0), in(ew, 1)), "inc.v", (const uint32_t[]){3, 12, 3, 17});
    chain_log log = chain_of(in(ax, 0));
    TEST_ASSERT_EQUAL_UINT32(2, log.count);
    assert_entry(&log.entries[1], ODIN3_SRCCHAIN_INCLUDE, "inc.v", at(top, "2:1"));
}

static void test_two_include_levels(void) {
    src_file top = add_text("top.v", 0, "module top;\n`include \"mid.vh\"\nendmodule\n");
    src_file mid = add_text("mid.vh", at(top, "2:1"), "`include \"leaf.vh\"\n");
    src_file leaf = add_text("leaf.vh", at(mid, "1:1"), "wire w;\n");
    TEST_ASSERT_EQUAL_STRING("leaf.vh:1:6: error: bad 'w'\n"
                             "  included from mid.vh:1\n"
                             "  included from top.v:2",
                             diag_at(at(leaf, "1:6"), "w"));
    TEST_ASSERT_EQUAL_UINT32(at(leaf, "1:6"), exp_loc(at(leaf, "1:6")));
    TEST_ASSERT_EQUAL_UINT32(at(leaf, "1:6"), file_loc(at(leaf, "1:6")));
}

/* A file included twice is two buffers; so is one file in two libraries (DESIGN §4.0). */
static void test_file_included_twice(void) {
    src_file top = add_text("top.v", 0, "`include \"h.vh\"\n`include \"h.vh\"\n");
    src_file h1 = add_text("h.vh", at(top, "1:1"), "wire a;\n");
    src_file h2 = add_text("h.vh", at(top, "2:1"), "wire a;\n");
    TEST_ASSERT_NOT_EQUAL(at(h1, "1:6"), at(h2, "1:6"));
    odin3_srcloc one;
    odin3_srcloc two;
    odin3_srcman_srcloc(sm, span(at(h1, "1:6"), at(h1, "1:7")), &one);
    odin3_srcman_srcloc(sm, span(at(h2, "1:6"), at(h2, "1:7")), &two);
    TEST_ASSERT_EQUAL_UINT32(one.file, two.file);
    TEST_ASSERT_EQUAL_UINT32(one.line, two.line);
    TEST_ASSERT_EQUAL_UINT32(one.col, two.col);
    TEST_ASSERT_NOT_EQUAL(one.loc, two.loc);
    TEST_ASSERT_EQUAL_STRING("h.vh:1:6: error: bad 'a'\n  included from top.v:2",
                             diag_at(at(h2, "1:6"), "a"));
}

static void test_same_file_in_two_libraries(void) {
    odin3_srcloc one;
    odin3_srcloc two;
    odin3_srcfile_spec spec = {.name = intern("lib.v"), .library = intern("work")};
    src_file work = add_spec(spec, "module m; endmodule\n");
    spec.library = intern("other");
    src_file other = add_spec(spec, "module m; endmodule\n");
    TEST_ASSERT_NOT_EQUAL(work.id.v, other.id.v);
    odin3_srcman_srcloc(sm, span(at(work, "1:8"), 0), &one);
    odin3_srcman_srcloc(sm, span(at(other, "1:8"), 0), &two);
    TEST_ASSERT_EQUAL_UINT32(one.file, two.file);
    TEST_ASSERT_NOT_EQUAL(one.loc, two.loc);
    odin3_srcbuf_info info;
    TEST_ASSERT_TRUE(odin3_srcman_buffer_info(sm, other.id, &info));
    TEST_ASSERT_EQUAL_UINT32(intern("other"), info.library);
}

/* SCRATCH: pasted text has no spelling; the chain names the pasting macro. */
static void test_scratch_buffer(void) {
    src_file sf = add_text("p.sv", 0, "`define CAT(a, b) a``b\nwire `CAT(x, y);\n");
    odin3_srcbuf_id em = expand((odin3_expansion_spec){.kind = EXP,
                                                       .name = intern("CAT"),
                                                       .parent = at(sf, "2:6"),
                                                       .parent_end = at(sf, "2:16"),
                                                       .def = at(sf, "1:19"),
                                                       .len = 4});
    odin3_srcbuf_id paste = expand((odin3_expansion_spec){
        .kind = SCRATCH, .parent = in(em, 1), .parent_end = in(em, 3), .len = 2});
    TEST_ASSERT_EQUAL_UINT32(0, spelling(in(paste, 0)));
    TEST_ASSERT_EQUAL_UINT32(at(sf, "2:6"), exp_loc(in(paste, 0)));
    TEST_ASSERT_EQUAL_UINT32(at(sf, "2:6"), file_loc(in(paste, 1)));
    TEST_ASSERT_EQUAL_STRING("p.sv:2:6: error: bad 'xy'\n"
                             "  pasted by macro 'CAT'\n"
                             "  expanded from macro 'CAT' at p.sv:1:20",
                             diag_at(in(paste, 0), "xy"));
    chain_log log = chain_of(in(paste, 0));
    assert_entry(&log.entries[0], ODIN3_SRCCHAIN_SCRATCH, "CAT", at(sf, "1:20"));
}

/* --- misuse --------------------------------------------------------------------------------- */

static uint32_t buffer_count(void) {
    return (uint32_t)sm->bufs.len;
}

static void expect_bad_expansion(odin3_expansion_spec spec) {
    uint32_t before = buffer_count();
    size_t errors = errors_logged;
    odin3_srcbuf_id id = {77};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_srcman_add_expansion(sm, &spec, &id));
    TEST_ASSERT_EQUAL_UINT32(77, id.v);
    TEST_ASSERT_EQUAL_UINT32(before, buffer_count());
    TEST_ASSERT_EQUAL_size_t(errors + 1, errors_logged);
}

static void test_bad_expansions(void) {
    src_file sf = add_text("b.v", 0, "`define M(p) (p)\nx = `M(1);\n");
    odin3_expansion_spec good = {.kind = EXP,
                                 .name = intern("M"),
                                 .parent = at(sf, "2:5"),
                                 .parent_end = at(sf, "2:10"),
                                 .def = at(sf, "1:14"),
                                 .len = 3};
    odin3_expansion_spec spec = good;
    spec.kind = ODIN3_SRCBUF_FILE;
    expect_bad_expansion(spec);
    spec = good;
    spec.kind = 9;
    expect_bad_expansion(spec);
    spec = good;
    spec.name = 0;
    expect_bad_expansion(spec);
    spec.name = 100000; /* not a strtab ID */
    expect_bad_expansion(spec);
    spec = good;
    spec.parent = 0;
    expect_bad_expansion(spec);
    spec.parent = 100000; /* past the space */
    expect_bad_expansion(spec);
    spec = good;
    spec.parent_end = 100000;
    expect_bad_expansion(spec);
    spec = good;
    spec.def = 0;
    expect_bad_expansion(spec);
    spec.def = at(sf, "2:11"); /* the body would run past the end of b.v */
    spec.len = 10;
    expect_bad_expansion(spec);
    spec = good;
    spec.reserved[1] = 1;
    expect_bad_expansion(spec);
    spec = good;
    spec.kind = SCRATCH;
    spec.def = 0;
    expect_bad_expansion(spec); /* SCRATCH takes no name */
    spec.name = 0;
    spec.def = at(sf, "1:14");
    expect_bad_expansion(spec); /* nor a def */
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_srcman_add_expansion(sm, &good, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_srcman_add_expansion(NULL, &good, NULL));
    good.parent_end = 0; /* unknown end is allowed */
    odin3_srcbuf_id last = expand(good);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_srcman_expansion_end(sm, lc(in(last, 3))).v);
}

static void expect_bad_file(odin3_srcfile_spec spec) {
    uint32_t before = buffer_count();
    size_t files = sm->files.len;
    odin3_srcbuf_id id = {77};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_srcman_add_file(sm, &spec, &id));
    TEST_ASSERT_EQUAL_UINT32(77, id.v);
    TEST_ASSERT_EQUAL_UINT32(before, buffer_count());
    TEST_ASSERT_EQUAL_size_t(files, sm->files.len);
}

static void test_bad_files_lines_segments(void) {
    src_file sf = add_text("g.v", 0, "ab\ncd\n");
    odin3_srcfile_spec good = {.name = intern("x.v"), .len = 3};
    odin3_srcfile_spec spec = good;
    spec.name = 0;
    expect_bad_file(spec);
    spec = good;
    spec.resolved = 100000;
    expect_bad_file(spec);
    spec = good;
    spec.library = 100000;
    expect_bad_file(spec);
    spec = good;
    spec.parent = 100000;
    expect_bad_file(spec);
    spec = good;
    spec.reserved[0] = 1;
    expect_bad_file(spec);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_srcman_add_file(sm, NULL, &sf.id));
    /* lines: strictly increasing, above 0, at most len, FILE buffers only */
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_srcman_add_line(sm, sf.id, 6));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_srcman_add_line(sm, sf.id, 7));
    odin3_srcfile_spec fresh_spec = {.name = intern("y.v"), .len = 4};
    odin3_srcbuf_id fresh = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_add_file(sm, &fresh_spec, &fresh));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_srcman_add_line(sm, fresh, 0));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_srcman_add_line(sm, fresh, 5));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_add_line(sm, fresh, 4));
    odin3_srcbuf_id em = expand((odin3_expansion_spec){
        .kind = EXP, .name = intern("M"), .parent = at(sf, "1:1"), .def = at(sf, "2:1"), .len = 1});
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_srcman_add_line(sm, em, 1));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_srcman_add_line(sm, (odin3_srcbuf_id){0}, 1));
    /* segments: a project FILE stream, increasing offsets, a real loc, zero reserved */
    odin3_srcfile_spec inc_spec = {.name = intern("i.vh"), .parent = at(sf, "1:1"), .len = 1};
    odin3_srcbuf_id inc = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_add_file(sm, &inc_spec, &inc));
    odin3_segment bad[] = {{inc.v, 0, at(sf, "1:1"), {0}},
                           {em.v, 0, at(sf, "1:1"), {0}},
                           {0, 0, at(sf, "1:1"), {0}},
                           {sf.id.v, 0, 100000, {0}},
                           {sf.id.v, 0, at(sf, "1:1"), {0, 1}}};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_srcman_add_segment(sm, &bad[i]));
    }
    odin3_segment seg = {sf.id.v, 5, at(sf, "1:1"), {0}};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_add_segment(sm, &seg));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_srcman_add_segment(sm, &seg));
    seg.out_offset = 4;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_srcman_add_segment(sm, &seg));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_srcman_add_segment(sm, NULL));
    odin3_srcman_cursor cur;
    odin3_srcman_cursor_init(&cur, sm, sf.id.v);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_srcman_cursor_loc(&cur, 4).v); /* before the first */
    TEST_ASSERT_EQUAL_UINT32(at(sf, "1:2"), odin3_srcman_cursor_loc(&cur, 6).v);
}

/* --- limits (spec §3.5) --------------------------------------------------------------------- */

static void test_limit_file_bytes(void) {
    src_file top = add_text("top.v", 0, "x\n`include \"big.v\"\n");
    odin3_srcfile_spec spec = {
        .name = intern("big.v"), .parent = at(top, "2:1"), .len = ODIN3_SRC_MAX_FILE_BYTES + 1};
    odin3_srcbuf_id id = {0};
    uint32_t before = buffer_count();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_srcman_add_file(sm, &spec, &id));
    TEST_ASSERT_EQUAL_UINT32(before, buffer_count());
    TEST_ASSERT_EQUAL_STRING_LEN("top.v:2:1: error: ", captured, strlen("top.v:2:1: error: "));
    TEST_ASSERT_NOT_NULL(strstr(captured, "big.v"));
    spec.len = ODIN3_SRC_MAX_FILE_BYTES; /* the limit itself is fine (no text is stored) */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_add_file(sm, &spec, &id));
    TEST_ASSERT_EQUAL_UINT32(1, errors_logged);
}

static void test_limit_buffers(void) {
    src_file sf = add_text("l.v", 0, "`define M 1\nx = `M;\n");
    odin3_srcman_test_set_limits(sm, (odin3_srcman_limits){2, UINT32_MAX});
    odin3_expansion_spec spec = {.kind = EXP,
                                 .name = intern("M"),
                                 .parent = at(sf, "2:5"),
                                 .parent_end = at(sf, "2:7"),
                                 .def = at(sf, "1:11"),
                                 .len = 1};
    (void)expand(spec);
    odin3_srcbuf_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_srcman_add_expansion(sm, &spec, &id));
    TEST_ASSERT_EQUAL_STRING_LEN("l.v:2:5: error: ", captured, strlen("l.v:2:5: error: "));
    TEST_ASSERT_NOT_NULL(strstr(captured, "buffers"));
    odin3_srcfile_spec file = {.name = intern("m.v"), .parent = at(sf, "1:1"), .len = 1};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_srcman_add_file(sm, &file, &id));
    TEST_ASSERT_EQUAL_UINT32(3, buffer_count());
    TEST_ASSERT_EQUAL_UINT32(2, errors_logged);
}

static void test_limit_location_space(void) {
    src_file sf = add_text("s.v", 0, "`define M 12\nx = `M;\n"); /* locs 1..22 */
    odin3_srcman_test_set_limits(sm, (odin3_srcman_limits){ODIN3_SRC_MAX_BUFFERS, 30});
    odin3_expansion_spec spec = {.kind = EXP,
                                 .name = intern("M"),
                                 .parent = at(sf, "2:5"),
                                 .parent_end = at(sf, "2:7"),
                                 .def = at(sf, "1:11"),
                                 .len = 2};
    odin3_srcbuf_id em = expand(spec); /* locs 23..25 */
    TEST_ASSERT_EQUAL_UINT32(25, in(em, 2));
    odin3_srcfile_spec file = {.name = intern("t.v"), .len = 5}; /* would need 26..31 */
    odin3_srcbuf_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_srcman_add_file(sm, &file, &id));
    TEST_ASSERT_EQUAL_STRING("<unknown>:0:0: error: source location space exhausted", captured);
    file.len = 4; /* 26..30 fits exactly */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_add_file(sm, &file, &id));
    TEST_ASSERT_EQUAL_UINT32(30, odin3_srcman_loc(sm, id, 4).v);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_srcman_add_expansion(sm, &spec, &id));
    TEST_ASSERT_EQUAL_STRING("s.v:2:5: error: source location space exhausted", captured);
    file.len = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_srcman_add_file(sm, &file, &id));
    TEST_ASSERT_EQUAL_size_t(3, errors_logged); /* three PARSE errors */
}

/* --- Review Focus (plan) -------------------------------------------------------------------- */

/* RF1: a chain longer than ODIN3_LOG_BUF arrives as one message with the header intact. */
static void test_review_focus_long_chain(void) {
    src_file sf = add_text("m.v", 0, "`define M(p) (p)\nx = `M(a);\n");
    uint32_t prev = at(sf, "2:8");
    for (uint32_t i = 0; i < DEEP_ARGS; i++) {
        odin3_srcbuf_id em = expand((odin3_expansion_spec){.kind = EXP,
                                                           .name = intern("M"),
                                                           .parent = at(sf, "2:5"),
                                                           .parent_end = at(sf, "2:10"),
                                                           .def = at(sf, "1:14"),
                                                           .len = 3});
        odin3_srcbuf_id arg = expand((odin3_expansion_spec){.kind = ARG,
                                                            .name = intern("M"),
                                                            .parent = in(em, 1),
                                                            .parent_end = in(em, 2),
                                                            .def = prev,
                                                            .len = 1});
        prev = in(arg, 0);
    }
    const char *msg = diag_at(prev, "a");
    const char *header = "m.v:2:8: error: bad 'a'\n  in argument of macro 'M' at m.v:1:15\n";
    TEST_ASSERT_EQUAL_STRING_LEN(header, msg, strlen(header));
    TEST_ASSERT_EQUAL_size_t(ODIN3_LOG_BUF - 1, strlen(msg));
    TEST_ASSERT_EQUAL_STRING("...", msg + strlen(msg) - 3);
    TEST_ASSERT_EQUAL_UINT32(DEEP_ARGS, chain_of(prev).count);
}

/* RF2: line and column at file edges. */
static void test_review_focus_file_edges(void) {
    src_file empty = add_text("empty.v", 0, "");
    TEST_ASSERT_EQUAL_UINT32(empty.id.v, odin3_srcman_buffer_of(sm, lc(in(empty.id, 0))).v);
    assert_pos(in(empty.id, 0), "1:1");
    TEST_ASSERT_EQUAL_STRING("empty.v:1:1", fmt(in(empty.id, 0), ODIN3_SRCFMT_GIVEN));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_srcman_loc(sm, empty.id, 1).v);
    src_file nonl = add_text("nonl.v", 0, "ab\ncd");
    assert_pos(in(nonl.id, 4), "2:2");
    assert_pos(in(nonl.id, 5), "2:3"); /* a token ending at end of file: one past the end */
    TEST_ASSERT_EQUAL_UINT32(nonl.id.v, odin3_srcman_buffer_of(sm, lc(in(nonl.id, 5))).v);
    assert_range((range_case){.want = span(in(nonl.id, 3), in(nonl.id, 5)),
                              .given = span(in(nonl.id, 3), in(nonl.id, 5))});
    assert_srcloc(span(in(nonl.id, 3), in(nonl.id, 5)), "nonl.v", (const uint32_t[]){2, 1, 2, 3});
    src_file crlf = add_text("crlf.v", 0, "a\r\nb\r\n");
    assert_pos(in(crlf.id, 1), "1:2"); /* \r is a column byte */
    assert_pos(in(crlf.id, 2), "1:3");
    assert_pos(in(crlf.id, 3), "2:1");
    assert_pos(in(crlf.id, 6), "3:1");
    odin3_srcpos pos = {0};
    TEST_ASSERT_TRUE(odin3_srcman_decode(sm, lc(in(crlf.id, 3)), &pos));
    TEST_ASSERT_EQUAL_UINT32(crlf.id.v, pos.buffer);
}

/* --- design ownership, srcloc into provenance ------------------------------------------------ */

static void test_design_owns_srcman(void) {
    odin3_srcman *again = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_srcman(design, &again));
    TEST_ASSERT_EQUAL_PTR(sm, again);
    size_t empty = odin3_srcman_bytes_reserved(sm);
    TEST_ASSERT_GREATER_THAN_size_t(sizeof(odin3_srcbuf_rec), empty);
    (void)build_fv();
    TEST_ASSERT_GREATER_THAN_size_t(empty, odin3_srcman_bytes_reserved(sm));
    odin3_srcman_destroy(NULL);
}

static void test_srcloc_into_provenance(void) {
    fv ex = build_fv();
    odin3_srcloc locs[2];
    odin3_srcman_srcloc(sm, span(in(ex.a2, 0), in(ex.a2, 2)), &locs[0]);
    odin3_srcman_srcloc(sm, span(in(ex.a1, 0), in(ex.a1, 2)), &locs[1]);
    TEST_ASSERT_EQUAL_UINT32(locs[0].line, locs[1].line);
    TEST_ASSERT_EQUAL_UINT32(locs[0].col, locs[1].col); /* both 3:19 */
    odin3_pass_ctx ctx = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(design, intern("read_verilog"), &ctx));
    odin3_prov_id first = {0};
    odin3_prov_id second = {0};
    odin3_prov_origin origin = {&locs[0], 1, 0, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_source(&ctx, &origin, &first));
    origin.locs = &locs[1];
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_source(&ctx, &origin, &second));
    TEST_ASSERT_NOT_EQUAL(first.v, second.v);
    const odin3_prov_record *rec = odin3_prov_get(design, first);
    TEST_ASSERT_EQUAL_STRING("f.v:3:19: error: bad 'xx'\n"
                             "  in argument of macro 'INNER' at f.v:1:19\n"
                             "  in argument of macro 'OUTER' at f.v:2:25",
                             diag_at(rec->locs[0].loc, "xx"));
}

/* --- OOM ------------------------------------------------------------------------------------ */

/* Fills the buffer table to its capacity so the next add must grow it. */
static void fill_buffers(void) {
    while (sm->bufs.len < sm->bufs.cap) {
        odin3_srcfile_spec spec = {.name = intern("fill.v"), .len = 1};
        odin3_srcbuf_id id = {0};
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_add_file(sm, &spec, &id));
    }
}

typedef struct counts {
    size_t bufs, files, lines, segs;
    uint64_t next;
} counts;

static counts counts_of(odin3_srcbuf_id file) {
    counts out = {sm->bufs.len, sm->files.len, 0, 0, sm->next};
    const odin3_srcbuf_rec *rec = odin3_vec_cat(&sm->bufs, file.v);
    const odin3_srcfile_rec *frec = odin3_vec_cat(&sm->files, rec->file);
    out.lines = frec->lines.len;
    out.segs = frec->segs.len;
    return out;
}

static void assert_counts(counts want, counts got) {
    TEST_ASSERT_EQUAL_size_t(want.bufs, got.bufs);
    TEST_ASSERT_EQUAL_size_t(want.files, got.files);
    TEST_ASSERT_EQUAL_size_t(want.lines, got.lines);
    TEST_ASSERT_EQUAL_size_t(want.segs, got.segs);
    TEST_ASSERT_TRUE(want.next == got.next);
}

/* What an OOM step runs against, prepared before failures are injected: macro has a macro and
 * lines, plain (a project file) no lines and no segments yet. */
typedef struct oom_files {
    src_file macro;
    src_file plain;
    odin3_srcfile_spec file;
    odin3_expansion_spec exp;
    odin3_segment seg;
} oom_files;

typedef odin3_status (*oom_step)(const oom_files *files);

static odin3_status step_add_file(const oom_files *files) {
    odin3_srcbuf_id id = {0};
    return odin3_srcman_add_file(sm, &files->file, &id);
}

static odin3_status step_add_expansion(const oom_files *files) {
    odin3_srcbuf_id id = {0};
    return odin3_srcman_add_expansion(sm, &files->exp, &id);
}

static odin3_status step_add_line(const oom_files *files) {
    return odin3_srcman_add_line(sm, files->plain.id, 3);
}

static odin3_status step_add_segment(const oom_files *files) {
    return odin3_srcman_add_segment(sm, &files->seg);
}

static oom_files oom_setup(void) {
    oom_files files;
    memset(&files, 0, sizeof files);
    files.macro = add_text("o.v", 0, "`define M 1\nx = `M;\n");
    odin3_srcfile_spec spec = {.name = intern("g.v")};
    files.plain = add_spec(spec, "x = 1;");
    src_file sf = files.macro;
    files.file = (odin3_srcfile_spec){.name = intern("n.v"), .len = 4};
    files.exp = (odin3_expansion_spec){.kind = EXP,
                                       .name = intern("M"),
                                       .parent = at(sf, "2:5"),
                                       .parent_end = at(sf, "2:7"),
                                       .def = at(sf, "1:11"),
                                       .len = 1};
    files.seg = (odin3_segment){files.plain.id.v, 0, in(files.plain.id, 0), {0}};
    fill_buffers();
    return files;
}

/*
 * Runs step at every failure point, each on a fresh design whose buffer table is full: every
 * failure is NO_MEMORY with the counts unchanged; returns the number of failure points.
 */
static long oom_sweep(oom_step step) {
    for (long k = 0; k < OOM_LIMIT; k++) {
        tearDown();
        setUp();
        oom_files files = oom_setup();
        counts before = counts_of(files.plain.id);
        counts before_f = counts_of(files.macro.id);
        odin3_util_set_alloc_fail_after(k);
        odin3_status st = step(&files);
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            return k;
        }
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        assert_counts(before, counts_of(files.plain.id));
        assert_counts(before_f, counts_of(files.macro.id));
    }
    TEST_FAIL_MESSAGE("no success within OOM_LIMIT");
    return 0;
}

static void test_oom_builders(void) {
    TEST_ASSERT_EQUAL_INT(2, oom_sweep(step_add_file)); /* buffer table, file table */
    TEST_ASSERT_EQUAL_INT(1, oom_sweep(step_add_expansion));
    TEST_ASSERT_EQUAL_INT(1, oom_sweep(step_add_line));
    TEST_ASSERT_EQUAL_INT(1, oom_sweep(step_add_segment));
}

static void test_oom_create(void) {
    odin3_util_set_alloc_fail_after(0);
    TEST_ASSERT_NULL(odin3_srcman_create(odin3_design_strtab(design)));
    for (long k = 0; k < OOM_LIMIT; k++) {
        odin3_srcman *made = NULL;
        odin3_util_set_alloc_fail_after(k);
        made = odin3_srcman_create(odin3_design_strtab(design));
        odin3_util_set_alloc_fail_after(-1);
        if (made != NULL) {
            TEST_ASSERT_GREATER_THAN_INT(0, k);
            odin3_srcman_destroy(made);
            return;
        }
    }
    TEST_FAIL_MESSAGE("create never succeeded");
}

static void test_oom_get_srcman_and_diag(void) {
    odin3_design *other = odin3_design_create();
    TEST_ASSERT_NOT_NULL(other);
    odin3_srcman *got = NULL;
    odin3_util_set_alloc_fail_after(0);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, odin3_design_get_srcman(other, &got));
    TEST_ASSERT_NULL(got);
    odin3_util_set_alloc_fail_after(-1);
    delivered = 0;
    odin3_util_set_alloc_fail_after(0); /* no source manager: header with <unknown> */
    odin3_diag(other, ODIN3_LOG_ERROR, lc(5), "lost %s", "it");
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_EQUAL_INT(1, delivered);
    TEST_ASSERT_EQUAL_STRING("<unknown>:0:0: error: lost it", captured);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_srcman(other, &got));
    TEST_ASSERT_NOT_NULL(got);
    odin3_design_destroy(other);
}

/* Every failure point of diag/format/format_chain: one message, the header alone or complete. */
static void test_oom_printing(void) {
    fv ex = build_fv();
    uint32_t star = in(ex.a3, 1);
    const char *full = "f.v:3:12: error: bad '*'\n"
                       "  in argument of macro 'INNER' at f.v:1:19\n"
                       "  expanded from macro 'OUTER' at f.v:2:27";
    long k = 0;
    for (;; k++) {
        TEST_ASSERT_LESS_THAN_INT(OOM_LIMIT, k);
        delivered = 0;
        odin3_util_set_alloc_fail_after(k);
        odin3_diag(design, ODIN3_LOG_ERROR, lc(star), "bad '%s'", "*");
        odin3_util_set_alloc_fail_after(-1);
        TEST_ASSERT_EQUAL_INT(1, delivered);
        if (strcmp(captured, full) == 0) {
            break;
        }
        TEST_ASSERT_EQUAL_STRING("f.v:3:12: error: bad '*'", captured);
    }
    TEST_ASSERT_GREATER_THAN_INT(0, k);
    odin3_strbuf buf;
    odin3_strbuf_init(&buf);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(&buf, odin3_bytes_cstr("x")));
    for (k = 0; k < 2; k++) {
        odin3_util_set_alloc_fail_after(k);
        odin3_status chain = odin3_srcman_format_chain(sm, lc(star), &buf);
        odin3_util_set_alloc_fail_after(-1);
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, chain);
        TEST_ASSERT_EQUAL_STRING("x", buf.data); /* unchanged */
    }
    odin3_strbuf_free(&buf);
    odin3_strbuf_init(&buf);
    odin3_util_set_alloc_fail_after(0);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY,
                          odin3_srcman_format(sm, lc(star), &buf, ODIN3_SRCFMT_GIVEN));
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_EQUAL_size_t(0, buf.len);
    odin3_strbuf_free(&buf);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_fv_buffer_info);
    RUN_TEST(test_fv_buffer_of_and_bounds);
    RUN_TEST(test_fv_spelling_expansion_file_locs);
    RUN_TEST(test_fv_segment_cursor);
    RUN_TEST(test_fv_chains_through_diag);
    RUN_TEST(test_chain_entries);
    RUN_TEST(test_out_ranges);
    RUN_TEST(test_w_ranges);
    RUN_TEST(test_token_spanning_segments);
    RUN_TEST(test_hex_digits_from_two_macros);
    RUN_TEST(test_comment_space_segment);
    RUN_TEST(test_command_line_buffer);
    RUN_TEST(test_nested_expansion_in_include);
    RUN_TEST(test_two_include_levels);
    RUN_TEST(test_file_included_twice);
    RUN_TEST(test_same_file_in_two_libraries);
    RUN_TEST(test_scratch_buffer);
    RUN_TEST(test_bad_expansions);
    RUN_TEST(test_bad_files_lines_segments);
    RUN_TEST(test_limit_file_bytes);
    RUN_TEST(test_limit_buffers);
    RUN_TEST(test_limit_location_space);
    RUN_TEST(test_review_focus_long_chain);
    RUN_TEST(test_review_focus_file_edges);
    RUN_TEST(test_design_owns_srcman);
    RUN_TEST(test_srcloc_into_provenance);
    RUN_TEST(test_oom_builders);
    RUN_TEST(test_oom_create);
    RUN_TEST(test_oom_get_srcman_and_diag);
    RUN_TEST(test_oom_printing);
    return UNITY_END();
}
