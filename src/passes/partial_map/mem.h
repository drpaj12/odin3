/*
 * mem.h — internal to mem.c, mem_merge.c and mem_plan.c (3C spec §6.2, R-8..R-12): the model of
 * one inferred `$mem` while hardmap decides what to do with it. The model is a copy of the cell's
 * ports as nets (mem_rport / mem_wport); R-8 rewrites the copy (a merged port reads from the
 * flip-flop's side) and records the flip-flops it makes redundant. Nothing in the design changes
 * until mem.c commits the mapping, so a `$mem` that ends soft keeps its original flip-flops.
 */
#ifndef ODIN3_PASSES_HARDMAP_MEM_H
#define ODIN3_PASSES_HARDMAP_MEM_H

#include "passes/partial_map/ram.h"

#include <stdbool.h>
#include <stdint.h>

/* The port indices of `$mem` (src/ir/cells/mem.c, declaration order). */
enum {
    MEM_P_RD_CLK,
    MEM_P_RD_EN,
    MEM_P_RD_ADDR,
    MEM_P_RD_DATA,
    MEM_P_WR_CLK,
    MEM_P_WR_EN,
    MEM_P_WR_ADDR,
    MEM_P_WR_DATA,
    /* a RAM of more address bits is never built from a `$mem` (R-11) */
    MEM_MAX_ABITS = PM_RAM_MAX_ADDR
};

typedef struct mem_rport {
    bool sync;        /* after R-8 */
    bool posedge;     /* rising clock edge */
    bool transparent; /* RD_TRANSPARENT */
    bool en_pos;      /* the enable is active high */
    bool en_always;   /* no enable (a merged `$dff`): reads every cycle */
    bool addr_merged; /* R-8 address merge: addr[] are the flip-flops' D nets */
    odin3_net_id clk;
    odin3_net_id en;
    odin3_net_id addr[MEM_MAX_ABITS];
    odin3_vec data; /* odin3_net_id x WIDTH: the nets the RAM output is tied to */
} mem_rport;

typedef struct mem_wport {
    bool sync;
    bool posedge;
    bool en_ok; /* the WIDTH enable bits are one net */
    odin3_net_id clk;
    odin3_net_id en;
    odin3_net_id addr[MEM_MAX_ABITS];
    odin3_vec data; /* odin3_net_id x WIDTH */
} mem_wport;

typedef enum mem_verdict {
    MEM_HARD,
    MEM_SOFT_OFFSET,  /* OFFSET != 0 (plan agent default e, D13) */
    MEM_SOFT_ASYNC,   /* R-9: an asynchronous read port, or more than one clock domain */
    MEM_SOFT_EDGE,    /* a port on the falling edge (agent default f, D14) */
    MEM_SOFT_EN,      /* a read enable that is not constant 1 (R-10, D9) */
    MEM_SOFT_PATTERN, /* R-11: no fixed-subset mapping */
} mem_verdict;

/* One RAM of the mapping: its kind and, per RAM port, the read port and the write port it carries
 * (-1: none). */
typedef struct mem_ram {
    hm_hard kind;
    int32_t rd[RAM_MAX_PORTS];
    int32_t wr[RAM_MAX_PORTS];
} mem_ram;

typedef struct mem_model {
    odin3_node_id node;
    uint32_t abits;
    uint32_t width;
    uint32_t n_rd;
    uint32_t n_wr;
    uint32_t name; /* MEMID (strtab ID) */
    int64_t offset;
    bool init_defined;
    bool too_wide;  /* ABITS above what hardmap splits: ports not loaded */
    odin3_vec rd;   /* mem_rport */
    odin3_vec wr;   /* mem_wport */
    odin3_vec dead; /* odin3_node_id: the flip-flops R-8 makes redundant */
    odin3_vec rams; /* mem_ram: the plan (mem_classify) */
} mem_model;

/* A pin of a node: pin port (the cell type's port index) and the pin's index in it. */
typedef struct mem_pin {
    uint32_t port;
    uint32_t index;
} mem_pin;

/* The net on a pin of the node (none: open or out of range). */
odin3_net_id mem_pin_net(const hm_ctx *hc, odin3_node_id node, mem_pin pin);

/* Copies the `$mem` node into mm (the design is untouched); an ABITS above MEM_MAX_ABITS sets
 * too_wide and loads no port. NO_MEMORY only. mem_model_free releases a loaded model. Shared with
 * lpmmap (5.1 R-1), which reuses the R-8 flip-flop merge below. */
odin3_status mem_load(const hm_ctx *hc, odin3_node_id node, mem_model *mm);
void mem_model_free(mem_model *mm);

/* R-8 on every asynchronous read port of mm (data merge, else address merge). NO_MEMORY only. */
odin3_status mem_merge(const hm_ctx *hc, mem_model *mm);

/* R-9..R-11 and the pattern table of R-10: the verdict, and for MEM_HARD the plan in mm->rams. */
odin3_status mem_classify(const hm_ctx *hc, mem_model *mm, mem_verdict *verdict);

#endif
