/*
 * gl_inst.c — the instance file of the graphlets pass (spec docs/specs/2026-11-06-odin3-graphlet-
 * instances.md): the node sets of one graphlet type, found by the same ESU run as the counts.
 */
#include "passes/graphlets/gl_int.h"
#include "util/alloc.h"
#include "util/log.h"

#include <stdio.h>
#include <string.h>

enum { INST_SCHEMA_VERSION = 1 };

odin3_status gl_inst_begin(glet_inst *inst, const gl_opts *opts, const glet_spec *mode) {
    memset(inst, 0, sizeof *inst);
    inst->graphlet = opts->inst_graphlet;
    inst->max = opts->inst_max;
    uint32_t nodes = glet_graphlet_nodes(
        mode->directed, mode->directed ? GLET_MAX_DIR_SIZE : mode->size, inst->graphlet);
    if (nodes == 0) {
        odin3_log(ODIN3_LOG_ERROR, "graphlets: --instances %u is not a graphlet of this mode",
                  (unsigned)inst->graphlet);
        return ODIN3_ERR_INVALID_ARG;
    }
    inst->out = odin3_util_calloc(((size_t)inst->max * nodes + 1) * sizeof *inst->out);
    return inst->out != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
}

void gl_inst_end(glet_inst *inst) {
    odin3_util_free(inst->out);
    inst->out = NULL;
}

/* The ids of instance idx as node IDs, ascending (the dense order differs after a halo). */
static void put_instance(FILE *fp, const glet_inst *inst, const gl_inst_info *info, uint64_t idx) {
    uint32_t ids[GLET_MAX_SIZE];
    const uint32_t *src = inst->out + idx * inst->nodes;
    for (uint32_t ii = 0; ii < inst->nodes; ii++) {
        uint32_t val = info->ids[src[ii]];
        uint32_t jj = ii;
        while (jj > 0 && ids[jj - 1] > val) {
            ids[jj] = ids[jj - 1];
            jj--;
        }
        ids[jj] = val;
    }
    (void)fputs(idx != 0 ? ",\n[" : "\n[", fp);
    for (uint32_t ii = 0; ii < inst->nodes; ii++) {
        (void)fprintf(fp, "%s%u", ii != 0 ? ", " : "", (unsigned)ids[ii]);
    }
    (void)fputc(']', fp);
}

odin3_status gl_inst_write(const glet_inst *inst, const gl_inst_info *info) {
    FILE *fp = fopen(info->path, "w");
    if (fp == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "graphlets: cannot write '%s'", info->path);
        return ODIN3_ERR_IO;
    }
    (void)fprintf(fp,
                  "{\"schema\": \"odin3-graphlet-instances\", \"version\": %d, \"mode\": \"%s\", "
                  "\"graphlet\": %u, \"nodes_per_instance\": %u, \"total\": %llu, "
                  "\"written\": %llu, \"truncated\": %d, \"max\": %llu, \"module\": ",
                  INST_SCHEMA_VERSION, info->mode, (unsigned)inst->graphlet, (unsigned)inst->nodes,
                  (unsigned long long)inst->total, (unsigned long long)inst->written,
                  inst->total > inst->written ? 1 : 0, (unsigned long long)info->max);
    odin3_strbuf mod;
    odin3_strbuf_init(&mod);
    odin3_status st = gl_put_escaped(&mod, info->module);
    if (st == ODIN3_OK) {
        (void)fwrite(mod.data, 1, mod.len, fp);
    }
    odin3_strbuf_free(&mod);
    (void)fputs(", \"instances\": [", fp);
    for (uint64_t idx = 0; idx < inst->written; idx++) {
        put_instance(fp, inst, info, idx);
    }
    (void)fputs("]}\n", fp);
    bool ok = fclose(fp) == 0 && st == ODIN3_OK;
    return ok ? ODIN3_OK : ODIN3_ERR_IO;
}
