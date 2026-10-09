/*
 * node.c — the public ABI's node group: liveness, cell type, name, parameters (as integers and as
 * text), pins and ports.
 */
#include "api/api.h"

#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "util/alloc.h"
#include "util/attr.h"
#include "util/hash.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Enough for INT64_MIN in decimal and the NUL. */
enum { INT_TEXT_BUF = 24 };

/* A resolved node: its module, ID and cell-type definition. */
typedef struct node_at {
    const odin3_module *module;
    odin3_node_id id;
    const odin3_celltype_def *def;
} node_at;

/* Resolves node into *at; false when node does not name a node of the design. */
static bool resolve(const odin3_design *design, odin3_ref node, node_at *at) {
    const odin3_module *mod = odin3_api_ref(design, node, ODIN3_API_NODE);
    if (mod == NULL) {
        return false;
    }
    at->module = mod;
    at->id = (odin3_node_id){node.id};
    at->def = odin3_celltype_get(design, odin3_node_type(mod, at->id));
    return at->def != NULL;
}

/* resolve, and index is a parameter of the node's type. */
static bool resolve_param(const odin3_design *design, odin3_ref node, uint32_t index, node_at *at) {
    return resolve(design, node, at) && index < at->def->n_params;
}

/* resolve, and port is a port of the node's type. */
static bool resolve_port(const odin3_design *design, odin3_ref node, uint32_t port, node_at *at) {
    return resolve(design, node, at) && port < at->def->n_ports;
}

ODIN3_EXPORT odin3_status odin3_node_is_live(const odin3_design *design, odin3_ref node,
                                             bool *live) {
    const odin3_module *mod = odin3_api_ref(design, node, ODIN3_API_NODE);
    if (mod == NULL || live == NULL) {
        return odin3_api_invalid(__func__);
    }
    *live = odin3_node_live(mod, (odin3_node_id){node.id});
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_node_get_type_name(const odin3_design *design, odin3_ref node,
                                                   const char **name) {
    node_at at;
    if (!resolve(design, node, &at) || name == NULL) {
        return odin3_api_invalid(__func__);
    }
    *name = at.def->name;
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_node_get_granularity(const odin3_design *design, odin3_ref node,
                                                     odin3_granularity *gran) {
    node_at at;
    if (!resolve(design, node, &at) || gran == NULL) {
        return odin3_api_invalid(__func__);
    }
    *gran = at.def->gran;
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_node_get_name(const odin3_design *design, odin3_ref node,
                                              const char **name) {
    const odin3_module *mod = odin3_api_ref(design, node, ODIN3_API_NODE);
    if (mod == NULL || name == NULL) {
        return odin3_api_invalid(__func__);
    }
    *name = odin3_api_str(design, odin3_node_name(mod, (odin3_node_id){node.id}));
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_node_get_param_count(const odin3_design *design, odin3_ref node,
                                                     uint32_t *count) {
    node_at at;
    if (!resolve(design, node, &at) || count == NULL) {
        return odin3_api_invalid(__func__);
    }
    *count = at.def->n_params;
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_node_get_param_name(const odin3_design *design, odin3_ref node,
                                                    uint32_t index, const char **name) {
    node_at at;
    if (!resolve_param(design, node, index, &at) || name == NULL) {
        return odin3_api_invalid(__func__);
    }
    *name = at.def->params[index].name;
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_node_get_param_kind(const odin3_design *design, odin3_ref node,
                                                    uint32_t index, odin3_value_kind *kind) {
    node_at at;
    if (!resolve_param(design, node, index, &at) || kind == NULL) {
        return odin3_api_invalid(__func__);
    }
    *kind = odin3_node_param(at.module, at.id, index)->kind;
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_node_get_param_int(const odin3_design *design, odin3_ref node,
                                                   uint32_t index, int64_t *value) {
    node_at at;
    if (!resolve_param(design, node, index, &at) || value == NULL) {
        return odin3_api_invalid(__func__);
    }
    const odin3_value *val = odin3_node_param(at.module, at.id, index);
    if (val->kind != ODIN3_VAL_INT) {
        return odin3_api_invalid(__func__);
    }
    *value = val->i;
    return ODIN3_OK;
}

/* Characters of a 4-state bit (odin3_bit order). */
static const char k_bit_chars[] = "01xz";

/* Bytes of the text of a BITS or COVER value, without the NUL. */
static size_t text_len(const odin3_value *val) {
    if (val->kind == ODIN3_VAL_BITS) {
        return val->len;
    }
    size_t row = (size_t)val->cover_inputs + 1;
    size_t rows = val->len / row;
    return rows * (row + (val->cover_inputs > 0 ? 1 : 0) + 1); /* inputs, ' ', output, '\n' */
}

/* Writes the text of a BITS (MSB first) or COVER (BLIF rows) value into out. */
static void write_text(const odin3_value *val, char *out) {
    if (val->kind == ODIN3_VAL_BITS) {
        for (uint32_t k = 0; k < val->len; k++) {
            out[val->len - 1 - k] = k_bit_chars[val->bits[k] & 3U];
        }
        return;
    }
    const char *rows = (const char *)val->bits;
    size_t row = (size_t)val->cover_inputs + 1;
    size_t at = 0;
    for (size_t start = 0; start + row <= val->len; start += row) {
        for (size_t k = 0; k < val->cover_inputs; k++) {
            out[at++] = rows[start + k];
        }
        if (val->cover_inputs > 0) {
            out[at++] = ' ';
        }
        out[at++] = rows[start + val->cover_inputs];
        out[at++] = '\n';
    }
}

/* The text of a BITS or COVER value, interned in the design (see odin3_api_text). */
static odin3_status payload_text(const odin3_design *design, const odin3_value *val,
                                 const char **text) {
    size_t len = text_len(val);
    if (len == 0) {
        *text = "";
        return ODIN3_OK;
    }
    char *buf = odin3_util_malloc(len);
    if (buf == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    write_text(val, buf);
    odin3_status st = odin3_api_text(design, (odin3_bytes){buf, len}, text);
    odin3_util_free(buf);
    return st;
}

ODIN3_EXPORT odin3_status odin3_node_get_param_text(const odin3_design *design, odin3_ref node,
                                                    uint32_t index, const char **text) {
    node_at at;
    if (!resolve_param(design, node, index, &at) || text == NULL) {
        return odin3_api_invalid(__func__);
    }
    const odin3_value *val = odin3_node_param(at.module, at.id, index);
    if (val->kind == ODIN3_VAL_STRING) {
        *text = odin3_api_str(design, val->str);
        return ODIN3_OK;
    }
    if (val->kind != ODIN3_VAL_INT) {
        return payload_text(design, val, text);
    }
    char buf[INT_TEXT_BUF];
    int len = snprintf(buf, sizeof buf, "%" PRId64, val->i);
    return odin3_api_text(design, (odin3_bytes){buf, (size_t)len}, text);
}

ODIN3_EXPORT odin3_status odin3_node_get_pins(const odin3_design *design, odin3_ref node,
                                              odin3_span *pins) {
    const odin3_module *mod = odin3_api_ref(design, node, ODIN3_API_NODE);
    if (mod == NULL || pins == NULL) {
        return odin3_api_invalid(__func__);
    }
    odin3_pinslice slice = odin3_node_pins(mod, (odin3_node_id){node.id});
    *pins = (odin3_span){slice.first.v, slice.count};
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_node_get_port_count(const odin3_design *design, odin3_ref node,
                                                    uint32_t *count) {
    node_at at;
    if (!resolve(design, node, &at) || count == NULL) {
        return odin3_api_invalid(__func__);
    }
    *count = at.def->n_ports;
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_node_get_port_pins(const odin3_design *design, odin3_ref node,
                                                   uint32_t port, odin3_span *pins) {
    node_at at;
    if (!resolve_port(design, node, port, &at) || pins == NULL) {
        return odin3_api_invalid(__func__);
    }
    odin3_pinslice slice = odin3_node_port(at.module, at.id, port);
    *pins = (odin3_span){slice.count > 0 ? slice.first.v : 0, slice.count};
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_node_get_port_name(const odin3_design *design, odin3_ref node,
                                                   uint32_t port, const char **name) {
    node_at at;
    if (!resolve_port(design, node, port, &at) || name == NULL) {
        return odin3_api_invalid(__func__);
    }
    *name = at.def->ports[port].name;
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_node_get_port_dir(const odin3_design *design, odin3_ref node,
                                                  uint32_t port, odin3_dir *dir) {
    node_at at;
    if (!resolve_port(design, node, port, &at) || dir == NULL) {
        return odin3_api_invalid(__func__);
    }
    *dir = at.def->ports[port].dir;
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_node_get_port_width(const odin3_design *design, odin3_ref node,
                                                    uint32_t port, uint32_t *width) {
    node_at at;
    if (!resolve_port(design, node, port, &at) || width == NULL) {
        return odin3_api_invalid(__func__);
    }
    *width = odin3_node_port(at.module, at.id, port).count;
    return ODIN3_OK;
}
