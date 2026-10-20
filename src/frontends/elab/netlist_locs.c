/* netlist_locs.c — read_verilog -netlist; see netlist_locs.h. */
#include "frontends/elab/netlist_locs.h"

#include "ast/ast.h"
#include "ast/kinds.h"
#include "ast/srcman.h"
#include "frontends/hdl_restore.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "util/alloc.h"
#include "util/arena.h"
#include "util/log.h"
#include "util/str.h"
#include "util/u64map.h"

#include <stdio.h>
#include <string.h>

enum { NL_BAD_TAG = 0x4e4c42, NL_BLANKS = 2, NL_TAG_SHIFT = 32 };

/* --- stage 1: the comment of an assign ----------------------------------------------------------
 */

/* The index of the first comment of span whose loc is at or after at (binary search). */
static uint32_t first_from(odin3_ast_comment_span span, uint32_t at) {
    uint32_t lo = 0;
    uint32_t hi = span.n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2U;
        if (span.items[mid].loc.v < at) {
            lo = mid + 1U;
        } else {
            hi = mid;
        }
    }
    return lo;
}

/* The text of a line comment after `//` and blanks, trailing blanks cut. */
static odin3_bytes comment_body(const odin3_ast_comment *cm) {
    const char *text = cm->text;
    uint32_t len = cm->len;
    if (len >= NL_BLANKS && text[0] == '/' && text[1] == '/') {
        text += NL_BLANKS;
        len -= NL_BLANKS;
    }
    while (len > 0 && (*text == ' ' || *text == '\t')) {
        text++;
        len--;
    }
    while (len > 0 && (text[len - 1U] == ' ' || text[len - 1U] == '\t' || text[len - 1U] == '\r')) {
        len--;
    }
    return (odin3_bytes){(const unsigned char *)text, len};
}

/* Whether the comment starts on the line of the byte at last (both in one file buffer). */
static bool same_line(const odin3_srcman *sm, uint32_t last, odin3_loc comment) {
    odin3_srcpos here;
    odin3_srcpos there;
    return odin3_srcman_get_position(sm, last, &here) == ODIN3_OK &&
           odin3_srcman_get_position(sm, comment.v, &there) == ODIN3_OK &&
           here.buffer == there.buffer && here.line == there.line;
}

bool odin3_netlist_assign_comment(odin3_design *design, const odin3_ast *parsed, odin3_ast_id item,
                                  odin3_bytes *text) {
    odin3_srcman *sm = NULL;
    odin3_loc end = odin3_ast_end(parsed, item);
    if (end.v < 2U || odin3_design_get_srcman(design, &sm) != ODIN3_OK) {
        return false;
    }
    uint32_t last = end.v - 1U;
    odin3_ast_comment_span span =
        odin3_ast_comments_of(parsed, odin3_srcman_buffer_of(sm, (odin3_loc){last}));
    uint32_t at = first_from(span, end.v);
    if (at >= span.n || span.items[at].kind != ODIN3_AST_COMMENT_LINE ||
        !same_line(sm, last, span.items[at].loc)) {
        return false;
    }
    odin3_bytes body = comment_body(&span.items[at]);
    odin3_hdl_loc one;
    if (odin3_hdl_loc_parse(body, &one, 1) != 1U) { /* not exactly one LOC */
        return false;
    }
    *text = body;
    return true;
}

/* --- stage 2 ------------------------------------------------------------------------------------
 */

void odin3_netlist_begin(odin3_elab2 *e2) {
    if (e2->opts.netlist && e2->ctx->op == 0) {
        odin3_prov_begin_op(e2->ctx);
    }
}

odin3_status odin3_netlist_name_decode(odin3_elab2 *e2, uint32_t raw_str, odin3_netlist_name *out) {
    *out = (odin3_netlist_name){0};
    const char *raw = odin3_strtab_get(odin3_design_strtab(e2->design), raw_str);
    size_t len = raw != NULL ? strlen(raw) : 0;
    if (len == 0) {
        return ODIN3_OK;
    }
    char *copy = odin3_arena_alloc(e2->scratch, len + 1U);
    if (copy == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    memcpy(copy, raw, len + 1U);
    odin3_hdl_name name;
    if (!odin3_hdl_name_decode((odin3_bytes){(const unsigned char *)copy, len}, &name)) {
        return ODIN3_OK;
    }
    odin3_status st = odin3_design_intern(e2->design, name.short_name, &out->short_str);
    if (st == ODIN3_OK && name.hier.len > 0) {
        st = odin3_design_intern(e2->design, name.hier, &out->hier_str);
    }
    out->loc = name.loc;
    out->matched = st == ODIN3_OK;
    return st;
}

static uint64_t raw_key(const odin3_elab2 *e2, uint32_t str) {
    return ((uint64_t)e2->mod_id.v << NL_TAG_SHIFT) | str;
}

odin3_status odin3_netlist_note_raw(odin3_elab2 *e2, uint32_t str) {
    if (e2->nl_raw == NULL) {
        e2->nl_raw = odin3_u64map_create(0);
    }
    return e2->nl_raw == NULL ? ODIN3_ERR_NO_MEMORY
                              : odin3_u64map_put(e2->nl_raw, (odin3_kv){raw_key(e2, str), 1});
}

/* Whether the current module has a wire of that name, or a declarator spelled so (raw wins). */
static bool name_taken(const odin3_elab2 *e2, uint32_t str) {
    return odin3_module_find_wire(e2->mod, str).v != 0 ||
           (e2->nl_raw != NULL && odin3_u64map_get(e2->nl_raw, raw_key(e2, str), NULL));
}

odin3_status odin3_netlist_unique_wire(odin3_elab2 *e2, uint32_t short_str, uint32_t *name) {
    *name = short_str;
    if (!name_taken(e2, short_str)) {
        return ODIN3_OK;
    }
    const char *base = odin3_strtab_get(odin3_design_strtab(e2->design), short_str);
    for (uint32_t suffix = 1; suffix != 0; suffix++) {
        odin3_strbuf_clear(&e2->text);
        odin3_status st = odin3_strbuf_appendf(&e2->text, "%s$%u", base, suffix);
        st = st != ODIN3_OK ? st
                            : odin3_design_intern(e2->design,
                                                  (odin3_bytes){e2->text.data, e2->text.len}, name);
        if (st != ODIN3_OK || !name_taken(e2, *name)) {
            return st;
        }
    }
    return ODIN3_ERR_NO_MEMORY;
}

/* Whether attr is named `src` with a string value; *text is the value. */
static bool src_text(const odin3_elab2 *e2, odin3_ast_id attr, const char **text) {
    if (strcmp(odin3_ast_name_str(e2->elab, attr), "src") != 0) {
        return false;
    }
    odin3_ast_id value = odin3_ast_child(e2->elab, attr, 0);
    if (value.v == 0 || odin3_ast_kind_of(e2->elab, value) != ODIN3_AST_STRING) {
        return false;
    }
    *text = odin3_ast_name_str(e2->elab, value);
    return true;
}

/* L-6: counts a `src` that is not a location, once per attribute. */
static void note_bad(odin3_elab2 *e2, odin3_ast_id attr, const char *text) {
    uint64_t key = ((uint64_t)NL_BAD_TAG << NL_TAG_SHIFT) | attr.v;
    if (odin3_u64map_get(e2->warned, key, NULL)) {
        return;
    }
    if (odin3_u64map_put(e2->warned, (odin3_kv){key, 1}) != ODIN3_OK) {
        e2->fatal = e2->fatal != ODIN3_OK ? e2->fatal : ODIN3_ERR_NO_MEMORY;
        return;
    }
    if (e2->nl_bad++ == 0) {
        e2->nl_bad_loc = odin3_ast_loc(e2->elab, attr);
        (void)snprintf(e2->nl_bad_text, sizeof e2->nl_bad_text, "%s", text);
    }
}

bool odin3_netlist_src_consumed(odin3_elab2 *e2, odin3_ast_id attr) {
    const char *text = NULL;
    if (!e2->opts.netlist || !src_text(e2, attr, &text)) {
        return false;
    }
    odin3_hdl_loc locs[ODIN3_HDL_LOC_MAX];
    if (odin3_hdl_loc_parse(odin3_bytes_cstr(text), locs, ODIN3_HDL_LOC_MAX) > 0) {
        return true;
    }
    note_bad(e2, attr, text);
    return false;
}

/* The locations of the first parsing `src` attribute of node. */
static uint32_t node_src(const odin3_elab2 *e2, odin3_ast_id node, odin3_hdl_loc *locs,
                         uint32_t *total) {
    if (node.v == 0) {
        return 0;
    }
    odin3_ast_span attrs = odin3_ast_attrs(e2->elab, node);
    for (uint32_t i = 0; i < attrs.n; i++) {
        const char *text = NULL;
        if (src_text(e2, attrs.ids[i], &text)) {
            *total = odin3_hdl_loc_parse(odin3_bytes_cstr(text), locs, ODIN3_HDL_LOC_MAX);
            if (*total > 0) {
                return *total < ODIN3_HDL_LOC_MAX ? *total : ODIN3_HDL_LOC_MAX;
            }
        }
    }
    return 0;
}

odin3_status odin3_netlist_prov(odin3_elab2 *e2, const odin3_elab2_prov_req *req,
                                const odin3_elab2_nl_extra *nl, odin3_prov_id base,
                                odin3_prov_id *out) {
    *out = base;
    if (!e2->opts.netlist) {
        return ODIN3_OK;
    }
    odin3_hdl_loc locs[ODIN3_HDL_LOC_MAX];
    uint32_t total = 0;
    uint32_t count = 0;
    if (e2->asg_loc != NULL) {
        count = odin3_hdl_loc_parse(odin3_bytes_cstr(e2->asg_loc), locs, 1) > 0 ? 1U : 0U;
    } else {
        count = node_src(e2, req->node, locs, &total);
        count = count != 0 || nl == NULL ? count : node_src(e2, nl->attrs, locs, &total);
    }
    const odin3_netlist_name *name = nl != NULL ? nl->name : NULL;
    bool named = name != NULL && name->matched;
    if (count == 0 && named) {
        locs[0] = name->loc;
        count = 1;
    }
    if (count == 0) {
        return ODIN3_OK;
    }
    odin3_hdl_restore_req restore = {base, locs, count, named ? name->hier_str : 0};
    odin3_hdl_restored done;
    odin3_status st = odin3_hdl_restore(e2->ctx, &restore, &done);
    *out = st == ODIN3_OK ? done.prov : base;
    return st;
}

void odin3_netlist_assign_begin(odin3_elab2 *e2, odin3_ast_id cont_assign) {
    e2->asg_loc = NULL;
    odin3_ast_span attrs = odin3_ast_attrs(e2->elab, cont_assign);
    for (uint32_t i = 0; e2->opts.netlist && i < attrs.n; i++) {
        if (strcmp(odin3_ast_name_str(e2->elab, attrs.ids[i]), ODIN3_NETLIST_ASSIGN_KEY) == 0) {
            odin3_ast_id value = odin3_ast_child(e2->elab, attrs.ids[i], 0);
            e2->asg_loc = value.v != 0 ? odin3_ast_name_str(e2->elab, value) : NULL;
        }
    }
}

void odin3_netlist_assign_end(odin3_elab2 *e2) {
    e2->asg_loc = NULL;
}

void odin3_netlist_finish(odin3_elab2 *e2) {
    if (e2->nl_bad == 0) {
        return;
    }
    odin3_elab2_warn(e2, e2->nl_bad_loc,
                     "read_verilog -netlist: %u `src` attribute(s) are not locations and were kept "
                     "as attributes (first: \"%s\")",
                     e2->nl_bad, e2->nl_bad_text);
}
