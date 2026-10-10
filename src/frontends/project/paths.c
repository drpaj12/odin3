/*
 * paths.c — lexical path resolution (PRJ-6), existence checks and the extension tables (§4.1).
 */
#include "frontends/project/paths.h"

#include "util/alloc.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>

enum { SUFFIX_MAX = 8 }; /* longer than any suffix in the tables below */

/* --- normalization (os.path.normpath) ------------------------------------------------------ */

/* A path normalized in place: buf[0, initial) are the kept leading slashes, buf[initial, wpos)
 * the components written so far (wpos never passes the read position). */
typedef struct norm {
    char *buf;
    size_t initial;
    size_t wpos;
} norm;

/* The start of the last component written. */
static size_t last_start(const norm *nm) {
    size_t pos = nm->wpos;
    while (pos > nm->initial && nm->buf[pos - 1] != '/') {
        pos--;
    }
    return pos;
}

static bool is_dotdot(const char *comp, size_t len) {
    return len == 2 && comp[0] == '.' && comp[1] == '.';
}

/* Applies the component comp[0, len) (inside nm->buf, at or after nm->wpos) to the result. */
static void apply_component(norm *nm, const char *comp, size_t len) {
    if (len == 0 || (len == 1 && comp[0] == '.')) {
        return;
    }
    bool dotdot = is_dotdot(comp, len);
    if (dotdot && nm->wpos > nm->initial) {
        size_t last = last_start(nm);
        if (!is_dotdot(nm->buf + last, nm->wpos - last)) { /* fold against the preceding one */
            nm->wpos = last > nm->initial ? last - 1 : nm->initial;
            return;
        }
    } else if (dotdot && nm->initial > 0) {
        return; /* ".." at the root */
    }
    if (nm->wpos > nm->initial) {
        nm->buf[nm->wpos++] = '/';
    }
    memmove(nm->buf + nm->wpos, comp, len);
    nm->wpos += len;
}

/* Normalizes buf[0, len) in place (len ≥ 1) and returns the new length (≥ 1). */
static size_t normalize_in_place(char *buf, size_t len) {
    size_t lead = 0;
    while (lead < len && buf[lead] == '/') {
        lead++;
    }
    size_t initial = 1; /* POSIX: one leading slash, or exactly two kept as they are */
    if (lead == 0 || lead == 2) {
        initial = lead;
    }
    norm nm = {buf, initial, initial};
    size_t pos = lead;
    while (pos < len) {
        size_t end = pos;
        while (end < len && buf[end] != '/') {
            end++;
        }
        apply_component(&nm, buf + pos, end - pos);
        pos = end < len ? end + 1 : end;
    }
    if (nm.wpos == 0) {
        buf[nm.wpos++] = '.';
    }
    return nm.wpos;
}

/* Replaces out with the normalization of the concatenation head + tail. */
static odin3_status normalize_parts(odin3_bytes head, odin3_bytes tail, odin3_strbuf *out) {
    odin3_strbuf_clear(out);
    size_t len = head.len + tail.len;
    char *tmp = odin3_util_malloc(len + 1);
    if (tmp == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    if (head.len > 0) {
        memcpy(tmp, head.ptr, head.len);
    }
    if (tail.len > 0) {
        memcpy(tmp + head.len, tail.ptr, tail.len);
    }
    size_t done = len == 0 ? 0 : normalize_in_place(tmp, len);
    odin3_status st = done == 0 ? odin3_strbuf_append(out, odin3_bytes_cstr("."))
                                : odin3_strbuf_append(out, (odin3_bytes){tmp, done});
    odin3_util_free(tmp);
    return st;
}

odin3_status odin3_path_normalize(odin3_bytes path, odin3_strbuf *out) {
    return normalize_parts(path, (odin3_bytes){NULL, 0}, out);
}

odin3_status odin3_path_resolve(odin3_path_join join, odin3_strbuf *out) {
    const char *given = join.given.ptr;
    if (join.base_dir.len == 0 || (join.given.len > 0 && given[0] == '/')) {
        return odin3_path_normalize(join.given, out);
    }
    odin3_strbuf_clear(out);
    odin3_strbuf head;
    odin3_strbuf_init(&head);
    odin3_status st = odin3_strbuf_append(&head, join.base_dir);
    if (((const char *)join.base_dir.ptr)[join.base_dir.len - 1] != '/') {
        st = st != ODIN3_OK ? st : odin3_strbuf_append(&head, odin3_bytes_cstr("/"));
    }
    st = st != ODIN3_OK ? st : normalize_parts((odin3_bytes){head.data, head.len}, join.given, out);
    odin3_strbuf_free(&head);
    return st;
}

odin3_path_kind odin3_path_stat(const char *path) {
    struct stat info;
    if (stat(path, &info) != 0) {
        return ODIN3_PATH_NONE;
    }
    if (S_ISREG(info.st_mode)) {
        return ODIN3_PATH_FILE;
    }
    return S_ISDIR(info.st_mode) ? ODIN3_PATH_DIR : ODIN3_PATH_NONE;
}

odin3_bytes odin3_path_dir(odin3_bytes path) {
    const char *text = path.ptr;
    size_t j = path.len;
    while (j > 0 && text[j - 1] != '/') {
        j--;
    }
    size_t head = j; /* Python's dirname: the head, trailing '/' stripped unless all slashes */
    while (head > 0 && text[head - 1] == '/') {
        head--;
    }
    return (odin3_bytes){text, head == 0 ? j : head};
}

/* --- extension tables ---------------------------------------------------------------------- */

/* The end of the last component of text[0, end) as pathlib sees it: trailing '/' and "."
 * components dropped. */
static size_t name_end(const char *text, size_t end) {
    for (;;) {
        while (end > 0 && text[end - 1] == '/') {
            end--;
        }
        bool dot = end > 0 && text[end - 1] == '.' && (end == 1 || text[end - 2] == '/');
        if (!dot) {
            return end;
        }
        end--;
    }
}

odin3_bytes odin3_path_suffix(odin3_bytes path) {
    const char *text = path.ptr;
    size_t end = path.len > 0 ? name_end(text, path.len) : 0;
    size_t start = end;
    while (start > 0 && text[start - 1] != '/') {
        start--;
    }
    size_t dot = end;
    while (dot > start && text[dot - 1] != '.') {
        dot--;
    }
    if (dot <= start + 1 || dot == end) { /* no '.', a leading '.' or a trailing '.' */
        return (odin3_bytes){"", 0};
    }
    return (odin3_bytes){text + dot - 1, end - dot + 1};
}

/*
 * Copies Python's Path(path).suffix, lower-cased (ASCII), into buf: the last component (trailing
 * '/' and "." components ignored) from its last '.', when that '.' is neither its first nor its
 * last byte. prefixed: the path is read as "x" + path (a libext). false when there is none or it
 * is longer than the tables' suffixes.
 */
static bool lower_suffix(odin3_bytes path, bool prefixed, char buf[SUFFIX_MAX + 1]) {
    const char *text = path.ptr;
    size_t end = name_end(text, path.len);
    size_t start = end;
    while (start > 0 && text[start - 1] != '/') {
        start--;
    }
    bool leading_x = prefixed && start == 0; /* the component begins with the "x" */
    size_t dot = end;
    while (dot > start && text[dot - 1] != '.') {
        dot--;
    }
    bool leading_dot = dot == start + 1 && !leading_x;
    if (dot == start || leading_dot || dot == end || end - (dot - 1) > SUFFIX_MAX) {
        return false; /* no '.', a leading '.', a trailing '.', or too long */
    }
    size_t len = end - (dot - 1);
    for (size_t i = 0; i < len; i++) {
        char byte = text[dot - 1 + i];
        if (byte >= 'A' && byte <= 'Z') {
            byte = (char)(byte - 'A' + 'a');
        }
        buf[i] = byte;
    }
    buf[len] = '\0';
    return true;
}

typedef struct suffix_entry {
    const char *suffix;
    uint8_t value;
} suffix_entry;

static const suffix_entry k_lang_suffixes[] = {
    {".v", ODIN3_LANG_VERILOG},        {".vh", ODIN3_LANG_VERILOG},
    {".sv", ODIN3_LANG_SYSTEMVERILOG}, {".svh", ODIN3_LANG_SYSTEMVERILOG},
    {".vhd", ODIN3_LANG_VHDL},         {".vhdl", ODIN3_LANG_VHDL},
    {".blif", ODIN3_LANG_BLIF},        {".vqm", ODIN3_LANG_VQM},
    {".edf", ODIN3_LANG_EDIF},         {".edif", ODIN3_LANG_EDIF},
};

static const suffix_entry k_arch_suffixes[] = {
    {".o3lib", ODIN3_PROJ_ARCH_O3LIB},
    {".xml", ODIN3_PROJ_ARCH_VPR_XML},
};

typedef struct suffix_table {
    const suffix_entry *entries;
    size_t count;
} suffix_table;

/* A path and whether it is read with an "x" before it (a libext). */
typedef struct suffix_of {
    odin3_bytes path;
    bool prefixed;
} suffix_of;

static bool lookup_suffix(suffix_of what, suffix_table table, uint8_t *value) {
    char buf[SUFFIX_MAX + 1];
    if (!lower_suffix(what.path, what.prefixed, buf)) {
        return false;
    }
    for (size_t i = 0; i < table.count; i++) {
        if (strcmp(buf, table.entries[i].suffix) == 0) {
            *value = table.entries[i].value;
            return true;
        }
    }
    return false;
}

static bool lang_lookup(suffix_of what, odin3_lang *out) {
    uint8_t value = 0;
    suffix_table table = {k_lang_suffixes, sizeof k_lang_suffixes / sizeof k_lang_suffixes[0]};
    if (!lookup_suffix(what, table, &value)) {
        return false;
    }
    *out = (odin3_lang)value;
    return true;
}

bool odin3_lang_of(odin3_bytes path, odin3_lang *out) {
    return lang_lookup((suffix_of){path, false}, out);
}

bool odin3_lang_of_ext(odin3_bytes ext, odin3_lang *out) {
    return lang_lookup((suffix_of){ext, true}, out);
}

bool odin3_arch_kind_of(odin3_bytes path, uint8_t *kind) {
    suffix_table table = {k_arch_suffixes, sizeof k_arch_suffixes / sizeof k_arch_suffixes[0]};
    return lookup_suffix((suffix_of){path, false}, table, kind);
}

bool odin3_lang_by_name(odin3_bytes name, odin3_lang *out) {
    for (int lang = ODIN3_LANG_VERILOG; lang <= ODIN3_LANG_EDIF; lang++) {
        const char *known = odin3_lang_name((odin3_lang)lang);
        if (strlen(known) == name.len && memcmp(known, name.ptr, name.len) == 0) {
            *out = (odin3_lang)lang;
            return true;
        }
    }
    return false;
}
