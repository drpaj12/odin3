/*
 * arch_test.h — shared helpers of the architecture importer tests: an error-capturing log sink, a
 * fixture loader and a parse-from-text entry.
 */
#ifndef ODIN3_TESTS_ARCH_TEST_H
#define ODIN3_TESTS_ARCH_TEST_H

#include "frontends/arch/arch.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { ARCH_TEST_MSG = 1024 };

static char g_msg[ARCH_TEST_MSG];   /* the first error */
static char g_warn[ARCH_TEST_MSG];  /* the first warning */
static char g_info[ARCH_TEST_MSG];  /* the first info */
static unsigned g_errors, g_warnings;

static void arch_test_sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (level == ODIN3_LOG_ERROR && g_errors++ == 0) {
        (void)snprintf(g_msg, sizeof g_msg, "%s", msg);
    } else if (level == ODIN3_LOG_WARN && g_warnings++ == 0) {
        (void)snprintf(g_warn, sizeof g_warn, "%s", msg);
    } else if (level == ODIN3_LOG_INFO && g_info[0] == '\0') {
        (void)snprintf(g_info, sizeof g_info, "%s", msg);
    }
}

static void arch_test_reset(void) {
    g_msg[0] = g_warn[0] = g_info[0] = '\0';
    g_errors = g_warnings = 0;
    odin3_log_set_sink(arch_test_sink, NULL);
}

static void arch_test_done(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
}

/* Parses text as the architecture `name` into arch. */
static odin3_status arch_test_parse(odin3_arch *arch, const char *name, const char *text) {
    const odin3_arch_src src = {name, odin3_bytes_cstr(text)};
    return odin3_arch_parse(arch, &src);
}

/* The contents of a file under tests/unit/data/arch; the caller frees. */
static char *arch_test_load(const char *file) {
    char path[512];
    (void)snprintf(path, sizeof path, "%s/%s", ODIN3_ARCH_DATA_DIR, file);
    FILE *fp = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(fp, path);
    char *buf = malloc(1u << 20);
    TEST_ASSERT_NOT_NULL(buf);
    size_t n = fread(buf, 1, (1u << 20) - 1u, fp);
    buf[n] = '\0';
    (void)fclose(fp);
    return buf;
}

static odin3_status arch_test_parse_file(odin3_arch *arch, const char *file) {
    char *text = arch_test_load(file);
    odin3_status st = arch_test_parse(arch, file, text);
    free(text);
    return st;
}

#endif
