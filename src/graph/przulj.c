/*
 * przulj.c — renames the undirected orbit ids of tables.c (graphlets numbered by node count, edge
 * count and canonical code) to Przulj's numbering 0..72 (orbit 0 the degree, 1-2 path of three,
 * 3 triangle, 4-14 the four-node graphlets, 15-72 the five-node ones). k_przulj[mine] = Przulj id.
 * The table was fixed by matching the columns of this counter's output with the output of the ORCA
 * binary (external oracle, test data only) on random graphs where all 73 columns differ; the match
 * is a unique bijection inside each node-count block.
 */
#include "graph/glet_int.h"

const uint8_t k_przulj[GLET_ORBITS_U5] = {
    0,  2,  1,  3,  7,  6,  5,  4,  11, 10, 9,  8,  13, 12, 14, 23, 22, 21, 20,
    18, 19, 16, 17, 15, 33, 32, 31, 26, 25, 24, 38, 36, 37, 35, 28, 30, 29, 27,
    34, 42, 41, 40, 39, 47, 48, 46, 45, 50, 49, 44, 43, 53, 51, 52, 58, 57, 56,
    55, 54, 61, 60, 59, 63, 64, 62, 67, 66, 65, 69, 68, 71, 70, 72};
