/* hdl_loc.c — HDL location text, names and the writers' location filter; see hdl_loc.h. */
#include "backends/common/hdl_loc.h"

#include "util/alloc.h"
#include "util/log.h"

#include <string.h>

enum {
    FIRST_PRINTABLE = 0x20,
    LAST_PRINTABLE = 0x7e,
    DECIMAL = 10,
    HEX_DIGITS = 2,
    HEX_BASE = 16,
    HEX_ALPHA = 10,
    NUM_MAX_FORM = 4, /* the numbers of `L.C-EL.EC` */
    WARN_KIND_SHIFT = 32
};

/* --- parsing -----------------------------------------------------------------------------------
 */

static bool is_digit(unsigned char chr) {
    return chr >= '0' && chr <= '9';
}

/* The digits of text[0..len) as a uint32_t; false when empty, not all digits, or over UINT32_MAX.
 */
static bool number_of(const unsigned char *text, size_t len, uint32_t *value) {
    uint64_t acc = 0;
    if (len == 0) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!is_digit(text[i])) {
            return false;
        }
        acc = acc * DECIMAL + (uint64_t)(text[i] - '0');
        if (acc > UINT32_MAX) {
            return false;
        }
    }
    *value = (uint32_t)acc;
    return true;
}

/* Reads the run of digits that ends just before *pos as a number and moves *pos to its start. */
static bool number_back(const unsigned char *text, size_t *pos, uint32_t *value) {
    size_t start = *pos;
    while (start > 0 && is_digit(text[start - 1])) {
        start--;
    }
    if (!number_of(text + start, *pos - start, value)) {
        return false;
    }
    *pos = start;
    return true;
}

/* After `count` numbers read from the right: whether ch ends the form (':') or continues it. */
typedef enum tail_step { TAIL_FAIL, TAIL_MORE, TAIL_END } tail_step;

/* The separator that may follow the count-th number from the right: ':' ends the form after 1, 2
 * or 4 numbers; '.', '-' and '.' continue it after 1, 2 and 3. */
static const char TAIL_END_AT[NUM_MAX_FORM + 1] = {0, ':', ':', 0, ':'};
static const char TAIL_MORE_AT[NUM_MAX_FORM + 1] = {0, '.', '-', '.', 0};

typedef struct tail_pos {
    uint32_t count;     /* numbers read so far, 1..NUM_MAX_FORM */
    unsigned char next; /* the byte before them */
} tail_pos;

static tail_step tail_step_of(tail_pos pos) {
    if (TAIL_END_AT[pos.count] == (char)pos.next) {
        return TAIL_END;
    }
    return TAIL_MORE_AT[pos.count] == (char)pos.next ? TAIL_MORE : TAIL_FAIL;
}

/* Stores the numbers read right to left (nums[0] is the rightmost) as the fields of the form. */
static void assign_form(const uint32_t *nums, uint32_t count, odin3_hdl_loc *loc) {
    loc->col = loc->end_line = loc->end_col = 0;
    if (count == 1) {
        loc->line = nums[0];
    } else if (count == 2) {
        loc->line = nums[1];
        loc->col = nums[0];
    } else {
        loc->line = nums[3];
        loc->col = nums[2];
        loc->end_line = nums[1];
        loc->end_col = nums[0];
    }
}

/* L-2: parses text[0..len) as one LOC from the right. */
static bool parse_loc(const unsigned char *text, size_t len, odin3_hdl_loc *loc) {
    uint32_t nums[NUM_MAX_FORM] = {0};
    uint32_t count = 0;
    size_t pos = len;
    tail_step step = TAIL_MORE;
    while (step == TAIL_MORE) {
        if (count == NUM_MAX_FORM || !number_back(text, &pos, &nums[count]) || pos == 0) {
            return false;
        }
        count++;
        step = tail_step_of((tail_pos){count, text[pos - 1]});
        pos--;
    }
    if (step != TAIL_END || pos == 0) { /* pos is the colon: an empty file is no location */
        return false;
    }
    assign_form(nums, count, loc);
    loc->file = (odin3_bytes){text, pos};
    return loc->line >= 1;
}

uint32_t odin3_hdl_loc_parse(odin3_bytes text, odin3_hdl_loc *out, uint32_t cap) {
    const unsigned char *bytes = text.ptr;
    odin3_hdl_loc loc = {{NULL, 0}, 0, 0, 0, 0};
    uint32_t count = 0;
    size_t start = 0;
    for (size_t i = 0; i < text.len; i++) {
        if (bytes[i] == '|' && i + 1 < text.len && parse_loc(bytes + start, i - start, &loc)) {
            if (count < cap) {
                out[count] = loc;
            }
            count++;
            start = i + 1;
        }
    }
    if (text.len == 0 || !parse_loc(bytes + start, text.len - start, &loc)) {
        return 0;
    }
    if (count < cap) {
        out[count] = loc;
    }
    return count + 1;
}

/* --- printing ----------------------------------------------------------------------------------
 */

static void truncate_to(odin3_strbuf *out, size_t len) {
    out->len = len;
    if (out->data != NULL) {
        out->data[len] = '\0';
    }
}

static odin3_status print_one(odin3_strbuf *out, const odin3_hdl_loc *loc) {
    odin3_status st = odin3_strbuf_append(out, loc->file);
    if (st != ODIN3_OK) {
        return st;
    }
    if (loc->end_line != 0) {
        return odin3_strbuf_appendf(out, ":%u.%u-%u.%u", (unsigned)loc->line, (unsigned)loc->col,
                                    (unsigned)loc->end_line, (unsigned)loc->end_col);
    }
    if (loc->col != 0) {
        return odin3_strbuf_appendf(out, ":%u.%u", (unsigned)loc->line, (unsigned)loc->col);
    }
    return odin3_strbuf_appendf(out, ":%u", (unsigned)loc->line);
}

odin3_status odin3_hdl_loc_print(const odin3_hdl_loc *locs, uint32_t n, odin3_strbuf *out) {
    size_t start = out->len;
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < n; i++) {
        st = i > 0 ? odin3_strbuf_append(out, odin3_bytes_cstr("|")) : ODIN3_OK;
        st = st == ODIN3_OK ? print_one(out, &locs[i]) : st;
    }
    if (st != ODIN3_OK) {
        truncate_to(out, start);
    }
    return st;
}

/* --- names -------------------------------------------------------------------------------------
 */

/* The writer's `$N` at the end of tail[0..len): the length before it, else len. */
static size_t strip_suffix(const unsigned char *tail, size_t len, odin3_hdl_name *out) {
    size_t at = len;
    while (at > 0 && is_digit(tail[at - 1])) {
        at--;
    }
    if (at == len || at < 2 || tail[at - 1] != '$' || !is_digit(tail[at - 2])) {
        return len;
    }
    uint32_t suffix = 0;
    if (!number_of(tail + at, len - at, &suffix)) {
        return len;
    }
    out->suffix = suffix;
    out->has_suffix = true;
    return at - 1;
}

bool odin3_hdl_name_decode(odin3_bytes name, odin3_hdl_name *out) {
    const unsigned char *bytes = name.ptr;
    size_t at = name.len;
    while (at > 0 && bytes[at - 1] != '@') {
        at--;
    }
    if (at < 2) { /* no `@`, or nothing before it */
        return false;
    }
    at--; /* the `@` */
    size_t slash = at;
    while (slash > 0 && bytes[slash - 1] != '/') {
        slash--;
    }
    if (slash == at) { /* an empty short name */
        return false;
    }
    odin3_hdl_name found = {{NULL, 0}, {NULL, 0}, {{NULL, 0}, 0, 0, 0, 0}, 0, false};
    size_t tail_len = strip_suffix(bytes + at + 1, name.len - at - 1, &found);
    if (!parse_loc(bytes + at + 1, tail_len, &found.loc) || found.loc.col != 0 ||
        found.loc.end_line != 0) {
        return false;
    }
    found.hier = (odin3_bytes){bytes, slash > 0 ? slash - 1 : 0};
    found.short_name = (odin3_bytes){bytes + slash, at - slash};
    *out = found;
    return true;
}

static bool upper_hex(unsigned char chr, unsigned *value) {
    if (is_digit(chr)) {
        *value = (unsigned)(chr - '0');
        return true;
    }
    if (chr >= 'A' && chr <= 'F') {
        *value = (unsigned)(chr - 'A') + HEX_ALPHA;
        return true;
    }
    return false;
}

/* True, and the byte, when text[at..) is `%` and two upper-case hex digits. */
static bool escape_at(const unsigned char *text, size_t len, size_t at, unsigned char *byte) {
    unsigned high = 0;
    unsigned low = 0;
    if (at + HEX_DIGITS >= len) {
        return false;
    }
    if (text[at] != '%' || !upper_hex(text[at + 1], &high) || !upper_hex(text[at + 2], &low)) {
        return false;
    }
    *byte = (unsigned char)(high * HEX_BASE + low);
    return true;
}

/* buf = name with VB-10 undone one step (see the header). */
static odin3_status unescape(odin3_bytes name, odin3_strbuf *buf) {
    const unsigned char *bytes = name.ptr;
    unsigned char byte = 0;
    size_t run = 0;
    size_t i = 0;
    odin3_status st = ODIN3_OK;
    if (name.len > 0 && bytes[0] == '%' && !escape_at(bytes, name.len, 0, &byte)) {
        run = i = 1; /* the reserved prefix */
    }
    while (st == ODIN3_OK && i < name.len) {
        if (!escape_at(bytes, name.len, i, &byte)) {
            i++;
            continue;
        }
        st = odin3_strbuf_append(buf, (odin3_bytes){bytes + run, i - run});
        st = st == ODIN3_OK ? odin3_strbuf_append(buf, (odin3_bytes){&byte, 1}) : st;
        i += HEX_DIGITS + 1;
        run = i;
    }
    return st == ODIN3_OK ? odin3_strbuf_append(buf, (odin3_bytes){bytes + run, name.len - run})
                          : st;
}

odin3_status odin3_hdl_name_vtr_decode(odin3_bytes name, odin3_strbuf *buf, odin3_hdl_name *out,
                                       bool *matched) {
    odin3_strbuf_clear(buf);
    *matched = false;
    odin3_status st = unescape(name, buf);
    if (st == ODIN3_OK && buf->len > 0) {
        *matched = odin3_hdl_name_decode((odin3_bytes){buf->data, buf->len}, out);
    }
    return st;
}

/* --- the object's HDL locations ----------------------------------------------------------------
 */

struct odin3_hdl_ctx {
    const odin3_design *design;
    const char *who;
    odin3_u64map *first;  /* prov ID -> the first HDL leaf (record ID), 0 for none */
    odin3_u64map *warned; /* file | kind << 32 -> 1 */
};

odin3_hdl_ctx *odin3_hdl_ctx_create(const odin3_design *design, const char *who) {
    odin3_hdl_ctx *ctx = odin3_util_calloc(sizeof *ctx);
    if (ctx == NULL) {
        return NULL;
    }
    ctx->design = design;
    ctx->who = who;
    ctx->first = odin3_u64map_create(0);
    ctx->warned = odin3_u64map_create(0);
    if (ctx->first == NULL || ctx->warned == NULL) {
        odin3_hdl_ctx_destroy(ctx);
        return NULL;
    }
    return ctx;
}

void odin3_hdl_ctx_destroy(odin3_hdl_ctx *ctx) {
    if (ctx == NULL) {
        return;
    }
    odin3_u64map_destroy(ctx->first);
    odin3_u64map_destroy(ctx->warned);
    odin3_util_free(ctx);
}

bool odin3_hdl_file_printable(const odin3_design *design, uint32_t file) {
    const odin3_strtab *tab = odin3_design_strtab(design);
    const unsigned char *bytes = (const unsigned char *)odin3_strtab_get(tab, file);
    size_t len = odin3_strtab_len(tab, file);
    if (file == 0 || bytes == NULL || len == 0) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (bytes[i] < FIRST_PRINTABLE || bytes[i] > LAST_PRINTABLE) {
            return false;
        }
    }
    return true;
}

/* One warning per (file, why): `<who>: source file name '<name>' <text>`, the name's odd bytes
 * shown as `?`. NO_MEMORY from the table of warned files. */
static odin3_status warn_file(odin3_hdl_ctx *ctx, uint32_t file, uint32_t why, const char *text) {
    uint64_t key = (uint64_t)file | ((uint64_t)why << WARN_KIND_SHIFT);
    if (odin3_u64map_get(ctx->warned, key, NULL)) {
        return ODIN3_OK;
    }
    if (odin3_u64map_put(ctx->warned, (odin3_kv){key, 1}) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    const odin3_strtab *tab = odin3_design_strtab(ctx->design);
    const unsigned char *bytes = (const unsigned char *)odin3_strtab_get(tab, file);
    size_t len = odin3_strtab_len(tab, file);
    odin3_strbuf shown;
    odin3_strbuf_init(&shown);
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < len; i++) {
        char chr = '?';
        if (bytes[i] >= FIRST_PRINTABLE && bytes[i] <= LAST_PRINTABLE) {
            chr = (char)bytes[i];
        }
        st = odin3_strbuf_append(&shown, (odin3_bytes){&chr, 1});
    }
    if (st == ODIN3_OK) {
        odin3_log(ODIN3_LOG_WARN, "%s: source file name '%s' %s", ctx->who,
                  shown.data != NULL ? shown.data : "", text);
    }
    odin3_strbuf_free(&shown);
    return st;
}

enum { WHY_BYTES = 1, WHY_TEXT = 2 };

/* The state of one backward walk. */
typedef struct collect {
    odin3_hdl_ctx *ctx;
    odin3_hdl_locs_out *out; /* NULL: only the first leaf is wanted */
    uint32_t first_leaf;     /* record ID of the first counted leaf, 0 for none yet */
    odin3_status st;
} collect;

static bool same_loc(const odin3_srcloc *lhs, const odin3_srcloc *rhs) {
    return lhs->file == rhs->file && lhs->line == rhs->line && lhs->col == rhs->col &&
           lhs->end_line == rhs->end_line && lhs->end_col == rhs->end_col;
}

static void add_loc(collect *col, const odin3_srcloc *loc) {
    odin3_hdl_locs_out *out = col->out;
    for (uint32_t i = 0; i < out->n; i++) {
        if (same_loc(&out->locs[i], loc)) {
            return;
        }
    }
    if (out->n == ODIN3_HDL_LOC_MAX) {
        out->capped = true;
        return;
    }
    out->locs[out->n++] = *loc;
}

/* A file with a byte outside 0x20..0x7e is warned about (once) and not counted. */
static bool usable_file(collect *col, uint32_t file) {
    if (file == 0) {
        return false;
    }
    if (odin3_hdl_file_printable(col->ctx->design, file)) {
        return true;
    }
    odin3_status st = warn_file(col->ctx, file, WHY_BYTES,
                                "cannot be written (a byte outside 0x20..0x7e); its locations are "
                                "skipped");
    col->st = col->st != ODIN3_OK ? col->st : st;
    return false;
}

static void visit_leaf(void *user, odin3_prov_id leaf) {
    collect *col = user;
    const odin3_prov_record *rec = odin3_prov_get(col->ctx->design, leaf);
    if (col->st != ODIN3_OK || rec == NULL || rec->kind != ODIN3_PROV_SOURCE || rec->n_locs == 0 ||
        !usable_file(col, rec->locs[0].file)) {
        return;
    }
    if (col->first_leaf == 0) {
        col->first_leaf = leaf.v;
    }
    for (uint32_t i = 0; col->out != NULL && i < rec->n_locs; i++) {
        if (usable_file(col, rec->locs[i].file)) {
            add_loc(col, &rec->locs[i]);
        }
    }
}

odin3_status odin3_hdl_locs(odin3_hdl_ctx *ctx, odin3_prov_id prov, odin3_hdl_locs_out *out) {
    collect col = {ctx, out, 0, ODIN3_OK};
    memset(out, 0, sizeof *out);
    if (odin3_prov_get(ctx->design, prov) == NULL) {
        return ODIN3_OK;
    }
    odin3_status st = odin3_prov_sources(ctx->design, prov, visit_leaf, &col);
    return st != ODIN3_OK ? st : col.st;
}

const odin3_design *odin3_hdl_ctx_design(const odin3_hdl_ctx *ctx) {
    return ctx->design;
}

odin3_status odin3_hdl_first_record(odin3_hdl_ctx *ctx, odin3_prov_id prov,
                                    const odin3_prov_record **rec) {
    uint64_t leaf = 0;
    *rec = NULL;
    if (odin3_prov_get(ctx->design, prov) == NULL) {
        return ODIN3_OK;
    }
    if (!odin3_u64map_get(ctx->first, prov.v, &leaf)) {
        collect col = {ctx, NULL, 0, ODIN3_OK};
        odin3_status st = odin3_prov_sources(ctx->design, prov, visit_leaf, &col);
        st = st != ODIN3_OK ? st : col.st;
        if (st != ODIN3_OK) {
            return st;
        }
        leaf = col.first_leaf;
        if (odin3_u64map_put(ctx->first, (odin3_kv){prov.v, leaf}) != ODIN3_OK) {
            return ODIN3_ERR_NO_MEMORY;
        }
    }
    *rec = leaf != 0 ? odin3_prov_get(ctx->design, (odin3_prov_id){(uint32_t)leaf}) : NULL;
    return ODIN3_OK;
}

odin3_status odin3_hdl_first(odin3_hdl_ctx *ctx, odin3_prov_id prov, odin3_srcloc *loc,
                             bool *found) {
    const odin3_prov_record *rec = NULL;
    odin3_status st = odin3_hdl_first_record(ctx, prov, &rec);
    *found = st == ODIN3_OK && rec != NULL;
    if (*found) {
        *loc = rec->locs[0];
    }
    return st;
}

/* True when text[0..len) has a blank at either end or two blanks in a row. */
static bool odd_blanks(const char *text, size_t len) {
    if (len > 0 && (text[0] == ' ' || text[len - 1] == ' ')) {
        return true;
    }
    for (size_t i = 1; i < len; i++) {
        if (text[i] == ' ' && text[i - 1] == ' ') {
            return true;
        }
    }
    return false;
}

/* True when the file's bytes can be written in a src text of the target. */
static bool file_writable(odin3_hdl_ctx *ctx, const odin3_hdl_print_req *req, uint32_t file,
                          odin3_status *st) {
    const odin3_strtab *tab = odin3_design_strtab(ctx->design);
    const char *text = odin3_strtab_get(tab, file);
    size_t len = odin3_strtab_len(tab, file);
    bool bad = memchr(text, '|', len) != NULL;
    if (req->plain_token) {
        bad = bad || memchr(text, '"', len) != NULL || memchr(text, '\\', len) != NULL ||
              memchr(text, '#', len) != NULL || odd_blanks(text, len);
    }
    if (bad) {
        odin3_status warned = warn_file(
            ctx, file, WHY_TEXT, "cannot be written in a src text; its locations are skipped");
        *st = *st != ODIN3_OK ? *st : warned;
    }
    return !bad;
}

odin3_status odin3_hdl_src_text(odin3_hdl_ctx *ctx, const odin3_hdl_print_req *req,
                                odin3_strbuf *out, uint32_t *n_written) {
    const odin3_strtab *tab = odin3_design_strtab(ctx->design);
    odin3_status st = ODIN3_OK;
    odin3_strbuf_clear(out);
    *n_written = 0;
    for (uint32_t i = 0; st == ODIN3_OK && i < req->locs->n; i++) {
        const odin3_srcloc *src = &req->locs->locs[i];
        if (!file_writable(ctx, req, src->file, &st) || st != ODIN3_OK) {
            continue;
        }
        odin3_hdl_loc loc = {{odin3_strtab_get(tab, src->file), odin3_strtab_len(tab, src->file)},
                             src->line,
                             src->col,
                             src->end_line,
                             src->end_col};
        st = *n_written > 0 ? odin3_strbuf_append(out, odin3_bytes_cstr("|")) : ODIN3_OK;
        st = st == ODIN3_OK ? print_one(out, &loc) : st;
        (*n_written)++;
    }
    return st;
}
