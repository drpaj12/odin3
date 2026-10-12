/*
 * test_sha256.c — unit tests for SHA-256 (FIPS 180-4 vectors, streaming, no allocation).
 */
#include "sim/prng.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/sha256.h"

#include <stdlib.h>
#include <string.h>

enum { MILLION = 1000000, PRNG_BYTES = 64 * 1024, ODD_CHUNK = 7 };

static const char *const EMPTY_HEX =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
static const char *const ABC_HEX =
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
static const char *const TWO_BLOCK_IN = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
static const char *const TWO_BLOCK_HEX =
    "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1";
static const char *const MILLION_A_HEX =
    "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0";

void setUp(void) {
}
void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
}

static void hash_chunked(const void *data, size_t len, size_t chunk,
                         char hex[ODIN3_SHA256_HEX_CHARS]) {
    odin3_sha256 ctx;
    uint8_t digest[ODIN3_SHA256_DIGEST_BYTES];
    const uint8_t *src = (const uint8_t *)data;
    odin3_sha256_init(&ctx);
    for (size_t off = 0; off < len; off += chunk) {
        size_t take = len - off < chunk ? len - off : chunk;
        odin3_sha256_update(&ctx, src + off, take);
    }
    odin3_sha256_final(&ctx, digest);
    odin3_sha256_hex(digest, hex);
}

typedef struct known_hash {
    const char *text;
    const char *hex;
} known_hash;

static void expect_hash(const known_hash *known) {
    char hex[ODIN3_SHA256_HEX_CHARS];
    size_t len = strlen(known->text);
    hash_chunked(known->text, len, len == 0 ? 1 : len, hex);
    TEST_ASSERT_EQUAL_STRING(known->hex, hex);
}

static void test_empty(void) {
    expect_hash(&(known_hash){"", EMPTY_HEX});
    odin3_sha256 ctx;
    uint8_t digest[ODIN3_SHA256_DIGEST_BYTES];
    char hex[ODIN3_SHA256_HEX_CHARS];
    odin3_sha256_init(&ctx);
    odin3_sha256_update(&ctx, NULL, 0);
    odin3_sha256_final(&ctx, digest);
    odin3_sha256_hex(digest, hex);
    TEST_ASSERT_EQUAL_STRING(EMPTY_HEX, hex);
}

static void test_abc(void) {
    expect_hash(&(known_hash){"abc", ABC_HEX});
}

static void test_two_block_message(void) {
    expect_hash(&(known_hash){TWO_BLOCK_IN, TWO_BLOCK_HEX});
}

static void test_million_a_all_chunkings(void) {
    char *data = malloc(MILLION);
    TEST_ASSERT_NOT_NULL(data);
    memset(data, 'a', MILLION);
    static const size_t CHUNKS[] = {MILLION, 1, 63, 64, 65, 1000};
    for (size_t i = 0; i < sizeof CHUNKS / sizeof CHUNKS[0]; i++) {
        char hex[ODIN3_SHA256_HEX_CHARS];
        hash_chunked(data, MILLION, CHUNKS[i], hex);
        TEST_ASSERT_EQUAL_STRING(MILLION_A_HEX, hex);
    }
    free(data);
}

/* Known answers (Python hashlib) for bytes (i * 7 + 3) & 255: tails of 55 and 63 bytes, a full
 * block, and 120 bytes (stale non-zero block bytes before the padding). */
static void test_padding_known_answers(void) {
    static const struct {
        size_t len;
        const char *hex;
    } CASES[] = {
        {55, "e7313d333c272e639f790978283f9eb392e843d0f29b7016828bb1daa4aac70b"},
        {63, "81c80242132f230c3bd41b3e63bbcff16107339549214a99614ff26664625055"},
        {64, "39e3d7b6b5d075d37d053ad89b24b41bef4f3c29760c84447cab3f3be1882241"},
        {120, "7836b787757e95e58b3ca5aec90b1b004e8deba1e50e9675af9cabf1a13a04b5"},
    };
    uint8_t data[ODIN3_SHA256_BLOCK_BYTES * 2];
    for (size_t i = 0; i < sizeof data; i++) {
        data[i] = (uint8_t)((i * 7U) + 3U);
    }
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        char hex[ODIN3_SHA256_HEX_CHARS];
        hash_chunked(data, CASES[i].len, CASES[i].len, hex);
        TEST_ASSERT_EQUAL_STRING(CASES[i].hex, hex);
        hash_chunked(data, CASES[i].len, 1, hex);
        TEST_ASSERT_EQUAL_STRING(CASES[i].hex, hex);
    }
}

static void test_padding_boundaries(void) {
    /* Lengths around the 55/56/63/64 padding edges agree whole and byte-by-byte. */
    uint8_t data[130];
    memset(data, 'x', sizeof data);
    for (size_t len = 0; len <= sizeof data; len++) {
        char whole[ODIN3_SHA256_HEX_CHARS];
        char bytewise[ODIN3_SHA256_HEX_CHARS];
        hash_chunked(data, len, len == 0 ? 1 : len, whole);
        hash_chunked(data, len, 1, bytewise);
        TEST_ASSERT_EQUAL_STRING(whole, bytewise);
    }
}

static void test_prng_stream_chunk_equivalence(void) {
    uint8_t *data = malloc(PRNG_BYTES);
    TEST_ASSERT_NOT_NULL(data);
    odin3_prng prng;
    odin3_prng_seed(&prng, 0x3D7);
    for (size_t i = 0; i < PRNG_BYTES; i++) {
        data[i] = (uint8_t)odin3_prng_next(&prng);
    }
    char one[ODIN3_SHA256_HEX_CHARS];
    char seven[ODIN3_SHA256_HEX_CHARS];
    hash_chunked(data, PRNG_BYTES, PRNG_BYTES, one);
    hash_chunked(data, PRNG_BYTES, ODD_CHUNK, seven);
    TEST_ASSERT_EQUAL_STRING(one, seven);
    free(data);
}

static void test_no_allocation(void) {
    odin3_util_set_alloc_fail_after(0);
    expect_hash(&(known_hash){"abc", ABC_HEX});
    expect_hash(&(known_hash){TWO_BLOCK_IN, TWO_BLOCK_HEX});
}

static void test_hex_lower_case_and_nul(void) {
    uint8_t digest[ODIN3_SHA256_DIGEST_BYTES];
    char hex[ODIN3_SHA256_HEX_CHARS];
    memset(digest, 0xAB, sizeof digest);
    memset(hex, 'Z', sizeof hex);
    odin3_sha256_hex(digest, hex);
    TEST_ASSERT_EQUAL_size_t(ODIN3_SHA256_HEX_CHARS - 1U, strlen(hex));
    TEST_ASSERT_EQUAL_CHAR('\0', hex[ODIN3_SHA256_HEX_CHARS - 1U]);
    TEST_ASSERT_EQUAL_CHAR('a', hex[0]);
    TEST_ASSERT_EQUAL_CHAR('b', hex[1]);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_empty);
    RUN_TEST(test_abc);
    RUN_TEST(test_two_block_message);
    RUN_TEST(test_million_a_all_chunkings);
    RUN_TEST(test_padding_known_answers);
    RUN_TEST(test_padding_boundaries);
    RUN_TEST(test_prng_stream_chunk_equivalence);
    RUN_TEST(test_no_allocation);
    RUN_TEST(test_hex_lower_case_and_nul);
    return UNITY_END();
}
