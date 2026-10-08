/*
 * test_util_str.c — unit tests for the string table and string builder.
 */
#include "unity.h"
#include "util/alloc.h"
#include "util/str.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MANY = 100000, LONG_LEN = 100 * 1024, BIG_FMT = 5000 };

void setUp(void) {
}
void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
}

static uint32_t intern_ok(odin3_strtab *tab, const char *str) {
    uint32_t id = UINT32_MAX;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strtab_intern(tab, odin3_bytes_cstr(str), &id));
    return id;
}

static void test_empty_string_is_id_zero(void) {
    odin3_strtab *tab = odin3_strtab_create();
    TEST_ASSERT_NOT_NULL(tab);
    TEST_ASSERT_EQUAL_size_t(1, odin3_strtab_count(tab));
    TEST_ASSERT_EQUAL_STRING("", odin3_strtab_get(tab, 0));
    TEST_ASSERT_EQUAL_UINT32(0, intern_ok(tab, ""));
    odin3_strtab_destroy(tab);
}

static void test_intern_is_idempotent_and_dense(void) {
    odin3_strtab *tab = odin3_strtab_create();
    TEST_ASSERT_EQUAL_UINT32(1, intern_ok(tab, "a"));
    TEST_ASSERT_EQUAL_UINT32(2, intern_ok(tab, "b"));
    TEST_ASSERT_EQUAL_UINT32(1, intern_ok(tab, "a"));
    TEST_ASSERT_EQUAL_size_t(3, odin3_strtab_count(tab));
    TEST_ASSERT_EQUAL_STRING("b", odin3_strtab_get(tab, 2));
    TEST_ASSERT_EQUAL_size_t(1, odin3_strtab_len(tab, 2));
    odin3_strtab_destroy(tab);
}

static void test_find_does_not_insert(void) {
    odin3_strtab *tab = odin3_strtab_create();
    uint32_t id = 7;
    TEST_ASSERT_FALSE(odin3_strtab_find(tab, odin3_bytes_cstr("x"), &id));
    TEST_ASSERT_EQUAL_size_t(1, odin3_strtab_count(tab));
    (void)intern_ok(tab, "x");
    TEST_ASSERT_TRUE(odin3_strtab_find(tab, odin3_bytes_cstr("x"), &id));
    TEST_ASSERT_EQUAL_UINT32(1, id);
    TEST_ASSERT_TRUE(odin3_strtab_find(tab, odin3_bytes_cstr("x"), NULL));
    odin3_strtab_destroy(tab);
}

static void test_get_pointer_stable(void) {
    odin3_strtab *tab = odin3_strtab_create();
    (void)intern_ok(tab, "first");
    const char *before = odin3_strtab_get(tab, 1);
    for (int i = 0; i < MANY; i++) {
        char name[32];
        (void)snprintf(name, sizeof name, "n%d", i);
        (void)intern_ok(tab, name);
    }
    TEST_ASSERT_TRUE(before == odin3_strtab_get(tab, 1));
    TEST_ASSERT_EQUAL_STRING("first", before);
    TEST_ASSERT_EQUAL_size_t((size_t)MANY + 2, odin3_strtab_count(tab));
    odin3_strtab_destroy(tab);
}

static void test_high_bytes_roundtrip(void) {
    odin3_strtab *tab = odin3_strtab_create();
    const odin3_bytes str = {"\xff\xfe\x80", 3};
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strtab_intern(tab, str, &id));
    TEST_ASSERT_EQUAL_size_t(3, odin3_strtab_len(tab, id));
    TEST_ASSERT_EQUAL_MEMORY("\xff\xfe\x80", odin3_strtab_get(tab, id), 3);
    odin3_strtab_destroy(tab);
}

static void test_embedded_nul_rejected(void) {
    odin3_strtab *tab = odin3_strtab_create();
    const odin3_bytes str = {"a\0b", 3};
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_strtab_intern(tab, str, &id));
    TEST_ASSERT_EQUAL_size_t(1, odin3_strtab_count(tab));
    odin3_strtab_destroy(tab);
}

static void test_long_string_100k(void) {
    odin3_strtab *tab = odin3_strtab_create();
    char *big = malloc(LONG_LEN);
    TEST_ASSERT_NOT_NULL(big);
    memset(big, 'x', LONG_LEN);
    const odin3_bytes str = {big, LONG_LEN};
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strtab_intern(tab, str, &id));
    TEST_ASSERT_EQUAL_size_t(LONG_LEN, odin3_strtab_len(tab, id));
    TEST_ASSERT_EQUAL_MEMORY(big, odin3_strtab_get(tab, id), LONG_LEN);
    TEST_ASSERT_EQUAL_CHAR('\0', odin3_strtab_get(tab, id)[LONG_LEN]);
    uint32_t again = 0;
    TEST_ASSERT_TRUE(odin3_strtab_find(tab, str, &again));
    TEST_ASSERT_EQUAL_UINT32(id, again);
    free(big);
    odin3_strtab_destroy(tab);
}

/* Interns name with the idx-th allocation failing. Returns true if that failed; then checks that
 * the table is unchanged and a retry succeeds. */
static bool intern_with_failure(odin3_strtab *tab, odin3_bytes name, long idx) {
    const size_t before = odin3_strtab_count(tab);
    uint32_t id = 0;
    odin3_util_set_alloc_fail_after(idx);
    const odin3_status st = odin3_strtab_intern(tab, name, &id);
    odin3_util_set_alloc_fail_after(-1);
    if (st == ODIN3_OK) {
        return false;
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
    TEST_ASSERT_EQUAL_size_t(before, odin3_strtab_count(tab));
    TEST_ASSERT_FALSE(odin3_strtab_find(tab, name, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strtab_intern(tab, name, &id));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)before, id);
    TEST_ASSERT_EQUAL_size_t(before + 1, odin3_strtab_count(tab));
    return true;
}

static void check_ids_intact(odin3_strtab *tab, uint32_t upto) {
    for (uint32_t i = 1; i < upto; i++) {
        char name[32];
        (void)snprintf(name, sizeof name, "n%u", (unsigned)i);
        TEST_ASSERT_EQUAL_STRING(name, odin3_strtab_get(tab, i));
        uint32_t id = 0;
        TEST_ASSERT_TRUE(odin3_strtab_find(tab, odin3_bytes_cstr(name), &id));
        TEST_ASSERT_EQUAL_UINT32(i, id);
    }
}

/* Each intern fails its first allocation (if it needs one): hits the vec growth boundaries
 * (8, 16, ...) and the idindex growth boundaries (load limit) with real failures. */
static void test_oom_intern(void) {
    enum { COUNT = 200, MIN_HITS = 4 };
    odin3_strtab *tab = odin3_strtab_create();
    int hits = 0;
    for (uint32_t i = 1; i < COUNT; i++) {
        char name[32];
        (void)snprintf(name, sizeof name, "n%u", (unsigned)i);
        if (intern_with_failure(tab, odin3_bytes_cstr(name), 0)) {
            hits++;
        } else {
            TEST_ASSERT_EQUAL_size_t(i + 1, odin3_strtab_count(tab));
        }
    }
    TEST_ASSERT_TRUE(hits >= MIN_HITS);
    TEST_ASSERT_EQUAL_size_t(COUNT, odin3_strtab_count(tab));
    check_ids_intact(tab, COUNT);
    odin3_strtab_destroy(tab);
}

static odin3_strtab *filled_table(uint32_t fill) {
    odin3_strtab *tab = odin3_strtab_create();
    for (uint32_t i = 1; i < fill; i++) {
        char name[32];
        (void)snprintf(name, sizeof name, "n%u", (unsigned)i);
        (void)intern_ok(tab, name);
    }
    return tab;
}

/* For every fill level up to 40 (crossing the vec and idindex growth points), fails each
 * allocation the next intern performs, in turn. */
static void test_oom_intern_at_growth_boundaries(void) {
    enum { MAX_FILL = 40, MAX_ALLOCS = 6, MIN_HITS = 4 };
    int hits = 0;
    for (uint32_t fill = 1; fill <= MAX_FILL; fill++) {
        for (long idx = 0; idx < MAX_ALLOCS; idx++) {
            odin3_strtab *tab = filled_table(fill);
            char next[32];
            (void)snprintf(next, sizeof next, "n%u", (unsigned)fill);
            const bool failed = intern_with_failure(tab, odin3_bytes_cstr(next), idx);
            if (failed) {
                hits++;
                check_ids_intact(tab, fill);
            }
            odin3_strtab_destroy(tab);
            if (!failed) {
                break;
            }
        }
    }
    TEST_ASSERT_TRUE(hits >= MIN_HITS);
}

static void test_oom_intern_larger_than_chunk(void) {
    enum { HUGE_LEN = 200 * 1024 };
    char *big = malloc(HUGE_LEN);
    TEST_ASSERT_NOT_NULL(big);
    memset(big, 'h', HUGE_LEN);
    const odin3_bytes str = {big, HUGE_LEN};
    odin3_strtab *tab = odin3_strtab_create();
    (void)intern_ok(tab, "keep");
    TEST_ASSERT_TRUE(intern_with_failure(tab, str, 0)); /* the arena chunk allocation fails */
    TEST_ASSERT_EQUAL_STRING("keep", odin3_strtab_get(tab, 1));
    TEST_ASSERT_EQUAL_size_t(HUGE_LEN, odin3_strtab_len(tab, 2));
    free(big);
    odin3_strtab_destroy(tab);
}

static void test_oom_create(void) {
    enum { MAX_ALLOCS = 16 };
    int nulls = 0;
    bool succeeded = false;
    for (long idx = 0; idx < MAX_ALLOCS && !succeeded; idx++) {
        odin3_util_set_alloc_fail_after(idx);
        odin3_strtab *tab = odin3_strtab_create();
        odin3_util_set_alloc_fail_after(-1);
        if (tab == NULL) {
            nulls++;
            continue;
        }
        succeeded = true;
        TEST_ASSERT_EQUAL_size_t(1, odin3_strtab_count(tab));
        TEST_ASSERT_EQUAL_STRING("", odin3_strtab_get(tab, 0));
        odin3_strtab_destroy(tab);
    }
    TEST_ASSERT_TRUE(nulls >= 1);
    TEST_ASSERT_TRUE(succeeded);
}

static void test_strbuf_append_and_appendf(void) {
    odin3_strbuf buf;
    odin3_strbuf_init(&buf);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(&buf, odin3_bytes_cstr("x=")));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_appendf(&buf, "%d-%s", 42, "y"));
    TEST_ASSERT_EQUAL_STRING("x=42-y", buf.data);
    TEST_ASSERT_EQUAL_size_t(6, buf.len);
    odin3_strbuf_free(&buf);
}

static void test_strbuf_appendf_grows(void) {
    odin3_strbuf buf;
    odin3_strbuf_init(&buf);
    char *text = malloc(BIG_FMT + 1);
    TEST_ASSERT_NOT_NULL(text);
    memset(text, 'z', BIG_FMT);
    text[BIG_FMT] = '\0';
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(&buf, odin3_bytes_cstr("<")));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_appendf(&buf, "%s>", text));
    TEST_ASSERT_EQUAL_size_t(BIG_FMT + 2, buf.len);
    TEST_ASSERT_EQUAL_CHAR('<', buf.data[0]);
    TEST_ASSERT_EQUAL_CHAR('>', buf.data[BIG_FMT + 1]);
    TEST_ASSERT_EQUAL_CHAR('\0', buf.data[buf.len]);
    TEST_ASSERT_TRUE(buf.cap > buf.len);
    free(text);
    odin3_strbuf_free(&buf);
}

static void test_strbuf_append_null_empty_noop(void) {
    odin3_strbuf buf;
    odin3_strbuf_init(&buf);
    const odin3_bytes none = {NULL, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(&buf, none));
    TEST_ASSERT_EQUAL_size_t(0, buf.len);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(&buf, odin3_bytes_cstr("ab")));
    char *before = buf.data;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(&buf, none));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(&buf, odin3_bytes_cstr("")));
    TEST_ASSERT_TRUE(before == buf.data);
    TEST_ASSERT_EQUAL_STRING("ab", buf.data);
    odin3_strbuf_free(&buf);
}

static void test_strbuf_always_nul_terminated(void) {
    odin3_strbuf buf;
    odin3_strbuf_init(&buf);
    for (int i = 0; i < 300; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(&buf, odin3_bytes_cstr("q")));
        TEST_ASSERT_EQUAL_CHAR('\0', buf.data[buf.len]);
        TEST_ASSERT_EQUAL_size_t((size_t)i + 1, buf.len);
    }
    odin3_strbuf_clear(&buf);
    TEST_ASSERT_EQUAL_size_t(0, buf.len);
    TEST_ASSERT_EQUAL_CHAR('\0', buf.data[0]);
    odin3_strbuf_free(&buf);
    TEST_ASSERT_NULL(buf.data);
}

static void test_strbuf_oom_leaves_buffer_intact(void) {
    odin3_strbuf buf;
    odin3_strbuf_init(&buf);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(&buf, odin3_bytes_cstr("ok")));
    odin3_util_set_alloc_fail_after(0);
    char *text = malloc(BIG_FMT + 1);
    memset(text, 'z', BIG_FMT);
    text[BIG_FMT] = '\0';
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, odin3_strbuf_appendf(&buf, "%s", text));
    free(text);
    TEST_ASSERT_EQUAL_STRING("ok", buf.data);
    TEST_ASSERT_EQUAL_size_t(2, buf.len);
    odin3_strbuf_free(&buf);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_empty_string_is_id_zero);
    RUN_TEST(test_intern_is_idempotent_and_dense);
    RUN_TEST(test_find_does_not_insert);
    RUN_TEST(test_get_pointer_stable);
    RUN_TEST(test_high_bytes_roundtrip);
    RUN_TEST(test_embedded_nul_rejected);
    RUN_TEST(test_long_string_100k);
    RUN_TEST(test_oom_intern);
    RUN_TEST(test_oom_intern_at_growth_boundaries);
    RUN_TEST(test_oom_intern_larger_than_chunk);
    RUN_TEST(test_oom_create);
    RUN_TEST(test_strbuf_append_and_appendf);
    RUN_TEST(test_strbuf_appendf_grows);
    RUN_TEST(test_strbuf_append_null_empty_noop);
    RUN_TEST(test_strbuf_always_nul_terminated);
    RUN_TEST(test_strbuf_oom_leaves_buffer_intact);
    return UNITY_END();
}
