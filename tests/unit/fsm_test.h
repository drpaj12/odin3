/*
 * fsm_test.h — helpers of the fsm pass's unit tests (4.8): a Verilog source is written to a
 * fresh temporary directory (mkdtemp under $TMPDIR, else /tmp), read with read_verilog
 * --nolatches and lowered with proc, passes run by name, every log message captured, and the
 * IR read back (wire attributes, nodes by type). Test code: it may allocate freely and relies on
 * Unity assertions. One design at a time (ft); setUp zeroes it, tearDown calls ft_close.
 */
#ifndef ODIN3_TESTS_UNIT_FSM_TEST_H
#define ODIN3_TESTS_UNIT_FSM_TEST_H

#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/manager.h"
#include "unity.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { FT_PATH = 512, FT_MSG = 512, FT_MAX_MSGS = 4096 };

typedef struct ft_msg {
    odin3_log_level level;
    char text[FT_MSG];
} ft_msg;

typedef struct ft_state {
    odin3_design *design;
    char dir[FT_PATH];
    char file[FT_PATH + 16];
    ft_msg msgs[FT_MAX_MSGS];
    uint32_t n_msgs;
} ft_state;

static ft_state ft;

static inline void ft_sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (ft.n_msgs < FT_MAX_MSGS) {
        ft_msg *slot = &ft.msgs[ft.n_msgs++];
        slot->level = level;
        (void)snprintf(slot->text, sizeof slot->text, "%s", msg);
    }
}

static inline void ft_close(void) {
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(ft.design);
    if (ft.file[0] != '\0') {
        (void)remove(ft.file);
    }
    if (ft.dir[0] != '\0') {
        (void)rmdir(ft.dir);
    }
    memset(&ft, 0, sizeof ft);
}

/* A fresh design and an empty log. */
static inline void ft_open(void) {
    ft_close();
    ft.design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(ft.design);
    odin3_log_set_sink(ft_sink, NULL);
}

/* The text of the messages at or above nothing: every message, one per line. */
static inline const char *ft_log(void) {
    static char buf[1 << 15];
    size_t len = 0;
    buf[0] = '\0';
    for (uint32_t i = 0; i < ft.n_msgs && len + FT_MSG < sizeof buf; i++) {
        int got = snprintf(buf + len, sizeof buf - len, "[%d] %s\n", (int)ft.msgs[i].level,
                           ft.msgs[i].text);
        len += got > 0 ? (size_t)got : 0U;
    }
    return buf;
}

static inline odin3_status ft_pass(const char *name, const char *args) {
    return odin3_pass_run(ft.design, name, odin3_bytes_cstr(args));
}

/* Writes verilog to a fresh temporary file; ft.file is its path. */
static inline void ft_write_source(const char *verilog) {
    const char *base = getenv("TMPDIR");
    (void)snprintf(ft.dir, sizeof ft.dir, "%s/fsm-test-XXXXXX",
                   base != NULL && base[0] != '\0' ? base : "/tmp");
    TEST_ASSERT_NOT_NULL(mkdtemp(ft.dir));
    (void)snprintf(ft.file, sizeof ft.file, "%s/t.v", ft.dir);
    FILE *out = fopen(ft.file, "w");
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_TRUE(fputs(verilog, out) >= 0);
    TEST_ASSERT_EQUAL_INT(0, fclose(out));
}

/* Reads verilog (read_verilog --nolatches) and runs proc. */
static inline void ft_read_proc(const char *verilog) {
    ft_open();
    ft_write_source(verilog);
    char args[FT_PATH + 32];
    (void)snprintf(args, sizeof args, "--nolatches %s", ft.file);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, ft_pass("read_verilog", args), ft_log());
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, ft_pass("proc", ""), ft_log());
}

/* Reads a Verilog file by path (read_verilog --nolatches) and runs proc. */
static inline void ft_read_proc_file(const char *path) {
    ft_open();
    char args[FT_PATH + 32];
    (void)snprintf(args, sizeof args, "--nolatches %s", path);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, ft_pass("read_verilog", args), ft_log());
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, ft_pass("proc", ""), ft_log());
}

static inline uint32_t ft_intern(const char *text) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(ft.design, odin3_bytes_cstr(text), &str));
    return str;
}

/* The module named name (the test fails when there is none). */
static inline odin3_module *ft_module(const char *name) {
    uint32_t str = ft_intern(name);
    for (uint32_t id = 1; id < odin3_design_module_end(ft.design); id++) {
        odin3_module *module = odin3_module_get(ft.design, (odin3_module_id){id});
        if (odin3_module_name(module) == str) {
            return module;
        }
    }
    TEST_FAIL_MESSAGE(name);
    return NULL;
}

/* A wire (or any object) by module and name. */
typedef struct ft_ref {
    const char *module, *name;
} ft_ref;

/* The string attribute `key` of a wire, NULL when the wire has none. */
static inline const char *ft_wire_attr(ft_ref ref, const char *key) {
    odin3_module *module = ft_module(ref.module);
    odin3_wire_id wire = odin3_module_find_wire(module, ft_intern(ref.name));
    TEST_ASSERT_TRUE_MESSAGE(odin3_wire_valid(wire), ref.name);
    const odin3_value *val =
        odin3_attr_get(module, (odin3_objref){ODIN3_OBJ_WIRE, wire.v}, ft_intern(key));
    if (val == NULL) {
        return NULL;
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_VAL_STRING, val->kind);
    return odin3_strtab_get(odin3_design_strtab(ft.design), val->str);
}

/* Number of messages at level whose text starts with prefix. */
static inline uint32_t ft_count(odin3_log_level level, const char *prefix) {
    uint32_t hits = 0;
    for (uint32_t i = 0; i < ft.n_msgs; i++) {
        hits += ft.msgs[i].level == level && strncmp(ft.msgs[i].text, prefix, strlen(prefix)) == 0
                    ? 1U
                    : 0U;
    }
    return hits;
}

/* True when some message at level has exactly this text. */
static inline bool ft_has(odin3_log_level level, const char *text) {
    for (uint32_t i = 0; i < ft.n_msgs; i++) {
        if (ft.msgs[i].level == level && strcmp(ft.msgs[i].text, text) == 0) {
            return true;
        }
    }
    return false;
}

/* The number of live nodes in ref.module whose cell type is named ref.name. */
static inline uint32_t ft_count_nodes(ft_ref ref) {
    odin3_module *module = ft_module(ref.module);
    odin3_celltype_id id = {0};
    if (!odin3_celltype_find(ft.design, ft_intern(ref.name), &id)) {
        return 0;
    }
    uint32_t count = 0;
    for (uint32_t node = 1; node < odin3_module_node_end(module); node++) {
        count += odin3_node_live(module, (odin3_node_id){node}) &&
                         odin3_node_type(module, (odin3_node_id){node}).v == id.v
                     ? 1U
                     : 0U;
    }
    return count;
}

#endif
