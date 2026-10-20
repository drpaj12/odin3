/*
 * test_il_celltypes.c — tools/opt-check/il_celltypes.txt against the cell registry and the order
 * walk (wreduce_order.c). il_order.py takes the pin order, the pin directions and the storage
 * types of the order walk from that file, so the two must agree: a registered type that is missing,
 * a port in another order or direction, a storage flag that differs, or a different walk order
 * fails here. `ODIN3_PRINT_CELL_TABLE=1 test_il_celltypes` prints the registered rows (the part of
 * the file this test checks), to regenerate it after a registry change.
 */
#include "ir/celltype.h"
#include "ir/design.h"
#include "passes/opt/wreduce.h"
#include "unity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { LINE_MAX_LEN = 1024, WORDS_MAX = 64, MAX_TYPES = 512 };

static const char *dir_code(odin3_dir dir) {
    if (dir == ODIN3_DIR_IN) {
        return "i";
    }
    return dir == ODIN3_DIR_OUT ? "o" : "io";
}

/* The row of def: `<type> seq|comb <port>:<i|o|io>... [walk=<port>,...]`. */
static void row_of(const odin3_celltype_def *def, char *out, size_t cap) {
    size_t at = (size_t)snprintf(out, cap, "%s %s", def->name,
                                 wr_order_is_source(def->name) ? "seq" : "comb");
    for (uint32_t k = 0; k < def->n_ports; k++) {
        at += (size_t)snprintf(out + at, cap - at, " %s:%s", def->ports[k].name,
                               dir_code(def->ports[k].dir));
    }
    bool walk = false;
    for (uint32_t k = 0; k < def->n_ports; k++) {
        walk = walk || wr_order_port(def->name, def->n_ports, k) != k;
    }
    for (uint32_t k = 0; walk && k < def->n_ports; k++) {
        const uint32_t port = wr_order_port(def->name, def->n_ports, k);
        at += (size_t)snprintf(out + at, cap - at, "%s%s", k == 0 ? " walk=" : ",",
                               def->ports[port].name);
    }
}

static const odin3_celltype_def *registered(odin3_design *design, uint32_t index) {
    return odin3_celltype_get(design, (odin3_celltype_id){index});
}

static bool is_registry_row(const char *line) {
    return line[0] != '#' && line[0] != '\0' && strncmp(line, "yosys ", 6) != 0;
}

static uint32_t read_rows(char rows[][LINE_MAX_LEN]) {
    FILE *file = fopen(IL_CELLTYPES_FILE, "r");
    TEST_ASSERT_NOT_NULL_MESSAGE(file, IL_CELLTYPES_FILE);
    uint32_t n_rows = 0;
    char line[LINE_MAX_LEN];
    while (fgets(line, sizeof line, file) != NULL) {
        line[strcspn(line, "\n")] = '\0';
        if (is_registry_row(line)) {
            TEST_ASSERT_TRUE(n_rows < MAX_TYPES);
            (void)snprintf(rows[n_rows++], LINE_MAX_LEN, "%s", line);
        }
    }
    (void)fclose(file);
    return n_rows;
}

static bool has_row(char rows[][LINE_MAX_LEN], uint32_t n_rows, const char *want) {
    for (uint32_t row = 0; row < n_rows; row++) {
        if (strcmp(rows[row], want) == 0) {
            return true;
        }
    }
    return false;
}

static char g_rows[MAX_TYPES][LINE_MAX_LEN];

static void test_table_matches_registry(void) {
    odin3_design *design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    const uint32_t n_rows = read_rows(g_rows);
    uint32_t n_types = 0;
    for (uint32_t id = 1; registered(design, id) != NULL; id++) {
        char want[LINE_MAX_LEN];
        row_of(registered(design, id), want, sizeof want);
        TEST_ASSERT_TRUE_MESSAGE(has_row(g_rows, n_rows, want), want);
        n_types++;
    }
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(n_types, n_rows, "a row for a type that is not registered");
    odin3_design_destroy(design);
}

static void print_table(void) {
    odin3_design *design = odin3_design_create();
    for (uint32_t id = 1; registered(design, id) != NULL; id++) {
        char row[LINE_MAX_LEN];
        row_of(registered(design, id), row, sizeof row);
        printf("%s\n", row);
    }
    odin3_design_destroy(design);
}

int main(void) {
    if (getenv("ODIN3_PRINT_CELL_TABLE") != NULL) {
        print_table();
        return 0;
    }
    UNITY_BEGIN();
    RUN_TEST(test_table_matches_registry);
    return UNITY_END();
}

void setUp(void) {
}
void tearDown(void) {
}
