/* sim_netnames.c — see sim_netnames.h. */
#include "passes/sim_netnames.h"

#include "backends/blif/writer.h"
#include "ir/design.h"
#include "ir/module.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* The names of one module, or none when naming was refused. */
typedef struct mod_names {
    uint32_t module;
    odin3_blif_names *names; /* NULL: refused */
} mod_names;

bool odin3_sim_netnames_style(const char *text, odin3_blif_name_style *style) {
    if (strcmp(text, "short") == 0) {
        *style = ODIN3_BLIF_NAMES_SHORT;
    } else if (strcmp(text, "provenance") == 0) {
        *style = ODIN3_BLIF_NAMES_PROVENANCE;
    } else if (strcmp(text, "hash") == 0) {
        *style = ODIN3_BLIF_NAMES_HASH;
    } else {
        return false;
    }
    return true;
}

void odin3_sim_netnames_init(odin3_sim_netnames *nn, const odin3_design *design, const char *who,
                             odin3_blif_name_style style) {
    *nn = (odin3_sim_netnames){.design = design, .who = who};
    nn->opts.dialect = ODIN3_BLIF_DIALECT_VTR;
    nn->opts.name_style = style;
    odin3_vec_init(&nn->mods, sizeof(mod_names));
}

void odin3_sim_netnames_free(odin3_sim_netnames *nn) {
    for (size_t i = 0; i < nn->mods.len; i++) {
        odin3_blif_names_free(((mod_names *)odin3_vec_at(&nn->mods, i))->names);
    }
    odin3_vec_free(&nn->mods);
}

/* The entry of module, naming it on first use. NULL when out of memory. */
static const mod_names *entry_of(odin3_sim_netnames *nn, odin3_module_id module) {
    for (size_t i = 0; i < nn->mods.len; i++) {
        const mod_names *have = odin3_vec_cat(&nn->mods, i);
        if (have->module == module.v) {
            return have;
        }
    }
    mod_names *add = odin3_vec_push(&nn->mods);
    if (add == NULL) {
        return NULL;
    }
    odin3_blif_names_req req = {.design = nn->design, .opts = &nn->opts, .module = module};
    uint32_t renamed = 0;
    *add = (mod_names){module.v, NULL};
    if (odin3_blif_vtr_name_top(&req, &add->names, &renamed) != ODIN3_OK) {
        add->names = NULL;
        odin3_log(ODIN3_LOG_WARN,
                  "%s: --vcd-all: cannot name the nets of module %u as write_blif --vtr does; "
                  "using the net names of the IR",
                  nn->who, (unsigned)module.v);
    }
    return add;
}

bool odin3_sim_netnames_name(void *ctx, odin3_module_id module, odin3_net_id net,
                             odin3_strbuf *out) {
    odin3_sim_netnames *nn = ctx;
    const mod_names *ent = entry_of(nn, module);
    if (ent == NULL || ent->names == NULL) {
        return false;
    }
    odin3_bytes name = odin3_blif_names_net(
        ent->names, nn->design, odin3_module_get((odin3_design *)nn->design, module), net);
    return name.len > 0 && odin3_strbuf_append(out, name) == ODIN3_OK;
}
