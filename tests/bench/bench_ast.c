/*
 * bench_ast.c — source manager and AST benchmark (spec §11): location decode and segment-cursor
 * throughput on mcml.v, the 200k-node mcml memory estimate, and a 2M-node AST (build, walk, cursor,
 * check FULL with and without locations, destroy). Run it from a Release build; smallest first.
 * Usage: bench_ast [path-to-mcml.v]   (default $VTR_ROOT/vtr_flow/benchmarks/verilog/mcml.v)
 *
 * bench_ast --parse <file>... (2C spec §7.6, plan Task 10): each file, smallest first, in a child
 * process of its own (so the peak RSS is the file's): the 2B preprocessor and the 2C parser on a
 * fresh design, timed separately, then check FULL; then `read_verilog --parse-only`'s reader
 * (odin3_read_verilog_files, steps 1-7) on another fresh design, timed end to end. One line per
 * file (bytes, nodes, comments, odin3_ast_bytes_reserved, odin3_srcman_bytes_reserved, strtab
 * growth, the times, peak RSS), then the targets: mcml.v AST + srcman <= 20 MB and parse-only
 * <= 1 s, and the files given in <= 5 s in total (read_verilog end to end; the target is
 * for the .v files of $VTR_ROOT/vtr_flow/benchmarks/verilog; koios times are reported only,
 * point (n)).
 */
#include "ast/ast.h"
#include "ast/ast_internal.h"
#include "ast/check.h"
#include "ast/srcman.h"
#include "frontends/project/paths.h"
#include "frontends/verilog/parser.h"
#include "frontends/verilog/preproc.h"
#include "frontends/verilog/read_verilog.h"
#include "ir/design.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum {
    MCML_BYTES = 650837,
    MCML_LINES = 24507,
    RATE_OPS = 10000000,
    SEG_BYTES = 64,
    EST_NODES = 200000,
    EST_COMMENTS = 20000,
    EST_BUDGET_BYTES = 20 * 1000 * 1000,
    BIG_NODES = 2000000,
    BIG_LINE_BYTES = 40,
    NAME_POOL = 4096,
    CLUSTERS_PER_MODULE = 5000,
    LEAVES = 5
};

/* The brief's targets. */
static const double RATE_TARGET = 10e6;
static const double BUILD_TARGET = 1.0;
static const double WALK_TARGET = 0.1;
static const double CHECK_TARGET = 0.3;
static const double BYTES_PER_NODE_TARGET = 40.0;

typedef struct bench {
    odin3_design *design;
    odin3_ast *ast;
    odin3_srcman *sm;
    uint64_t lcg;
    uint32_t file_start; /* loc of offset 0 of the synthetic FILE buffer; 0 = no locations */
    uint32_t file_len;
    uint32_t step;
    uint32_t leaves;
    uint32_t names[NAME_POOL];
    uint32_t counts[ODIN3_AST_KIND_COUNT];
} bench;

typedef struct nspec {
    uint8_t kind;
    uint8_t sub;
    uint16_t flags;
    uint32_t name;
    uint32_t payload;
} nspec;

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static long max_rss_kib(void) {
    struct rusage usage;
    return getrusage(RUSAGE_SELF, &usage) == 0 ? usage.ru_maxrss : -1;
}

static const char *verdict(bool ok) {
    return ok ? "ok" : "MISSED";
}

/* Knuth MMIX LCG, high bits; deterministic for a fixed seed. */
static uint32_t next_rand(bench *b, uint32_t bound) {
    b->lcg = b->lcg * 6364136223846793005ULL + 1442695040888963407ULL;
    return (uint32_t)((b->lcg >> 33) % bound);
}

/* --- (1) source manager throughput on mcml.v ------------------------------------------------ */

static unsigned char *read_file(const char *path, size_t *len) {
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return NULL;
    }
    unsigned char *buf = odin3_util_malloc(MCML_BYTES * 2);
    *len = buf != NULL ? fread(buf, 1, MCML_BYTES * 2, fp) : 0;
    (void)fclose(fp);
    return buf;
}

static int add_file_lines(bench *b, const unsigned char *text, size_t len, odin3_srcbuf_id *buf) {
    odin3_srcfile_spec spec = {.len = (uint32_t)len};
    if (odin3_ast_intern(b->ast, (odin3_bytes){"mcml.v", 6}, &spec.name) != ODIN3_OK ||
        odin3_srcman_add_file(b->sm, &spec, buf) != ODIN3_OK) {
        return 1;
    }
    for (size_t i = 0; i < len; i++) {
        if (text[i] == '\n' && odin3_srcman_add_line(b->sm, *buf, (uint32_t)i + 1) != ODIN3_OK) {
            return 1;
        }
    }
    return 0;
}

static void rate_line(const char *what, double secs) {
    double rate = RATE_OPS / secs;
    printf("%-34s: %9.3f s  %8.1f M/s  (target >= 10 M/s: %s)\n", what, secs, rate / 1e6,
           verdict(rate >= RATE_TARGET));
}

static int bench_decode(bench *b, odin3_srcbuf_id buf, uint32_t len) {
    odin3_srcbuf_info info;
    if (!odin3_srcman_buffer_info(b->sm, buf, &info)) {
        return 1;
    }
    uint64_t sum = 0;
    double start = now_seconds();
    for (uint32_t i = 0; i < RATE_OPS; i++) {
        odin3_srcpos pos;
        if (odin3_srcman_decode(b->sm, (odin3_loc){info.start + next_rand(b, len + 1)}, &pos)) {
            sum += pos.line + pos.col;
        }
    }
    rate_line("mcml.v decode (random locs)", now_seconds() - start);
    return sum == 0;
}

static int bench_cursor(bench *b, odin3_srcbuf_id buf, uint32_t len) {
    odin3_srcbuf_info info;
    if (!odin3_srcman_buffer_info(b->sm, buf, &info)) {
        return 1;
    }
    for (uint32_t off = 0; off < len; off += SEG_BYTES) {
        odin3_segment seg = {.stream = buf.v, .out_offset = off, .loc = info.start + off};
        if (odin3_srcman_add_segment(b->sm, &seg) != ODIN3_OK) {
            return 1;
        }
    }
    odin3_srcman_cursor cur;
    odin3_srcman_cursor_init(&cur, b->sm, buf.v);
    uint64_t sum = 0;
    double start = now_seconds();
    for (uint32_t i = 0; i < RATE_OPS; i++) {
        sum += odin3_srcman_cursor_loc(&cur, i % len).v;
    }
    rate_line("mcml.v segment cursor (sequential)", now_seconds() - start);
    return sum == 0;
}

static int bench_mcml(bench *b, const char *path) {
    size_t len = 0;
    unsigned char *text = read_file(path, &len);
    if (text == NULL) {
        printf("mcml.v                            : %s not found, skipped\n", path);
        return 0;
    }
    odin3_srcbuf_id buf = {0};
    int rc = add_file_lines(b, text, len, &buf);
    odin3_util_free(text);
    if (rc != 0) {
        return 1;
    }
    odin3_srcbuf_info info;
    (void)odin3_srcman_buffer_info(b->sm, buf, &info);
    printf("mcml.v buffer                     : %zu bytes (expected %d), %" PRIu32
           " lines (expected %d), line map + buffer table %zu bytes\n",
           len, MCML_BYTES, info.lines, MCML_LINES, odin3_srcman_bytes_reserved(b->sm));
    return bench_decode(b, buf, (uint32_t)len) | bench_cursor(b, buf, (uint32_t)len);
}

/* --- synthetic AST ------------------------------------------------------------------------- */

static int open_design(bench *b) {
    b->lcg = 0x2545F4914F6CDD1DULL;
    b->design = odin3_design_create();
    if (b->design == NULL || odin3_design_get_srcman(b->design, &b->sm) != ODIN3_OK ||
        odin3_ast_create(b->design, ODIN3_AST_FORM_PARSED, (odin3_passrun_id){1}, &b->ast) !=
            ODIN3_OK) {
        return 1;
    }
    char text[32];
    for (uint32_t i = 0; i < NAME_POOL; i++) {
        int n = snprintf(text, sizeof text, "n%" PRIu32, i);
        if (odin3_ast_intern(b->ast, (odin3_bytes){text, (size_t)n}, &b->names[i]) != ODIN3_OK) {
            return 1;
        }
    }
    return 0;
}

static void close_design(bench *b) {
    odin3_ast_destroy(b->ast);
    b->ast = NULL;
    odin3_design_destroy(b->design);
    b->design = NULL;
    b->sm = NULL;
}

/* A FILE buffer of len bytes for the nodes' locations, with a line every line_bytes. */
static int add_locs(bench *b, uint32_t len, uint32_t line_bytes, uint32_t nodes) {
    odin3_srcfile_spec spec = {.len = len};
    odin3_srcbuf_id buf = {0};
    if (odin3_ast_intern(b->ast, (odin3_bytes){"bench.v", 7}, &spec.name) != ODIN3_OK ||
        odin3_srcman_add_file(b->sm, &spec, &buf) != ODIN3_OK) {
        return 1;
    }
    for (uint32_t off = line_bytes; off < len; off += line_bytes) {
        if (odin3_srcman_add_line(b->sm, buf, off) != ODIN3_OK) {
            return 1;
        }
    }
    b->file_start = odin3_srcman_loc(b->sm, buf, 0).v;
    b->file_len = len;
    b->step = len / nodes > 0 ? len / nodes : 1;
    return 0;
}

/* The loc range of a new node: a leaf takes the next slice; a parent spans its children. */
static void range_of(bench *b, const odin3_ast_id *kids, uint32_t n, odin3_ast_spec *spec) {
    if (b->file_start == 0) {
        return;
    }
    uint32_t lo = UINT32_MAX;
    uint32_t hi = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (kids[i].v != 0) {
            uint32_t l = odin3_ast_loc(b->ast, kids[i]).v;
            uint32_t e = odin3_ast_end(b->ast, kids[i]).v;
            lo = l < lo ? l : lo;
            hi = e > hi ? e : hi;
        }
    }
    if (hi == 0) {
        uint32_t off = b->leaves * b->step;
        off = off + b->step < b->file_len ? off : b->file_len - b->step;
        lo = b->file_start + off;
        hi = lo + b->step;
        b->leaves++;
    }
    spec->loc = lo;
    spec->end = hi;
}

static int emit(bench *b, nspec ns, const odin3_ast_id *kids, uint32_t n, odin3_ast_id *out) {
    odin3_ast_spec spec = {
        .kind = ns.kind, .sub = ns.sub, .flags = ns.flags, .name = ns.name, .payload = ns.payload};
    range_of(b, kids, n, &spec);
    b->counts[ns.kind]++;
    return odin3_ast_make(b->ast, &spec, kids, n, out) != ODIN3_OK;
}

static int leaf_ident(bench *b, odin3_ast_id *out) {
    return emit(b, (nspec){.kind = ODIN3_AST_IDENT, .name = b->names[next_rand(b, NAME_POOL)]},
                NULL, 0, out);
}

static int leaf_number(bench *b, odin3_ast_id *out) {
    char digits[8];
    int n = snprintf(digits, sizeof digits, "%u", (unsigned)next_rand(b, 1000));
    odin3_number_text text = {{"", 0}, {"", 0}, {digits, (size_t)n}};
    nspec ns = {.kind = ODIN3_AST_NUMBER};
    if (odin3_ast_number_kind(text, &ns.sub, &ns.flags) != ODIN3_OK ||
        odin3_ast_number_parse(b->ast, (odin3_loc){0}, text, &ns.payload) != ODIN3_OK) {
        return 1;
    }
    return emit(b, ns, NULL, 0, out);
}

static int leaf_select(bench *b, odin3_ast_id *out) {
    odin3_ast_id kids[3] = {{0}, {0}, {0}};
    if (leaf_ident(b, &kids[0]) != 0 || leaf_number(b, &kids[1]) != 0) {
        return 1;
    }
    return emit(b, (nspec){.kind = ODIN3_AST_SELECT, .sub = ODIN3_AST_SELECT_BIT}, kids, 3, out);
}

/* Leaf i of a cluster's expression: two selects, then identifiers and a number every other time. */
static int leaf_at(bench *b, uint32_t i, uint32_t cluster, odin3_ast_id *out) {
    if (i < 2) {
        return leaf_select(b, out);
    }
    if (i == 3 && (cluster & 1U) != 0) {
        return leaf_number(b, out);
    }
    return leaf_ident(b, out);
}

/* rhs = ((((l0 op l1) op l2) op l3) op l4): iterative, left-deep. */
static int make_rhs(bench *b, uint32_t cluster, odin3_ast_id *out) {
    odin3_ast_id acc = {0};
    if (leaf_at(b, 0, cluster, &acc) != 0) {
        return 1;
    }
    for (uint32_t i = 1; i < LEAVES; i++) {
        odin3_ast_id kids[2] = {acc, {0}};
        nspec op = {.kind = ODIN3_AST_BINARY, .sub = (uint8_t)(1 + next_rand(b, 3))};
        if (leaf_at(b, i, cluster, &kids[1]) != 0 || emit(b, op, kids, 2, &acc) != 0) {
            return 1;
        }
    }
    *out = acc;
    return 0;
}

/* always [ if (c) y <= rhs; ] */
static int make_cluster(bench *b, uint32_t cluster, odin3_ast_id *out) {
    odin3_ast_id cond = {0};
    odin3_ast_id nba[3] = {{0}, {0}, {0}};
    odin3_ast_id iff[3] = {{0}, {0}, {0}};
    if (leaf_ident(b, &cond) != 0 || leaf_ident(b, &nba[0]) != 0 ||
        make_rhs(b, cluster, &nba[2]) != 0 ||
        emit(b, (nspec){.kind = ODIN3_AST_NONBLOCKING_ASSIGN}, nba, 3, &iff[1]) != 0) {
        return 1;
    }
    iff[0] = cond;
    odin3_ast_id stmt = {0};
    return emit(b, (nspec){.kind = ODIN3_AST_IF}, iff, 3, &stmt) != 0 ||
           emit(b, (nspec){.kind = ODIN3_AST_ALWAYS, .sub = ODIN3_AST_ALWAYS_ALWAYS}, &stmt, 1,
                out) != 0;
}

/* A MODULE of up to CLUSTERS_PER_MODULE clusters, pushed on the pending stack. */
static int make_module(bench *b, uint32_t *cluster, uint32_t target) {
    uint32_t mark = odin3_ast_mark(b->ast);
    odin3_ast_id none = {0};
    odin3_ast_id item = {0};
    odin3_ast_id mod = {0};
    if (odin3_ast_push(b->ast, none) != ODIN3_OK || odin3_ast_push(b->ast, none) != ODIN3_OK) {
        return 1;
    }
    for (uint32_t i = 0; i < CLUSTERS_PER_MODULE && odin3_ast_node_end(b->ast).v <= target; i++) {
        if (make_cluster(b, (*cluster)++, &item) != 0 || odin3_ast_push(b->ast, item) != ODIN3_OK) {
            return 1;
        }
    }
    odin3_ast_spec spec = {.kind = ODIN3_AST_MODULE, .name = b->names[0]};
    range_of(b, NULL, 0, &spec);
    b->counts[ODIN3_AST_MODULE]++;
    if (odin3_ast_make_marked(b->ast, &spec, mark, &mod) != ODIN3_OK) {
        return 1;
    }
    return odin3_ast_push(b->ast, mod) != ODIN3_OK;
}

/* Builds the tree to about target nodes and seals it. */
static int build_tree(bench *b, uint32_t target) {
    uint32_t mark = odin3_ast_mark(b->ast);
    uint32_t cluster = 0;
    while (odin3_ast_node_end(b->ast).v <= target) {
        if (make_module(b, &cluster, target) != 0) {
            return 1;
        }
    }
    odin3_ast_spec spec = {.kind = ODIN3_AST_UNIT, .name = b->names[1]};
    odin3_ast_id unit = {0};
    b->counts[ODIN3_AST_UNIT]++;
    return odin3_ast_make_marked(b->ast, &spec, mark, &unit) != ODIN3_OK ||
           odin3_ast_finish(b->ast) != ODIN3_OK;
}

static uint32_t node_count(const bench *b) {
    return odin3_ast_node_end(b->ast).v - 1;
}

static void print_mix(const bench *b) {
    double total = (double)node_count(b);
    uint32_t other = node_count(b);
    const uint32_t shown[4] = {ODIN3_AST_IDENT, ODIN3_AST_BINARY, ODIN3_AST_NUMBER,
                               ODIN3_AST_SELECT};
    const char *names[4] = {"IDENT", "BINARY", "NUMBER", "SELECT"};
    printf("mix                               :");
    for (uint32_t i = 0; i < 4; i++) {
        printf(" %s %.1f%%", names[i], 100.0 * b->counts[shown[i]] / total);
        other -= b->counts[shown[i]];
    }
    printf(" other %.1f%%\n", 100.0 * other / total);
}

/* --- (2) mcml-size estimate ---------------------------------------------------------------- */

static int add_comments(bench *b) {
    for (uint32_t i = 0; i < EST_COMMENTS; i++) {
        uint32_t off = next_rand(b, b->file_len - 40);
        odin3_ast_comment_spec spec = {.loc = b->file_start + off,
                                       .end = b->file_start + off + 30,
                                       .kind = ODIN3_AST_COMMENT_LINE,
                                       .text_ptr = "// a typical comment line",
                                       .text_len = 25};
        if (odin3_ast_comment_add(b->ast, &spec) != ODIN3_OK) {
            return 1;
        }
    }
    return 0;
}

static int bench_estimate(void) {
    bench *b = odin3_util_calloc(sizeof *b);
    if (b == NULL || open_design(b) != 0 || add_locs(b, MCML_BYTES, 27, EST_NODES) != 0 ||
        add_comments(b) != 0 || build_tree(b, EST_NODES) != 0) {
        odin3_util_free(b);
        return 1;
    }
    size_t ast_bytes = odin3_ast_bytes_reserved(b->ast);
    size_t sm_bytes = odin3_srcman_bytes_reserved(b->sm);
    printf("estimate (200k nodes, 20k comments): %" PRIu32
           " nodes, AST+child table+payloads+comments"
           " %zu B, srcman %zu B, total %zu B (budget 20,000,000: %s) -- an estimate until 2C's"
           " --parse-only run\n",
           node_count(b), ast_bytes, sm_bytes, ast_bytes + sm_bytes,
           verdict(ast_bytes + sm_bytes <= EST_BUDGET_BYTES));
    close_design(b);
    odin3_util_free(b);
    return 0;
}

/* --- (3) the 2M-node AST ------------------------------------------------------------------- */

static odin3_ast_walk_action count_pre(const odin3_ast *ast, odin3_ast_id node, void *user) {
    (void)ast;
    (void)node;
    (*(uint64_t *)user)++;
    return ODIN3_AST_WALK_CONTINUE;
}

static int time_walk(const bench *b) {
    uint64_t visited = 0;
    double start = now_seconds();
    odin3_status st = odin3_ast_walk(b->ast, odin3_ast_root(b->ast, 0), count_pre, NULL, &visited);
    double secs = now_seconds() - start;
    printf("walk                              : %9.3f s  %" PRIu64
           " nodes (target <= 0.100 s: %s)\n",
           secs, visited, verdict(secs <= WALK_TARGET));
    return st != ODIN3_OK;
}

static int time_cursor(const bench *b) {
    odin3_ast_cursor cur;
    odin3_ast_event ev;
    odin3_ast_id node;
    uint64_t events = 0;
    if (odin3_ast_cursor_init(&cur, b->ast, odin3_ast_root(b->ast, 0)) != ODIN3_OK) {
        return 1;
    }
    double start = now_seconds();
    while (odin3_ast_cursor_next(&cur, &ev, &node)) {
        events++;
    }
    printf("cursor                            : %9.3f s  %" PRIu64 " events\n",
           now_seconds() - start, events);
    odin3_ast_cursor_free(&cur);
    return 0;
}

static double time_check(const bench *b, const char *what, odin3_check_level level) {
    double start = now_seconds();
    odin3_status st = odin3_ast_check(b->ast, level);
    double secs = now_seconds() - start;
    printf("%-34s: %9.3f s  status %d\n", what, secs, (int)st);
    return st == ODIN3_OK ? secs : -1.0;
}

/* Where the bytes go: reserved (vec capacity, doubling) against used. */
static void print_breakdown(const bench *b) {
    const odin3_ast *a = b->ast;
    double n = node_count(b);
    size_t nodes = odin3_pagevec_bytes_reserved(a->nodes);
    size_t arena = odin3_arena_bytes_reserved(a->arena);
    size_t used_kids = a->children.len * a->children.elem_size;
    size_t used_pay = a->payloads.len * a->payloads.elem_size;
    printf("breakdown (reserved/used B)       : nodes %zu; child table %zu/%zu (%zu entries); "
           "payload recs %zu/%zu (%zu); arena %zu\n",
           nodes, a->children.cap * a->children.elem_size, used_kids, a->children.len,
           a->payloads.cap * a->payloads.elem_size, used_pay, a->payloads.len, arena);
    printf("bytes per node, used              : %9.2f  (nodes + child entries + payload recs + "
           "arena reserved)\n",
           (double)(nodes + used_kids + used_pay + arena) / n);
}

static int build_big(bench *b, bool locs) {
    double start = now_seconds();
    if (open_design(b) != 0 ||
        (locs && add_locs(b, BIG_NODES * 4, BIG_LINE_BYTES, BIG_NODES) != 0) ||
        build_tree(b, BIG_NODES) != 0) {
        return 1;
    }
    double secs = now_seconds() - start;
    size_t total = odin3_ast_bytes_reserved(b->ast);
    char label[48];
    (void)snprintf(label, sizeof label, "build (%s)", locs ? "with locs" : "no locs");
    printf("%-34s: %9.3f s  %" PRIu32 " nodes (target <= 1.000 s: %s)\n", label, secs,
           node_count(b), verdict(secs <= BUILD_TARGET));
    (void)snprintf(label, sizeof label, "bytes per node (%s)", locs ? "with locs" : "no locs");
    printf("%-34s: %9.2f  AST reserved %zu B (target <= 40: %s); srcman %zu B\n", label,
           (double)total / node_count(b), total,
           verdict((double)total / node_count(b) <= BYTES_PER_NODE_TARGET),
           odin3_srcman_bytes_reserved(b->sm));
    print_breakdown(b);
    return 0;
}

static int bench_big(void) {
    bench *b = odin3_util_calloc(sizeof *b);
    if (b == NULL || build_big(b, true) != 0) {
        odin3_util_free(b);
        return 1;
    }
    print_mix(b);
    int rc = time_walk(b) | time_cursor(b);
    double full = time_check(b, "check FULL (with locations)", ODIN3_CHECK_FULL);
    (void)time_check(b, "check FAST (with locations)", ODIN3_CHECK_FAST);
    printf("check FULL target <= 0.300 s       : %s\n", verdict(full >= 0 && full <= CHECK_TARGET));
    double start = now_seconds();
    close_design(b);
    printf("destroy                           : %9.3f s\n", now_seconds() - start);
    odin3_util_free(b);
    b = odin3_util_calloc(sizeof *b);
    if (b == NULL || build_big(b, false) != 0) {
        odin3_util_free(b);
        return 1;
    }
    double plain = time_check(b, "check FULL (no locations)", ODIN3_CHECK_FULL);
    printf("rule-1 location decode cost       : %9.3f s  (FULL with locs - FULL without)\n",
           full - plain);
    close_design(b);
    odin3_util_free(b);
    return rc;
}

/* --- (4) --parse: the 2C parser on real files (spec §7.6) ----------------------------------- */

enum { PARSE_MAX_FILES = 1024, PARSE_PATH_BYTES = 4096 };

static const double PARSE_ONLY_TARGET = 1.0;
static const double SET_TARGET = 5.0;
static const double MEMORY_TARGET = 20e6;

/* What one file measured (the child's report through the pipe). */
typedef struct parse_result {
    int status; /* 0 ok */
    uint32_t nodes, comments, strings;
    uint64_t bytes, ast_bytes, sm_bytes, string_bytes;
    double pp_secs, parse_secs, check_secs, read_secs;
    long rss_kib;
} parse_result;

typedef struct parse_file {
    const char *path;
    long size;
} parse_file;

static int by_size(const void *lhs, const void *rhs) {
    const parse_file *left = lhs;
    const parse_file *right = rhs;
    if (left->size != right->size) {
        return left->size < right->size ? -1 : 1;
    }
    return strcmp(left->path, right->path);
}

/* The design-file source of path (given as given, resolved against the working directory as
 * read_verilog does, library work). */
static int parse_source(odin3_design *design, const char *path, odin3_pp_source *src) {
    char cwd[PARSE_PATH_BYTES];
    odin3_strbuf resolved;
    odin3_strbuf_init(&resolved);
    *src = (odin3_pp_source){0};
    int rc = getcwd(cwd, sizeof cwd) == NULL ||
             odin3_path_resolve((odin3_path_join){odin3_bytes_cstr(cwd), odin3_bytes_cstr(path)},
                                &resolved) != ODIN3_OK ||
             odin3_design_intern(design, odin3_bytes_cstr(path), &src->given) != ODIN3_OK ||
             odin3_design_intern(design, (odin3_bytes){resolved.data, resolved.len},
                                 &src->resolved) != ODIN3_OK ||
             odin3_design_intern(design, odin3_bytes_cstr("work"), &src->library) != ODIN3_OK;
    odin3_strbuf_free(&resolved);
    return rc;
}

/* The strings interned since count `from`: how many and their bytes. */
static void strtab_growth(const odin3_design *design, size_t from, parse_result *res) {
    const odin3_strtab *tab = odin3_design_strtab(design);
    size_t count = odin3_strtab_count(tab);
    res->strings = (uint32_t)(count - from);
    for (size_t id = from; id < count; id++) {
        res->string_bytes += odin3_strtab_len(tab, (uint32_t)id);
    }
}

/* Preprocess, parse, seal and check one file on design, each timed. */
static int parse_staged(odin3_design *design, const odin3_pp_source *src, parse_result *res) {
    odin3_pp *pp = NULL;
    odin3_ast *ast = NULL;
    odin3_pp_stream stream;
    odin3_ast_id unit = {0};
    odin3_vl_run run;
    odin3_vl_run_init(&run, design);
    double start = now_seconds();
    int rc = odin3_pp_create(design, &(odin3_pp_config){0}, &pp) != ODIN3_OK ||
             odin3_pp_run(pp, src, &stream) != ODIN3_OK;
    double pp_done = now_seconds();
    rc = rc ||
         odin3_ast_create(design, ODIN3_AST_FORM_PARSED, (odin3_passrun_id){1}, &ast) != ODIN3_OK;
    odin3_vp_stream vs = {stream.text, stream.stream, src->given};
    rc = rc || odin3_verilog_parse_stream(&run, ast, &vs, &unit) != ODIN3_OK ||
         odin3_ast_finish(ast) != ODIN3_OK;
    double parsed = now_seconds();
    rc = rc || odin3_ast_check(ast, ODIN3_CHECK_FULL) != ODIN3_OK;
    res->pp_secs = pp_done - start;
    res->parse_secs = parsed - pp_done;
    res->check_secs = now_seconds() - parsed;
    if (rc == 0) {
        odin3_srcman *sm = NULL;
        res->bytes = stream.text.len;
        res->nodes = odin3_ast_node_end(ast).v - 1;
        res->comments = odin3_ast_comment_count(ast);
        res->ast_bytes = odin3_ast_bytes_reserved(ast);
        rc = odin3_design_get_srcman(design, &sm) != ODIN3_OK;
        res->sm_bytes = rc == 0 ? odin3_srcman_bytes_reserved(sm) : 0;
    }
    odin3_ast_destroy(ast);
    odin3_pp_destroy(pp);
    return rc;
}

/* read_verilog --parse-only's reader on a fresh design, end to end. */
static int parse_read(const char *path, parse_result *res) {
    odin3_design *design = odin3_design_create();
    odin3_pp_source src;
    odin3_ast *ast = NULL;
    double start = now_seconds();
    int rc = design == NULL || parse_source(design, path, &src) != 0 ||
             odin3_read_verilog_files(
                 design, &(odin3_rv_opts){&src, 1, (odin3_passrun_id){1}, NULL}, &ast) != ODIN3_OK;
    res->read_secs = now_seconds() - start;
    odin3_ast_destroy(ast);
    odin3_design_destroy(design);
    return rc;
}

/* The child's measurement of one file. */
static void parse_measure(const char *path, parse_result *res) {
    *res = (parse_result){0};
    odin3_design *design = odin3_design_create();
    odin3_pp_source src;
    size_t strings = 0;
    int rc = design == NULL;
    if (rc == 0) {
        strings = odin3_strtab_count(odin3_design_strtab(design));
        rc = parse_source(design, path, &src);
    }
    rc = rc || parse_staged(design, &src, res);
    if (rc == 0) {
        strtab_growth(design, strings, res);
    }
    odin3_design_destroy(design);
    rc = rc || parse_read(path, res);
    res->status = rc;
    res->rss_kib = max_rss_kib();
}

/* Runs parse_measure in a child; its result through a pipe. */
static int parse_in_child(const char *path, parse_result *res) {
    int fds[2];
    if (pipe(fds) != 0) {
        return 1;
    }
    (void)fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        (void)close(fds[0]);
        parse_measure(path, res);
        ssize_t n = write(fds[1], res, sizeof *res);
        _exit(n == (ssize_t)sizeof *res ? 0 : 1);
    }
    (void)close(fds[1]);
    ssize_t got = pid > 0 ? read(fds[0], res, sizeof *res) : -1;
    (void)close(fds[0]);
    int wstatus = 0;
    if (pid < 0 || waitpid(pid, &wstatus, 0) != pid || !WIFEXITED(wstatus) ||
        WEXITSTATUS(wstatus) != 0 || got != (ssize_t)sizeof *res) {
        return 1;
    }
    return res->status;
}

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

/* One file: its bytes on disk and in the stream, the store, the times and the peak RSS. */
static void parse_line(const parse_file *file, const parse_result *res) {
    printf("%-32s %9ld B (stream %9" PRIu64 ")  nodes %8" PRIu32 "  comments %6" PRIu32
           "  ast %10" PRIu64 " B  srcman %8" PRIu64 " B  strtab +%6" PRIu32 " (%7" PRIu64
           " B)  pp %.3f s  parse %.3f s  check %.3f s  read_verilog %.3f s  peak RSS %ld KiB\n",
           base_name(file->path), file->size, res->bytes, res->nodes, res->comments, res->ast_bytes,
           res->sm_bytes, res->strings, res->string_bytes, res->pp_secs, res->parse_secs,
           res->check_secs, res->read_secs, res->rss_kib);
}

/* mcml.v's two targets. */
static void parse_mcml_targets(const parse_result *res) {
    double memory = (double)(res->ast_bytes + res->sm_bytes);
    printf("mcml.v AST + srcman               : %9.0f B  (target <= 20,000,000 B: %s)\n", memory,
           verdict(memory <= MEMORY_TARGET));
    printf("mcml.v parse-only (read_verilog)  : %9.3f s  (pp %.3f s + parse %.3f s; target <= "
           "1.000 s: %s)\n",
           res->read_secs, res->pp_secs, res->parse_secs,
           verdict(res->read_secs <= PARSE_ONLY_TARGET));
}

static int bench_parse(int nfiles, char **paths) {
    static parse_file files[PARSE_MAX_FILES];
    if (nfiles <= 0 || nfiles > PARSE_MAX_FILES) {
        fprintf(stderr, "bench_ast --parse: 1 to %d files\n", PARSE_MAX_FILES);
        return 2;
    }
    for (int i = 0; i < nfiles; i++) {
        struct stat sb;
        files[i] = (parse_file){paths[i], stat(paths[i], &sb) == 0 ? (long)sb.st_size : 0};
    }
    qsort(files, (size_t)nfiles, sizeof files[0], by_size);
    int rc = 0;
    double total = 0;
    double staged = 0;
    for (int i = 0; i < nfiles; i++) {
        parse_result res;
        if (parse_in_child(files[i].path, &res) != 0) {
            fprintf(stderr, "bench_ast --parse: %s failed\n", files[i].path);
            rc = 1;
            continue;
        }
        parse_line(&files[i], &res);
        total += res.read_secs;
        staged += res.pp_secs + res.parse_secs;
        if (strcmp(base_name(files[i].path), "mcml.v") == 0) {
            parse_mcml_targets(&res);
        }
    }
    printf("%d files, read_verilog total     : %9.3f s  (pp + parse %.3f s; target for "
           "verilog/*.v <= 5.000 s: %s)\n",
           nfiles, total, staged, verdict(rc == 0 && total <= SET_TARGET));
    return rc;
}

static int quiet_run(void) {
    odin3_log_set_level(ODIN3_LOG_ERROR);
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--parse") == 0) {
        (void)quiet_run();
        return bench_parse(argc - 2, argv + 2);
    }
    char path[1024];
    if (argc > 1) {
        (void)snprintf(path, sizeof path, "%s", argv[1]);
    } else {
        const char *root = getenv("VTR_ROOT");
        (void)snprintf(path, sizeof path, "%s/vtr_flow/benchmarks/verilog/mcml.v",
                       root != NULL ? root : ".");
    }
    int rc = quiet_run();
    bench *b = odin3_util_calloc(sizeof *b);
    if (b == NULL || open_design(b) != 0) {
        odin3_util_free(b);
        return 1;
    }
    rc |= bench_mcml(b, path);
    close_design(b);
    odin3_util_free(b);
    rc |= bench_estimate();
    rc |= bench_big();
    printf("max RSS                           : %ld KiB\n", max_rss_kib());
    if (rc != 0) {
        fprintf(stderr, "bench_ast: failed\n");
    }
    return rc;
}
