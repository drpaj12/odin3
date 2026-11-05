/*
 * gv_common.c — JSON helpers of the write_graph pass (spec section 2: ids, `prov`, `src`).
 */
#include "passes/graphviews/gv.h"

#include "ir/prov.h"
#include "util/str.h"

#include <stdarg.h>
#include <stdio.h>

enum { FIRST_PRINTABLE = 0x20, GV_FMT_MAX = 512 };

odin3_status gv_put_str(odin3_strbuf *out, const char *text) {
    odin3_status st = odin3_strbuf_append(out, odin3_bytes_cstr("\""));
    for (const unsigned char *at = (const unsigned char *)text; st == ODIN3_OK && *at != 0; at++) {
        if (*at == '"' || *at == '\\') {
            st = odin3_strbuf_appendf(out, "\\%c", *at);
        } else if (*at < FIRST_PRINTABLE) {
            st = odin3_strbuf_appendf(out, "\\u%04x", (unsigned)*at);
        } else {
            st = odin3_strbuf_append(out, (odin3_bytes){at, 1});
        }
    }
    return st == ODIN3_OK ? odin3_strbuf_append(out, odin3_bytes_cstr("\"")) : st;
}

const char *gv_str(const gv_run *run, uint32_t str) {
    return str == 0 ? "" : odin3_strtab_get(odin3_design_strtab(run->design), str);
}

odin3_status gv_sep(odin3_strbuf *out, bool *first) {
    odin3_status st = *first ? ODIN3_OK : odin3_strbuf_append(out, odin3_bytes_cstr(", "));
    *first = false;
    return st;
}

odin3_status gv_put_prov(gv_run *run, odin3_strbuf *out, odin3_prov_id prov) {
    odin3_hdl_locs_out locs;
    odin3_status st = odin3_hdl_locs(run->hdl, prov, &locs);
    st = st == ODIN3_OK ? odin3_strbuf_appendf(out, "\"prov\": %u, \"src\": [", (unsigned)prov.v)
                        : st;
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    bool first = true;
    for (uint32_t i = 0; st == ODIN3_OK && i < locs.n; i++) {
        odin3_hdl_locs_out one = {{locs.locs[i]}, 1, false};
        odin3_hdl_print_req req = {&one, false};
        uint32_t written = 0;
        st = odin3_hdl_src_text(run->hdl, &req, &text, &written);
        if (st == ODIN3_OK && written > 0) {
            st = gv_sep(out, &first);
            st = st == ODIN3_OK ? gv_put_str(out, text.data) : st;
        }
    }
    odin3_strbuf_free(&text);
    return st == ODIN3_OK ? odin3_strbuf_append(out, odin3_bytes_cstr("]")) : st;
}

void gv_w_raw(gv_w *wr, const char *text) {
    if (wr->st == ODIN3_OK) {
        wr->st = odin3_strbuf_append(wr->out, odin3_bytes_cstr(text));
    }
}

void gv_w_fmt(gv_w *wr, const char *fmt, ...) {
    if (wr->st != ODIN3_OK) {
        return;
    }
    char buf[GV_FMT_MAX];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof buf, fmt, args);
    va_end(args);
    if (len < 0 || (size_t)len >= sizeof buf) {
        wr->st = ODIN3_ERR_INVALID_ARG;
        return;
    }
    wr->st = odin3_strbuf_append(wr->out, (odin3_bytes){(const uint8_t *)buf, (size_t)len});
}

void gv_w_str(gv_w *wr, const char *text) {
    if (wr->st == ODIN3_OK) {
        wr->st = gv_put_str(wr->out, text != NULL ? text : "");
    }
}

void gv_w_prov(gv_w *wr, odin3_prov_id prov) {
    if (wr->st == ODIN3_OK) {
        wr->st = gv_put_prov(wr->run, wr->out, prov);
    }
}
