/*
 * abc_fixture.h — helpers shared by the abc pass unit tests (3D plan Tasks 14 to 16; static inline:
 * a test uses what it needs): a capturing log sink, a design built from BLIF text or through the
 * IR API, a pass context, and the test directory every file lands in.
 */
#ifndef ODIN3_TESTS_UNIT_ABC_FIXTURE_H
#define ODIN3_TESTS_UNIT_ABC_FIXTURE_H

#include "backends/blif/writer.h"
#include "frontends/blif/reader.h"
#include "ir/celltype.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "unity.h"
#include "util/log.h"
#include "util/str.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef ODIN3_ABC_TEST_DIR
#error "ODIN3_ABC_TEST_DIR must name this test's output directory"
#endif

enum {
    AFX_MAX_PORTS = 8,
    AFX_PATH = 300,          /* one path under the test directory */
    AFX_JOIN = 2 * AFX_PATH, /* a path joined with a file name */
    AFX_LINES = 64,
    AFX_LINE = 1200,
    AFX_TEXT = 1 << 16
};

/* The captured log: level and text of each message, in order. */
typedef struct afx_log {
    char text[AFX_LINES][AFX_LINE];
    uint8_t level[AFX_LINES];
    uint32_t count;
} afx_log;

static afx_log afx_messages;

static inline void afx_sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (afx_messages.count < AFX_LINES) {
        (void)snprintf(afx_messages.text[afx_messages.count], AFX_LINE, "%s", msg);
        afx_messages.level[afx_messages.count] = (uint8_t)level;
    }
    afx_messages.count++;
}

static inline void afx_log_reset(void) {
    afx_messages.count = 0;
    odin3_log_set_sink(afx_sink, NULL);
}

/* How many captured messages of the level start with prefix ("" matches all). */
static inline uint32_t afx_count(odin3_log_level level, const char *prefix) {
    uint32_t found = 0;
    uint32_t shown = afx_messages.count < AFX_LINES ? afx_messages.count : AFX_LINES;
    for (uint32_t i = 0; i < shown; i++) {
        found += afx_messages.level[i] == level &&
                         strncmp(afx_messages.text[i], prefix, strlen(prefix)) == 0
                     ? 1U
                     : 0U;
    }
    return found;
}

/* The one message of the level that starts with prefix; fails the test unless there is one. */
static inline const char *afx_only(odin3_log_level level, const char *prefix) {
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, afx_count(level, prefix), prefix);
    uint32_t shown = afx_messages.count < AFX_LINES ? afx_messages.count : AFX_LINES;
    for (uint32_t i = 0; i < shown; i++) {
        if (afx_messages.level[i] == level &&
            strncmp(afx_messages.text[i], prefix, strlen(prefix)) == 0) {
            return afx_messages.text[i];
        }
    }
    return "";
}

/* True when some message of the level equals text exactly. */
static inline bool afx_has(odin3_log_level level, const char *text) {
    uint32_t shown = afx_messages.count < AFX_LINES ? afx_messages.count : AFX_LINES;
    for (uint32_t i = 0; i < shown; i++) {
        if (afx_messages.level[i] == level && strcmp(afx_messages.text[i], text) == 0) {
            return true;
        }
    }
    return false;
}

/* The error messages, one per line (for a failing assertion's message). */
static inline const char *afx_errors(void) {
    static char buf[AFX_TEXT];
    buf[0] = '\0';
    uint32_t shown = afx_messages.count < AFX_LINES ? afx_messages.count : AFX_LINES;
    for (uint32_t i = 0; i < shown; i++) {
        if (afx_messages.level[i] == ODIN3_LOG_ERROR) {
            size_t used = strlen(buf);
            (void)snprintf(buf + used, sizeof buf - used, "%s\n", afx_messages.text[i]);
        }
    }
    return buf;
}

/* --- files --------------------------------------------------------------------------------- */

static inline void afx_mkdir(const char *path) {
    TEST_ASSERT_TRUE_MESSAGE(mkdir(path, 0755) == 0 || errno == EEXIST, path);
}

/* ODIN3_ABC_TEST_DIR/name into path; the directory exists. */
static inline void afx_path(char *path, const char *name) {
    afx_mkdir(ODIN3_ABC_TEST_DIR);
    int len = snprintf(path, AFX_PATH, "%s/%s", ODIN3_ABC_TEST_DIR, name);
    TEST_ASSERT_TRUE(len > 0 && len < AFX_PATH);
}

static inline void afx_write_bytes(const char *path, odin3_bytes text) {
    FILE *file = fopen(path, "w");
    TEST_ASSERT_NOT_NULL_MESSAGE(file, path);
    TEST_ASSERT_EQUAL_UINT(text.len, fwrite(text.ptr, 1, text.len, file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

/* Writes the NUL-terminated text to the file at path. */
#define AFX_WRITE(path, text) afx_write_bytes((path), odin3_bytes_cstr(text))

/* The whole file into a static buffer (valid until the next call); "" for a missing file. */
static inline const char *afx_slurp(const char *path) {
    static char buf[AFX_TEXT];
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        buf[0] = '\0';
        return buf;
    }
    size_t len = fread(buf, 1, sizeof buf - 1, file);
    TEST_ASSERT_TRUE(len < sizeof buf - 1);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    buf[len] = '\0';
    return buf;
}

static inline bool afx_exists(const char *path) {
    struct stat info;
    return stat(path, &info) == 0;
}

/* --- designs ------------------------------------------------------------------------------- */

static inline uint32_t afx_intern(odin3_design *design, const char *text) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr(text), &str));
    return str;
}

static inline odin3_module *afx_top(odin3_design *design) {
    odin3_module *mod = odin3_module_get(design, odin3_design_top(design));
    TEST_ASSERT_NOT_NULL(mod);
    return mod;
}

static inline void afx_check(odin3_design *design) {
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NETLIST};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(design, opts));
}

/* The design as plain BLIF text (for "the IR is unchanged" assertions); malloc'd, freed by caller.
 */
static inline char *afx_blif_text(odin3_design *design) {
    char path[AFX_PATH];
    afx_path(path, "snapshot.blif");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_blif_write(design, path));
    return strdup(afx_slurp(path));
}

/* A scalar input or output port of mod; the port's net. */
static inline odin3_net_id afx_port(odin3_design *design, odin3_module *mod, const char *name,
                                    odin3_dir dir) {
    odin3_port_spec spec = {
        afx_intern(design, name), dir, 1, true, (odin3_prov_id){0}, false, 0, 0, false};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_add_port(mod, &spec, &node));
    return odin3_pin_net(mod, odin3_node_pins(mod, node).first);
}

static inline odin3_net_id afx_net(odin3_design *design, odin3_module *mod, const char *name) {
    odin3_net_id net = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_net_create(mod, name == NULL ? 0 : afx_intern(design, name),
                                           (odin3_prov_id){0}, &net));
    return net;
}

static inline odin3_celltype_id afx_type(odin3_design *design, const char *name) {
    odin3_celltype_id id = {0};
    TEST_ASSERT_TRUE_MESSAGE(odin3_celltype_find(design, afx_intern(design, name), &id), name);
    return id;
}

/* One cell to build: its type and name, the parameter values (NULL: the defaults), and one net
 * per port in port order (a scalar-port cell). */
typedef struct afx_cell_spec {
    const char *type;
    const char *name;
    const odin3_value *params;
    const odin3_net_id *nets;
    uint32_t n_ports;
} afx_cell_spec;

static inline odin3_node_id afx_make(odin3_design *design, odin3_module *mod,
                                     const afx_cell_spec *cell) {
    odin3_netvec ports[AFX_MAX_PORTS];
    TEST_ASSERT_TRUE(cell->n_ports <= AFX_MAX_PORTS);
    for (uint32_t i = 0; i < cell->n_ports; i++) {
        ports[i] = (odin3_netvec){&cell->nets[i], 1};
    }
    odin3_celltype_id type = afx_type(design, cell->type);
    odin3_node_spec spec = {type, cell->name == NULL ? 0 : afx_intern(design, cell->name),
                            (odin3_prov_id){0}, cell->params,
                            odin3_celltype_get(design, type)->n_params};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(mod, &spec, ports, &node));
    return node;
}

/* A bit-level cell with default parameters; nets are one per port, in port order. */
#define AFX_CELL(design, mod, type, name, nets, count)                                             \
    afx_make((design), (mod), &(afx_cell_spec){(type), (name), NULL, (nets), (count)})

/* The same with explicit parameter values. */
#define AFX_CELL_P(design, mod, type, params, nets, count)                                         \
    afx_make((design), (mod), &(afx_cell_spec){(type), NULL, (params), (nets), (count)})

/* A zero-input, zero-row $sop (constant 0) driving net: the `unconn` generator when net is so
 * named. */
static inline odin3_node_id afx_const_sop(odin3_design *design, odin3_module *mod,
                                          odin3_net_id net) {
    odin3_net_id none[1] = {{0}};
    odin3_netvec ports[2] = {{none, 0}, {&net, 1}};
    odin3_node_spec spec = {afx_type(design, "$sop"), 0, (odin3_prov_id){0}, NULL, 2};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(mod, &spec, ports, &node));
    return node;
}

/* An INT attribute (e.g. keep = 1) on a node. */
static inline void afx_keep_node(odin3_design *design, odin3_module *mod, odin3_node_id node) {
    odin3_value one = odin3_value_int(1);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_attr_set(mod, (odin3_objref){ODIN3_OBJ_NODE, node.v},
                                                   afx_intern(design, "keep"), &one));
}

#endif
