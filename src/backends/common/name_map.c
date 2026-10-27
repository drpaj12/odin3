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
    HEADER_TOKENS = 7,
    HEX_DIGITS = 2,
    LINE_DIGITS_MAX = 10,
    DECIMAL = 10,
    WIDTH_KEY_LEN = 6, /* strlen("width=") */
    BYTE_VALUES = 256,
    V1_HEADER_TOKENS = 3, /* after the version: hash=, width= ... */
    V2_HEADER_TOKENS = 5  /* ... and prov=, ast= */
};

/* The sort order of `kind` is its enum order, which must equal the byte order of the words. */
_Static_assert(ODIN3_NAME_NET == 0 && ODIN3_NAME_NODE == 1 && ODIN3_NAME_WIRE == 2 &&
                   ODIN3_NAME_KIND_COUNT == 3,
               "kind enum order must be net, node, wire (H-4 sort order)");
static const char *const KIND_NAMES[ODIN3_NAME_KIND_COUNT] = {"net", "node", "wire"};
static const char HEADER_WORD[] = "# odin3-names";
static const char *const AST_WORDS[] = {"refs", "constructs", "full", "none"};
static const char PROV_TOKEN[] = "prov=1";
static const char AST_KEY[] = "ast=";
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

/* The escape of one byte of a field (TAB, LF, `\` and `%`; in version 2 also `,` and `|`), or NULL
 * for any other byte. */
static const char *escape_of(char ch, bool v2) {
    if (v2 && ch == ',') {
        return "%2C";
    }
    if (v2 && ch == '|') {
        return "%7C";
    }
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
static odin3_status put_field_ex(odin3_strbuf *out, odin3_bytes field, bool v2) {
    if (field.len == 0) {
        return ODIN3_OK; /* an empty field may have a NULL pointer: no offset on it */
    }
    const char *bytes = (const char *)field.ptr;
    size_t run = 0;
    for (size_t i = 0; i < field.len; i++) {
        const char *esc = escape_of(bytes[i], v2);
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

static odin3_status put_field(odin3_strbuf *out, odin3_bytes field) {
    return put_field_ex(out, field, false);
}

static odin3_status put_entry(odin3_strbuf *out, const odin3_name_entry *entry) {
    odin3_bytes module = entry->module;
    if (module.len > 0 && ((const char *)module.ptr)[0] == '#') {
        /* a line starting `#` is a comment: escape the first byte of the line */
        odin3_status first = odin3_strbuf_append(out, odin3_bytes_cstr("%23"));
        if (first != ODIN3_OK) {
            return first;
        }
        module = (odin3_bytes){(const char *)module.ptr + 1, module.len - 1};
    }
    const odin3_bytes fields[] = {module,
                                  odin3_bytes_cstr(odin3_name_kind_str(entry->kind)),
                                  entry->short_name,
                                  entry->full,
                                  entry->hier,
                                  entry->file};
    for (size_t i = 0; i < sizeof fields / sizeof fields[0]; i++) {
        odin3_status st = put_field(out, fields[i]);
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
    map->version = ODIN3_NAME_MAP_VERSION;
    map->ast = (odin3_bytes){NULL, 0};
    odin3_vec_init(&map->lines, sizeof(odin3_nm_line));
    return map->arena == NULL ? ODIN3_ERR_NO_MEMORY : ODIN3_OK;
}

void odin3_name_map_free(odin3_name_map *map) {
    odin3_vec_free(&map->entries);
    odin3_vec_free(&map->lines);
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
    bool warned[BYTE_VALUES]; /* version 2: a type letter already warned about */
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

/* Is tok `ast=` and one of the four mode words? */
static bool parse_ast_token(odin3_bytes tok, odin3_bytes *word) {
    size_t key = sizeof AST_KEY - 1;
    if (tok.len <= key || memcmp(tok.ptr, AST_KEY, key) != 0) {
        return false;
    }
    odin3_bytes value = {(const char *)tok.ptr + key, tok.len - key};
    for (size_t k = 0; k < sizeof AST_WORDS / sizeof AST_WORDS[0]; k++) {
        if (bytes_cmp(value, odin3_bytes_cstr(AST_WORDS[k])) == 0) {
            *word = odin3_bytes_cstr(AST_WORDS[k]); /* the static word, not the caller's text */
            return true;
        }
    }
    return false;
}

/* Do the tokens after the version fit the version's header? Stores the width and, for version 2,
 * the ast word. */
static bool header_shape_ok(nm_parser *ps, const odin3_bytes tok[HEADER_TOKENS], size_t count,
                            uint32_t *width) {
    bool v2 = tok[0].len == 1 && ((const char *)tok[0].ptr)[0] == '0' + ODIN3_NAME_MAP_VERSION_2;
    bool shape = count == (v2 ? (size_t)V2_HEADER_TOKENS : (size_t)V1_HEADER_TOKENS) &&
                 bytes_cmp(tok[1], odin3_bytes_cstr("hash=sha256")) == 0 &&
                 parse_width_token(tok[2], width);
    if (shape && v2) {
        shape = bytes_cmp(tok[3], odin3_bytes_cstr(PROV_TOKEN)) == 0 &&
                parse_ast_token(tok[4], &ps->map->ast);
    }
    return shape;
}

/* Header: `# odin3-names 1 hash=sha256 width=N`, or version 2's `2 hash=sha256 width=N prov=1
 * ast=<mode>`. ODIN3_ERR_PARSE (logged) for anything else. */
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
    if (((const char *)line.ptr)[line.len - 1] == '\r') {
        parse_error(ps, "CRLF line ends are not supported (a name map uses LF)",
                    (odin3_bytes){NULL, 0});
        return ODIN3_ERR_PARSE;
    }
    size_t count = header_tokens(line, tok);
    uint32_t version = tok[0].len == 1 ? (uint32_t)(((const char *)tok[0].ptr)[0] - '0') : 0;
    if (version != ODIN3_NAME_MAP_VERSION && version != ODIN3_NAME_MAP_VERSION_2) {
        parse_error(ps, "unsupported name map version", tok[0]);
        return ODIN3_ERR_PARSE;
    }
    if (!header_shape_ok(ps, tok, count, &width)) {
        parse_error(ps,
                    version == ODIN3_NAME_MAP_VERSION_2
                        ? "malformed name map header (want 'hash=sha256 width=1..52 prov=1 "
                          "ast=refs|constructs|full|none')"
                        : "malformed name map header (want 'hash=sha256 width=1..52')",
                    (odin3_bytes){NULL, 0});
        return ODIN3_ERR_PARSE;
    }
    ps->map->width = width;
    ps->map->version = version;
    return ODIN3_OK;
}

/* One escape `%XX` at bytes[i]: its byte in *ch, or false when it is not one of the four. */
static bool decode_escape(const char *bytes, size_t left, bool v2, char *ch) {
    static const char CODES[] = {'\t', '\n', '\\', '%', '#', ',', '|'};
    static const char *const TEXTS[] = {"09", "0A", "5C", "25", "23", "2C", "7C"};
    if (left < 1 + HEX_DIGITS) {
        return false;
    }
    size_t known = v2 ? sizeof CODES : sizeof CODES - 2; /* %2C and %7C are version 2's */
    for (size_t k = 0; k < known; k++) {
        if (bytes[1] == TEXTS[k][0] && bytes[2] == TEXTS[k][1]) {
            *ch = CODES[k];
            return true;
        }
    }
    return false;
}

/* Unescapes field into the arena. INVALID_ARG for a bad escape, NO_MEMORY on out of memory. */
static odin3_status unescape_ex(odin3_arena *arena, odin3_bytes field, bool v2, odin3_bytes *out) {
    const char *src = (const char *)field.ptr;
    char *dst = (char *)odin3_arena_alloc(arena, field.len + 1);
    size_t len = 0;
    if (dst == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (size_t i = 0; i < field.len; i++) {
        if (src[i] != '%') {
            dst[len++] = src[i];
        } else if (decode_escape(src + i, field.len - i, v2, &dst[len])) {
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
        odin3_status st = unescape_ex(ps->map->arena, raw[i], ps->map->version == 2, slots[i]);
        if (st != ODIN3_OK) {
            *why = ps->map->version == 2
                       ? "bad escape (only %09 %0A %5C %25 %23 %2C %7C are allowed)"
                       : "bad escape (only %09 %0A %5C %25 %23 are allowed)";
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

/* Keeps an entry. ODIN3_ERR_PARSE past the limit, NO_MEMORY. */
static odin3_status push_entry(nm_parser *ps, const odin3_name_entry *entry) {
    if (ps->map->entries.len >= ODIN3_NAME_MAP_MAX_ENTRIES) {
        parse_error(ps, "too many entries in the name map", (odin3_bytes){NULL, 0});
        return ODIN3_ERR_PARSE;
    }
    odin3_name_entry *slot = (odin3_name_entry *)odin3_vec_push(&ps->map->entries);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = *entry;
    return ODIN3_OK;
}

/* One entry line: kept, or warned about and ignored. NO_MEMORY or PARSE end the parse. */
static odin3_status parse_entry_v1(nm_parser *ps, odin3_bytes line) {
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
    return push_entry(ps, &entry);
}

/* --- parse, version 2 ------------------------------------------------------------------------ */

/* The type letters of version 2 and the fewest fields each takes (after the letter). */
typedef struct nm2_type {
    char letter;
    uint32_t min_fields;
} nm2_type;

enum {
    O_FIELDS = 9,
    N_FIELDS = 5,
    R_FIELDS = 9,
    C_FIELDS = 7,
    F_FIELDS = 2,
    T_FIELDS = 4,
    A_FIELDS = 3,
    Q_FIELDS = 12,
    K_FIELDS = 7
};
static const nm2_type NM2_TYPES[] = {{'O', O_FIELDS}, {'N', N_FIELDS}, {'R', R_FIELDS},
                                     {'C', C_FIELDS}, {'F', F_FIELDS}, {'T', T_FIELDS},
                                     {'A', A_FIELDS}, {'Q', Q_FIELDS}, {'K', K_FIELDS}};

static const nm2_type *nm2_type_of(char letter) {
    for (size_t k = 0; k < sizeof NM2_TYPES / sizeof NM2_TYPES[0]; k++) {
        if (NM2_TYPES[k].letter == letter) {
            return &NM2_TYPES[k];
        }
    }
    return NULL;
}

/* Splits rest at TABs; keeps the first ODIN3_NM2_MAX_FIELDS and returns how many there were. */
static uint32_t split_fields_v2(odin3_bytes rest, odin3_bytes out[ODIN3_NM2_MAX_FIELDS]) {
    const char *bytes = (const char *)rest.ptr;
    uint32_t count = 0;
    size_t start = 0;
    for (size_t i = 0; i <= rest.len; i++) {
        if (i < rest.len && bytes[i] != '\t') {
            continue;
        }
        if (count < ODIN3_NM2_MAX_FIELDS) {
            out[count] = (odin3_bytes){bytes + start, i - start};
        }
        count++;
        start = i + 1;
    }
    return count;
}

/* Keeps a parsed line (a copy of its text in the arena). NO_MEMORY or PARSE (over the limit). */
static odin3_status push_line(nm_parser *ps, char type, odin3_bytes line) {
    odin3_bytes fields[ODIN3_NM2_MAX_FIELDS] = {{NULL, 0}};
    char *copy = (char *)odin3_arena_alloc(ps->map->arena, line.len + 1);
    if (copy == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    memcpy(copy, line.ptr, line.len);
    copy[line.len] = '\0';
    uint32_t have = split_fields_v2((odin3_bytes){copy + 2, line.len - 2}, fields);
    if (ps->map->lines.len >= ODIN3_NAME_MAP_MAX_ENTRIES) {
        parse_error(ps, "too many lines in the name map", (odin3_bytes){NULL, 0});
        return ODIN3_ERR_PARSE;
    }
    odin3_nm_line *slot = (odin3_nm_line *)odin3_vec_push(&ps->map->lines);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = (odin3_nm_line){.type = type, .lineno = ps->lineno};
    slot->nfields = have < ODIN3_NM2_MAX_FIELDS ? have : (uint32_t)ODIN3_NM2_MAX_FIELDS;
    memcpy(slot->f, fields, sizeof fields);
    return ODIN3_OK;
}

/* An O line: the seven version 1 fields become an entry (checked as there). */
static odin3_status parse_o_line(nm_parser *ps, odin3_bytes line,
                                 const odin3_bytes raw[ODIN3_NM2_MAX_FIELDS]) {
    odin3_name_entry entry = {{NULL, 0}, ODIN3_NAME_NET, {NULL, 0}, {NULL, 0},
                              {NULL, 0}, {NULL, 0},      0};
    const char *why = NULL;
    odin3_status st = build_entry(ps, raw, &entry, &why);
    if (st == ODIN3_ERR_INVALID_ARG) {
        if (why != NULL) {
            ignore_line(ps, why);
        }
        return ODIN3_OK;
    }
    st = st == ODIN3_OK ? push_line(ps, 'O', line) : st;
    return st == ODIN3_OK ? push_entry(ps, &entry) : st;
}

/* One version 2 line: kept, or warned about and ignored. NO_MEMORY or PARSE end the parse. */
static odin3_status parse_entry(nm_parser *ps, odin3_bytes line) {
    if (ps->map->version != ODIN3_NAME_MAP_VERSION_2) {
        return parse_entry_v1(ps, line);
    }
    odin3_bytes raw[ODIN3_NM2_MAX_FIELDS] = {{NULL, 0}};
    if (line.len > ODIN3_NAME_MAP_MAX_LINE) {
        ignore_line(ps, "line is too long");
        return ODIN3_OK;
    }
    if (((const char *)line.ptr)[line.len - 1] == '\r') {
        ignore_line(ps, "CR at the end of the line (a name map uses LF line ends)");
        return ODIN3_OK;
    }
    char letter = ((const char *)line.ptr)[0];
    if (line.len < 2 || ((const char *)line.ptr)[1] != '\t') {
        ignore_line(ps, "no type letter and TAB at the start of the line");
        return ODIN3_OK;
    }
    const nm2_type *type = nm2_type_of(letter);
    if (type == NULL) {
        if (!ps->warned[(unsigned char)letter]) {
            ps->warned[(unsigned char)letter] = true;
            odin3_log(ODIN3_LOG_WARN, "%s:%u: name map line type '%c' is unknown and ignored",
                      ps->path, ps->lineno, letter);
        }
        ps->map->ignored++;
        return ODIN3_OK;
    }
    uint32_t nfields =
        split_fields_v2((odin3_bytes){(const char *)line.ptr + 2, line.len - 2}, raw);
    if (nfields < type->min_fields) {
        ignore_line(ps, "too few fields for its type");
        return ODIN3_OK;
    }
    if (letter == 'O') {
        return parse_o_line(ps, line, raw);
    }
    return push_line(ps, letter, line);
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
    nm_parser ps = {.map = map, .path = path};
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

/* --- version 2: write --------------------------------------------------------------------------
 */

int odin3_name_entry_cmp(const odin3_name_entry *lhs, const odin3_name_entry *rhs) {
    return key_cmp(lhs, rhs);
}

odin3_status odin3_name_map_escape(odin3_strbuf *out, odin3_bytes text) {
    return put_field_ex(out, text, true);
}

odin3_status odin3_name_map_unescape(odin3_name_map *map, odin3_bytes field, odin3_bytes *out) {
    return unescape_ex(map->arena, field, true, out);
}

odin3_status odin3_name_map_write_header_v2(odin3_strbuf *out, uint32_t width, const char *ast) {
    bool known = false;
    for (size_t k = 0; ast != NULL && k < sizeof AST_WORDS / sizeof AST_WORDS[0]; k++) {
        known = known || strcmp(ast, AST_WORDS[k]) == 0;
    }
    if (width < 1 || width > ODIN3_HASH_NAME_MAX_CHARS || !known) {
        odin3_log(ODIN3_LOG_ERROR, "name map: version 2 header width %u or ast '%s' out of range",
                  width, ast != NULL ? ast : "");
        return ODIN3_ERR_INVALID_ARG;
    }
    return odin3_strbuf_appendf(out, "# odin3-names %d hash=sha256 width=%u %s ast=%s\n",
                                ODIN3_NAME_MAP_VERSION_2, width, PROV_TOKEN, ast);
}

static odin3_status put_line_fields(odin3_strbuf *out, char type, const odin3_nm_field *fields,
                                    size_t n) {
    odin3_status st = odin3_strbuf_appendf(out, "%c\t", type);
    for (size_t i = 0; st == ODIN3_OK && i < n; i++) {
        if (i > 0) {
            st = odin3_strbuf_append(out, odin3_bytes_cstr("\t"));
        }
        if (st == ODIN3_OK && fields[i].raw) {
            st = fields[i].text.len == 0 ? ODIN3_OK : odin3_strbuf_append(out, fields[i].text);
        } else if (st == ODIN3_OK) {
            st = put_field_ex(out, fields[i].text, true);
        }
    }
    return st == ODIN3_OK ? odin3_strbuf_append(out, odin3_bytes_cstr("\n")) : st;
}

odin3_status odin3_name_map_write_line_v2(odin3_strbuf *out, char type,
                                          const odin3_nm_field *fields, size_t n) {
    bool letter = (type >= 'A' && type <= 'Z') || (type >= 'a' && type <= 'z');
    if (!letter || n == 0 || n > ODIN3_NM2_MAX_FIELDS) {
        odin3_log(ODIN3_LOG_ERROR, "name map: a version 2 line of type '%c' with %zu fields", type,
                  n);
        return ODIN3_ERR_INVALID_ARG;
    }
    size_t start = out->len;
    odin3_status st = put_line_fields(out, type, fields, n);
    if (st == ODIN3_OK && out->len - start - 1 > ODIN3_NAME_MAP_MAX_LINE) {
        odin3_log(ODIN3_LOG_ERROR,
                  "name map: a version 2 line of type '%c' is %zu bytes, over the "
                  "limit of %d",
                  type, out->len - start - 1, ODIN3_NAME_MAP_MAX_LINE);
        st = ODIN3_ERR_INVALID_ARG;
    }
    if (st != ODIN3_OK && out->len != start) {
        truncate_to(out, start);
    }
    return st;
}

/* --- version 2: check --------------------------------------------------------------------------
 */

/* The number in a reference like `r12` (prefix r), false for anything else. */
bool odin3_name_map_ref_number(odin3_bytes tok, char prefix, uint32_t *num) {
    if (tok.len < 2 || ((const char *)tok.ptr)[0] != prefix) {
        return false;
    }
    return parse_decimal((odin3_bytes){(const char *)tok.ptr + 1, tok.len - 1}, num) && *num > 0;
}

/* Calls fn for each element of a list field split at sep (an empty field has none). */
typedef void (*elem_fn)(void *ctx, odin3_bytes elem);

static void each_element(odin3_bytes field, char sep, elem_fn fn, void *ctx) {
    const char *bytes = (const char *)field.ptr;
    size_t start = 0;
    for (size_t i = 0; field.len > 0 && i <= field.len; i++) {
        if (i == field.len || bytes[i] == sep) {
            fn(ctx, (odin3_bytes){bytes + start, i - start});
            start = i + 1;
        }
    }
}

/* What a check of one line needs: the counters, the line's own number and the table sizes. */
typedef struct chk {
    odin3_name_check_v2 *res;
    uint32_t own;      /* the number of the line being checked */
    uint32_t limit[4]; /* r, c, f, t counts */
} chk;

enum { LIM_R, LIM_C, LIM_F, LIM_T };

static void ref_elem(chk *ck, odin3_bytes tok, char prefix, uint32_t limit, bool before) {
    uint32_t num = 0;
    if (!odin3_name_map_ref_number(tok, prefix, &num) || num > limit) {
        ck->res->bad_ref++;
    } else if (before && num >= ck->own) {
        ck->res->bad_order++;
    }
}

typedef struct list_job {
    chk *ck;
    char prefix;
    uint32_t limit;
    bool before; /* every element must be numbered before the line's own */
} list_job;

static void list_elem(void *ctx, odin3_bytes elem) {
    const list_job *job = ctx;
    ref_elem(job->ck, elem, job->prefix, job->limit, job->before);
}

static uint32_t limit_of(const chk *ck, char prefix) {
    switch (prefix) {
    case 'r':
        return ck->limit[LIM_R];
    case 'c':
        return ck->limit[LIM_C];
    case 'f':
        return ck->limit[LIM_F];
    default:
        return ck->limit[LIM_T];
    }
}

static void check_list(chk *ck, odin3_bytes field, char prefix, bool before) {
    list_job job = {ck, prefix, limit_of(ck, prefix), before};
    each_element(field, ',', list_elem, &job);
}

/* An origin `f<n>:span` or `-:span`. */
static void origin_elem(void *ctx, odin3_bytes elem) {
    chk *ck = ctx;
    const char *bytes = (const char *)elem.ptr;
    const char *colon = elem.len > 0 ? (const char *)memchr(bytes, ':', elem.len) : NULL;
    if (colon == NULL) {
        ck->res->bad_ref++;
        return;
    }
    odin3_bytes file = {bytes, (size_t)(colon - bytes)};
    if (bytes_cmp(file, odin3_bytes_cstr("-")) != 0) {
        ref_elem(ck, file, 'f', ck->limit[LIM_F], false);
    }
}

/* `call=c<n>` in a flags element resolves. */
static void flag_elem(void *ctx, odin3_bytes elem) {
    chk *ck = ctx;
    static const char CALL[] = "call=";
    if (elem.len > sizeof CALL - 1 && memcmp(elem.ptr, CALL, sizeof CALL - 1) == 0) {
        ref_elem(
            ck,
            (odin3_bytes){(const char *)elem.ptr + sizeof CALL - 1, elem.len - (sizeof CALL - 1)},
            'c', ck->limit[LIM_C], false);
    }
}

/* The dir field: `origin` or `<refine|rewrite|coarsen|context>:<level>`. */
static bool dir_valid(odin3_bytes dir, char kind) {
    static const char *const DIRS[] = {"refine", "rewrite", "coarsen", "context"};
    if (kind != 'D') {
        return bytes_cmp(dir, odin3_bytes_cstr("origin")) == 0;
    }
    const char *colon = dir.len > 0 ? (const char *)memchr(dir.ptr, ':', dir.len) : NULL;
    if (colon == NULL) {
        return false;
    }
    odin3_bytes word = {dir.ptr, (size_t)(colon - (const char *)dir.ptr)};
    for (size_t k = 0; k < sizeof DIRS / sizeof DIRS[0]; k++) {
        if (bytes_cmp(word, odin3_bytes_cstr(DIRS[k])) == 0) {
            return dir.len > word.len + 1;
        }
    }
    return false;
}

/* The step `t<n>.<op>`: t resolves and op is a number. */
static bool step_valid(const chk *ck, odin3_bytes step) {
    const char *dot = step.len > 0 ? (const char *)memchr(step.ptr, '.', step.len) : NULL;
    uint32_t num = 0;
    uint32_t op = 0;
    if (dot == NULL) {
        return false;
    }
    odin3_bytes run = {step.ptr, (size_t)(dot - (const char *)step.ptr)};
    odin3_bytes op_text = {dot + 1, step.len - run.len - 1};
    return odin3_name_map_ref_number(run, 't', &num) && num <= ck->limit[LIM_T] &&
           parse_decimal(op_text, &op) && op > 0;
}

static void check_r(chk *ck, const odin3_nm_line *line) {
    const odin3_bytes *fld = line->f;
    char kind = '?';
    if (fld[NM_R_KIND].len == 1) {
        kind = ((const char *)fld[NM_R_KIND].ptr)[0];
    }
    if ((kind != 'S' && kind != 'I' && kind != 'D') || fld[NM_R_LEVEL].len == 0 ||
        !dir_valid(fld[NM_R_DIR], kind) || !step_valid(ck, fld[NM_R_STEP])) {
        ck->res->bad_class++;
    }
    check_list(ck, fld[NM_R_PARENTS], 'r', true);
    each_element(fld[NM_R_ORIGINS], '|', origin_elem, ck);
    check_list(ck, fld[NM_R_CONS], 'c', false);
    each_element(fld[NM_R_FLAGS], ',', flag_elem, ck);
}

static void check_c(chk *ck, const odin3_nm_line *line) {
    if (line->f[NM_C_PARENT].len > 0) {
        ref_elem(ck, line->f[NM_C_PARENT], 'c', ck->limit[LIM_C], true);
    }
    if (line->f[NM_C_FILE].len > 0) {
        ref_elem(ck, line->f[NM_C_FILE], 'f', ck->limit[LIM_F], false);
    }
}

/* The stale field of an A, Q or K line: `-` or a run `t<n>` that resolves. */
static void check_stale(chk *ck, odin3_bytes stale) {
    if (stale.len == 1 && ((const char *)stale.ptr)[0] == '-') {
        return;
    }
    ref_elem(ck, stale, 't', ck->limit[LIM_T], false);
}

/* A, Q and K lines (post-Odin data, V1.2.0 3.2): the record ids and the stale run resolve. */
static void check_extra(chk *ck, const odin3_nm_line *line) {
    const odin3_bytes *fld = line->f;
    if (line->type == 'A') {
        check_list(ck, fld[NM_A_RECS], 'r', false);
        check_stale(ck, fld[NM_A_STALE]);
    } else if (line->type == 'Q') {
        ref_elem(ck, fld[NM_Q_ID], 'r', ck->limit[LIM_R], false);
        check_stale(ck, fld[NM_Q_STALE]);
    } else {
        ref_elem(ck, fld[NM_K_ID], 'r', ck->limit[LIM_R], false);
        check_stale(ck, fld[NM_K_STALE]);
    }
}

static void check_object(chk *ck, const odin3_nm_line *line) {
    odin3_bytes recs = line->type == 'O' ? line->f[NM_O_RECS] : line->f[NM_N_RECS];
    uint32_t before = ck->res->bad_ref;
    check_list(ck, recs, 'r', false);
    if (recs.len == 0 || ck->res->bad_ref != before) {
        ck->res->bad_ref = before;
        ck->res->no_record++;
    }
}

/* Counts the lines of each numbered type and checks that their ids run 1, 2, 3, ... */
static void count_lines(const odin3_name_map *map, chk *ck) {
    static const char LETTERS[] = {'R', 'C', 'F', 'T'};
    static const char PREFIX[] = {'r', 'c', 'f', 't'};
    uint32_t seen[4] = {0, 0, 0, 0};
    for (size_t i = 0; i < map->lines.len; i++) {
        const odin3_nm_line *line = (const odin3_nm_line *)odin3_vec_cat(&map->lines, i);
        for (size_t k = 0; k < sizeof LETTERS; k++) {
            uint32_t num = 0;
            if (line->type != LETTERS[k]) {
                continue;
            }
            seen[k]++;
            if (!odin3_name_map_ref_number(line->f[0], PREFIX[k], &num) || num != seen[k]) {
                ck->res->bad_id++;
            }
        }
        ck->res->objects += line->type == 'O' || line->type == 'N' ? 1U : 0U;
    }
    for (size_t k = 0; k < 4; k++) {
        ck->limit[k] = seen[k];
    }
    ck->res->records = seen[LIM_R];
    ck->res->constructs = seen[LIM_C];
    ck->res->files = seen[LIM_F];
    ck->res->runs = seen[LIM_T];
}

odin3_name_check_v2 odin3_name_map_check_v2(const odin3_name_map *map) {
    odin3_name_check_v2 res = {0};
    chk ck = {&res, 0, {0, 0, 0, 0}};
    uint32_t number[4] = {0, 0, 0, 0};
    if (map->version != ODIN3_NAME_MAP_VERSION_2) {
        return res;
    }
    res.ignored = map->ignored;
    count_lines(map, &ck);
    for (size_t i = 0; i < map->lines.len; i++) {
        const odin3_nm_line *line = (const odin3_nm_line *)odin3_vec_cat(&map->lines, i);
        if (line->type == 'R') {
            ck.own = ++number[LIM_R];
            check_r(&ck, line);
        } else if (line->type == 'C') {
            ck.own = ++number[LIM_C];
            check_c(&ck, line);
        } else if (line->type == 'O' || line->type == 'N') {
            check_object(&ck, line);
        } else if (line->type == 'A' || line->type == 'Q' || line->type == 'K') {
            check_extra(&ck, line);
        }
    }
    return res;
}

bool odin3_name_check_v2_ok(const odin3_name_check_v2 *check) {
    return check->bad_id == 0 && check->bad_ref == 0 && check->bad_order == 0 &&
           check->bad_class == 0 && check->no_record == 0;
}

bool odin3_name_check_v2_report(const odin3_name_check_v2 *check, const char *who) {
    const struct {
        uint32_t count;
        const char *what;
    } problems[] = {{check->bad_id, "line(s) whose id is not their line number"},
                    {check->bad_ref, "reference(s) that do not resolve"},
                    {check->bad_order, "parent(s) not numbered before their child"},
                    {check->bad_class, "R line(s) with no valid kind, dir or step"},
                    {check->no_record, "O or N line(s) whose record is missing"}};
    for (size_t k = 0; k < sizeof problems / sizeof problems[0]; k++) {
        if (problems[k].count != 0) {
            odin3_log(ODIN3_LOG_ERROR, "%s: name map check: %u %s", who != NULL ? who : "name map",
                      (unsigned)problems[k].count, problems[k].what);
        }
    }
    if (check->ignored != 0) {
        odin3_log(ODIN3_LOG_WARN, "%s: name map check: %u line(s) ignored by the parse",
                  who != NULL ? who : "name map", (unsigned)check->ignored);
    }
    return odin3_name_check_v2_ok(check);
}
