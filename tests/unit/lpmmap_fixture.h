/*
 * lpmmap_fixture.h — shared helpers of the 5.1 lpmmap unit tests: flat IR built from RTLIL text
 * with lib/lpm.o3lib read, a check FULL around an lpmmap run, and readers for the nodes it makes.
 * Test code: it asserts on every call instead of returning statuses, except where a status is the
 * point.
 */
#ifndef ODIN3_TESTS_UNIT_LPMMAP_FIXTURE_H
#define ODIN3_TESTS_UNIT_LPMMAP_FIXTURE_H

#include "ir/ids.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/lpmmap/lpmmap.h"
#include "proc_build.h"

#include <stdbool.h>
#include <stdint.h>

/* pb_build_lib with ODIN3_LIB_DIR/lpm.o3lib, asserting ODIN3_OK; the last module is the top. */
void lm_build_ok(const char *rtlil, pb_result *out);

/* odin3_lpmmap_run on res (family NULL: the default), with check FULL before and after it when the
 * run is OK; returns the run's status. stats may be NULL. */
odin3_status lm_run(pb_result *res, const char *family, odin3_lpmmap_stats *stats);

/* The number of live nodes of module whose cell type is named type. */
uint32_t lm_count(const odin3_module *module, const char *type);

/* The nth live node (from 0) of that cell type in node-ID order, none when absent. */
odin3_node_id lm_nth(const odin3_module *module, const char *type, uint32_t nth);

/* The INT parameter `name` of node (asserts it exists and is INT). */
int64_t lm_param_int(const odin3_module *module, odin3_node_id node, const char *name);

/* The STRING parameter `name` of node (asserts it exists and is a STRING). */
const char *lm_param_str(const odin3_module *module, odin3_node_id node, const char *name);

/* The net on bit `bit` of port `port` of node; none when the pin is unconnected. */
odin3_net_id lm_pin_net(const odin3_module *module, odin3_node_id node, const char *port,
                        uint32_t bit);

/* The width of port `port` of node (0 when absent). */
uint32_t lm_port_width(const odin3_module *module, odin3_node_id node, const char *port);

/* True when every bit of port `port` of node is unconnected. */
bool lm_port_open(const odin3_module *module, odin3_node_id node, const char *port);

#endif
