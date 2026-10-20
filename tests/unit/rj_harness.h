/*
 * rj_harness.h — shared helpers of the read_json tests: a temporary directory (mkdtemp), a log
 * capture, rj_read (a JSON string through the reader) and lookups in the design read.
 */
#ifndef ODIN3_TESTS_RJ_HARNESS_H
#define ODIN3_TESTS_RJ_HARNESS_H

#include "backends/json/writer.h"
#include "frontends/json/reader.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/module.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { RJ_PATH = 512, RJ_LOG = 32768, RJ_FILES = 32 };

static odin3_design *rj_design;
static char rj_dir[256];
static char rj_log[RJ_LOG];
static size_t rj_log_len;
static char rj_files[RJ_FILES][RJ_PATH];
static int rj_n_files;

static inline void rj_sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    (void)level;
    size_t len = strlen(msg);
    if (rj_log_len + len + 2 < sizeof rj_log) {
        memcpy(rj_log + rj_log_len, msg, len);
        rj_log_len += len;
        rj_log[rj_log_len++] = '\n';
        rj_log[rj_log_len] = '\0';
    }
}

static inline void rj_log_clear(void) {
    rj_log_len = 0;
    rj_log[0] = '\0';
}

static inline void rj_setup(void) {
    (void)snprintf(rj_dir, sizeof rj_dir, "%s/rj.XXXXXX", ODIN3_RJ_ROOT);
    TEST_ASSERT_NOT_NULL(mkdtemp(rj_dir));
    rj_n_files = 0;
    rj_log_clear();
    odin3_log_set_level(ODIN3_LOG_INFO);
    odin3_log_set_sink(rj_sink, NULL);
    rj_design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(rj_design);
}

static inline void rj_teardown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_design_destroy(rj_design);
    rj_design = NULL;
    odin3_log_set_sink(NULL, NULL);
    for (int i = 0; i < rj_n_files; i++) {
        (void)remove(rj_files[i]);
    }
    (void)rmdir(rj_dir);
}

/* The path of a file of the test directory (recorded once for removal). */
static inline const char *rj_path(const char *name) {
    char path[RJ_PATH];
    (void)snprintf(path, sizeof path, "%s/%.200s", rj_dir, name);
    for (int i = 0; i < rj_n_files; i++) {
        if (strcmp(rj_files[i], path) == 0) {
            return rj_files[i];
        }
    }
    TEST_ASSERT_TRUE(rj_n_files < RJ_FILES);
    (void)snprintf(rj_files[rj_n_files], RJ_PATH, "%s", path);
    return rj_files[rj_n_files++];
}

/* A file to write: its name in the test directory and its text. */
typedef struct rj_file {
    const char *name;
    const char *text;
} rj_file;

static inline const char *rj_put_file(rj_file file) {
    const char *path = rj_path(file.name);
    FILE *fp = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(fp);
    TEST_ASSERT_EQUAL_UINT64(strlen(file.text), fwrite(file.text, 1, strlen(file.text), fp));
    TEST_ASSERT_EQUAL_INT(0, fclose(fp));
    return path;
}

#define rj_put(file_name, file_text) rj_put_file((rj_file){(file_name), (file_text)})

/* The whole file, owned by the caller (odin3_util_free). */
static inline char *rj_slurp(const char *path) {
    FILE *fp = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(fp);
    (void)fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    (void)fseek(fp, 0, SEEK_SET);
    char *text = odin3_util_malloc((size_t)size + 1);
    TEST_ASSERT_NOT_NULL(text);
    TEST_ASSERT_EQUAL_UINT64((size_t)size, fread(text, 1, (size_t)size, fp));
    text[size] = '\0';
    (void)fclose(fp);
    return text;
}

/* Replaces rj_design by a fresh one. */
static inline void rj_fresh(void) {
    odin3_design_destroy(rj_design);
    rj_design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(rj_design);
}

/* Reads the JSON text into a fresh design (in.json of the test directory). */
static inline odin3_status rj_read_opts(const char *json, const odin3_json_opts *opts) {
    rj_fresh();
    rj_log_clear();
    return odin3_json_read(rj_design, rj_put("in.json", json), opts);
}

static inline odin3_status rj_read(const char *json) {
    return rj_read_opts(json, NULL);
}

static inline void rj_expect_ok(const char *json) {
    odin3_status st = rj_read(json);
    if (st != ODIN3_OK) {
        TEST_FAIL_MESSAGE(rj_log);
    }
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK,
        odin3_check_design(rj_design, (odin3_check_opts){ODIN3_CHECK_FULL, ODIN3_VIEW_NONE}));
}

/* The read of json failed with ODIN3_ERR_PARSE and the log holds text. */
typedef struct rj_failure {
    const char *json;
    const char *text;
    const odin3_json_opts *opts;
} rj_failure;

static inline void rj_expect_failure(rj_failure item) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, rj_read_opts(item.json, item.opts));
    if (strstr(rj_log, item.text) == NULL) {
        char msg[RJ_LOG + 128];
        (void)snprintf(msg, sizeof msg, "log lacks '%s':\n%s", item.text, rj_log);
        TEST_FAIL_MESSAGE(msg);
    }
}

#define rj_expect_error(json_text, log_text)                                                       \
    rj_expect_failure((rj_failure){(json_text), (log_text), NULL})

static inline void rj_log_has(const char *text) {
    if (strstr(rj_log, text) == NULL) {
        char msg[RJ_LOG + 128];
        (void)snprintf(msg, sizeof msg, "log lacks '%s':\n%s", text, rj_log);
        TEST_FAIL_MESSAGE(msg);
    }
}

static inline uint32_t rj_str(const char *name) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(rj_design, odin3_bytes_cstr(name), &str));
    return str;
}

static inline odin3_module *rj_module(const char *name) {
    uint32_t str = rj_str(name);
    for (uint32_t i = 1; i < odin3_design_module_end(rj_design); i++) {
        odin3_module *mod = odin3_module_get(rj_design, (odin3_module_id){i});
        if (odin3_module_name(mod) == str) {
            return mod;
        }
    }
    TEST_FAIL_MESSAGE(name);
    return NULL;
}

static inline odin3_wire_id rj_wire(odin3_module *mod, const char *name) {
    odin3_wire_id wire = odin3_module_find_wire(mod, rj_str(name));
    TEST_ASSERT_TRUE_MESSAGE(odin3_wire_valid(wire), name);
    return wire;
}

static inline odin3_node_id rj_node(odin3_module *mod, const char *name) {
    odin3_node_id node = odin3_module_find_node(mod, rj_str(name));
    TEST_ASSERT_TRUE_MESSAGE(odin3_node_valid(node), name);
    return node;
}

/* The module name of the design's top. */
static inline const char *rj_top_name(void) {
    odin3_module_id top = odin3_design_top(rj_design);
    TEST_ASSERT_TRUE(odin3_module_valid(top));
    return odin3_strtab_get(odin3_design_strtab(rj_design),
                            odin3_module_name(odin3_module_get(rj_design, top)));
}

#endif
