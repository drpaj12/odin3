/* glet_int.h — private tables shared by tables.c and count.c. */
#ifndef ODIN3_PASSES_GRAPHLETS_GLET_INT_H
#define ODIN3_PASSES_GRAPHLETS_GLET_INT_H

#include "graph/glet.h"

/* u_orb[s][code * s + position] = orbit id, 0xFF: the code is not a connected graph.
 * d_orb the same for directed codes (two bits per pair; 0xFF also for the state 3). */
typedef struct glet_tables {
    uint8_t u_orb[GLET_MAX_SIZE + 1][1024 * GLET_MAX_SIZE];
    uint8_t d_orb[GLET_MAX_DIR_SIZE + 1][4096 * GLET_MAX_DIR_SIZE];
    glet_orbit_info info_u[GLET_ORBITS_U5];
    glet_orbit_info info_d[GLET_ORBITS_D4];
} glet_tables;

extern glet_tables g_glet_tab;
extern const uint8_t k_przulj[GLET_ORBITS_U5];

void glet_tables_init(void);

#endif
