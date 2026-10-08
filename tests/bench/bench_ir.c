/* bench_ir.c — times a 2,000,000-node IR module: build, check, fanout walk, delete, compact. */
#include "ir/check.h"
#include "ir/design.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "util/alloc.h"
#include "util/log.h"
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/resource.h>
#include <time.h>

enum { BENCH_NODES = 2000000, BENCH_NETS = 2200000, BENCH_PROV_EVERY = 1000, BENCH_DEL_EVERY = 10 };

typedef struct bench_ctx {
    odin3_design *design;
    odin3_module *module;
    odin3_celltype_id and_type;
    odin3_celltype_id dff_type;
    odin3_pass_ctx pass;
    uint64_t lcg;
    odin3_node_id *ids; /* node ID returned at creation, by creation index */
} bench_ctx;

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static long max_rss_kib(void) {
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return -1;
    }
    return usage.ru_maxrss;
}

static void report(const char *phase, double start) {
    printf("%-14s: %9.3f s  maxrss-so-far %ld KiB\n", phase, now_seconds() - start, max_rss_kib());
}

/* Knuth MMIX LCG, high bits; deterministic for a fixed seed. */
static uint32_t next_rand(bench_ctx *ctx, uint32_t bound) {
    ctx->lcg = ctx->lcg * 6364136223846793005ULL + 1442695040888963407ULL;
    return (uint32_t)((ctx->lcg >> 33) % bound);
}

static int find_type(bench_ctx *ctx, const char *name, odin3_celltype_id *out) {
    uint32_t str = 0;
    size_t len = 0;
    while (name[len] != '\0') {
        len++;
    }
    if (odin3_design_intern(ctx->design, (odin3_bytes){name, len}, &str) != ODIN3_OK) {
        return 1;
    }
    return odin3_celltype_find(ctx->design, str, out) ? 0 : 1;
}

static int setup(bench_ctx *ctx) {
    ctx->lcg = 0x2545F4914F6CDD1DULL;
    ctx->design = odin3_design_create();
    if (ctx->design == NULL) {
        return 1;
    }
    uint32_t name = 0;
    odin3_module_id id = {0};
    odin3_prov_id none = {0};
    if (find_type(ctx, "$_AND_", &ctx->and_type) != 0 ||
        find_type(ctx, "$_DFF_P_", &ctx->dff_type) != 0 ||
        odin3_design_intern(ctx->design, (odin3_bytes){"top", 3}, &name) != ODIN3_OK ||
        odin3_module_create(ctx->design, name, none, &id) != ODIN3_OK ||
        odin3_pass_run_begin(ctx->design, name, &ctx->pass) != ODIN3_OK) {
        return 1;
    }
    ctx->module = odin3_module_get(ctx->design, id);
    return ctx->module == NULL;
}

static int make_nets(bench_ctx *ctx) {
    odin3_prov_id none = {0};
    for (uint32_t i = 0; i < BENCH_NETS; i++) {
        odin3_net_id net = {0};
        if (odin3_net_create(ctx->module, 0, none, &net) != ODIN3_OK) {
            return 1;
        }
    }
    return 0;
}

/* One source record per BENCH_PROV_EVERY nodes; updates *prov when i starts a new group. */
static int node_prov(bench_ctx *ctx, uint32_t i, odin3_prov_id *prov) {
    if (i % BENCH_PROV_EVERY != 0) {
        return 0;
    }
    odin3_srcloc loc = {0, i / BENCH_PROV_EVERY + 1, 1, i / BENCH_PROV_EVERY + 1, 2};
    odin3_prov_origin origin = {&loc, 1, 0, 0};
    odin3_prov_begin_op(&ctx->pass);
    return odin3_prov_source(&ctx->pass, &origin, prov) != ODIN3_OK;
}

/*
 * Node i drives net i+1. Even nodes are DFFs (inputs anywhere); odd nodes are ANDs whose inputs
 * come from lower-numbered nets, so the combinational graph is acyclic.
 */
static int make_node(bench_ctx *ctx, uint32_t i, odin3_prov_id prov) {
    bool is_and = (i % 2) != 0;
    odin3_net_id nets[3];
    uint32_t bound = is_and ? i : BENCH_NETS;
    nets[0].v = 1 + next_rand(ctx, bound);
    nets[1].v = 1 + next_rand(ctx, bound);
    nets[2].v = i + 1;
    odin3_netvec ports[3] = {{&nets[0], 1}, {&nets[1], 1}, {&nets[2], 1}};
    odin3_node_spec spec = {is_and ? ctx->and_type : ctx->dff_type, 0, prov, NULL, 0};
    odin3_node_id node = {0};
    if (odin3_node_create_connected(ctx->module, &spec, ports, &node) != ODIN3_OK) {
        return 1;
    }
    ctx->ids[i] = node;
    return 0;
}

static int build(bench_ctx *ctx) {
    ctx->ids = odin3_util_calloc(BENCH_NODES * sizeof *ctx->ids);
    if (ctx->ids == NULL) {
        return 1;
    }
    if (make_nets(ctx) != 0) {
        return 1;
    }
    odin3_prov_id prov = {0};
    for (uint32_t i = 0; i < BENCH_NODES; i++) {
        if (node_prov(ctx, i, &prov) != 0 || make_node(ctx, i, prov) != 0) {
            return 1;
        }
    }
    return 0;
}

static uint64_t fanout_walk(const bench_ctx *ctx) {
    uint64_t visited = 0;
    uint64_t checksum = 0;
    uint32_t end = odin3_module_node_end(ctx->module);
    for (uint32_t i = 1; i < end; i++) {
        odin3_node_id node = {i};
        if (!odin3_node_live(ctx->module, node)) {
            continue;
        }
        odin3_pinslice pins = odin3_node_pins(ctx->module, node);
        for (uint32_t k = 0; k < pins.count; k++) {
            odin3_pin_id pin = {pins.first.v + k};
            if (!odin3_pin_drives(ctx->module, pin)) {
                continue;
            }
            odin3_pinlist sinks = odin3_net_sinks(ctx->module, odin3_pin_net(ctx->module, pin));
            for (uint32_t j = 0; j < sinks.count; j++) {
                checksum += odin3_pin_node(ctx->module, sinks.pins[j]).v;
                visited++;
            }
        }
    }
    printf("fanout visits : %" PRIu64 " sinks  checksum %" PRIu64 "\n", visited, checksum);
    return visited;
}

static int delete_some(bench_ctx *ctx, uint32_t *nodes_deleted, uint32_t *nets_deleted) {
    /* 2 of every 20 creation indices (3 and 10): 10%, one odd (AND) and one even (DFF). */
    for (uint32_t i = 0; i < BENCH_NODES; i++) {
        uint32_t r = i % (2 * BENCH_DEL_EVERY);
        if (r != 3 && r != BENCH_DEL_EVERY) {
            continue;
        }
        if (odin3_node_delete(ctx->module, ctx->ids[i]) != ODIN3_OK) {
            return 1;
        }
        (*nodes_deleted)++;
    }
    uint32_t end = odin3_module_net_end(ctx->module);
    for (uint32_t i = 1; i < end; i++) {
        odin3_net_id net = {i};
        if (odin3_net_live(ctx->module, net) && odin3_net_pins(ctx->module, net).count == 0) {
            if (odin3_net_delete(ctx->module, net) != ODIN3_OK) {
                return 1;
            }
            (*nets_deleted)++;
        }
    }
    return 0;
}

static uint32_t count_live_nodes(const bench_ctx *ctx) {
    uint32_t live = 0;
    uint32_t end = odin3_module_node_end(ctx->module);
    for (uint32_t i = 1; i < end; i++) {
        odin3_node_id node = {i};
        live += odin3_node_live(ctx->module, node) ? 1U : 0U;
    }
    return live;
}

static int run(bench_ctx *ctx) {
    odin3_check_opts full = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    double start = now_seconds();
    if (build(ctx) != 0) {
        return 1;
    }
    report("build", start);
    start = now_seconds();
    odin3_log_reset_counts();
    odin3_log_set_level(ODIN3_LOG_ERROR);
    odin3_status check = odin3_check_module(ctx->module, full);
    odin3_log_set_level(ODIN3_LOG_INFO);
    report("check FULL", start);
    printf("check status  : %d  (%zu errors, %zu warnings logged)\n", (int)check,
           odin3_log_count(ODIN3_LOG_ERROR), odin3_log_count(ODIN3_LOG_WARN));
    if (check != ODIN3_OK) {
        fprintf(stderr, "bench_ir: check FULL failed\n");
        return 1;
    }
    start = now_seconds();
    if (fanout_walk(ctx) == 0) {
        fprintf(stderr, "bench_ir: fanout walk visited no sinks\n");
        return 1;
    }
    report("fanout walk", start);
    uint32_t nodes_deleted = 0;
    uint32_t nets_deleted = 0;
    start = now_seconds();
    if (delete_some(ctx, &nodes_deleted, &nets_deleted) != 0) {
        return 1;
    }
    report("delete 10%", start);
    printf("deleted       : %u nodes, %u pinless nets\n", (unsigned)nodes_deleted,
           (unsigned)nets_deleted);
    odin3_compact_map map = {0};
    start = now_seconds();
    if (odin3_module_compact(ctx->module, &map) != ODIN3_OK) {
        return 1;
    }
    report("compact", start);
    odin3_compact_map_free(&map);
    printf("final         : %u live nodes, %u node ids, %u net ids, %u pin ids\n",
           (unsigned)count_live_nodes(ctx), (unsigned)odin3_module_node_end(ctx->module) - 1,
           (unsigned)odin3_module_net_end(ctx->module) - 1,
           (unsigned)odin3_module_pin_end(ctx->module) - 1);
    return 0;
}

int main(void) {
    bench_ctx ctx = {0};
    int rc = setup(&ctx);
    if (rc == 0) {
        rc = run(&ctx);
    }
    printf("max RSS       : %ld KiB\n", max_rss_kib());
    odin3_util_free(ctx.ids);
    odin3_design_destroy(ctx.design);
    if (rc != 0) {
        fprintf(stderr, "bench_ir: failed\n");
    }
    return rc;
}
