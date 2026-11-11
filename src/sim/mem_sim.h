/*
 * mem_sim.h — simulating a `$mem` without `lower` (design note
 * docs/specs/2026-11-11-sim-mem-hook-design.md).
 *
 * One `$mem` node becomes up to two flat cells that share one state block:
 *   E  (edge cell)    write ports and synchronous read ports; pins CLK, WR_EN, WR_ADDR, WR_DATA,
 * RD_EN, RD_ADDR (all read ports), RD_DATA (synchronous ports only, output). A  (level cell)
 * asynchronous read ports; pins RD_ADDR, RD_DATA (those ports only). State block: [synchronous read
 * registers: n_sync * WIDTH][array: words * WIDTH], one byte per bit (codes 0/1, and x = 2, z = 3
 * in 4-state mode). The semantics are those of src/passes/lower/mem.c.
 */
#ifndef ODIN3_SIM_MEM_SIM_H
#define ODIN3_SIM_MEM_SIM_H

#include "ir/celltype.h"
#include "ir/value.h"
#include "sim/build_internal.h"
#include "sim/cell.h"

#include <stdbool.h>
#include <stdint.h>

/* The geometry of one `$mem`, from its parameters. */
typedef struct odin3_sim_mem_geo {
    uint64_t size;
    uint64_t words; /* min(SIZE, 2^ABITS): the words an address can reach */
    uint64_t mask;  /* 2^ABITS - 1 */
    int64_t offset;
    uint32_t abits;
    uint32_t width;
    uint32_t n_rd;
    uint32_t n_wr;
    uint32_t n_sync;     /* synchronous read ports */
    uint64_t state_size; /* n_sync * WIDTH + words * WIDTH */
} odin3_sim_mem_geo;

/* Fills geo from the `$mem` parameter list. */
void odin3_sim_mem_geometry(const odin3_value *params, odin3_sim_mem_geo *geo);

/* The two cell types (ports and hooks); the 4-state hooks are mem_e4 / mem_a4. */
extern const odin3_celltype_def odin3_sim_mem_edge_def;
extern const odin3_celltype_def odin3_sim_mem_async_def;
void odin3_sim_mem_edge4(const odin3_sim_cell *cell);
void odin3_sim_mem_async4(const odin3_sim_cell *cell);

/* True when def is the `$mem` cell type. */
bool odin3_sim_is_mem_type(const odin3_celltype_def *def);

/*
 * Builder: expands the `$mem` node of frame into its flat cells (build_mem.c). Errors are logged
 * (located) and ODIN3_ERR_INVALID_ARG, or ODIN3_ERR_NO_MEMORY.
 */
odin3_status odin3_sim_add_mem(odin3_sim_builder *bld, uint32_t frame, odin3_node_id node);

/* Value of the `$mem` parameter p (index in declaration order of ir/cells/mem.c). */
enum {
    ODIN3_SIM_MEM_P_SIZE = 1,
    ODIN3_SIM_MEM_P_OFFSET = 2,
    ODIN3_SIM_MEM_P_ABITS = 3,
    ODIN3_SIM_MEM_P_WIDTH = 4,
    ODIN3_SIM_MEM_P_RD_PORTS = 5,
    ODIN3_SIM_MEM_P_WR_PORTS = 6,
    ODIN3_SIM_MEM_P_RD_CLK_ENABLE = 7,
    ODIN3_SIM_MEM_P_RD_CLK_POLARITY = 8,
    ODIN3_SIM_MEM_P_RD_TRANSPARENT = 9,
    ODIN3_SIM_MEM_P_WR_CLK_ENABLE = 10,
    ODIN3_SIM_MEM_P_WR_CLK_POLARITY = 11,
    ODIN3_SIM_MEM_P_INIT = 12
};

/* Port numbers of the `$mem` node. */
enum {
    ODIN3_SIM_MEM_PORT_RD_CLK,
    ODIN3_SIM_MEM_PORT_RD_EN,
    ODIN3_SIM_MEM_PORT_RD_ADDR,
    ODIN3_SIM_MEM_PORT_RD_DATA,
    ODIN3_SIM_MEM_PORT_WR_CLK,
    ODIN3_SIM_MEM_PORT_WR_EN,
    ODIN3_SIM_MEM_PORT_WR_ADDR,
    ODIN3_SIM_MEM_PORT_WR_DATA
};

#endif
