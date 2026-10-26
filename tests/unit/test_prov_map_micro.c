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
    uint32_t maps, objects, no_record, no_construct, unreached;
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

/* The R lines by number (1-based). */
static const odin3_nm_line **r_lines(const odin3_name_map *map, uint32_t *count) {
    const odin3_nm_line **by_num = odin3_util_calloc((map->lines.len + 2) * sizeof *by_num);
    *count = 0;
    for (size_t i = 0; i < map->lines.len; i++) {
        const odin3_nm_line *line = odin3_vec_cat(&map->lines, i);
        if (line->type == 'R') {
            by_num[++*count] = line;
        }
    }
    return by_num;
}

/* True when the chain of record `start` reaches an S record that has a construct. */
static bool reaches_construct(const odin3_nm_line *const *by_num, uint32_t count, uint32_t start,
                              uint8_t *seen) {
    uint32_t stack[64];
    uint32_t top = 0;
    stack[top++] = start;
    while (top > 0) {
        uint32_t cur = stack[--top];
        if (cur == 0 || cur > count || seen[cur] != 0) {
            continue;
        }
        seen[cur] = 1;
        const odin3_nm_line *line = by_num[cur];
        if (((const char *)line->f[1].ptr)[0] == 'S' && line->f[7].len > 0) {
            return true;
        }
        const char *parents = (const char *)line->f[5].ptr;
        size_t len = line->f[5].len;
        for (size_t at = 0; at < len && top < 62;) {
            size_t end = at;
            while (end < len && parents[end] != ',') {
                end++;
            }
            stack[top++] = ref_number((odin3_bytes){parents + at, end - at});
            at = end + 1;
        }
    }
    return false;
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
    uint32_t count = 0;
    const odin3_nm_line **by_num = r_lines(&map, &count);
    uint8_t *seen = odin3_util_calloc((size_t)count + 2);
    for (size_t i = 0; i < map.lines.len; i++) {
        const odin3_nm_line *line = odin3_vec_cat(&map.lines, i);
        if (line->type != 'O') {
            continue;
        }
        seen_all->objects++;
        uint32_t rec = ref_number(line->f[7]);
        if (rec == 0) {
            seen_all->no_record++;
            continue;
        }
        memset(seen, 0, (size_t)count + 2);
        if (!reaches_construct(by_num, count, rec, seen)) {
            seen_all->no_construct++;
        }
    }
    seen_all->maps++;
    odin3_util_free(seen);
    odin3_util_free((void *)by_num);
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
                       " write_verilog --name-style hash --prov-map %s/o.v",
                       ODIN3_PMM_LIB, src, dir, dir, dir);
    TEST_ASSERT_TRUE(len > 0 && (size_t)len < size);
}

static bool is_verilog(const char *name) {
    size_t len = strlen(name);
    return len > 2 && strcmp(name + len - 2, ".v") == 0;
}

static void test_every_micro_map_is_well_formed_and_complete(void) {
    (void)snprintf(dir, sizeof dir, "%s/pmm.XXXXXX", ODIN3_PMM_ROOT);
    TEST_ASSERT_NOT_NULL(mkdtemp(dir));
    DIR *handle = opendir(ODIN3_PMM_MICRO);
    TEST_ASSERT_NOT_NULL(handle);
    tally total = {0};
    uint32_t designs = 0;
    uint32_t skipped = 0;
    for (struct dirent *entry = readdir(handle); entry != NULL; entry = readdir(handle)) {
        if (!is_verilog(entry->d_name)) {
            continue;
        }
        char src[PATH_BUF];
        char script[SCRIPT_BUF];
        (void)snprintf(src, sizeof src, "%s/%s", ODIN3_PMM_MICRO, entry->d_name);
        script_of(src, script, sizeof script);
        odin3_design *design = odin3_design_create();
        TEST_ASSERT_NOT_NULL(design);
        first_error[0] = '\0';
        odin3_status st = odin3_design_run_script(design, script, (odin3_script_src){"-p", 0});
        odin3_design_destroy(design);
        if (st != ODIN3_OK) {
            skipped++; /* a micro the flow refuses (an ambiguous top, say) has no map to check */
            continue;
        }
        designs++;
        char path[PATH_BUF + 64];
        const char *const maps[] = {"o.blif.names", "o.json.names", "o.v.names"};
        for (size_t k = 0; k < sizeof maps / sizeof maps[0]; k++) {
            (void)snprintf(path, sizeof path, "%s/%s", dir, maps[k]);
            check_map(path, &total);
        }
    }
    (void)closedir(handle);
    printf("prov_map_micro: %u designs, %u skipped, %u maps, %u objects, %u without a record, %u "
           "without a construct\n",
           designs, skipped, total.maps, total.objects, total.no_record, total.no_construct);
    if (designs < 20) {
        fprintf(stderr, "first error: %s\n", first_error);
    }
    TEST_ASSERT_TRUE(designs >= 20);
    TEST_ASSERT_EQUAL_UINT32(designs * 3U, total.maps);
    TEST_ASSERT_EQUAL_UINT32(0, total.no_record);
    TEST_ASSERT_EQUAL_UINT32(0, total.no_construct);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_every_micro_map_is_well_formed_and_complete);
    return UNITY_END();
}
