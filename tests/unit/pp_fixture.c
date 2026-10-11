/*
 * pp_fixture.c — the fixture shared by the preprocessor unit tests (see pp_fixture.h).
 */
#include "pp_fixture.h"

#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { MAX_MADE = 4096 };

odin3_design *design;
odin3_srcman *sm;
odin3_pp *pp;
odin3_pp_stream stream;
char tmp[PP_PATH_BUF / 2];
char captured[PP_MSG_MAX];
char warned[PP_MSG_MAX];
int errors;
int warnings;

static char made[MAX_MADE][PP_PATH_BUF];
static int nmade;

static void sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (level == ODIN3_LOG_ERROR) {
        errors++;
        (void)snprintf(captured, sizeof captured, "%s", msg);
    } else if (level == ODIN3_LOG_WARN) {
        warnings++;
        (void)snprintf(warned, sizeof warned, "%s", msg);
    }
}

void pp_fixture_set_up(void) {
    odin3_log_set_sink(sink, NULL);
    captured[0] = '\0';
    warned[0] = '\0';
    errors = 0;
    warnings = 0;
    nmade = 0;
    pp = NULL;
    memset(&stream, 0, sizeof stream);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    sm = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_srcman(design, &sm));
    const char *base = getenv("TMPDIR");
    (void)snprintf(tmp, sizeof tmp, "%s/odin3_pp_XXXXXX",
                   base != NULL && base[0] != '\0' ? base : "/tmp");
    TEST_ASSERT_NOT_NULL(mkdtemp(tmp));
}

void pp_fixture_tear_down(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    odin3_pp_destroy(pp);
    pp = NULL;
    odin3_design_destroy(design);
    design = NULL;
    int failed = 0;
    for (int i = nmade - 1; i >= 0; i--) {
        failed += remove(made[i]) != 0;
    }
    nmade = 0;
    failed += rmdir(tmp) != 0;
    TEST_ASSERT_EQUAL_INT(0, failed);
}

uint32_t id(const char *str) {
    uint32_t got = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr(str), &got));
    return got;
}

void remember(const char *path) {
    TEST_ASSERT_TRUE(nmade < MAX_MADE);
    (void)snprintf(made[nmade++], PP_PATH_BUF, "%s", path);
}

void put(const char *name, odin3_bytes text, char *path) {
    char full[PP_PATH_BUF];
    (void)snprintf(full, sizeof full, "%s/%s", tmp, name);
    bool fresh = access(full, F_OK) != 0;
    FILE *fp = fopen(full, "wb");
    TEST_ASSERT_NOT_NULL_MESSAGE(fp, full);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(text.len, fwrite(text.ptr, 1, text.len, fp), name);
    TEST_ASSERT_EQUAL_INT(0, fclose(fp));
    if (fresh) {
        remember(full);
    }
    if (path != NULL) {
        (void)snprintf(path, PP_PATH_BUF, "%s", full);
    }
}

void create_pp(const odin3_pp_config *config) {
    odin3_pp_config none = {NULL, 0, NULL, 0};
    odin3_pp_destroy(pp);
    pp = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pp_create(design, config != NULL ? config : &none, &pp));
}

void ensure_pp(void) {
    if (pp == NULL) {
        create_pp(NULL);
    }
}

odin3_status run_at(const char *name, const char *path, odin3_loc at) {
    ensure_pp();
    odin3_pp_source src = {id(name), id(path), id("work"), at};
    return odin3_pp_run(pp, &src, &stream);
}

odin3_status run_path(const char *name, const char *path) {
    return run_at(name, path, (odin3_loc){0});
}

odin3_status run_fresh(const char *shown, odin3_bytes text) {
    static unsigned serial;
    char file[PP_PATH_BUF / 4];
    char path[PP_PATH_BUF];
    (void)snprintf(file, sizeof file, "r%u_%s", serial++, shown);
    put(file, text, path);
    return run_path(shown, path);
}

void assert_text(const char *want) {
    TEST_ASSERT_EQUAL_size_t_MESSAGE(strlen(want), stream.text.len, want);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(want, stream.text.ptr, strlen(want) == 0 ? 1 : strlen(want),
                                     want);
}

const odin3_srcseg_rec *segments(uint32_t *count) {
    odin3_srcbuf_info info;
    TEST_ASSERT_TRUE(odin3_srcman_buffer_info(sm, stream.stream, &info));
    const odin3_srcbuf_rec *rec = odin3_srcman_rec_of(sm, (odin3_loc){info.start});
    const odin3_srcfile_rec *file = odin3_srcman_file_of(sm, rec);
    *count = (uint32_t)file->segs.len;
    return (const odin3_srcseg_rec *)file->segs.data;
}

uint32_t at_offset(uint32_t offset) {
    return odin3_srcman_loc(sm, stream.stream, offset).v;
}
