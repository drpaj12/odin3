/*
 * act_out.c — outputs of the activity pass (spec section 4): the VPR `.act` file, the JSON file and
 * the module attributes. Net names are the ones `write_blif --vtr` gives (sim_netnames).
 */
#include "passes/activity/act_int.h"

#include "passes/graphlets/gl_int.h"
#include "passes/sim_netnames.h"
#include "util/log.h"

#include <stdio.h>
#include <string.h>

enum { SCHEMA_VERSION = 1, NUM_BUF = 32 };

static const char *k_kind_names[ACT_NKINDS] = {"unused", "pi", "clock",  "const",
                                               "comb",   "ff", "opaque", "undriven"};

bool act_name_net(void *ctx, uint32_t net, odin3_strbuf *out) {
    act_namer *nm = ctx;
    if (odin3_sim_netnames_name(nm->nn, nm->mid, (odin3_net_id){net}, out)) {
        return true;
    }
    uint32_t name = odin3_net_name(nm->mod, (odin3_net_id){net});
    const char *text =
        name != 0 ? odin3_strtab_get(odin3_design_strtab(odin3_module_design(nm->mod)), name)
                  : NULL;
    return (text != NULL ? odin3_strbuf_appendf(out, "%s", text)
                         : odin3_strbuf_appendf(out, "$n%u", (unsigned)net)) == ODIN3_OK;
}

static odin3_status save_file(const char *path, const odin3_strbuf *text) {
    FILE *fp = fopen(path, "w");
    if (fp == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "activity: cannot write '%s'", path);
        return ODIN3_ERR_IO;
    }
    bool ok = fwrite(text->data, 1, text->len, fp) == text->len;
    return (fclose(fp) == 0 && ok) ? ODIN3_OK : ODIN3_ERR_IO;
}

/* Nets that appear in the outputs: any net with a pin (not ACT_UNUSED). */
static bool reported(const act_model *md, uint32_t id) {
    return md->kind[id] != ACT_UNUSED && odin3_net_live(md->mod, (odin3_net_id){id});
}

odin3_status act_write_act(const act_model *md, act_namer *nm, const char *path) {
    odin3_strbuf body;
    odin3_strbuf_init(&body);
    odin3_status st = ODIN3_OK;
    for (uint32_t id = 1; st == ODIN3_OK && id < md->n_nets; id++) {
        if (!reported(md, id)) {
            continue;
        }
        odin3_strbuf name;
        odin3_strbuf_init(&name);
        if (act_name_net(nm, id, &name) && name.len > 0 && strcmp(name.data, "unconn") != 0) {
            st = odin3_strbuf_appendf(&body, "%s %.9g %.9g\n", name.data, md->prob[id],
                                      md->dens[id]);
        }
        odin3_strbuf_free(&name);
    }
    st = st == ODIN3_OK ? save_file(path, &body) : st;
    odin3_strbuf_free(&body);
    return st;
}

static odin3_status put_net(odin3_strbuf *out, const act_model *md, act_namer *nm, uint32_t id) {
    odin3_strbuf name;
    odin3_strbuf_init(&name);
    (void)act_name_net(nm, id, &name);
    odin3_status st = odin3_strbuf_appendf(out, "{\"id\": %u, \"name\": ", (unsigned)id);
    st = st == ODIN3_OK ? gl_put_escaped(out, name.len > 0 ? name.data : "") : st;
    st = st == ODIN3_OK
             ? odin3_strbuf_appendf(out, ", \"p1\": %.9g, \"d\": %.9g, \"kind\": \"%s\"%s}",
                                    md->prob[id], md->dens[id], k_kind_names[md->kind[id]],
                                    md->clock[id] != 0 ? ", \"drives_clock\": true" : "")
             : st;
    odin3_strbuf_free(&name);
    return st;
}

static const char *pi_mode(const act_opts *opts) {
    if (opts->vcd.len > 0) {
        return "vcd";
    }
    return opts->pi_file.len > 0 ? "file" : "default";
}

odin3_status act_write_json(const act_model *md, act_namer *nm, const act_opts *opts,
                            const char *path, double ms) {
    odin3_strbuf body;
    odin3_strbuf_init(&body);
    odin3_status st = odin3_strbuf_appendf(
        &body,
        "{\n  \"schema\": \"odin3-activity\",\n  \"version\": %d,\n  \"source\": "
        "\"odin3-vectorless\",\n  \"inputs\": {\"mode\": \"%s\", \"pi_prob\": %.9g, "
        "\"pi_density\": %.9g, \"ff_start\": %.9g, \"tol\": %g, \"max_iter\": %u},\n  "
        "\"iterations\": %u, \"converged\": %s, \"ff_in_loops\": %llu, \"cone_capped\": %llu, "
        "\"opaque_cells\": "
        "%llu, \"wide_cells\": %llu, \"latches\": %llu, \"undriven_nets\": %llu, \"ms\": %.0f,\n  "
        "\"nets\": [",
        SCHEMA_VERSION, pi_mode(opts), opts->pi_prob, opts->pi_density, opts->ff_start, opts->tol,
        opts->max_iter, md->iterations, md->converged ? "true" : "false",
        (unsigned long long)md->ff_loop, (unsigned long long)md->cone_capped,
        (unsigned long long)md->opaque_cells, (unsigned long long)md->wide_cells,
        (unsigned long long)md->latches, (unsigned long long)md->undriven, ms);
    bool first = true;
    for (uint32_t id = 1; st == ODIN3_OK && id < md->n_nets; id++) {
        if (reported(md, id)) {
            st = odin3_strbuf_append(&body, odin3_bytes_cstr(first ? "\n    " : ",\n    "));
            st = st == ODIN3_OK ? put_net(&body, md, nm, id) : st;
            first = false;
        }
    }
    st = st == ODIN3_OK ? odin3_strbuf_append(&body, odin3_bytes_cstr("\n  ]\n}\n")) : st;
    st = st == ODIN3_OK ? save_file(path, &body) : st;
    odin3_strbuf_free(&body);
    return st;
}

static odin3_status set_int(odin3_module *mod, const char *key, int64_t num) {
    uint32_t k = 0;
    odin3_status st = odin3_design_intern(odin3_module_design(mod), odin3_bytes_cstr(key), &k);
    odin3_value val = odin3_value_int(num);
    return st == ODIN3_OK
               ? odin3_attr_set(mod, (odin3_objref){ODIN3_OBJ_MODULE, odin3_module_id_of(mod).v}, k,
                                &val)
               : st;
}

static int pi_mode_code(const act_opts *opts) {
    if (opts->vcd.len > 0) {
        return 2;
    }
    return opts->pi_file.len > 0 ? 1 : 0;
}

odin3_status act_write_attrs(const act_model *md, const act_opts *opts) {
    uint64_t nets = 0;
    for (uint32_t id = 1; id < md->n_nets; id++) {
        nets += reported(md, id) ? 1U : 0U;
    }
    const struct {
        const char *key;
        int64_t val;
    } table[] = {
        {"odin3:activity_version", SCHEMA_VERSION},
        {"odin3:activity_netlist_hash", (int64_t)(md->hash & INT64_MAX)},
        {"odin3:activity_nets", (int64_t)nets},
        {"odin3:activity_cells", (int64_t)md->n_cells},
        {"odin3:activity_opaque_cells", (int64_t)md->opaque_cells},
        {"odin3:activity_wide_cells", (int64_t)md->wide_cells},
        {"odin3:activity_iterations", md->iterations},
        {"odin3:activity_converged", md->converged ? 1 : 0},
        {"odin3:activity_pi_mode", pi_mode_code(opts)},
    };
    odin3_status st = ODIN3_OK;
    for (size_t k = 0; st == ODIN3_OK && k < sizeof table / sizeof table[0]; k++) {
        st = set_int(md->mod, table[k].key, table[k].val);
    }
    return st;
}
