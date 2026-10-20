/*
 * name_map.c — write, parse and check the name map of hashed names (spec 5.4f, H-4). See
 * name_map.h.
 */
#include "backends/common/name_map.h"

#include "backends/common/hash_name.h"
#include "util/alloc.h"
#include "util/arena.h"
#include "util/log.h"

#include <stdlib.h>
#include <string.h>

enum {
    HEADER_TOKENS = 5,
    HEX_DIGITS = 2,
    LINE_DIGITS_MAX = 10,
    DECIMAL = 10,
    WIDTH_KEY_LEN = 6 /* strlen("width=") */
};

/* The sort order of `kind` is its enum order, which must equal the byte order of the words. */
_Static_assert(ODIN3_NAME_NET == 0 && ODIN3_NAME_NODE == 1 && ODIN3_NAME_WIRE == 2 &&
                   ODIN3_NAME_KIND_COUNT == 3,
               "kind enum order must be net, node, wire (H-4 sort order)");
static const char *const KIND_NAMES[ODIN3_NAME_KIND_COUNT] = {"net", "node", "wire"};
static const char HEADER_WORD[] = "# odin3-names";
static const char HEADER_PREFIX[] = "# odin3-names ";

const char *odin3_name_kind_str(odin3_name_kind kind) {
    return (unsigned)kind < (unsigned)ODIN3_NAME_KIND_COUNT ? KIND_NAMES[kind] : "";
}

bool odin3_name_kind_parse(odin3_bytes text, odin3_name_kind *kind) {
    for (int k = 0; k < ODIN3_NAME_KIND_COUNT; k++) {
        if (text.len == strlen(KIND_NAMES[k]) && memcmp(text.ptr, KIND_NAMES[k], text.len) == 0) {
            *kind = (odin3_name_kind)k;
            return true;
        }
    }
    return false;
}

static int bytes_cmp(odin3_bytes lhs, odin3_bytes rhs) {
    size_t common = lhs.len < rhs.len ? lhs.len : rhs.len;
    int cmp = common == 0 ? 0 : memcmp(lhs.ptr, rhs.ptr, common);
    if (cmp != 0 || lhs.len == rhs.len) {
        return cmp;
    }
    return lhs.len < rhs.len ? -1 : 1;
}

/* The bytes of a view as a pointer that is never NULL (for a "%.*s" argument). */
static const char *text_or_empty(odin3_bytes view) {
    return view.ptr == NULL ? "" : (const char *)view.ptr;
}

/* The sort and search order of a map: module, kind, short in byte order. */
static int key_cmp(const odin3_name_entry *lhs, const odin3_name_entry *rhs) {
    int cmp = bytes_cmp(lhs->module, rhs->module);
    if (cmp != 0) {
        return cmp;
    }
    if (lhs->kind != rhs->kind) {
        return lhs->kind < rhs->kind ? -1 : 1;
    }
    return bytes_cmp(lhs->short_name, rhs->short_name);
}

/* Total order for sorting: the key, then full, line, hier and file, so duplicate keys sort the same
 * way whatever the input order (qsort is not stable). */
static int entry_cmp(const void *lhs, const void *rhs) {
    const odin3_name_entry *left = (const odin3_name_entry *)lhs;
    const odin3_name_entry *right = (const odin3_name_entry *)rhs;
    int cmp = key_cmp(left, right);
    if (cmp == 0) {
        cmp = bytes_cmp(left->full, right->full);
    }
    if (cmp == 0 && left->line != right->line) {
        cmp = left->line < right->line ? -1 : 1;
    }
    if (cmp == 0) {
        cmp = bytes_cmp(left->hier, right->hier);
    }
    return cmp != 0 ? cmp : bytes_cmp(left->file, right->file);
}

/* --- write ---------------------------------------------------------------------------------------
 */

/* The escape of one byte of a field (TAB, LF, `\` and `%`), or NULL for any other byte. */
static const char *escape_of(char ch) {
    switch (ch) {
    case '\t':
        return "%09";
    case '\n':
        return "%0A";
    case '\\':
        return "%5C";
    case '%':
        return "%25";
    default:
        return NULL;
    }
}

/* Appends field with TAB, LF, `\` and `%` written as %09, %0A, %5C, %25. */
static odin3_status put_field(odin3_strbuf *out, odin3_bytes field) {
    const char *bytes = (const char *)field.ptr;
    size_t run = 0;
    for (size_t i = 0; i < field.len; i++) {
        const char *esc = escape_of(bytes[i]);
        if (esc == NULL) {
            continue;
        }
        odin3_status st = odin3_strbuf_append(out, (odin3_bytes){bytes + run, i - run});
        if (st == ODIN3_OK) {
            st = odin3_strbuf_append(out, odin3_bytes_cstr(esc));
        }
        if (st != ODIN3_OK) {
            return st;
        }
        run = i + 1;
    }
    return odin3_strbuf_append(out, (odin3_bytes){bytes + run, field.len - run});
}

static odin3_status put_entry(odin3_strbuf *out, const odin3_name_entry *entry) {
    odin3_bytes module = entry->module;
    odin3_status st = ODIN3_OK;
    if (module.len > 0 && ((const char *)module.ptr)[0] == '#') {
        /* a line starting `#` is a comment: escape the first byte of the line */
        st = odin3_strbuf_append(out, odin3_bytes_cstr("%23"));
        module = (odin3_bytes){(const char *)module.ptr + 1, module.len - 1};
        if (st != ODIN3_OK) {
            return st;
        }
    }
    const odin3_bytes fields[] = {module,
                                  odin3_bytes_cstr(odin3_name_kind_str(entry->kind)),
                                  entry->short_name,
                                  entry->full,
                                  entry->hier,
                                  entry->file};
    for (size_t i = 0; i < sizeof fields / sizeof fields[0]; i++) {
        st = put_field(out, fields[i]);
        if (st == ODIN3_OK) {
            st = odin3_strbuf_append(out, odin3_bytes_cstr("\t"));
        }
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return odin3_strbuf_appendf(out, "%u\n", entry->line);
}

/* Sorted copy of the entries; the caller frees it. NULL on out of memory. */
static odin3_name_entry *sorted_copy(const odin3_name_entry *entries, uint32_t n) {
    if (n == 0) {
        return NULL;
    }
    odin3_name_entry *copy = (odin3_name_entry *)odin3_util_malloc((size_t)n * sizeof *copy);
    if (copy != NULL) {
        memcpy(copy, entries, (size_t)n * sizeof *copy);
        qsort(copy, n, sizeof *copy, entry_cmp);
    }
    return copy;
}

/* INVALID_ARG (logged) for a kind out of range or a repeated key in the sorted entries. */
static odin3_status check_write_entries(const odin3_name_entry *sorted, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if ((unsigned)sorted[i].kind >= (unsigned)ODIN3_NAME_KIND_COUNT) {
            odin3_log(ODIN3_LOG_ERROR, "name map: entry %u has an invalid kind", i);
            return ODIN3_ERR_INVALID_ARG;
        }
        if (i > 0 && key_cmp(&sorted[i - 1], &sorted[i]) == 0) {
            odin3_log(ODIN3_LOG_ERROR, "name map: two entries name '%.*s' (%s) in module '%.*s'",
                      (int)sorted[i].short_name.len, text_or_empty(sorted[i].short_name),
                      odin3_name_kind_str(sorted[i].kind), (int)sorted[i].module.len,
                      text_or_empty(sorted[i].module));
            return ODIN3_ERR_INVALID_ARG;
        }
    }
    return ODIN3_OK;
}

static void truncate_to(odin3_strbuf *out, size_t len) {
    out->len = len;
    if (out->data != NULL) {
        out->data[len] = '\0';
    }
}

odin3_status odin3_name_map_write(const odin3_name_entry *entries, uint32_t n, uint32_t width,
                                  odin3_strbuf *out) {
    if (width < 1 || width > ODIN3_HASH_NAME_MAX_CHARS || n > ODIN3_NAME_MAP_MAX_ENTRIES) {
        odin3_log(ODIN3_LOG_ERROR, "name map: width %u or entry count %u out of range", width, n);
        return ODIN3_ERR_INVALID_ARG;
    }
    size_t start = out->len;
    odin3_name_entry *sorted = sorted_copy(entries, n);
    if (sorted == NULL && n > 0) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_status st = check_write_entries(sorted, n);
    if (st == ODIN3_OK) {
        st = odin3_strbuf_appendf(out, "# odin3-names %d hash=sha256 width=%u\n",
                                  ODIN3_NAME_MAP_VERSION, width);
    }
    for (uint32_t i = 0; st == ODIN3_OK && i < n; i++) {
        size_t before = out->len;
        st = put_entry(out, &sorted[i]);
        if (st == ODIN3_OK && out->len - before - 1 > ODIN3_NAME_MAP_MAX_LINE) {
            odin3_log(ODIN3_LOG_ERROR,
                      "name map: the line of '%.*s' (module '%.*s') is %zu bytes, over the "
                      "limit of %d",
                      (int)sorted[i].short_name.len, text_or_empty(sorted[i].short_name),
                      (int)sorted[i].module.len, text_or_empty(sorted[i].module),
                      out->len - before - 1, ODIN3_NAME_MAP_MAX_LINE);
            st = ODIN3_ERR_INVALID_ARG;
        }
    }
    odin3_util_free(sorted);
    if (st != ODIN3_OK && out->len != start) {
        truncate_to(out, start);
    }
    return st;
}

/* --- parse ---------------------------------------------------------------------------------------
 */

odin3_status odin3_name_map_init(odin3_name_map *map) {
    odin3_vec_init(&map->entries, sizeof(odin3_name_entry));
    map->arena = odin3_arena_create(0);
    map->width = ODIN3_HASH_NAME_WIDTH;
    map->ignored = 0;
    map->by_full = NULL;
    return map->arena == NULL ? ODIN3_ERR_NO_MEMORY : ODIN3_OK;
}

void odin3_name_map_free(odin3_name_map *map) {
    odin3_vec_free(&map->entries);
    odin3_util_free(map->by_full);
    map->by_full = NULL;
    odin3_arena_destroy(map->arena);
    map->arena = NULL;
}

/* The state of one parse, so the helpers stay under the parameter limit. */
typedef struct nm_parser {
    odin3_name_map *map;
    const char *path;
    uint32_t lineno;
} nm_parser;

static void parse_error(const nm_parser *ps, const char *msg, odin3_bytes detail) {
    if (detail.len == 0 || detail.ptr == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "%s:%u: %s", ps->path, ps->lineno, msg);
        return;
    }
    odin3_log(ODIN3_LOG_ERROR, "%s:%u: %s '%.*s'", ps->path, ps->lineno, msg, (int)detail.len,
              (const char *)detail.ptr);
}

/* A located warning; the entry line is then ignored. */
static void ignore_line(nm_parser *ps, const char *why) {
    odin3_log(ODIN3_LOG_WARN, "%s:%u: name map entry ignored: %s", ps->path, ps->lineno, why);
    ps->map->ignored++;
}

static bool all_digits(odin3_bytes text, size_t max_len) {
    const char *bytes = (const char *)text.ptr;
    if (text.len == 0 || text.len > max_len) {
        return false;
    }
    for (size_t i = 0; i < text.len; i++) {
        if (bytes[i] < '0' || bytes[i] > '9') {
            return false;
        }
    }
    return true;
}

static bool parse_decimal(odin3_bytes text, uint32_t *value) {
    const char *bytes = (const char *)text.ptr;
    uint64_t acc = 0;
    if (!all_digits(text, LINE_DIGITS_MAX)) {
        return false;
    }
    for (size_t i = 0; i < text.len; i++) {
        acc = acc * DECIMAL + (uint64_t)(bytes[i] - '0');
    }
    if (acc > UINT32_MAX) {
        return false;
    }
    *value = (uint32_t)acc;
    return true;
}

/* Is tok exactly `width=N` with N in 1..52? Stores N. */
static bool parse_width_token(odin3_bytes tok, uint32_t *width) {
    if (tok.len <= WIDTH_KEY_LEN || memcmp(tok.ptr, "width=", WIDTH_KEY_LEN) != 0) {
        return false;
    }
    odin3_bytes digits = {(const char *)tok.ptr + WIDTH_KEY_LEN, tok.len - WIDTH_KEY_LEN};
    return parse_decimal(digits, width) && *width >= 1 && *width <= ODIN3_HASH_NAME_MAX_CHARS;
}

/* Splits the header after its prefix at spaces into at most HEADER_TOKENS - 1 tokens. */
static size_t header_tokens(odin3_bytes line, odin3_bytes tok[HEADER_TOKENS]) {
    const char *bytes = (const char *)line.ptr;
    size_t count = 0;
    size_t start = sizeof HEADER_PREFIX - 1;
    for (size_t i = start; i <= line.len && count < HEADER_TOKENS - 1; i++) {
        if (i == line.len || bytes[i] == ' ') {
            tok[count++] = (odin3_bytes){bytes + start, i - start};
            start = i + 1;
        }
    }
    return count;
}

/* Header: `# odin3-names 1 hash=sha256 width=N`. ODIN3_ERR_PARSE (logged) for anything else. */
static odin3_status parse_header(nm_parser *ps, odin3_bytes line) {
    odin3_bytes tok[HEADER_TOKENS] = {{NULL, 0}};
    uint32_t width = 0;
    if (line.len >= sizeof HEADER_WORD - 1 &&
        memcmp(line.ptr, HEADER_WORD, sizeof HEADER_WORD - 1) == 0 &&
        (line.len == sizeof HEADER_WORD - 1 ||
         ((const char *)line.ptr)[sizeof HEADER_WORD - 1] != ' ')) {
        parse_error(ps,
                    "malformed name map header ('# odin3-names' must be followed by a space and "
                    "'VERSION hash=sha256 width=N')",
                    (odin3_bytes){NULL, 0});
        return ODIN3_ERR_PARSE;
    }
    if (line.len < sizeof HEADER_PREFIX - 1 ||
        memcmp(line.ptr, HEADER_PREFIX, sizeof HEADER_PREFIX - 1) != 0) {
        parse_error(ps, "not a name map (no '# odin3-names' header)", (odin3_bytes){NULL, 0});
        return ODIN3_ERR_PARSE;
    }
    if (line.len > 0 && ((const char *)line.ptr)[line.len - 1] == '\r') {
        parse_error(ps, "CRLF line ends are not supported (a name map uses LF)",
                    (odin3_bytes){NULL, 0});
        return ODIN3_ERR_PARSE;
    }
    size_t count = header_tokens(line, tok);
    if (tok[0].len != 1 || ((const char *)tok[0].ptr)[0] != '0' + ODIN3_NAME_MAP_VERSION) {
        parse_error(ps, "unsupported name map version", tok[0]);
        return ODIN3_ERR_PARSE;
    }
    if (count != 3 || bytes_cmp(tok[1], odin3_bytes_cstr("hash=sha256")) != 0 ||
        !parse_width_token(tok[2], &width)) {
        parse_error(ps, "malformed name map header (want 'hash=sha256 width=1..52')",
                    (odin3_bytes){NULL, 0});
        return ODIN3_ERR_PARSE;
    }
    ps->map->width = width;
    return ODIN3_OK;
}

/* One escape `%XX` at bytes[i]: its byte in *ch, or false when it is not one of the four. */
static bool decode_escape(const char *bytes, size_t left, char *ch) {
    static const char CODES[] = {'\t', '\n', '\\', '%', '#'};
    static const char *const TEXTS[] = {"09", "0A", "5C", "25", "23"};
    if (left < 1 + HEX_DIGITS) {
        return false;
    }
    for (size_t k = 0; k < sizeof CODES; k++) {
        if (bytes[1] == TEXTS[k][0] && bytes[2] == TEXTS[k][1]) {
            *ch = CODES[k];
            return true;
        }
    }
    return false;
}

/* Unescapes field into the arena. INVALID_ARG for a bad escape, NO_MEMORY on out of memory. */
static odin3_status unescape(odin3_arena *arena, odin3_bytes field, odin3_bytes *out) {
    const char *src = (const char *)field.ptr;
    char *dst = (char *)odin3_arena_alloc(arena, field.len + 1);
    size_t len = 0;
    if (dst == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (size_t i = 0; i < field.len; i++) {
        if (src[i] != '%') {
            dst[len++] = src[i];
        } else if (decode_escape(src + i, field.len - i, &dst[len])) {
            len++;
            i += HEX_DIGITS;
        } else {
            return ODIN3_ERR_INVALID_ARG;
        }
    }
    dst[len] = '\0';
    *out = (odin3_bytes){dst, len};
    return ODIN3_OK;
}

/* Splits line at TABs into exactly ODIN3_NAME_MAP_FIELDS fields; false for any other count. */
static bool split_fields(odin3_bytes line, odin3_bytes fields[ODIN3_NAME_MAP_FIELDS]) {
    const char *bytes = (const char *)line.ptr;
    size_t count = 0;
    size_t start = 0;
    for (size_t i = 0; i <= line.len; i++) {
        if (i < line.len && bytes[i] != '\t') {
            continue;
        }
        if (count == ODIN3_NAME_MAP_FIELDS) {
            return false;
        }
        fields[count++] = (odin3_bytes){bytes + start, i - start};
        start = i + 1;
    }
    return count == ODIN3_NAME_MAP_FIELDS;
}

/* Does the entry's short name equal the hash of its full name at the width or longer (H-3)? */
static bool short_matches_full(const odin3_name_entry *entry, uint32_t width) {
    const char *text = (const char *)entry->short_name.ptr;
    odin3_hash_name want;
    if (entry->short_name.len < 1 + (size_t)width ||
        entry->short_name.len > 1 + (size_t)ODIN3_HASH_NAME_MAX_CHARS || text[0] != 'h') {
        return false;
    }
    odin3_hash_name_make(entry->full, (uint32_t)entry->short_name.len - 1, &want);
    return strlen(want.text) == entry->short_name.len &&
           memcmp(want.text, text, entry->short_name.len) == 0;
}

/* Fills entry from the 7 raw fields. OK, INVALID_ARG (the line is bad, reason in *why) or
 * NO_MEMORY. */
static odin3_status build_entry(nm_parser *ps, const odin3_bytes raw[ODIN3_NAME_MAP_FIELDS],
                                odin3_name_entry *entry, const char **why) {
    odin3_bytes *const slots[] = {&entry->module, NULL,         &entry->short_name,
                                  &entry->full,   &entry->hier, &entry->file};
    for (size_t i = 0; i < sizeof slots / sizeof slots[0]; i++) {
        if (slots[i] == NULL) {
            continue;
        }
        odin3_status st = unescape(ps->map->arena, raw[i], slots[i]);
        if (st != ODIN3_OK) {
            *why = "bad escape (only %09 %0A %5C %25 %23 are allowed)";
            return st;
        }
    }
    if (!odin3_name_kind_parse(raw[1], &entry->kind)) {
        *why = "unknown kind (want net, node or wire)";
        return ODIN3_ERR_INVALID_ARG;
    }
    if (!parse_decimal(raw[ODIN3_NAME_MAP_FIELDS - 1], &entry->line)) {
        *why = "the line field is not a decimal number";
        return ODIN3_ERR_INVALID_ARG;
    }
    if (!short_matches_full(entry, ps->map->width)) {
        odin3_log(ODIN3_LOG_WARN,
                  "%s:%u: name map entry ignored: short name '%.*s' is not the hash of the full "
                  "name",
                  ps->path, ps->lineno, (int)entry->short_name.len,
                  text_or_empty(entry->short_name));
        ps->map->ignored++;
        *why = NULL; /* already reported */
        return ODIN3_ERR_INVALID_ARG;
    }
    return ODIN3_OK;
}

/* One entry line: kept, or warned about and ignored. NO_MEMORY or PARSE end the parse. */
static odin3_status parse_entry(nm_parser *ps, odin3_bytes line) {
    odin3_bytes raw[ODIN3_NAME_MAP_FIELDS];
    odin3_name_entry entry = {{NULL, 0}, ODIN3_NAME_NET, {NULL, 0}, {NULL, 0},
                              {NULL, 0}, {NULL, 0},      0};
    const char *why = NULL;
    if (line.len > ODIN3_NAME_MAP_MAX_LINE) {
        ignore_line(ps, "line is too long");
        return ODIN3_OK;
    }
    if (line.len > 0 && ((const char *)line.ptr)[line.len - 1] == '\r') {
        ignore_line(ps, "CR at the end of the line (a name map uses LF line ends)");
        return ODIN3_OK;
    }
    if (!split_fields(line, raw)) {
        ignore_line(ps, "not 7 tab-separated fields");
        return ODIN3_OK;
    }
    odin3_status st = build_entry(ps, raw, &entry, &why);
    if (st == ODIN3_ERR_INVALID_ARG) {
        if (why != NULL) {
            ignore_line(ps, why);
        }
        return ODIN3_OK;
    }
    if (st != ODIN3_OK) {
        return st;
    }
    if (ps->map->entries.len >= ODIN3_NAME_MAP_MAX_ENTRIES) {
        parse_error(ps, "too many entries in the name map", (odin3_bytes){NULL, 0});
        return ODIN3_ERR_PARSE;
    }
    odin3_name_entry *slot = (odin3_name_entry *)odin3_vec_push(&ps->map->entries);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = entry;
    return ODIN3_OK;
}

/* The next line of text from *pos (without its LF); false at the end of the text. */
static bool next_line(odin3_bytes text, size_t *pos, odin3_bytes *line) {
    const char *bytes = (const char *)text.ptr;
    if (*pos >= text.len) {
        return false;
    }
    const char *lf = (const char *)memchr(bytes + *pos, '\n', text.len - *pos);
    size_t end = lf == NULL ? text.len : (size_t)(lf - bytes);
    *line = (odin3_bytes){bytes + *pos, end - *pos};
    *pos = end + 1;
    return true;
}

/* Compares an entry with the key (module, kind, full) of a find_full lookup. */
static int full_key_cmp(const odin3_name_entry *entry, const odin3_name_entry *key) {
    int cmp = bytes_cmp(entry->module, key->module);
    if (cmp == 0 && entry->kind != key->kind) {
        cmp = entry->kind < key->kind ? -1 : 1;
    }
    return cmp != 0 ? cmp : bytes_cmp(entry->full, key->full);
}

/* Order of the by-full index: module, kind, full, then the entry order (short, line, ...). */
static int full_cmp(const void *lhs, const void *rhs) {
    const odin3_name_entry *left = ((const odin3_name_ref *)lhs)->entry;
    const odin3_name_entry *right = ((const odin3_name_ref *)rhs)->entry;
    int cmp = full_key_cmp(left, right);
    return cmp != 0 ? cmp : entry_cmp(left, right);
}

/* The by-full index: references to the entries sorted by full_cmp. NO_MEMORY on out of memory. */
static odin3_status build_full_index(odin3_name_map *map) {
    size_t count = map->entries.len;
    if (count == 0) {
        return ODIN3_OK;
    }
    odin3_name_ref *index = (odin3_name_ref *)odin3_util_malloc(count * sizeof *index);
    if (index == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (size_t i = 0; i < count; i++) {
        index[i].entry = (const odin3_name_entry *)odin3_vec_cat(&map->entries, i);
    }
    qsort(index, count, sizeof *index, full_cmp);
    map->by_full = index;
    return ODIN3_OK;
}

odin3_status odin3_name_map_parse(odin3_name_map *map, odin3_bytes text, const char *path) {
    nm_parser ps = {map, path, 0};
    odin3_bytes line = {NULL, 0};
    size_t pos = 0;
    odin3_status st = ODIN3_OK;
    ps.lineno = 1;
    if (!next_line(text, &pos, &line)) {
        parse_error(&ps, "empty name map (no '# odin3-names' header)", (odin3_bytes){NULL, 0});
        return ODIN3_ERR_PARSE;
    }
    st = parse_header(&ps, line);
    while (st == ODIN3_OK && next_line(text, &pos, &line)) {
        ps.lineno++;
        if (line.len == 0 || ((const char *)line.ptr)[0] == '#') {
            continue;
        }
        st = parse_entry(&ps, line);
    }
    if (st == ODIN3_OK && map->entries.len > 1) {
        qsort(map->entries.data, map->entries.len, sizeof(odin3_name_entry), entry_cmp);
    }
    return st == ODIN3_OK ? build_full_index(map) : st;
}

/* --- lookup and check ------------------------------------------------------------------------ */

const odin3_name_entry *odin3_name_map_find(const odin3_name_map *map, odin3_bytes module,
                                            odin3_name_kind kind, odin3_bytes short_name) {
    odin3_name_entry key = {module, kind, short_name, {NULL, 0}, {NULL, 0}, {NULL, 0}, 0};
    size_t lo = 0;
    size_t hi = map->entries.len;
    while (lo < hi) { /* first entry not less than key */
        size_t mid = lo + (hi - lo) / 2;
        if (key_cmp((const odin3_name_entry *)odin3_vec_cat(&map->entries, mid), &key) < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo < map->entries.len) {
        const odin3_name_entry *hit = (const odin3_name_entry *)odin3_vec_cat(&map->entries, lo);
        return key_cmp(hit, &key) == 0 ? hit : NULL;
    }
    return NULL;
}

const odin3_name_entry *odin3_name_map_find_full(const odin3_name_map *map, odin3_bytes module,
                                                 odin3_name_kind kind, odin3_bytes full) {
    const odin3_name_entry key = {module, kind, {NULL, 0}, full, {NULL, 0}, {NULL, 0}, 0};
    size_t count = map->by_full == NULL ? 0 : map->entries.len;
    size_t lo = 0;
    size_t hi = count;
    while (lo < hi) { /* first entry not less than (module, kind, full) */
        size_t mid = lo + (hi - lo) / 2;
        if (full_key_cmp(map->by_full[mid].entry, &key) < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo < count && full_key_cmp(map->by_full[lo].entry, &key) == 0) {
        return map->by_full[lo].entry;
    }
    return NULL;
}

odin3_name_check odin3_name_map_check(const odin3_name_map *map) {
    odin3_name_check res = {(uint32_t)map->entries.len, 0, 0};
    const odin3_name_entry *prev = NULL;
    for (size_t i = 0; i < map->entries.len; i++) {
        const odin3_name_entry *entry = (const odin3_name_entry *)odin3_vec_cat(&map->entries, i);
        if (!short_matches_full(entry, map->width)) {
            res.bad_hash++;
        }
        if (prev != NULL && key_cmp(prev, entry) == 0) {
            res.duplicates++;
        }
        prev = entry;
    }
    return res;
}
