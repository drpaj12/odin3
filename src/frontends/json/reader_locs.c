/*
 * reader_locs.c — the `src` restore of the JSON reader (spec 5.4 D-1, D-2, L-6, L-8, D-7): a
 * `src` attribute that parses as HDL locations becomes one DERIVED record (parents: the restored
 * SOURCE, then the IMPORTED record of the JSON value) and is not kept as an attribute.
 */
#include "frontends/hdl_restore.h"
#include "frontends/json/reader_internal.h"

#include <stdio.h>
#include <string.h>

/* The `src` member of an attributes object when it is a string. */
static const odin3_json_node *src_member(const odin3_json_node *attrs) {
    if (jr_kind(attrs) != ODIN3_JSON_OBJECT) {
        return NULL;
    }
    const odin3_json_node *src = jr_member(attrs, "src");
    return jr_kind(src) == ODIN3_JSON_STRING ? src : NULL;
}

bool jr_src_member_restorable(const jr *rd, const odin3_json_node *member) {
    if (rd->opts.keep_src_attr || jr_kind(member) != ODIN3_JSON_STRING) {
        return false;
    }
    size_t len = 0;
    const char *text = jr_text(member, &len);
    return odin3_hdl_loc_parse((odin3_bytes){text, len}, NULL, 0) > 0;
}

bool jr_src_restorable(const jr *rd, const odin3_json_node *attrs) {
    return jr_src_member_restorable(rd, src_member(attrs));
}

/* The shown prefix of an untrusted text: at most JR_BAD_SHOW bytes, never ending inside a UTF-8
 * sequence; every byte outside 0x20..0x7e is written as \xNN. */
static void escape_text(const char *text, size_t len, char *out) {
    size_t shown = len < JR_BAD_SHOW ? len : (size_t)JR_BAD_SHOW;
    while (shown < len && shown > 0 && ((unsigned char)text[shown] & 0xC0U) == 0x80U) {
        shown--; /* the cut would split a sequence: stop before it */
    }
    size_t at = 0;
    for (size_t i = 0; i < shown; i++) {
        unsigned char byte = (unsigned char)text[i];
        if (byte >= 0x20U && byte <= 0x7eU) {
            out[at++] = (char)byte;
        } else {
            at += (size_t)snprintf(out + at, 5, "\\x%02x", (unsigned)byte);
        }
    }
    out[at] = '\0';
}

/* L-6: counts a text that is no location list; the first one is kept for the report. */
static void note_bad(jr *rd, const odin3_json_node *src, const char *text, size_t len) {
    if (rd->n_bad_src == 0) {
        rd->bad_src_pos = odin3_json_key_pos(src);
        escape_text(text, len, rd->bad_src_text);
    }
    rd->n_bad_src++;
}

odin3_status jr_restore_src(jr *rd, const jr_restore_in *in, odin3_prov_id *out) {
    *out = in->base;
    const odin3_json_node *src = rd->opts.keep_src_attr ? NULL : src_member(in->attrs);
    if (src == NULL) {
        return ODIN3_OK;
    }
    size_t len = 0;
    const char *text = jr_text(src, &len);
    odin3_hdl_loc locs[ODIN3_HDL_LOC_MAX];
    uint32_t count = odin3_hdl_loc_parse((odin3_bytes){text, len}, locs, ODIN3_HDL_LOC_MAX);
    if (count == 0) {
        if (in->warn) {
            note_bad(rd, src, text, len);
        }
        return ODIN3_OK;
    }
    if (!rd->op_begun) {
        odin3_prov_begin_op(&rd->ctx); /* the one operation of every restored record (D-1) */
        rd->op_begun = true;
    }
    odin3_hdl_restore_req req = {in->base, locs,
                                 count < ODIN3_HDL_LOC_MAX ? count : ODIN3_HDL_LOC_MAX, 0};
    odin3_hdl_restored made = {0};
    odin3_status st = jr_fail(rd, odin3_hdl_restore(&rd->ctx, &req, &made));
    if (st == ODIN3_OK) {
        *out = made.prov;
        rd->n_capped_src += in->warn && count > ODIN3_HDL_LOC_MAX ? 1U : 0U;
    }
    return st;
}

/* --- ports (D-2) ---------------------------------------------------------------------------- */

odin3_status jr_port_srcs_open(jr *rd, const odin3_json_node *mod, jr_port_srcs *srcs) {
    *srcs = (jr_port_srcs){0};
    odin3_vec_init(&srcs->nodes, sizeof(const odin3_json_node *));
    if (rd->opts.keep_src_attr) {
        return ODIN3_OK;
    }
    const odin3_json_node *names = jr_member(mod, "netnames");
    srcs->by_name = odin3_u64map_create(0);
    if (srcs->by_name == NULL) {
        return jr_fail(rd, ODIN3_ERR_NO_MEMORY);
    }
    odin3_status st = ODIN3_OK;
    for (const odin3_json_node *name = jr_first(names); st == ODIN3_OK && name != NULL;
         name = odin3_json_next(name)) {
        if (!jr_src_restorable(rd, jr_member(name, "attributes"))) {
            continue;
        }
        uint32_t str = 0;
        const odin3_json_node **slot = (const odin3_json_node **)odin3_vec_push(&srcs->nodes);
        st = slot != NULL ? jr_intern(rd, odin3_json_key(name), &str)
                          : jr_fail(rd, ODIN3_ERR_NO_MEMORY);
        if (st == ODIN3_OK) {
            *slot = name;
            st = jr_fail(rd, odin3_u64map_put(srcs->by_name, (odin3_kv){str, srcs->nodes.len - 1}));
        }
    }
    return st;
}

void jr_port_srcs_close(jr_port_srcs *srcs) {
    odin3_u64map_destroy(srcs->by_name);
    odin3_vec_free(&srcs->nodes);
    srcs->by_name = NULL;
}

odin3_status jr_port_prov(jr *rd, const jr_port_srcs *srcs, const odin3_json_node *port,
                          odin3_prov_id base, odin3_prov_id *out) {
    *out = base;
    uint64_t index = 0;
    uint32_t str = 0;
    if (srcs->by_name == NULL) {
        return ODIN3_OK;
    }
    odin3_status st = jr_intern(rd, odin3_json_key(port), &str);
    if (st != ODIN3_OK || !odin3_u64map_get(srcs->by_name, str, &index)) {
        return st;
    }
    const odin3_json_node *name =
        *(const odin3_json_node *const *)odin3_vec_cat(&srcs->nodes, (size_t)index);
    return jr_restore_src(rd, &(jr_restore_in){jr_member(name, "attributes"), base, false}, out);
}

/* --- one-bit netnames (D-2) ----------------------------------------------------------------- */

/* The label of the bit of a one-bit netname that is an integer net; false otherwise. */
static bool one_bit_label(const odin3_json_node *name, uint64_t *label) {
    const odin3_json_node *bits = jr_member(name, "bits");
    const odin3_json_node *bit = jr_first(bits);
    if (jr_kind(bits) != ODIN3_JSON_ARRAY || jr_count(bits) != 1 ||
        jr_kind(bit) != ODIN3_JSON_INT || jr_int(bit) < 0) {
        return false;
    }
    *label = (uint64_t)jr_int(bit);
    return true;
}

static odin3_status collect_one(jr *rd, const odin3_json_node *name) {
    uint64_t label = 0;
    uint64_t have = 0;
    const odin3_json_node *attrs = jr_member(name, "attributes");
    if (!one_bit_label(name, &label) || !jr_src_restorable(rd, attrs) ||
        odin3_u64map_get(rd->cur.netloc, label, &have)) {
        return ODIN3_OK;
    }
    odin3_prov_id base = {0};
    odin3_prov_id prov = {0};
    odin3_status st = jr_prov(rd, name, &base);
    if (st == ODIN3_OK) {
        st = jr_restore_src(rd, &(jr_restore_in){attrs, base, false}, &prov);
    }
    if (st == ODIN3_OK) {
        st = jr_fail(rd, odin3_u64map_put(rd->cur.netloc, (odin3_kv){label, prov.v}));
    }
    return st;
}

odin3_status jr_collect_netlocs(jr *rd) {
    const odin3_json_node *names = jr_member(rd->cur.mod->node, "netnames");
    odin3_status st = ODIN3_OK;
    for (const odin3_json_node *name = jr_first(names); st == ODIN3_OK && name != NULL;
         name = odin3_json_next(name)) {
        st = collect_one(rd, name);
    }
    return st;
}

/* --- the report ----------------------------------------------------------------------------- */

void jr_report_src(const jr *rd) {
    if (rd->n_bad_src > 0) {
        odin3_log(ODIN3_LOG_WARN,
                  "%s:%u:%u: %u `src` attribute(s) are not location lists and stay attributes; "
                  "first: '%s'",
                  rd->path, (unsigned)rd->bad_src_pos.line, (unsigned)rd->bad_src_pos.col,
                  (unsigned)rd->n_bad_src, rd->bad_src_text);
    }
    if (rd->n_capped_src > 0) {
        odin3_log(ODIN3_LOG_INFO,
                  "%s: %u `src` text(s) hold more than %u locations; the first %u are kept",
                  rd->path, (unsigned)rd->n_capped_src, ODIN3_HDL_LOC_MAX, ODIN3_HDL_LOC_MAX);
    }
}
