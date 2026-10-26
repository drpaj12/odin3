/*
 * test_prov_map_micro.c — a version 2 name map for the micro set is well formed and complete
 * (V1.2.0 Task 5). Each micro design runs the VTR flow with `--prov-ast refs`, then is written by
 * the three hash writers with `--prov-map`; every map is parsed with the name_map.c parse and
 * checked (odin3_name_map_check_v2: every reference resolves, parents before children, a class on
 * every record), and complete: each O line has a record whose chain reaches an S record with a
 * construct. `names --check` has no version 2 mode yet (Task 6), so this test is the check.
 * Work files go to a fresh mkdtemp directory under the build tree, which is left in place.
 */
#include "backends/common/name_map.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ODIN3_PMM_ROOT
#error "ODIN3_PMM_ROOT must name the build tree"
#endif
#ifndef ODIN3_PMM_MICRO
#error "ODIN3_PMM_MICRO must name the micro design directory"
#endif
#ifndef ODIN3_PMM_LIB
#error "ODIN3_PMM_LIB must name the lib directory"
#endif

enum { PATH_BUF = 1024, SCRIPT_BUF = 4096, MAX_PARENTS = 1 << 20, SKIP_BIG_BYTES = 40000 };

static char dir[PATH_BUF];
static char first_error[SCRIPT_BUF];

static void sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (level <= ODIN3_LOG_ERROR && first_error[0] == '\0') {
        (void)snprintf(first_error, sizeof first_error, "%s", msg);
    }
}

void setUp(void) {
    first_error[0] = '\0';
    odin3_log_set_sink(sink, NULL);
}

void tearDown(void) {
    odin3_log_set_sink(NULL, NULL);
}

static char *slurp(const char *path) {
    FILE *file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(file, path);
    char *text = odin3_util_calloc(1);
    size_t len = 0;
    char chunk[4096];
    for (size_t got = fread(chunk, 1, sizeof chunk, file); got > 0;
         got = fread(chunk, 1, sizeof chunk, file)) {
        text = odin3_util_realloc(text, len + got + 1);
        memcpy(text + len, chunk, got);
        len += got;
        text[len] = '\0';
    }
    (void)fclose(file);
    return text;
}

/* What one map showed. */
typedef struct tally {
    uint32_t designs, skipped, maps, objects, no_record, no_construct;
    uint32_t kept_lines, refine_records, refine_joins, byte_diffs, netlists;
} tally;

/* The number in `r12` (or the first of a list), 0 when not a record reference. */
static uint32_t ref_number(odin3_bytes text) {
    const char *bytes = (const char *)text.ptr;
    uint32_t num = 0;
    if (text.len < 2 || bytes[0] != 'r') {
        return 0;
    }
    for (size_t i = 1; i < text.len && bytes[i] >= '0' && bytes[i] <= '9'; i++) {
        num = num * 10U + (uint32_t)(bytes[i] - '0');
    }
    return num;
}

/* One R line, so that a vec of them has a struct element type. */
typedef struct rline {
    const odin3_nm_line *line;
} rline;

/* The R lines of a map by number (index = number - 1) and the marks of one walk. */
typedef struct rtable {
    odin3_vec lines; /* rline */
    odin3_vec seen;  /* uint8_t per line */
} rtable;

static void rtable_add(rtable *table, const odin3_nm_line *line) {
    if (line->type != 'R') {
        return;
    }
    rline *slot = odin3_vec_push(&table->lines);
    TEST_ASSERT_NOT_NULL(slot);
    slot->line = line;
    TEST_ASSERT_NOT_NULL(odin3_vec_push(&table->seen));
}

static void rtable_make(rtable *table, const odin3_name_map *map) {
    odin3_vec_init(&table->lines, sizeof(rline));
    odin3_vec_init(&table->seen, sizeof(uint8_t));
    for (size_t i = 0; i < map->lines.len; i++) {
        rtable_add(table, odin3_vec_cat(&map->lines, i));
    }
}

static void rtable_free(rtable *table) {
    odin3_vec_free(&table->lines);
    odin3_vec_free(&table->seen);
}

/* Pushes the parents of an R line (its comma list) on the stack. */
static void push_parents(odin3_vec *stack, const odin3_nm_line *line) {
    const char *parents = (const char *)line->f[5].ptr;
    size_t len = line->f[5].len;
    for (size_t at = 0; at < len;) {
        size_t end = at;
        while (end < len && parents[end] != ',') {
            end++;
        }
        uint32_t *slot = odin3_vec_push(stack);
        TEST_ASSERT_NOT_NULL(slot);
        *slot = ref_number((odin3_bytes){parents + at, end - at});
        at = end + 1;
    }
}

/* True when the chain of record number `start` reaches an S record that has a construct. */
static bool reaches_construct(rtable *table, uint32_t start) {
    odin3_vec stack;
    odin3_vec_init(&stack, sizeof(uint32_t));
    memset(table->seen.data, 0, table->seen.len);
    *(uint32_t *)odin3_vec_push(&stack) = start;
    bool found = false;
    while (!found && stack.len > 0) {
        uint32_t cur = *(const uint32_t *)odin3_vec_cat(&stack, stack.len - 1U);
        odin3_vec_pop(&stack);
        uint8_t *mark =
            cur >= 1 && cur <= table->lines.len ? odin3_vec_at(&table->seen, cur - 1U) : NULL;
        if (mark == NULL || *mark != 0) {
            continue;
        }
        *mark = 1;
        const odin3_nm_line *line = ((const rline *)odin3_vec_cat(&table->lines, cur - 1U))->line;
        found = ((const char *)line->f[1].ptr)[0] == 'S' && line->f[7].len > 0;
        if (!found) {
            push_parents(&stack, line);
        }
    }
    odin3_vec_free(&stack);
    return found;
}

static void check_map(const char *path, tally *seen_all) {
    char *text = slurp(path);
    odin3_name_map map;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_name_map_init(&map));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK,
                                  odin3_name_map_parse(&map, odin3_bytes_cstr(text), path), path);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, map.ignored, path);
    TEST_ASSERT_EQUAL_UINT32(2, map.version);
    odin3_name_check_v2 check = odin3_name_map_check_v2(&map);
    TEST_ASSERT_TRUE_MESSAGE(odin3_name_check_v2_ok(&check), path);
    rtable table;
    rtable_make(&table, &map);
    for (size_t i = 0; i < map.lines.len; i++) {
        const odin3_nm_line *line = odin3_vec_cat(&map.lines, i);
        seen_all->kept_lines += line->type == 'N' ? 1U : 0U;
        if (line->type == 'R' && line->f[3].len > 7 && memcmp(line->f[3].ptr, "refine:", 7) == 0) {
            seen_all->refine_records++; /* the proc mux (a refine record) is never a join */
            seen_all->refine_joins += line->f[8].len >= 4 && memcmp(line->f[8].ptr, "join", 4) == 0;
        }
        if (line->type != 'O') {
            continue;
        }
        seen_all->objects++;
        uint32_t rec = ref_number(line->f[7]);
        seen_all->no_record += rec == 0 ? 1U : 0U;
        seen_all->no_construct += rec != 0 && !reaches_construct(&table, rec) ? 1U : 0U;
    }
    seen_all->maps++;
    rtable_free(&table);
    odin3_name_map_free(&map);
    odin3_util_free(text);
}

/* The script: the VTR flow to gates, then the three writers with hash names and --prov-map. */
static void script_of(const char *src, char *out, size_t size) {
    int len = snprintf(out, size,
                       "read_techlib %s/vtr.o3lib; read_verilog --nolatches --vtr-compat "
                       "--prov-ast refs %s; proc; opt; flatten; opt -full; wreduce; hardmap; lower;"
#ifdef ODIN3_PMM_ABC
                       " abc --lut 6;"
#endif
                       " write_blif --vtr --name-style hash --prov-map %s/o.blif;"
                       " write_json --name-style hash --prov-map %s/o.json;"
                       " write_verilog --name-style hash --prov-map %s/o.v;"
                       " write_blif --vtr --name-style hash %s/p.blif;"
                       " write_json --name-style hash %s/p.json;"
                       " write_verilog --name-style hash %s/p.v",
                       ODIN3_PMM_LIB, src, dir, dir, dir, dir, dir, dir);
    TEST_ASSERT_TRUE(len > 0 && (size_t)len < size);
}

static bool is_verilog(const char *name) {
    size_t len = strlen(name);
    return len > 2 && strcmp(name + len - 2, ".v") == 0;
}

static void compare_netlists(tally *total);

/* Runs the script of one micro and checks the three maps it wrote; false when the flow refused. */
static bool check_one_micro(const char *name, tally *total) {
    char src[PATH_BUF];
    char script[SCRIPT_BUF];
    (void)snprintf(src, sizeof src, "%s/%s", ODIN3_PMM_MICRO, name);
    script_of(src, script, sizeof script);
    odin3_design *design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    first_error[0] = '\0';
    odin3_status st = odin3_design_run_script(design, script, (odin3_script_src){"-p", 0});
    odin3_design_destroy(design);
    if (st != ODIN3_OK) {
        return false; /* a micro the flow refuses (an ambiguous top, say) has no map to check */
    }
    const char *const maps[] = {"o.blif.names", "o.json.names", "o.v.names"};
    for (size_t k = 0; k < sizeof maps / sizeof maps[0]; k++) {
        char path[PATH_BUF + 64];
        (void)snprintf(path, sizeof path, "%s/%s", dir, maps[k]);
        check_map(path, total);
    }
    compare_netlists(total);
    return true;
}

/* The netlist written with --prov-map equals the one written without it, byte for byte. */
static void compare_netlists(tally *total) {
    const char *const pairs[][2] = {{"o.blif", "p.blif"}, {"o.json", "p.json"}, {"o.v", "p.v"}};
    for (size_t k = 0; k < sizeof pairs / sizeof pairs[0]; k++) {
        char left[PATH_BUF + 64];
        char right[PATH_BUF + 64];
        (void)snprintf(left, sizeof left, "%s/%s", dir, pairs[k][0]);
        (void)snprintf(right, sizeof right, "%s/%s", dir, pairs[k][1]);
        char *one = slurp(left);
        char *two = slurp(right);
        total->byte_diffs += strcmp(one, two) != 0 ? 1U : 0U;
        total->netlists++;
        odin3_util_free(one);
        odin3_util_free(two);
    }
}

static void report_and_expect(const tally *total) {
    (void)printf("prov_map_micro: %u designs, %u skipped, %u maps, %u objects, %u without a "
                 "record, %u without a construct\n",
                 total->designs, total->skipped, total->maps, total->objects, total->no_record,
                 total->no_construct);
    (void)printf("prov_map_micro: %u N lines, %u refine records (%u with join), %u netlists, %u "
                 "differ with and without --prov-map\n",
                 total->kept_lines, total->refine_records, total->refine_joins, total->netlists,
                 total->byte_diffs);
    TEST_ASSERT_TRUE(total->designs >= 20);
    TEST_ASSERT_EQUAL_UINT32(total->designs * 3U, total->maps);
    TEST_ASSERT_EQUAL_UINT32(0, total->no_record);
    TEST_ASSERT_EQUAL_UINT32(0, total->no_construct);
    TEST_ASSERT_EQUAL_UINT32(total->designs * 3U, total->netlists);
    TEST_ASSERT_EQUAL_UINT32(0, total->byte_diffs);
    TEST_ASSERT_TRUE(total->kept_lines > 0);     /* N lines from all three writers' maps */
    TEST_ASSERT_TRUE(total->refine_records > 0); /* a real proc mux among the micros */
    TEST_ASSERT_EQUAL_UINT32(0, total->refine_joins);
}

static void test_every_micro_map_is_well_formed_and_complete(void) {
    (void)snprintf(dir, sizeof dir, "%s/pmm.XXXXXX", ODIN3_PMM_ROOT);
    TEST_ASSERT_NOT_NULL(mkdtemp(dir));
    DIR *handle = opendir(ODIN3_PMM_MICRO);
    TEST_ASSERT_NOT_NULL(handle);
    tally total = {0};
    for (struct dirent *entry = readdir(handle); entry != NULL; entry = readdir(handle)) {
        if (!is_verilog(entry->d_name)) {
            continue;
        }
        bool ok = check_one_micro(entry->d_name, &total);
        total.designs += ok ? 1U : 0U;
        total.skipped += ok ? 0U : 1U;
    }
    (void)closedir(handle);
    report_and_expect(&total);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_every_micro_map_is_well_formed_and_complete);
    return UNITY_END();
}
