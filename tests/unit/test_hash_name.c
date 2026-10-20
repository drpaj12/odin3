/*
 * test_hash_name.c — unit tests for the hashed names (spec 5.4f: H-2, H-3; T-hash-1, T-hash-3).
 */
#include "backends/common/hash_name.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/sha256.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    MILLION = 1000000,
    MANY = 300,
    NAME_LEN = 40,
    LCG_MUL = 1103515245,
    LCG_ADD = 12345,
    LCG_SEED = 20261020,
    RAND_SHIFT = 8,
    SMALL = 5,
    NARROW = 1,
    TEXT_MAX = 64,
    SHA_BITS_CHARS = 51
};

void setUp(void) {
    odin3_log_set_sink(NULL, NULL);
}

void tearDown(void) {
    odin3_hash_name_test_width(0);
    odin3_util_set_alloc_fail_after(-1);
}

/* --- T-hash-1: SHA-256 vectors (FIPS 180-4 / NIST) -------------------------------------------- */

static void expect_sha(const void *data, size_t len, const char *want_hex) {
    odin3_sha256 ctx;
    uint8_t digest[ODIN3_SHA256_DIGEST_BYTES];
    char hex[ODIN3_SHA256_HEX_CHARS];
    odin3_sha256_init(&ctx);
    odin3_sha256_update(&ctx, data, len);
    odin3_sha256_final(&ctx, digest);
    odin3_sha256_hex(digest, hex);
    TEST_ASSERT_EQUAL_STRING(want_hex, hex);
}

static void test_sha256_nist_vectors(void) {
    expect_sha("", 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    expect_sha("abc", 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const char *m448 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    TEST_ASSERT_EQUAL_UINT(56, strlen(m448));
    expect_sha(m448, 56, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    const char *m896 =
        "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopq"
        "klmnopqrlmnopqrsmnopqrstnopqrstu";
    TEST_ASSERT_EQUAL_UINT(112, strlen(m896));
    expect_sha(m896, 112, "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
    char *million = (char *)malloc(MILLION);
    TEST_ASSERT_NOT_NULL(million);
    memset(million, 'a', MILLION);
    expect_sha(million, MILLION,
               "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    free(million);
}

static void test_base32_edges(void) {
    uint8_t zero[ODIN3_SHA256_DIGEST_BYTES];
    uint8_t ones[ODIN3_SHA256_DIGEST_BYTES];
    char out[TEXT_MAX];
    memset(zero, 0x00, sizeof zero);
    memset(ones, 0xFF, sizeof ones);
    odin3_base32_encode(zero, ODIN3_HASH_NAME_MAX_CHARS, out);
    TEST_ASSERT_EQUAL_UINT(52, strlen(out));
    for (size_t i = 0; i < 52; i++) {
        TEST_ASSERT_EQUAL_CHAR('a', out[i]);
    }
    odin3_base32_encode(ones, ODIN3_HASH_NAME_MAX_CHARS, out);
    for (size_t i = 0; i < SHA_BITS_CHARS; i++) {
        TEST_ASSERT_EQUAL_CHAR('7', out[i]);
    }
    TEST_ASSERT_EQUAL_CHAR('q', out[SHA_BITS_CHARS]); /* one real bit, then four zero bits */
    odin3_base32_encode(ones, 3, out);
    TEST_ASSERT_EQUAL_STRING("777", out);
}

static void test_base32_bit_order(void) {
    uint8_t digest[ODIN3_SHA256_DIGEST_BYTES] = {0x00, 0x44, 0x32, 0x14, 0xC7,
                                                 0x42, 0x54, 0xB6, 0x35, 0xCF};
    char out[TEXT_MAX];
    /* The ten bytes 00 44 32 14 C7 42 54 B6 35 CF are the 5-bit groups 0 to 15: a to p. */
    odin3_base32_encode(digest, 16, out);
    TEST_ASSERT_EQUAL_STRING("abcdefghijklmnop", out);
}

typedef struct known_name {
    const char *full;
    const char *want10;
    const char *want15;
} known_name;

static const known_name KNOWN[] = {
    {"a@t.v:1", "h42xiy3yiju", "h42xiy3yijuqwvoj"},
    {"top/u1/q@rtl/a b.v:42", "hwlvqrodhp5", "hwlvqrodhp5x2s5x"},
    {"x@f.v:7$2", "hfxxdd62auq", "hfxxdd62auq4otb6"},
    {"", "h4oymiquy7q", "h4oymiquy7qobjgx"},
    {"n@d.v:1", "h5sipxd2a2f", "h5sipxd2a2fvhjb7"},
    {"top/u/n@src/m.v:123456", "hchwsq7sxg4", "hchwsq7sxg4penlk"},
    {"\xC3\xA9@f.v:3", "ha2bk6ez2df", "ha2bk6ez2dfzpdle"},
};

static void test_known_names(void) {
    for (size_t i = 0; i < sizeof KNOWN / sizeof KNOWN[0]; i++) {
        odin3_hash_name name;
        odin3_hash_name_make(odin3_bytes_cstr(KNOWN[i].full), ODIN3_HASH_NAME_WIDTH, &name);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(KNOWN[i].want10, name.text, KNOWN[i].full);
        odin3_hash_name_make(odin3_bytes_cstr(KNOWN[i].full), 15, &name);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(KNOWN[i].want15, name.text, KNOWN[i].full);
    }
}

static void test_known_52_and_clamp(void) {
    odin3_hash_name name;
    odin3_hash_name_make(odin3_bytes_cstr("abc"), ODIN3_HASH_NAME_MAX_CHARS, &name);
    TEST_ASSERT_EQUAL_STRING("hxj4bnp4pahh6uqkbidpf3lrceoyagyndsylxvhfucd7wd4qacwwq", name.text);
    odin3_hash_name_make(odin3_bytes_cstr("abc"), 99, &name);
    TEST_ASSERT_EQUAL_UINT(53, strlen(name.text));
    odin3_hash_name_make(odin3_bytes_cstr("abc"), 0, &name);
    TEST_ASSERT_EQUAL_UINT(2, strlen(name.text));
}

static void test_embedded_nul_is_hashed(void) {
    odin3_hash_name first;
    odin3_hash_name second;
    odin3_hash_name_make((odin3_bytes){"a\0b", 3}, 10, &first);
    odin3_hash_name_make((odin3_bytes){"a\0c", 3}, 10, &second);
    TEST_ASSERT_NOT_EQUAL(0, strcmp(first.text, second.text));
}

/* --- T-hash-3: collisions ----------------------------------------------------------------------
 */

typedef struct name_set {
    char text[MANY][NAME_LEN];
    odin3_bytes view[MANY];
    odin3_hash_name out[MANY];
} name_set;

static void fill_names(name_set *set, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        (void)snprintf(set->text[i], NAME_LEN, "top/u%u/n%u@f.v:%u", i % 7, i, i * 3 + 1);
        set->view[i] = odin3_bytes_cstr(set->text[i]);
    }
}

static uint32_t chars_of(const odin3_hash_name *name) {
    return (uint32_t)strlen(name->text) - 1;
}

static void test_default_width_without_collision(void) {
    name_set *set = (name_set *)calloc(1, sizeof *set);
    TEST_ASSERT_NOT_NULL(set);
    fill_names(set, SMALL);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_hash_names_resolve(set->view, SMALL, NULL, 0, set->out));
    for (uint32_t i = 0; i < SMALL; i++) {
        odin3_hash_name want;
        odin3_hash_name_make(set->view[i], ODIN3_HASH_NAME_WIDTH, &want);
        TEST_ASSERT_EQUAL_STRING(want.text, set->out[i].text);
    }
    free(set);
}

static void test_empty_set(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_hash_names_resolve(NULL, 0, NULL, 0, NULL));
}

/* Does another name match set->view[idx] at `chars` characters and itself reach `reach`? */
static bool has_partner(const name_set *set, uint32_t idx, uint32_t chars) {
    uint32_t reach = chars + ODIN3_HASH_NAME_STEP;
    odin3_hash_name mine;
    odin3_hash_name_make(set->view[idx], chars, &mine);
    for (uint32_t j = 0; j < MANY; j++) {
        odin3_hash_name theirs;
        odin3_hash_name_make(set->view[j], chars, &theirs);
        if (j != idx && strcmp(mine.text, theirs.text) == 0 && chars_of(&set->out[j]) >= reach) {
            return true;
        }
    }
    return false;
}

/* Every colliding name extends together: a name wider than NARROW has a partner that matched it at
 * the width below and is itself at least as wide. */
static void expect_extension_is_pairwise(const name_set *set) {
    for (uint32_t i = 0; i < MANY; i++) {
        uint32_t width = chars_of(&set->out[i]);
        if (width > NARROW) {
            TEST_ASSERT_TRUE_MESSAGE(has_partner(set, i, width - ODIN3_HASH_NAME_STEP),
                                     set->text[i]);
        }
    }
}

static void expect_all_distinct(const name_set *set, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        for (uint32_t j = i + 1; j < n; j++) {
            TEST_ASSERT_NOT_EQUAL_MESSAGE(0, strcmp(set->out[i].text, set->out[j].text),
                                          set->text[i]);
        }
    }
}

static void test_narrow_width_forces_collisions(void) {
    name_set *set = (name_set *)calloc(1, sizeof *set);
    TEST_ASSERT_NOT_NULL(set);
    fill_names(set, MANY);
    odin3_hash_name_test_width(NARROW);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_hash_names_resolve(set->view, MANY, NULL, 0, set->out));
    expect_all_distinct(set, MANY);
    expect_extension_is_pairwise(set);
    uint32_t widest = 0;
    for (uint32_t i = 0; i < MANY; i++) {
        widest = chars_of(&set->out[i]) > widest ? chars_of(&set->out[i]) : widest;
        TEST_ASSERT_EQUAL_UINT(0, (chars_of(&set->out[i]) - NARROW) % ODIN3_HASH_NAME_STEP);
    }
    TEST_ASSERT_TRUE(widest > NARROW); /* 300 names in 32 buckets: collisions are certain */
    free(set);
}

static uint32_t lcg_state;
static uint32_t lcg_next(void) {
    lcg_state = lcg_state * LCG_MUL + LCG_ADD;
    return lcg_state >> RAND_SHIFT;
}

static void shuffle(odin3_bytes *view, uint32_t n) {
    for (uint32_t i = n - 1; i > 0; i--) {
        uint32_t j = lcg_next() % (i + 1);
        odin3_bytes tmp = view[i];
        view[i] = view[j];
        view[j] = tmp;
    }
}

static const char *find_out(const name_set *set, odin3_bytes full, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (set->view[i].len == full.len && memcmp(set->view[i].ptr, full.ptr, full.len) == 0) {
            return set->out[i].text;
        }
    }
    return NULL;
}

/* Every name of first has the same result in second, which holds the same names in another order.
 */
static void expect_same_results(const name_set *first, const name_set *second) {
    for (uint32_t i = 0; i < MANY; i++) {
        const char *other = find_out(second, first->view[i], MANY);
        TEST_ASSERT_NOT_NULL(other);
        TEST_ASSERT_EQUAL_STRING(first->out[i].text, other);
    }
}

static void test_result_independent_of_order(void) {
    name_set *first = (name_set *)calloc(1, sizeof *first);
    name_set *second = (name_set *)calloc(1, sizeof *second);
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_NOT_NULL(second);
    fill_names(first, MANY);
    fill_names(second, MANY);
    odin3_hash_name_test_width(NARROW);
    lcg_state = LCG_SEED;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_hash_names_resolve(first->view, MANY, NULL, 0, first->out));
    for (int round = 0; round < SMALL; round++) {
        shuffle(second->view, MANY);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_hash_names_resolve(second->view, MANY, NULL, 0, second->out));
        expect_same_results(first, second);
    }
    free(first);
    free(second);
}

static void test_small_set_all_orders(void) {
    odin3_bytes base[4] = {{"a", 1}, {"b", 1}, {"c", 1}, {"d", 1}};
    odin3_hash_name want[4];
    odin3_hash_name_test_width(NARROW);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_hash_names_resolve(base, 4, NULL, 0, want));
    for (uint32_t perm = 0; perm < 24; perm++) {
        uint32_t order[4] = {0, 1, 2, 3};
        uint32_t code = perm;
        for (uint32_t i = 0; i < 4; i++) { /* the perm-th permutation, factorial digits */
            uint32_t pick = i + code % (4 - i);
            uint32_t tmp = order[pick];
            code /= (4 - i);
            order[pick] = order[i];
            order[i] = tmp;
        }
        odin3_bytes view[4];
        odin3_hash_name out[4];
        for (uint32_t i = 0; i < 4; i++) {
            view[i] = base[order[i]];
        }
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_hash_names_resolve(view, 4, NULL, 0, out));
        for (uint32_t i = 0; i < 4; i++) {
            TEST_ASSERT_EQUAL_STRING(want[order[i]].text, out[i].text);
        }
    }
}

static void test_reserved_name_extends(void) {
    odin3_bytes full = odin3_bytes_cstr("top/n@f.v:9");
    odin3_hash_name plain;
    odin3_hash_name out;
    odin3_hash_name_make(full, ODIN3_HASH_NAME_WIDTH, &plain);
    odin3_bytes reserved[2] = {odin3_bytes_cstr("hzzzzzzzzzz"), odin3_bytes_cstr(plain.text)};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_hash_names_resolve(&full, 1, reserved, 2, &out));
    TEST_ASSERT_EQUAL_UINT(15, chars_of(&out));
    odin3_hash_name want;
    odin3_hash_name_make(full, 15, &want);
    TEST_ASSERT_EQUAL_STRING(want.text, out.text);
    /* a reserved name of the other width does not matter */
    reserved[1] = odin3_bytes_cstr("h42xiy3yijuqwvoj");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_hash_names_resolve(&full, 1, reserved, 2, &out));
    TEST_ASSERT_EQUAL_STRING(plain.text, out.text);
}

static void test_equal_full_names_are_one_name(void) {
    odin3_bytes fulls[3] = {odin3_bytes_cstr("x@f.v:1"), odin3_bytes_cstr("x@f.v:1"),
                            odin3_bytes_cstr("y@f.v:1")};
    odin3_hash_name out[3];
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_hash_names_resolve(fulls, 3, NULL, 0, out));
    TEST_ASSERT_EQUAL_STRING(out[0].text, out[1].text);
    TEST_ASSERT_EQUAL_UINT(ODIN3_HASH_NAME_WIDTH, chars_of(&out[0]));
}

static void test_unresolvable_at_cap_is_an_error(void) {
    odin3_bytes full = odin3_bytes_cstr("x@f.v:1");
    odin3_hash_name cap;
    odin3_hash_name out;
    odin3_hash_name_make(full, ODIN3_HASH_NAME_MAX_CHARS, &cap);
    odin3_bytes reserved = odin3_bytes_cstr(cap.text);
    odin3_hash_name_test_width(ODIN3_HASH_NAME_MAX_CHARS);
    odin3_log_mute(true);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_hash_names_resolve(&full, 1, &reserved, 1, &out));
    odin3_log_mute(false);
}

static void test_extension_stops_at_cap(void) {
    odin3_bytes full = odin3_bytes_cstr("x@f.v:1");
    odin3_hash_name at50;
    odin3_hash_name out;
    odin3_hash_name_make(full, 50, &at50);
    odin3_bytes reserved = odin3_bytes_cstr(at50.text);
    odin3_hash_name_test_width(50);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_hash_names_resolve(&full, 1, &reserved, 1, &out));
    TEST_ASSERT_EQUAL_UINT(ODIN3_HASH_NAME_MAX_CHARS, chars_of(&out)); /* 50 + 5 clamps to 52 */
}

static void test_over_limit_is_an_error(void) {
    odin3_log_mute(true);
    TEST_ASSERT_EQUAL_INT(
        ODIN3_ERR_INVALID_ARG,
        odin3_hash_names_resolve(NULL, ODIN3_HASH_NAME_MAX_ENTRIES + 1, NULL, 0, NULL));
    odin3_log_mute(false);
}

static void test_out_of_memory_at_every_allocation(void) {
    name_set *set = (name_set *)calloc(1, sizeof *set);
    TEST_ASSERT_NOT_NULL(set);
    fill_names(set, SMALL);
    odin3_bytes reserved = odin3_bytes_cstr("hzzzzzzzzzz");
    int fails = 0;
    for (long k = 0; k < 16; k++) {
        odin3_util_set_alloc_fail_after(k);
        odin3_status st = odin3_hash_names_resolve(set->view, SMALL, &reserved, 1, set->out);
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            break;
        }
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        fails++;
    }
    TEST_ASSERT_TRUE(fails > 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_hash_names_resolve(set->view, SMALL, &reserved, 1, set->out));
    free(set);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_sha256_nist_vectors);
    RUN_TEST(test_base32_edges);
    RUN_TEST(test_base32_bit_order);
    RUN_TEST(test_known_names);
    RUN_TEST(test_known_52_and_clamp);
    RUN_TEST(test_embedded_nul_is_hashed);
    RUN_TEST(test_default_width_without_collision);
    RUN_TEST(test_empty_set);
    RUN_TEST(test_narrow_width_forces_collisions);
    RUN_TEST(test_result_independent_of_order);
    RUN_TEST(test_small_set_all_orders);
    RUN_TEST(test_reserved_name_extends);
    RUN_TEST(test_equal_full_names_are_one_name);
    RUN_TEST(test_unresolvable_at_cap_is_an_error);
    RUN_TEST(test_extension_stops_at_cap);
    RUN_TEST(test_over_limit_is_an_error);
    RUN_TEST(test_out_of_memory_at_every_allocation);
    return UNITY_END();
}
