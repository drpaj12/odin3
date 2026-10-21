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

enum {
    NL_BAD_TAG = 0x4e4c42,
    NL_TAKEN_TAG = 0x4e4c54,
    NL_CAP_TAG = 0x4e4c43,
    NL_BLANKS = 2,
    NL_TAG_SHIFT = 32,
    NL_PRINT_FIRST = 0x20,
    NL_PRINT_LAST = 0x7e,
    NL_HEX_ESC = 4 /* the bytes of `\\xNN` */
};

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

/* Whether the item that follows starts before the comment on the same line: a token lies between
 * the assign and the comment, so the comment is not the assign's. */
static bool item_between(const odin3_srcman *sm, const odin3_ast *parsed, odin3_ast_id next,
                         odin3_loc comment) {
    odin3_srcpos there;
    odin3_srcpos after;
    if (next.v == 0 || odin3_srcman_get_position(sm, comment.v, &there) != ODIN3_OK ||
        odin3_srcman_get_position(sm, odin3_ast_loc(parsed, next).v, &after) != ODIN3_OK) {
        return false;
    }
    return after.buffer == there.buffer && after.line == there.line && after.col < there.col;
}

bool odin3_netlist_assign_comment(odin3_design *design, const odin3_ast *parsed,
                                  const odin3_netlist_assign_src *src, odin3_bytes *text) {
    odin3_srcman *sm = NULL;
    odin3_loc end = odin3_ast_end(parsed, src->item);
    if (end.v < 2U || odin3_design_get_srcman(design, &sm) != ODIN3_OK) {
        return false;
    }
    uint32_t last = end.v - 1U;
    odin3_ast_comment_span span =
        odin3_ast_comments_of(parsed, odin3_srcman_buffer_of(sm, (odin3_loc){last}));
    uint32_t at = first_from(span, end.v);
    if (at >= span.n || span.items[at].kind != ODIN3_AST_COMMENT_LINE ||
        !same_line(sm, last, span.items[at].loc) ||
        item_between(sm, parsed, src->next, span.items[at].loc)) {
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

/* H-6: the full name of the hash name raw in the current module when the map holds it, else raw
 * (a hash-shaped name the map lacks is counted). */
static odin3_bytes resolve_hash(odin3_elab2 *e2, odin3_bytes raw, odin3_loc at) {
    if (e2->opts.names == NULL || !odin3_name_hash_shaped(raw)) {
        return raw;
    }
    const odin3_strtab *tab = odin3_design_strtab(e2->design);
    odin3_bytes module = {odin3_strtab_get(tab, odin3_module_name(e2->mod)),
                          odin3_strtab_len(tab, odin3_module_name(e2->mod))};
    odin3_bytes full = {NULL, 0};
    if (odin3_name_resolver_find_decl(e2->opts.names, module, raw, &full)) {
        return full;
    }
    if (e2->nl_miss.count == 0) {
        e2->nl_miss_loc = at;
    }
    odin3_name_miss_note(&e2->nl_miss, raw, 0);
    return raw;
}

odin3_status odin3_netlist_name_decode(odin3_elab2 *e2, uint32_t raw_str, odin3_loc at,
                                       odin3_netlist_name *out) {
    *out = (odin3_netlist_name){0};
    const odin3_strtab *tab = odin3_design_strtab(e2->design);
    size_t len = odin3_strtab_len(tab, raw_str);
    if (len == 0) {
        return ODIN3_OK;
    }
    /* the table never moves a string: the decoded views stay valid while the name is interned */
    odin3_bytes raw = {(const unsigned char *)odin3_strtab_get(tab, raw_str), len};
    raw = resolve_hash(e2, raw, at);
    odin3_hdl_name name;
    if (!odin3_hdl_name_decode(raw, &name)) {
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

/* Whether the key was new in the warned map (false: seen before, or out of memory: e2->fatal). */
static bool first_time(odin3_elab2 *e2, uint32_t tag, odin3_ast_id attr) {
    uint64_t key = ((uint64_t)tag << NL_TAG_SHIFT) | attr.v;
    if (odin3_u64map_get(e2->warned, key, NULL)) {
        return false;
    }
    if (odin3_u64map_put(e2->warned, (odin3_kv){key, 1}) != ODIN3_OK) {
        e2->fatal = e2->fatal != ODIN3_OK ? e2->fatal : ODIN3_ERR_NO_MEMORY;
        return false;
    }
    return true;
}

/* Copies text into the first-example buffer: a byte outside 0x20..0x7e is written as `\xNN`, and
 * the copy stops before an escape that does not fit, so no UTF-8 sequence is cut in two. */
static void copy_escaped(char *out, size_t cap, const char *text) {
    size_t used = 0;
    for (const unsigned char *at = (const unsigned char *)text; *at != 0; at++) {
        bool plain = *at >= NL_PRINT_FIRST && *at <= NL_PRINT_LAST;
        if (used + (plain ? 1U : NL_HEX_ESC) >= cap) {
            break;
        }
        used += plain ? (size_t)snprintf(out + used, cap - used, "%c", *at)
                      : (size_t)snprintf(out + used, cap - used, "\\x%02x", (unsigned)*at);
    }
    out[used] = '\0';
}

/* L-6: counts a `src` that is not a location, once per attribute. */
static void note_bad(odin3_elab2 *e2, odin3_ast_id attr, const char *text) {
    if (!first_time(e2, NL_BAD_TAG, attr)) {
        return;
    }
    if (e2->nl_bad++ == 0) {
        e2->nl_bad_loc = odin3_ast_loc(e2->elab, attr);
        copy_escaped(e2->nl_bad_text, sizeof e2->nl_bad_text, text);
    }
}

bool odin3_netlist_src_consumed(odin3_elab2 *e2, odin3_ast_id attr) {
    const char *text = NULL;
    if (!e2->opts.netlist || !src_text(e2, attr, &text)) {
        return false;
    }
    odin3_hdl_loc locs[ODIN3_HDL_LOC_MAX];
    if (odin3_hdl_loc_parse(odin3_bytes_cstr(text), locs, ODIN3_HDL_LOC_MAX) > 0) {
        /* a src is dropped only when a record took it (an assign comment, or an earlier src of
         * the same object, wins and leaves it as an attribute) */
        return odin3_u64map_get(e2->warned, ((uint64_t)NL_TAKEN_TAG << NL_TAG_SHIFT) | attr.v,
                                NULL);
    }
    note_bad(e2, attr, text);
    return false;
}

/* The locations of the first parsing `src` attribute of node; *took is that attribute. */
static uint32_t node_src(odin3_elab2 *e2, odin3_ast_id node, odin3_hdl_loc *locs,
                         odin3_ast_id *took) {
    if (node.v == 0) {
        return 0;
    }
    odin3_ast_span attrs = odin3_ast_attrs(e2->elab, node);
    for (uint32_t i = 0; i < attrs.n; i++) {
        const char *text = NULL;
        if (!src_text(e2, attrs.ids[i], &text)) {
            continue;
        }
        uint32_t total = odin3_hdl_loc_parse(odin3_bytes_cstr(text), locs, ODIN3_HDL_LOC_MAX);
        if (total > 0) {
            *took = attrs.ids[i];
            if (total > ODIN3_HDL_LOC_MAX && first_time(e2, NL_CAP_TAG, *took)) {
                e2->nl_capped++; /* the first 8 are kept; one INFO line per read (X-3) */
            }
            return total < ODIN3_HDL_LOC_MAX ? total : ODIN3_HDL_LOC_MAX;
        }
    }
    return 0;
}

/* The locations the record of an object takes (assign comment, `src`, or decoded name): how many;
 * *took is the `src` attribute used, if any. */
static uint32_t prov_locs(odin3_elab2 *e2, const odin3_elab2_prov_req *req,
                          const odin3_elab2_nl_extra *nl, odin3_hdl_loc *locs, odin3_ast_id *took) {
    if (e2->asg_loc != NULL) {
        return odin3_hdl_loc_parse(odin3_bytes_cstr(e2->asg_loc), locs, 1) > 0 ? 1U : 0U;
    }
    uint32_t count = node_src(e2, req->node, locs, took);
    count = count != 0 || nl == NULL ? count : node_src(e2, nl->attrs, locs, took);
    if (count == 0 && nl != NULL && nl->name != NULL && nl->name->matched) {
        locs[0] = nl->name->loc;
        count = 1;
    }
    return count;
}

odin3_status odin3_netlist_prov(odin3_elab2 *e2, const odin3_elab2_prov_req *req,
                                const odin3_elab2_nl_extra *nl, odin3_prov_id base,
                                odin3_prov_id *out) {
    *out = base;
    if (!e2->opts.netlist) {
        return ODIN3_OK;
    }
    odin3_hdl_loc locs[ODIN3_HDL_LOC_MAX];
    odin3_ast_id took = {0};
    uint32_t count = prov_locs(e2, req, nl, locs, &took);
    if (count == 0) {
        return ODIN3_OK;
    }
    bool named = nl != NULL && nl->name != NULL && nl->name->matched;
    odin3_hdl_restore_req restore = {base, locs, count, named ? nl->name->hier_str : 0};
    odin3_hdl_restored done;
    odin3_status st = odin3_hdl_restore(e2->ctx, &restore, &done);
    *out = st == ODIN3_OK ? done.prov : base;
    if (st == ODIN3_OK && took.v != 0) {
        (void)first_time(e2, NL_TAKEN_TAG,
                         took); /* the record took it: not stored as an attribute */
    }
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
    if (e2->nl_capped > 0) {
        odin3_log(ODIN3_LOG_INFO,
                  "read_verilog -netlist: %u `src` attribute(s) hold more than %u locations; the "
                  "first %u of each were kept",
                  e2->nl_capped, (unsigned)ODIN3_HDL_LOC_MAX, (unsigned)ODIN3_HDL_LOC_MAX);
    }
    if (e2->nl_miss.count > 0) {
        odin3_elab2_warn(
            e2, e2->nl_miss_loc,
            "read_verilog -netlist: %u hash-shaped name(s) are not in the name map and "
            "stay as written (first: '%s')",
            e2->nl_miss.count, e2->nl_miss.first);
    }
    if (e2->nl_bad == 0) {
        return;
    }
    odin3_elab2_warn(e2, e2->nl_bad_loc,
                     "read_verilog -netlist: %u `src` attribute(s) are not locations and were kept "
                     "as attributes (first: \"%s\")",
                     e2->nl_bad, e2->nl_bad_text);
}
