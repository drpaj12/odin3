/*
 * ramstyle.c — G-3 (4.2 spec section 7): the attributes `ram_style`, `rom_style`, `ramstyle`,
 * `logic_block`, `block_ram` and `no_rw_check` on a memory are ignored with a warning; mapping
 * follows the architecture. This is the only place that names them: no rule reads an attribute's
 * value (the source grep in test_pm_ramstyle checks that).
 */
#include "ir/celltype.h"
#include "passes/partial_map/mem.h"
#include "util/log.h"

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static const char *const k_ramstyle_names[] = {"ram_style",   "rom_style", "ramstyle",
                                               "logic_block", "block_ram", "no_rw_check"};

enum { N_RAMSTYLE = (int)(sizeof k_ramstyle_names / sizeof k_ramstyle_names[0]) };

/* The attribute name without a leading RTLIL backslash. */
static const char *plain_name(const char *key) {
    return key[0] == '\\' ? key + 1 : key;
}

/* Equal ignoring case (ASCII). */
static bool same_name(const char *lhs, const char *rhs) {
    for (; *lhs != '\0' && *rhs != '\0'; lhs++, rhs++) {
        if (tolower((unsigned char)*lhs) != tolower((unsigned char)*rhs)) {
            return false;
        }
    }
    return *lhs == *rhs;
}

static bool is_ramstyle(const char *key) {
    for (int i = 0; i < N_RAMSTYLE; i++) {
        if (same_name(plain_name(key), k_ramstyle_names[i])) {
            return true;
        }
    }
    return false;
}

typedef struct warn_ctx {
    const hm_ctx *hc;
    odin3_node_id node;
    const char *pass;
    char label[ODIN3_LOG_BUF / 4];
} warn_ctx;

static odin3_status warn_attr(void *user, uint32_t key_str, const odin3_value *value) {
    (void)value;
    const warn_ctx *wc = user;
    const char *key = odin3_strtab_get(odin3_design_strtab(wc->hc->design), key_str);
    if (key != NULL && is_ramstyle(key)) {
        hm_diag(wc->hc, ODIN3_LOG_WARN, wc->node,
                "%s: memory '%s': attribute '%s' is ignored; mapping follows the architecture",
                wc->pass, wc->label, plain_name(key));
    }
    return ODIN3_OK;
}

/* The memory's name for messages: MEMID of a `$mem` without the RTLIL backslash, else the node's
 * label. */
static void memory_label(const hm_ctx *hc, odin3_node_id node, warn_ctx *wc) {
    const odin3_celltype_def *def =
        odin3_celltype_get(hc->design, odin3_node_type(hc->module, node));
    if (def != NULL && strcmp(def->name, "$mem") == 0) {
        const char *name = odin3_strtab_get(odin3_design_strtab(hc->design),
                                            odin3_node_param(hc->module, node, MP_MEMID)->str);
        if (name != NULL) {
            (void)snprintf(wc->label, sizeof wc->label, "%s", plain_name(name));
            return;
        }
    }
    hm_label(hc, node, wc->label, sizeof wc->label);
}

void pm_memattr_warn(const hm_ctx *hc, odin3_node_id node, const char *pass) {
    warn_ctx wc = {.hc = hc, .node = node, .pass = pass};
    memory_label(hc, node, &wc);
    (void)odin3_attr_foreach(hc->module, (odin3_objref){ODIN3_OBJ_NODE, node.v}, warn_attr, &wc);
}
