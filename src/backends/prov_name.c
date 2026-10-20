/* prov_name.c — provenance names for writers; see prov_name.h. */
#include "backends/prov_name.h"

#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>

odin3_status odin3_prov_name(odin3_hdl_ctx *hdl, odin3_prov_id prov, odin3_bytes short_name,
                             odin3_strbuf *out) {
    odin3_strbuf_clear(out);
    const odin3_prov_record *rec = NULL;
    odin3_status st = odin3_hdl_first_record(hdl, prov, &rec);
    if (st != ODIN3_OK || rec == NULL) {
        return st;
    }
    const odin3_strtab *tab = odin3_design_strtab(odin3_hdl_ctx_design(hdl));
    if (rec->hier != 0) {
        st = odin3_strbuf_appendf(out, "%s/", odin3_strtab_get(tab, rec->hier));
    }
    st = st == ODIN3_OK ? odin3_strbuf_append(out, short_name) : st;
    if (st == ODIN3_OK) {
        st = odin3_strbuf_appendf(out, "@%s:%u", odin3_strtab_get(tab, rec->locs[0].file),
                                  (unsigned)rec->locs[0].line);
    }
    return st;
}

/* True when the len bytes of tail, with `/` read as `.`, start name and a `.` follows.
 */
static bool suffix_starts(const char *tail, size_t len, const char *name) {
    for (size_t i = 0; i < len; i++) {
        char want = tail[i];
        if (want == '/') {
            want = '.';
        }
        if (want != name[i]) {
            return false;
        }
    }
    return name[len] == '.';
}

size_t odin3_prov_hier_overlap(odin3_bytes hier, odin3_bytes short_name) {
    for (size_t start = 0; start < hier.len; start++) {
        size_t len = hier.len - start;
        bool at_component = start == 0 || ((const char *)hier.ptr)[start - 1] == '/';
        if (at_component && short_name.len > len + 1 &&
            suffix_starts((const char *)hier.ptr + start, len, short_name.ptr)) {
            return len + 1;
        }
    }
    return 0;
}
