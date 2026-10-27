/*
 * test_name_map.c — unit tests for the name map of hashed names (spec 5.4f: H-4; T-hash-5).
 */
#include "backends/common/hash_name.h"
#include "backends/common/name_map.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MSG_MAX = 16, MSG_LEN = 256, SPEC_COUNT = 6, TEXT_BUF = 4096, HASH_CHARS = 10 };

static const char HEADER[] = "# odin3-names 1 hash=sha256 width=10\n";
/* Real hash names of "a@t.v:1" and "x@f.v:7$2" (T-hash-1 table), at 10 and 15 characters. */
static const char GOOD1[] = "m\tnet\th42xiy3yiju\ta@t.v:1\t\tt.v\t1\n";
static const char GOOD2[] = "m\tnode\thfxxdd62auq\tx@f.v:7$2\t\tf.v\t7\n";
static const char LONG15[] = "m\tnet\th42xiy3yijuqwvoj\ta@t.v:1\t\tt.v\t1\n";

static char warnings[MSG_MAX][MSG_LEN];
static unsigned warn_count;
static char errors[MSG_MAX][MSG_LEN];
static unsigned error_count;

static void sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (level == ODIN3_LOG_WARN && warn_count < MSG_MAX) {
        (void)snprintf(warnings[warn_count++], MSG_LEN, "%s", msg);
    } else if (level == ODIN3_LOG_ERROR && error_count < MSG_MAX) {
        (void)snprintf(errors[error_count++], MSG_LEN, "%s", msg);
    }
}

void setUp(void) {
    warn_count = 0;
    error_count = 0;
    odin3_log_set_sink(sink, NULL);
}

void tearDown(void) {
    odin3_log_set_sink(NULL, NULL);
    odin3_util_set_alloc_fail_after(-1);
}

/* --- entries with real hash names ------------------------------------------------------------ */

typedef struct spec {
    const char *module;
    odin3_name_kind kind;
    const char *full, *hier, *file;
    uint32_t line;
} spec;

/* Fields with TAB, LF, `%`, `\`, a space in a file, empty fields and the largest line number. */
static const spec SPECS[SPEC_COUNT] = {
    {"top", ODIN3_NAME_NET, "top/u/n@src/a.v:10", "top/u", "src/a.v", 10},
    {"top", ODIN3_NAME_NODE, "top/u/g@src/a.v:11", "top/u", "src/a.v", 11},
    {"top", ODIN3_NAME_WIRE, "top/w@src/a.v:12", "top", "src/a.v", 12},
    {"sub", ODIN3_NAME_NET, "q@src/b.v:3", "", "src/b.v", 3},
    {"we\tird%mod", ODIN3_NAME_NET, "x\ty%z\\w\nv@dir name/a\tb.v:4", "h\nier\\", "dir name/a\tb.v",
     4},
    {"", ODIN3_NAME_NODE, "", "", "", UINT32_MAX},
};

typedef struct fixture {
    odin3_name_entry entry[SPEC_COUNT];
    odin3_hash_name hash[SPEC_COUNT];
} fixture;

static void make_entries(fixture *fx) {
    for (uint32_t i = 0; i < SPEC_COUNT; i++) {
        odin3_hash_name_make(odin3_bytes_cstr(SPECS[i].full), HASH_CHARS, &fx->hash[i]);
        fx->entry[i] = (odin3_name_entry){odin3_bytes_cstr(SPECS[i].module),
                                          SPECS[i].kind,
                                          odin3_bytes_cstr(fx->hash[i].text),
                                          odin3_bytes_cstr(SPECS[i].full),
                                          odin3_bytes_cstr(SPECS[i].hier),
                                          odin3_bytes_cstr(SPECS[i].file),
                                          SPECS[i].line};
    }
}

static void expect_bytes(odin3_bytes got, odin3_bytes want) {
    TEST_ASSERT_EQUAL_UINT(want.len, got.len);
    if (want.len > 0) {
        TEST_ASSERT_EQUAL_MEMORY(want.ptr, got.ptr, want.len);
    }
}

static void expect_same_entry(const odin3_name_entry *got, const odin3_name_entry *want) {
    expect_bytes(got->module, want->module);
    TEST_ASSERT_EQUAL_INT(want->kind, got->kind);
    expect_bytes(got->short_name, want->short_name);
    expect_bytes(got->full, want->full);
    expect_bytes(got->hier, want->hier);
    expect_bytes(got->file, want->file);
    TEST_ASSERT_EQUAL_UINT(want->line, got->line);
}

static odin3_bytes view_of(const odin3_strbuf *buf) {
    return (odin3_bytes){buf->data, buf->len};
}

static odin3_bytes text_of(const char *text) {
    return odin3_bytes_cstr(text);
}

/* Parses text as "name.map" into a fresh map. */
static odin3_status parse_text(odin3_name_map *map, odin3_bytes text) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_name_map_init(map));
    return odin3_name_map_parse(map, text, "name.map");
}

static void expect_warning(unsigned idx, const char *needle) {
    TEST_ASSERT_TRUE_MESSAGE(idx < warn_count, needle);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(warnings[idx], needle), warnings[idx]);
}

/* --- write ---------------------------------------------------------------------------------------
 */

static void test_write_exact_text(void) {
    odin3_name_entry entry = {text_of("m"),
                              ODIN3_NAME_NET,
                              text_of("h42xiy3yiju"),
                              text_of("a@t.v:1"),
                              text_of(""),
                              text_of("t.v"),
                              1};
    odin3_strbuf out;
    odin3_strbuf_init(&out);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_name_map_write(&entry, 1, 10, &out));
    char want[TEXT_BUF];
    (void)snprintf(want, sizeof want, "%s%s", HEADER, GOOD1);
    TEST_ASSERT_EQUAL_STRING(want, out.data);
    odin3_strbuf_free(&out);
}

static void test_write_escapes(void) {
    odin3_name_entry entry = {text_of("a\tb"),
                              ODIN3_NAME_WIRE,
                              text_of("s"),
                              text_of("c%d"),
                              text_of("e\\f"),
                              text_of("g\nh i"),
                              5};
    odin3_strbuf out;
    odin3_strbuf_init(&out);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_name_map_write(&entry, 1, 10, &out));
    TEST_ASSERT_EQUAL_STRING("# odin3-names 1 hash=sha256 width=10\n"
                             "a%09b\twire\ts\tc%25d\te%5Cf\tg%0Ah i\t5\n",
                             out.data);
    odin3_strbuf_free(&out);
}

/* Module "" sorts first, then "sub", "top", then "we\tird%mod" (byte order). */
static const char *position_of(const char *text, const char *needle) {
    const char *at = strstr(text, needle);
    TEST_ASSERT_NOT_NULL_MESSAGE(at, needle);
    return at;
}

static void expect_module_order(const char *text) {
    const char *sub = position_of(text, "\nsub\t");
    const char *top = position_of(text, "\ntop\t");
    const char *weird = position_of(text, "\nwe%09ird");
    TEST_ASSERT_TRUE(sub < top && top < weird);
}

static void test_write_sorted_and_order_independent(void) {
    fixture fx;
    odin3_name_entry rev[SPEC_COUNT];
    odin3_strbuf one;
    odin3_strbuf two;
    make_entries(&fx);
    for (uint32_t i = 0; i < SPEC_COUNT; i++) {
        rev[i] = fx.entry[SPEC_COUNT - 1 - i];
    }
    odin3_strbuf_init(&one);
    odin3_strbuf_init(&two);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_name_map_write(fx.entry, SPEC_COUNT, 10, &one));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_name_map_write(rev, SPEC_COUNT, 10, &two));
    TEST_ASSERT_EQUAL_STRING(one.data, two.data);
    expect_module_order(one.data);
    odin3_strbuf_free(&one);
    odin3_strbuf_free(&two);
}

static void test_write_appends_and_refuses_duplicates(void) {
    fixture fx;
    odin3_name_entry dup[2];
    odin3_strbuf out;
    make_entries(&fx);
    dup[0] = fx.entry[0];
    dup[1] = fx.entry[0];
    odin3_strbuf_init(&out);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(&out, text_of("keep\n")));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_name_map_write(dup, 2, 10, &out));
    TEST_ASSERT_EQUAL_STRING("keep\n", out.data);
    TEST_ASSERT_EQUAL_UINT(1, error_count);
    TEST_ASSERT_TRUE(strstr(errors[0], "two entries name") != NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_name_map_write(fx.entry, 1, 10, &out));
    TEST_ASSERT_EQUAL_UINT(0, strncmp(out.data, "keep\n# odin3-names 1", 20));
    odin3_strbuf_free(&out);
}

static void test_write_rejects_bad_arguments(void) {
    fixture fx;
    odin3_strbuf out;
    make_entries(&fx);
    odin3_strbuf_init(&out);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_name_map_write(fx.entry, 1, 0, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_name_map_write(fx.entry, 1, 53, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_name_map_write(NULL, ODIN3_NAME_MAP_MAX_ENTRIES + 1U, 10, &out));
    fx.entry[0].kind = ODIN3_NAME_KIND_COUNT;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_name_map_write(fx.entry, 1, 10, &out));
    TEST_ASSERT_EQUAL_UINT(0, out.len);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_name_map_write(NULL, 0, 52, &out));
    TEST_ASSERT_EQUAL_STRING("# odin3-names 1 hash=sha256 width=52\n", out.data);
    odin3_strbuf_free(&out);
}

static void test_write_out_of_memory(void) {
    fixture fx;
    make_entries(&fx);
    int fails = 0;
    for (long k = 0; k < 40; k++) {
        odin3_strbuf out;
        odin3_strbuf_init(&out);
        odin3_util_set_alloc_fail_after(k);
        odin3_status st = odin3_name_map_write(fx.entry, SPEC_COUNT, 10, &out);
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            odin3_strbuf_free(&out);
            break;
        }
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        TEST_ASSERT_EQUAL_UINT(0, out.len);
        odin3_strbuf_free(&out);
        fails++;
    }
    TEST_ASSERT_TRUE(fails > 0);
}

/* --- round trip ----------------------------------------------------------------------------------
 */

static void test_round_trip(void) {
    fixture fx;
    odin3_strbuf out;
    odin3_strbuf again;
    odin3_name_map map;
    make_entries(&fx);
    odin3_strbuf_init(&out);
    odin3_strbuf_init(&again);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_name_map_write(fx.entry, SPEC_COUNT, 10, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, parse_text(&map, view_of(&out)));
    TEST_ASSERT_EQUAL_UINT(SPEC_COUNT, map.entries.len);
    TEST_ASSERT_EQUAL_UINT(0, map.ignored);
    TEST_ASSERT_EQUAL_UINT(10, map.width);
    TEST_ASSERT_EQUAL_UINT(0, warn_count);
    for (uint32_t i = 0; i < SPEC_COUNT; i++) {
        const odin3_name_entry *hit =
            odin3_name_map_find(&map, fx.entry[i].module, fx.entry[i].kind, fx.entry[i].short_name);
        TEST_ASSERT_NOT_NULL(hit);
        expect_same_entry(hit, &fx.entry[i]);
        hit =
            odin3_name_map_find_full(&map, fx.entry[i].module, fx.entry[i].kind, fx.entry[i].full);
        TEST_ASSERT_NOT_NULL(hit);
        expect_same_entry(hit, &fx.entry[i]);
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_name_map_write((const odin3_name_entry *)map.entries.data,
                                               (uint32_t)map.entries.len, map.width, &again));
    TEST_ASSERT_EQUAL_STRING(out.data, again.data);
    odin3_name_check chk = odin3_name_map_check(&map);
    TEST_ASSERT_EQUAL_UINT(SPEC_COUNT, chk.entries);
    TEST_ASSERT_EQUAL_UINT(0, chk.bad_hash);
    TEST_ASSERT_EQUAL_UINT(0, chk.duplicates);
    odin3_name_map_free(&map);
    odin3_strbuf_free(&out);
    odin3_strbuf_free(&again);
}

typedef struct find_case {
    const char *module;
    odin3_name_kind kind;
    const char *short_name;
    bool want;
} find_case;

static void test_find_misses(void) {
    static const find_case CASES[] = {
        {"m", ODIN3_NAME_NET, "h42xiy3yiju", true},   {"m", ODIN3_NAME_NODE, "hfxxdd62auq", true},
        {"m", ODIN3_NAME_NODE, "h42xiy3yiju", false}, {"n", ODIN3_NAME_NET, "h42xiy3yiju", false},
        {"m", ODIN3_NAME_NET, "hzz", false},          {"z", ODIN3_NAME_WIRE, "zz", false},
    };
    odin3_name_map map;
    char text[TEXT_BUF];
    (void)snprintf(text, sizeof text, "%s%s%s", HEADER, GOOD1, GOOD2);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, parse_text(&map, text_of(text)));
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        const odin3_name_entry *hit = odin3_name_map_find(
            &map, text_of(CASES[i].module), CASES[i].kind, text_of(CASES[i].short_name));
        TEST_ASSERT_EQUAL_INT_MESSAGE(CASES[i].want ? 1 : 0, hit != NULL ? 1 : 0,
                                      CASES[i].short_name);
    }
    TEST_ASSERT_NULL(odin3_name_map_find_full(&map, text_of("m"), ODIN3_NAME_NET, text_of("q")));
    odin3_name_map_free(&map);
}

static void test_find_in_empty_map(void) {
    odin3_name_map map;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, parse_text(&map, text_of(HEADER)));
    TEST_ASSERT_NULL(odin3_name_map_find(&map, text_of("m"), ODIN3_NAME_NET, text_of("h")));
    odin3_name_map_free(&map);
}

static void test_kind_words(void) {
    TEST_ASSERT_EQUAL_STRING("net", odin3_name_kind_str(ODIN3_NAME_NET));
    TEST_ASSERT_EQUAL_STRING("node", odin3_name_kind_str(ODIN3_NAME_NODE));
    TEST_ASSERT_EQUAL_STRING("wire", odin3_name_kind_str(ODIN3_NAME_WIRE));
    TEST_ASSERT_EQUAL_STRING("", odin3_name_kind_str(ODIN3_NAME_KIND_COUNT));
}

static void test_kind_parse(void) {
    odin3_name_kind kind = ODIN3_NAME_NET;
    TEST_ASSERT_TRUE(odin3_name_kind_parse(text_of("wire"), &kind));
    TEST_ASSERT_EQUAL_INT(ODIN3_NAME_WIRE, kind);
}

static void test_kind_parse_rejects(void) {
    static const char *const BAD[] = {"wires", "", "Net", "no", "nodes"};
    odin3_name_kind kind = ODIN3_NAME_NET;
    for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
        TEST_ASSERT_FALSE_MESSAGE(odin3_name_kind_parse(text_of(BAD[i]), &kind), BAD[i]);
    }
}

/* --- corrupted entries and bad headers -------------------------------------------------------- */

static void test_corrupted_entries_warn_and_are_ignored(void) {
    odin3_name_map map;
    char text[TEXT_BUF];
    (void)snprintf(text, sizeof text,
                   "%s"                                                /* line 1 */
                   "%s"                                                /* 2 good */
                   "m\tnet\thzzzzzzzzzz\ta@t.v:1\t\tt.v\t1\n"          /* 3 not the hash */
                   "m\tnet\th42xiy3yiju\ta@t.v:1\t\tt.v\n"             /* 4 six fields */
                   "m%%41\tnet\th42xiy3yiju\ta@t.v:1\t\tt.v\t1\n"      /* 5 escape not allowed */
                   "m\tgate\th42xiy3yiju\ta@t.v:1\t\tt.v\t1\n"         /* 6 unknown kind */
                   "m\tnet\th42xiy3yiju\ta@t.v:1\t\tt.v\t12x\n"        /* 7 line not a number */
                   "m\tnet\th42xiy3yiju\ta@t.v:1\t\tt.v\t4294967296\n" /* 8 line over u32 */
                   "\n"                                                /* 9 blank */
                   "# a comment\n"                                     /* 10 comment */
                   "m\tnode\thfxxdd62auq\tx@f.v:7$2\t\tf.v\t7",        /* 11 good, no LF */
                   HEADER, GOOD1);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, parse_text(&map, text_of(text)));
    TEST_ASSERT_EQUAL_UINT(2, map.entries.len);
    TEST_ASSERT_EQUAL_UINT(6, map.ignored);
    TEST_ASSERT_EQUAL_UINT(6, warn_count);
    expect_warning(0,
                   "name.map:3: name map entry ignored: short name 'hzzzzzzzzzz' is not the hash");
    expect_warning(1, "name.map:4: name map entry ignored: not 7 tab-separated fields");
    expect_warning(2, "name.map:5: name map entry ignored: bad escape");
    expect_warning(3, "name.map:6: name map entry ignored: unknown kind");
    expect_warning(4, "name.map:7: name map entry ignored: the line field");
    expect_warning(5, "name.map:8: name map entry ignored: the line field");
    TEST_ASSERT_NOT_NULL(
        odin3_name_map_find(&map, text_of("m"), ODIN3_NAME_NODE, text_of("hfxxdd62auq")));
    odin3_name_map_free(&map);
}

static void test_line_11_truncated_escape_warns(void) {
    odin3_name_map map;
    char text[TEXT_BUF];
    (void)snprintf(text, sizeof text, "%sm\tnet\th42xiy3yiju\ta@t.v:1\t\tt.v%%\t1\n", HEADER);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, parse_text(&map, text_of(text)));
    TEST_ASSERT_EQUAL_UINT(0, map.entries.len);
    expect_warning(0, "name.map:2: name map entry ignored: bad escape");
    odin3_name_map_free(&map);
}

static void test_width_rule(void) {
    odin3_name_map map;
    char text[TEXT_BUF];
    /* width 15 header: a 10-character short is too short, ignored */
    (void)snprintf(text, sizeof text, "# odin3-names 1 hash=sha256 width=15\n%s%s", GOOD1, LONG15);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, parse_text(&map, text_of(text)));
    TEST_ASSERT_EQUAL_UINT(15, map.width);
    TEST_ASSERT_EQUAL_UINT(1, map.entries.len);
    TEST_ASSERT_EQUAL_UINT(1, map.ignored);
    odin3_name_map_free(&map);
    /* width 10 header: the 15-character short is the hash at a longer width, accepted */
    (void)snprintf(text, sizeof text, "%s%s", HEADER, LONG15);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, parse_text(&map, text_of(text)));
    TEST_ASSERT_EQUAL_UINT(1, map.entries.len);
    TEST_ASSERT_EQUAL_UINT(0, map.ignored);
    odin3_name_map_free(&map);
}

static void test_duplicates_are_reported_by_check(void) {
    odin3_name_map map;
    char text[TEXT_BUF];
    (void)snprintf(text, sizeof text, "%s%s%s%s", HEADER, GOOD1, GOOD2, GOOD1);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, parse_text(&map, text_of(text)));
    odin3_name_check chk = odin3_name_map_check(&map);
    TEST_ASSERT_EQUAL_UINT(3, chk.entries);
    TEST_ASSERT_EQUAL_UINT(1, chk.duplicates);
    TEST_ASSERT_EQUAL_UINT(0, chk.bad_hash);
    odin3_name_map_free(&map);
}

static void test_check_counts_a_bad_hash(void) {
    odin3_name_map map;
    char text[TEXT_BUF];
    (void)snprintf(text, sizeof text, "%s%s", HEADER, GOOD1);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, parse_text(&map, text_of(text)));
    ((odin3_name_entry *)map.entries.data)[0].line = 9; /* the line is not part of the hash */
    TEST_ASSERT_EQUAL_UINT(0, odin3_name_map_check(&map).bad_hash);
    ((odin3_name_entry *)map.entries.data)[0].full = text_of("other");
    TEST_ASSERT_EQUAL_UINT(1, odin3_name_map_check(&map).bad_hash);
    odin3_name_map_free(&map);
}

typedef struct header_case {
    const char *text, *needle;
} header_case;

static void test_unknown_version_and_bad_headers_are_errors(void) {
    static const header_case CASES[] = {
        {"# odin3-names 3 hash=sha256 width=10\n", "name.map:1: unsupported name map version '3'"},
        {"# odin3-names 10 hash=sha256 width=10\n", "unsupported name map version '10'"},
        {"# odin3-names  hash=sha256 width=10\n", "unsupported name map version"},
        {"# odin3-names 1 hash=md5 width=10\n", "name.map:1: malformed name map header"},
        {"# odin3-names 1 hash=sha256 width=0\n", "malformed name map header"},
        {"# odin3-names 1 hash=sha256 width=53\n", "malformed name map header"},
        {"# odin3-names 1 hash=sha256 width=x\n", "malformed name map header"},
        {"# odin3-names 1 hash=sha256\n", "malformed name map header"},
        {"# odin3-names 1 hash=sha256 width=10 extra\n", "malformed name map header"},
        {"m\tnet\th42xiy3yiju\ta@t.v:1\t\tt.v\t1\n", "name.map:1: not a name map"},
        {"# odin3-names", "malformed name map header ('# odin3-names' must be followed by a space"},
        {"# odin3-names\t1 hash=sha256 width=10\n", "name.map:1: malformed name map header"},
        {"# odin3-names 1 hash=sha256 width=10\r\n",
         "name.map:1: CRLF line ends are not supported"},
        {"", "empty name map"},
    };
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        odin3_name_map map;
        error_count = 0;
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, parse_text(&map, text_of(CASES[i].text)));
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1, error_count, CASES[i].text);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(errors[0], CASES[i].needle), errors[0]);
        TEST_ASSERT_EQUAL_UINT(0, map.entries.len);
        odin3_name_map_free(&map);
    }
}

static void test_over_long_line_is_ignored(void) {
    odin3_name_map map;
    size_t len = (size_t)ODIN3_NAME_MAP_MAX_LINE + 1;
    char *text = (char *)malloc(sizeof HEADER + len + 2);
    TEST_ASSERT_NOT_NULL(text);
    memcpy(text, HEADER, sizeof HEADER - 1);
    memset(text + sizeof HEADER - 1, 'x', len);
    text[sizeof HEADER - 1 + len] = '\n';
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          parse_text(&map, (odin3_bytes){text, sizeof HEADER - 1 + len + 1}));
    TEST_ASSERT_EQUAL_UINT(1, map.ignored);
    expect_warning(0, "name.map:2: name map entry ignored: line is too long");
    odin3_name_map_free(&map);
    free(text);
}

static void test_parse_out_of_memory(void) {
    char text[TEXT_BUF];
    int fails = 0;
    (void)snprintf(text, sizeof text, "%s%s%s", HEADER, GOOD1, GOOD2);
    for (long k = 0; k < 40; k++) {
        odin3_name_map map;
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_name_map_init(&map));
        odin3_util_set_alloc_fail_after(k);
        odin3_status st = odin3_name_map_parse(&map, text_of(text), "name.map");
        odin3_util_set_alloc_fail_after(-1);
        odin3_name_map_free(&map);
        if (st == ODIN3_OK) {
            break;
        }
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        fails++;
    }
    TEST_ASSERT_TRUE(fails > 0);
    odin3_util_set_alloc_fail_after(0);
    odin3_name_map map;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, odin3_name_map_init(&map));
    odin3_util_set_alloc_fail_after(-1);
    odin3_name_map_free(&map);
}

static void test_module_starting_with_hash_round_trips(void) {
    fixture fx;
    odin3_strbuf out;
    odin3_name_map map;
    make_entries(&fx);
    fx.entry[0].module = text_of("#x");
    fx.entry[1].module = text_of("#");
    odin3_strbuf_init(&out);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_name_map_write(fx.entry, SPEC_COUNT, 10, &out));
    TEST_ASSERT_NOT_NULL(strstr(out.data, "\n%23x\tnet\t"));
    TEST_ASSERT_NOT_NULL(strstr(out.data, "\n%23\tnode\t"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, parse_text(&map, view_of(&out)));
    TEST_ASSERT_EQUAL_UINT(SPEC_COUNT, map.entries.len);
    TEST_ASSERT_EQUAL_UINT(0, map.ignored);
    for (uint32_t i = 0; i < 2; i++) {
        const odin3_name_entry *hit =
            odin3_name_map_find(&map, fx.entry[i].module, fx.entry[i].kind, fx.entry[i].short_name);
        TEST_ASSERT_NOT_NULL(hit);
        expect_same_entry(hit, &fx.entry[i]);
    }
    odin3_name_map_free(&map);
    odin3_strbuf_free(&out);
}

static void test_write_refuses_an_over_long_line(void) {
    odin3_name_entry entry = {text_of("m"),
                              ODIN3_NAME_NET,
                              text_of("h42xiy3yiju"),
                              {NULL, 0},
                              text_of(""),
                              text_of("t.v"),
                              1};
    size_t len = (size_t)ODIN3_NAME_MAP_MAX_LINE;
    char *big = (char *)malloc(len + 1);
    odin3_strbuf out;
    TEST_ASSERT_NOT_NULL(big);
    memset(big, 'x', len);
    big[len] = '\0';
    entry.full = text_of(big);
    odin3_strbuf_init(&out);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_name_map_write(&entry, 1, 10, &out));
    TEST_ASSERT_EQUAL_UINT(0, out.len);
    TEST_ASSERT_EQUAL_UINT(1, error_count);
    TEST_ASSERT_NOT_NULL(strstr(errors[0], "over the limit"));
    big[len - 100] = '\0'; /* short enough: written, and the parser reads it back */
    entry.full = text_of(big);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_name_map_write(&entry, 1, 10, &out));
    odin3_strbuf_free(&out);
    free(big);
}

static void test_duplicate_keys_resolve_the_same_in_any_order(void) {
    static const char *const LINES[2] = {"m\tnet\th42xiy3yiju\ta@t.v:1\t\tt.v\t5\n",
                                         "m\tnet\th42xiy3yiju\ta@t.v:1\t\tt.v\t1\n"};
    for (int order = 0; order < 2; order++) {
        odin3_name_map map;
        char text[TEXT_BUF];
        (void)snprintf(text, sizeof text, "%s%s%s", HEADER, LINES[order], LINES[1 - order]);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, parse_text(&map, text_of(text)));
        const odin3_name_entry *hit =
            odin3_name_map_find(&map, text_of("m"), ODIN3_NAME_NET, text_of("h42xiy3yiju"));
        TEST_ASSERT_NOT_NULL(hit);
        TEST_ASSERT_EQUAL_UINT(1, hit->line);
        hit = odin3_name_map_find_full(&map, text_of("m"), ODIN3_NAME_NET, text_of("a@t.v:1"));
        TEST_ASSERT_NOT_NULL(hit);
        TEST_ASSERT_EQUAL_UINT(1, hit->line);
        odin3_name_map_free(&map);
    }
}

enum { MANY_ENTRIES = 600 };

static void expect_all_found(const odin3_name_map *map) {
    for (int i = 0; i < MANY_ENTRIES; i++) {
        char module[8];
        char full[64];
        (void)snprintf(module, sizeof module, "m%d", i % 7);
        (void)snprintf(full, sizeof full, "top/n%d@f.v:%d", i, i + 1);
        const odin3_name_entry *hit =
            odin3_name_map_find_full(map, text_of(module), ODIN3_NAME_NET, text_of(full));
        TEST_ASSERT_NOT_NULL(hit);
        TEST_ASSERT_EQUAL_UINT((uint32_t)(i + 1), hit->line);
    }
}

/* find_full over many entries: every full name is found, in log n steps by index. */
static void test_find_full_over_many_entries(void) {
    odin3_strbuf text;
    odin3_name_map map;
    odin3_strbuf_init(&text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(&text, text_of(HEADER)));
    for (int i = 0; i < MANY_ENTRIES; i++) {
        char full[64];
        odin3_hash_name hash;
        (void)snprintf(full, sizeof full, "top/n%d@f.v:%d", i, i + 1);
        odin3_hash_name_make(text_of(full), HASH_CHARS, &hash);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_strbuf_appendf(&text, "m%d\tnet\t%s\t%s\ttop\tf.v\t%d\n", i % 7,
                                                   hash.text, full, i + 1));
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, parse_text(&map, view_of(&text)));
    TEST_ASSERT_EQUAL_UINT(MANY_ENTRIES, map.entries.len);
    expect_all_found(&map);
    TEST_ASSERT_NULL(odin3_name_map_find_full(&map, text_of("m0"), ODIN3_NAME_NET, text_of("zz")));
    TEST_ASSERT_NULL(
        odin3_name_map_find_full(&map, text_of("m0"), ODIN3_NAME_WIRE, text_of("top/n0@f.v:1")));
    odin3_name_map_free(&map);
    odin3_strbuf_free(&text);
}

static void test_escape_case_and_short_length(void) {
    odin3_name_map map;
    char text[TEXT_BUF];
    /* lowercase hex is not an escape of the format: ignored with a warning */
    (void)snprintf(text, sizeof text, "%sm\tnet\th42xiy3yiju\ta@t.v:1\t\tt%%0av\t1\n", HEADER);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, parse_text(&map, text_of(text)));
    TEST_ASSERT_EQUAL_UINT(0, map.entries.len);
    expect_warning(0, "name.map:2: name map entry ignored: bad escape");
    odin3_name_map_free(&map);
    /* a short of 12 characters (11 hash characters) is a prefix of the hash at a longer width */
    (void)snprintf(text, sizeof text, "%sm\tnet\th42xiy3yijuq\ta@t.v:1\t\tt.v\t1\n", HEADER);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, parse_text(&map, text_of(text)));
    TEST_ASSERT_EQUAL_UINT(1, map.entries.len);
    TEST_ASSERT_EQUAL_UINT(0, map.ignored);
    odin3_name_map_free(&map);
}

static void test_crlf_entry_warns(void) {
    odin3_name_map map;
    char text[TEXT_BUF];
    (void)snprintf(text, sizeof text, "%sm\tnet\th42xiy3yiju\ta@t.v:1\t\tt.v\t1\r\n", HEADER);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, parse_text(&map, text_of(text)));
    TEST_ASSERT_EQUAL_UINT(0, map.entries.len);
    expect_warning(0, "name.map:2: name map entry ignored: CR at the end of the line");
    odin3_name_map_free(&map);
}

/* --- version 2 -------------------------------------------------------------------------------- */

static const char HEADER2[] = "# odin3-names 2 hash=sha256 width=10 prov=1 ast=refs\n";

/* One O line for the real hash name of "a@t.v:1": the seven version 1 fields, recs, level. */
#define O_LINE "O\tm\tnet\th42xiy3yiju\ta@t.v:1\t\tt.v\t1\tr1\tgate\n"

static odin3_status write_line(odin3_strbuf *out, char type, const char *const *texts, size_t n,
                               bool raw) {
    odin3_nm_field fields[ODIN3_NM2_MAX_FIELDS];
    for (size_t i = 0; i < n; i++) {
        fields[i] = (odin3_nm_field){text_of(texts[i]), raw};
    }
    return odin3_name_map_write_line_v2(out, type, fields, n);
}

static void test_v2_header_and_line_writer(void) {
    odin3_strbuf out;
    odin3_strbuf_init(&out);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_name_map_write_header_v2(&out, 10, "refs"));
    TEST_ASSERT_EQUAL_STRING(HEADER2, out.data);
    const char *const texts[] = {"a,b|c", "x\ty", "100%", "back\\slash", ""};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, write_line(&out, 'N', texts, 5, false));
    TEST_ASSERT_NOT_NULL(strstr(out.data, "N\ta%2Cb%7Cc\tx%09y\t100%25\tback%5Cslash\t\n"));
    /* a raw field is written as given (a list the caller escaped element by element) */
    size_t before = out.len;
    const char *const raw[] = {"r1,r2", "c1|c2"};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, write_line(&out, 'R', raw, 2, true));
    TEST_ASSERT_EQUAL_STRING("R\tr1,r2\tc1|c2\n", out.data + before);
    odin3_strbuf_free(&out);
}

static void test_v2_writer_refusals_leave_the_buffer_alone(void) {
    odin3_strbuf out;
    odin3_strbuf_init(&out);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_name_map_write_header_v2(&out, 10, "none"));
    size_t len = out.len;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_name_map_write_header_v2(&out, 0, "refs"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_name_map_write_header_v2(&out, 53, "refs"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_name_map_write_header_v2(&out, 10, "all"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_name_map_write_header_v2(&out, 10, NULL));
    const char *const one[] = {"x"};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, write_line(&out, '1', one, 1, false));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, write_line(&out, 'R', one, 0, false));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_name_map_write_line_v2(&out, 'R', NULL, ODIN3_NM2_MAX_FIELDS + 1));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)len, (uint32_t)out.len);
    odin3_strbuf_free(&out);
}

static void test_v2_parse_keeps_every_type_in_file_order(void) {
    char text[TEXT_BUF];
    (void)snprintf(text, sizeof text,
                   "%s%sN\tm\tnet\tp\tr1\tgate\nR\tr1\tS\tstmt\torigin\tt1.1\t\tf1:1.1-1.5\tc1\t\n"
                   "C\tc1\tMODULE\tmodule\t\tf1\t1.1-9.1\tm\nF\tf1\tt.v\nT\tt1\tread_verilog\t\t\n",
                   HEADER2, O_LINE);
    odin3_name_map map;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, parse_text(&map, text_of(text)));
    TEST_ASSERT_EQUAL_UINT32(2, map.version);
    expect_bytes(map.ast, text_of("refs"));
    TEST_ASSERT_EQUAL_UINT(1, map.entries.len); /* the O line is an entry; N is not */
    TEST_ASSERT_EQUAL_UINT(6, map.lines.len);
    static const char ORDER[] = "ONRCFT";
    for (size_t i = 0; i < map.lines.len; i++) {
        TEST_ASSERT_EQUAL_CHAR(ORDER[i],
                               ((const odin3_nm_line *)odin3_vec_cat(&map.lines, i))->type);
    }
    const odin3_nm_line *o_line = odin3_vec_cat(&map.lines, 0);
    expect_bytes(o_line->f[7], text_of("r1"));
    expect_bytes(o_line->f[8], text_of("gate"));
    const odin3_name_entry *entry = odin3_vec_cat(&map.entries, 0);
    expect_bytes(entry->short_name, text_of("h42xiy3yiju"));
    TEST_ASSERT_EQUAL_UINT32(1, entry->line);
    odin3_name_check_v2 check = odin3_name_map_check_v2(&map);
    TEST_ASSERT_TRUE(odin3_name_check_v2_ok(&check));
    TEST_ASSERT_EQUAL_UINT32(1, check.records);
    TEST_ASSERT_EQUAL_UINT32(1, check.constructs);
    TEST_ASSERT_EQUAL_UINT32(1, check.files);
    TEST_ASSERT_EQUAL_UINT32(1, check.runs);
    TEST_ASSERT_EQUAL_UINT32(2, check.objects);
    odin3_name_map_free(&map);
}

static void test_v2_escapes_comma_and_pipe_in_every_field(void) {
    /* the module name has a comma and a pipe: the O line spells them %2C %7C, the parse reads them
     */
    odin3_hash_name hash;
    odin3_hash_name_make(text_of("a@t.v:1"), HASH_CHARS, &hash);
    odin3_name_entry entry = {text_of("m,1|2"),
                              ODIN3_NAME_NET,
                              text_of(hash.text),
                              text_of("a@t.v:1"),
                              text_of("h,i"),
                              text_of("t|v"),
                              1};
    odin3_strbuf out;
    odin3_strbuf_init(&out);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_name_map_write_header_v2(&out, 10, "none"));
    odin3_nm_field fields[] = {
        {entry.module, false}, {text_of("net"), false}, {entry.short_name, false},
        {entry.full, false},   {entry.hier, false},     {entry.file, false},
        {text_of("1"), true},  {text_of(""), true},     {text_of("none"), false}};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_name_map_write_line_v2(&out, 'O', fields, 9));
    TEST_ASSERT_NOT_NULL(strstr(out.data, "m%2C1%7C2"));
    odin3_name_map map;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, parse_text(&map, view_of(&out)));
    TEST_ASSERT_EQUAL_UINT(1, map.entries.len);
    expect_same_entry(odin3_vec_cat(&map.entries, 0), &entry);
    odin3_name_map_free(&map);
    /* a version 1 map does not know %2C: the line is ignored with a warning */
    char text[TEXT_BUF];
    (void)snprintf(text, sizeof text, "%sm%%2C\tnet\th42xiy3yiju\ta@t.v:1\t\tt.v\t1\n", HEADER);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, parse_text(&map, text_of(text)));
    TEST_ASSERT_EQUAL_UINT32(1, map.ignored);
    TEST_ASSERT_EQUAL_UINT(0, map.entries.len);
    expect_warning(0, "bad escape");
    odin3_name_map_free(&map);
    odin3_strbuf_free(&out);
}

static void test_v2_malformed_lines_warn_and_are_ignored(void) {
    char text[TEXT_BUF];
    (void)snprintf(text, sizeof text,
                   "%s"
                   "R\tr1\n"        /* too few fields */
                   "O\tm\tnet\tx\n" /* too few fields for O */
                   "XYZ\tr1\n"      /* no type letter and TAB */
                   "O\tm%%2G\tnet\th42xiy3yiju\ta@t.v:1\t\tt.v\t1\tr1\tgate\n" /* bad escape */
                   "O\tm\tnet\tnothash\ta@t.v:1\t\tt.v\t1\tr1\tgate\n" /* short is not the hash */
                   "O\tm\tbox\th42xiy3yiju\ta@t.v:1\t\tt.v\t1\tr1\tgate\n" /* unknown kind */
                   "O\tm\tnet\th42xiy3yiju\ta@t.v:1\t\tt.v\tx\tr1\tgate\n" /* bad line number */
                   "N\tm\tnet\tp\tr1\tgate\r\n"                            /* CR */
                   "F\tf1\tt.v\n",
                   HEADER2);
    odin3_name_map map;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, parse_text(&map, text_of(text)));
    TEST_ASSERT_EQUAL_UINT32(8, map.ignored);
    TEST_ASSERT_EQUAL_UINT(0, map.entries.len);
    TEST_ASSERT_EQUAL_UINT(1, map.lines.len); /* the F line only */
    TEST_ASSERT_EQUAL_UINT(8, warn_count);
    expect_warning(0, "name.map:2: name map entry ignored: too few fields");
    expect_warning(2, "no type letter");
    expect_warning(3, "bad escape");
    expect_warning(7, "CR at the end");
    odin3_name_map_free(&map);
}

static void test_v2_unknown_type_warns_once_per_type(void) {
    char text[TEXT_BUF];
    (void)snprintf(text, sizeof text, "%sQ\tq1\tx\nQ\tq2\ty\nK\tk1\nQ\tq3\nF\tf1\tt.v\n", HEADER2);
    odin3_name_map map;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, parse_text(&map, text_of(text)));
    TEST_ASSERT_EQUAL_UINT(2, warn_count); /* Q once, K once */
    expect_warning(0, "line type 'Q' is unknown");
    expect_warning(1, "line type 'K' is unknown");
    TEST_ASSERT_EQUAL_UINT(1, map.lines.len);
    TEST_ASSERT_EQUAL_UINT32(4, map.ignored);
    odin3_name_map_free(&map);
}

static void test_v2_header_errors(void) {
    static const header_case CASES[] = {
        {"# odin3-names 2 hash=sha256 width=10\n", "malformed name map header (want 'hash=sha256"},
        {"# odin3-names 2 hash=sha256 width=10 prov=0 ast=refs\n", "malformed name map header"},
        {"# odin3-names 2 hash=sha256 width=10 prov=1 ast=most\n", "malformed name map header"},
        {"# odin3-names 2 hash=sha256 width=10 prov=1 ast=refs extra\n",
         "malformed name map header"},
        {"# odin3-names 1 hash=sha256 width=10 prov=1 ast=refs\n", "malformed name map header"},
    };
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        odin3_name_map map;
        error_count = 0;
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, parse_text(&map, text_of(CASES[i].text)));
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1, error_count, CASES[i].text);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(errors[0], CASES[i].needle), errors[0]);
        odin3_name_map_free(&map);
    }
    const char *words[] = {"refs", "constructs", "full", "none"};
    for (size_t i = 0; i < 4; i++) {
        char text[TEXT_BUF];
        odin3_name_map map;
        (void)snprintf(text, sizeof text, "# odin3-names 2 hash=sha256 width=12 prov=1 ast=%s\n",
                       words[i]);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, parse_text(&map, text_of(text)));
        expect_bytes(map.ast, text_of(words[i]));
        TEST_ASSERT_EQUAL_UINT32(12, map.width);
        odin3_name_map_free(&map);
    }
}

/* check_v2 on a map with one planted problem; returns the counters. */
static odin3_name_check_v2 check_of(const char *body) {
    char text[TEXT_BUF];
    (void)snprintf(text, sizeof text, "%s%s", HEADER2, body);
    odin3_name_map map;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, parse_text(&map, text_of(text)));
    odin3_name_check_v2 check = odin3_name_map_check_v2(&map);
    odin3_name_map_free(&map);
    return check;
}

static void test_v2_check_finds_each_problem(void) {
    static const char GOOD_R[] = "R\tr1\tS\tstmt\torigin\tt1.1\t\tf1:1.1-1.5\t\t\n";
    static const char TAIL[] = "F\tf1\tt.v\nT\tt1\tread_verilog\t\t\n";
    char body[TEXT_BUF];
    (void)snprintf(body, sizeof body, "%s%s", GOOD_R, TAIL);
    odin3_name_check_v2 good = check_of(body);
    TEST_ASSERT_TRUE(odin3_name_check_v2_ok(&good));
    /* an id that is not the line number */
    (void)snprintf(body, sizeof body, "R\tr2\tS\tstmt\torigin\tt1.1\t\t\t\t\n%s", TAIL);
    TEST_ASSERT_EQUAL_UINT32(1, check_of(body).bad_id);
    /* a parent that does not exist, and one numbered after its child */
    (void)snprintf(body, sizeof body, "R\tr1\tD\tword\trewrite:word\tt1.1\tr9\t\t\t\n%s", TAIL);
    TEST_ASSERT_EQUAL_UINT32(1, check_of(body).bad_ref);
    (void)snprintf(
        body, sizeof body,
        "R\tr1\tD\tword\trewrite:word\tt1.1\tr2\t\t\t\nR\tr2\tS\tstmt\torigin\tt1.1\t\t\t\t\n%s",
        TAIL);
    TEST_ASSERT_EQUAL_UINT32(1, check_of(body).bad_order);
    /* a self-parent is the cycle case: not numbered before itself */
    (void)snprintf(body, sizeof body, "R\tr1\tD\tword\trewrite:word\tt1.1\tr1\t\t\t\n%s", TAIL);
    TEST_ASSERT_EQUAL_UINT32(1, check_of(body).bad_order);
    /* a class that is missing, or a step with no pass run */
    (void)snprintf(body, sizeof body, "R\tr1\tD\tword\tsideways:word\tt1.1\t\t\t\t\n%s", TAIL);
    TEST_ASSERT_EQUAL_UINT32(1, check_of(body).bad_class);
    (void)snprintf(body, sizeof body, "R\tr1\tS\tstmt\torigin\tt5.1\t\t\t\t\n%s", TAIL);
    TEST_ASSERT_EQUAL_UINT32(1, check_of(body).bad_class);
    /* a file or a construct that does not resolve */
    (void)snprintf(body, sizeof body, "R\tr1\tS\tstmt\torigin\tt1.1\t\tf7:1.1-1.5\tc3\tcall=c4\n%s",
                   TAIL);
    TEST_ASSERT_EQUAL_UINT32(3, check_of(body).bad_ref);
    /* an object whose record is missing */
    (void)snprintf(body, sizeof body, "N\tm\tnet\tp\tr4\tgate\n%s%s", GOOD_R, TAIL);
    TEST_ASSERT_EQUAL_UINT32(1, check_of(body).no_record);
    /* a construct under a parent numbered later */
    (void)snprintf(
        body, sizeof body,
        "C\tc1\tIF\tstmt\tc2\tf1\t1.1-2.1\t\nC\tc2\tMODULE\tmodule\t\tf1\t1.1-9.1\tm\n%s", TAIL);
    TEST_ASSERT_EQUAL_UINT32(1, check_of(body).bad_order);
}

/* The ast word of the header is the parser's own, not a view into the caller's text: free the text
 * after the parse and read it (ASan flags a dangling view). */
static void test_v2_ast_word_survives_the_callers_text(void) {
    const char *const words[] = {"refs", "constructs", "full", "none"};
    for (size_t i = 0; i < 4; i++) {
        char *text = odin3_util_calloc(TEXT_BUF);
        TEST_ASSERT_NOT_NULL(text);
        (void)snprintf(text, TEXT_BUF, "# odin3-names 2 hash=sha256 width=10 prov=1 ast=%s\n",
                       words[i]);
        odin3_name_map map;
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, parse_text(&map, text_of(text)));
        odin3_util_free(text); /* the caller frees its buffer */
        expect_bytes(map.ast, text_of(words[i]));
        odin3_name_map_free(&map);
    }
}

/* The check reports how many lines the parse ignored; a map with only ignored lines is still ok. */
static void test_v2_check_counts_ignored_lines(void) {
    static const char BODY[] = "Q\tq1\tx\nK\tk1\nF\tf1\tt.v\nT\tt1\tread_verilog\t\t\nR\tbad\n";
    odin3_name_check_v2 check = check_of(BODY);
    TEST_ASSERT_EQUAL_UINT32(3, check.ignored); /* Q, K, and the R line with too few fields */
    TEST_ASSERT_TRUE(odin3_name_check_v2_ok(&check));
    TEST_ASSERT_TRUE(odin3_name_check_v2_report(&check, "test"));
}

/* The writer refuses a line the parser would ignore (over ODIN3_NAME_MAP_MAX_LINE), and leaves
 * the buffer as it was. */
static void test_v2_writer_refuses_a_line_over_the_parser_cap(void) {
    odin3_strbuf out;
    odin3_strbuf_init(&out);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_name_map_write_header_v2(&out, 10, "refs"));
    size_t before = out.len;
    size_t big = (size_t)ODIN3_NAME_MAP_MAX_LINE + 1U;
    char *text = odin3_util_calloc(big + 1U);
    TEST_ASSERT_NOT_NULL(text);
    memset(text, 'x', big);
    const odin3_nm_field fields[] = {{{text, big}, false}};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_name_map_write_line_v2(&out, 'F', fields, 1));
    TEST_ASSERT_EQUAL_size_t(before, out.len);
    memset(text, 'x', (size_t)ODIN3_NAME_MAP_MAX_LINE - 8U); /* a line just inside the cap parses */
    const odin3_nm_field fit[] = {{text_of("f1"), false},
                                  {{text, (size_t)ODIN3_NAME_MAP_MAX_LINE - 8U}, false}};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_name_map_write_line_v2(&out, 'F', fit, 2));
    odin3_name_map map;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_name_map_init(&map));
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_name_map_parse(&map, (odin3_bytes){out.data, out.len}, "cap.names"));
    TEST_ASSERT_EQUAL_UINT32(0, map.ignored);
    odin3_name_map_free(&map);
    odin3_util_free(text);
    odin3_strbuf_free(&out);
}

static void test_check_of_a_version_1_map_is_empty(void) {
    char text[TEXT_BUF];
    (void)snprintf(text, sizeof text, "%s%s", HEADER, GOOD1);
    odin3_name_map map;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, parse_text(&map, text_of(text)));
    odin3_name_check_v2 check = odin3_name_map_check_v2(&map);
    TEST_ASSERT_EQUAL_UINT32(0, check.objects);
    TEST_ASSERT_TRUE(odin3_name_check_v2_ok(&check));
    odin3_name_map_free(&map);
}

static void test_v2_line_writer_out_of_memory(void) {
    odin3_strbuf out;
    odin3_strbuf_init(&out);
    const char *const texts[] = {"abc", "def"};
    odin3_util_set_alloc_fail_after(0);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, write_line(&out, 'F', texts, 2, false));
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)out.len);
    odin3_strbuf_free(&out);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_write_exact_text);
    RUN_TEST(test_write_escapes);
    RUN_TEST(test_write_sorted_and_order_independent);
    RUN_TEST(test_write_appends_and_refuses_duplicates);
    RUN_TEST(test_write_rejects_bad_arguments);
    RUN_TEST(test_write_out_of_memory);
    RUN_TEST(test_round_trip);
    RUN_TEST(test_find_misses);
    RUN_TEST(test_find_in_empty_map);
    RUN_TEST(test_kind_words);
    RUN_TEST(test_kind_parse);
    RUN_TEST(test_kind_parse_rejects);
    RUN_TEST(test_corrupted_entries_warn_and_are_ignored);
    RUN_TEST(test_line_11_truncated_escape_warns);
    RUN_TEST(test_width_rule);
    RUN_TEST(test_duplicates_are_reported_by_check);
    RUN_TEST(test_check_counts_a_bad_hash);
    RUN_TEST(test_unknown_version_and_bad_headers_are_errors);
    RUN_TEST(test_over_long_line_is_ignored);
    RUN_TEST(test_module_starting_with_hash_round_trips);
    RUN_TEST(test_write_refuses_an_over_long_line);
    RUN_TEST(test_duplicate_keys_resolve_the_same_in_any_order);
    RUN_TEST(test_find_full_over_many_entries);
    RUN_TEST(test_escape_case_and_short_length);
    RUN_TEST(test_crlf_entry_warns);
    RUN_TEST(test_parse_out_of_memory);
    RUN_TEST(test_v2_header_and_line_writer);
    RUN_TEST(test_v2_writer_refusals_leave_the_buffer_alone);
    RUN_TEST(test_v2_parse_keeps_every_type_in_file_order);
    RUN_TEST(test_v2_escapes_comma_and_pipe_in_every_field);
    RUN_TEST(test_v2_malformed_lines_warn_and_are_ignored);
    RUN_TEST(test_v2_unknown_type_warns_once_per_type);
    RUN_TEST(test_v2_header_errors);
    RUN_TEST(test_v2_check_finds_each_problem);
    RUN_TEST(test_v2_ast_word_survives_the_callers_text);
    RUN_TEST(test_v2_check_counts_ignored_lines);
    RUN_TEST(test_v2_writer_refuses_a_line_over_the_parser_cap);
    RUN_TEST(test_check_of_a_version_1_map_is_empty);
    RUN_TEST(test_v2_line_writer_out_of_memory);
    return UNITY_END();
}
