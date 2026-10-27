/* prov_extra.c — see prov_extra.h. */
#include "backends/common/prov_extra.h"

#include "ir/restore.h"
#include "util/alloc.h"
#include "util/log.h"

#include <stdio.h>
#include <string.h>

enum { DECIMAL_BASE = 10, ID_TEXT = 16 };

bool odin3_xline_split(odin3_bytes line, odin3_xline *out) {
    const char *bytes = line.ptr;
    if (line.len < 2U || bytes[1] != '\t' ||
        (bytes[0] != 'A' && bytes[0] != 'Q' && bytes[0] != 'K')) {
        return false;
    }
    *out = (odin3_xline){.type = bytes[0]};
    size_t start = 2;
    for (size_t i = 2; i <= line.len; i++) {
        if (i == line.len || bytes[i] == '\t') {
            if (out->n < ODIN3_NM2_MAX_FIELDS) {
                out->f[out->n] = (odin3_bytes){bytes + start, i - start};
            }
            out->n++;
            start = i + 1;
        }
    }
    out->refs = out->type == 'A' ? NM_A_RECS : NM_Q_ID;
    out->stale = NM_K_STALE;
    if (out->type == 'A') {
        out->stale = NM_A_STALE;
    } else if (out->type == 'Q') {
        out->stale = NM_Q_STALE;
    }
    return out->n > out->stale && out->n <= ODIN3_NM2_MAX_FIELDS;
}

/* Reads a decimal at *at (up to end); false when there is none. */
static bool take_number(const char **at, const char *end, uint32_t *out) {
    uint64_t value = 0;
    const char *from = *at;
    while (*at < end && **at >= '0' && **at <= '9' && value <= UINT32_MAX) {
        value = (value * DECIMAL_BASE) + (uint64_t)(**at - '0');
        (*at)++;
    }
    *out = (uint32_t)value;
    return *at > from && value <= UINT32_MAX;
}

/* `<letter><n>` as a whole field. */
static bool parse_ref(odin3_bytes field, char letter, uint32_t *out) {
    const char *at = (const char *)field.ptr + 1;
    const char *end = (const char *)field.ptr + field.len;
    return field.len > 1U && ((const char *)field.ptr)[0] == letter && take_number(&at, end, out) &&
           at == end;
}

uint32_t odin3_xline_stale(const odin3_design *design, const odin3_xline *line) {
    uint32_t run = 0;
    if (parse_ref(line->f[line->stale], 't', &run)) {
        return run;
    }
    return odin3_extra_mutating(odin3_design_extra(design));
}

odin3_status odin3_xline_records(const odin3_xline *line, odin3_vec *ids) {
    odin3_bytes field = line->f[line->refs];
    const char *bytes = field.ptr;
    size_t start = 0;
    for (size_t i = 0; field.len > 0 && i <= field.len; i++) {
        if (i != field.len && bytes[i] != ',') {
            continue;
        }
        uint32_t id = 0;
        uint32_t *slot = parse_ref((odin3_bytes){bytes + start, i - start}, 'r', &id)
                             ? odin3_vec_push(ids)
                             : NULL;
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = id;
        start = i + 1;
    }
    return ODIN3_OK;
}

/* A list of `r<id>` written with the numbers of this map. */
static odin3_status append_records(const odin3_xline_nums *nums, odin3_bytes field,
                                   odin3_strbuf *buf) {
    const char *bytes = field.ptr;
    size_t start = 0;
    odin3_status status = ODIN3_OK;
    for (size_t i = 0; status == ODIN3_OK && field.len > 0 && i <= field.len; i++) {
        if (i != field.len && bytes[i] != ',') {
            continue;
        }
        uint32_t id = 0;
        uint64_t num = 0;
        if (!parse_ref((odin3_bytes){bytes + start, i - start}, 'r', &id) ||
            !odin3_u64map_get(nums->num_of, id, &num)) {
            odin3_log(ODIN3_LOG_ERROR,
                      "name map writer: an extra line names a record the map does not hold");
            return ODIN3_ERR_INVALID_ARG;
        }
        status = buf->len > 0 ? odin3_strbuf_append(buf, odin3_bytes_cstr(",")) : status;
        status = status == ODIN3_OK ? odin3_strbuf_appendf(buf, "r%u", (unsigned)num) : status;
        start = i + 1;
    }
    return status;
}

/* The stale field: `-`, or the number of the run that makes the line stale. */
static odin3_status append_stale(const odin3_xline_nums *nums, const odin3_xline *line,
                                 odin3_strbuf *buf) {
    uint32_t run = odin3_xline_stale(nums->design, line);
    uint64_t num = 0;
    if (run == 0 || !odin3_u64map_get(nums->run_num, run, &num)) {
        return odin3_strbuf_append(buf, odin3_bytes_cstr("-"));
    }
    return odin3_strbuf_appendf(buf, "t%u", (unsigned)num);
}

odin3_status odin3_xline_emit(const odin3_xline_nums *nums, const odin3_xline *line,
                              odin3_strbuf *out) {
    odin3_strbuf bufs[ODIN3_NM2_MAX_FIELDS];
    odin3_nm_field fields[ODIN3_NM2_MAX_FIELDS];
    odin3_status status = ODIN3_OK;
    for (uint32_t k = 0; k < line->n; k++) {
        odin3_strbuf_init(&bufs[k]);
        if (status == ODIN3_OK && k == line->refs) {
            status = append_records(nums, line->f[k], &bufs[k]);
        } else if (status == ODIN3_OK && k == line->stale) {
            status = append_stale(nums, line, &bufs[k]);
        } else if (status == ODIN3_OK) {
            status = odin3_strbuf_append(&bufs[k], line->f[k]);
        }
        fields[k] = (odin3_nm_field){{bufs[k].data, bufs[k].len}, true};
    }
    status = status == ODIN3_OK ? odin3_name_map_write_line_v2(out, line->type, fields, line->n)
                                : status;
    for (uint32_t k = 0; k < line->n; k++) {
        odin3_strbuf_free(&bufs[k]);
    }
    return status;
}

/* The value of `key=` among the blank-separated words of a pass run's info (a value may be
 * double-quoted, with backslash escapes, and then holds blanks); false when the key is absent,
 * the value is quoted, or it does not fit in cap bytes with its NUL. */
static bool info_word(odin3_bytes info, const char *key, char *out, size_t cap) {
    const char *text = info.ptr;
    const size_t key_len = strlen(key);
    size_t i = 0;
    while (text != NULL && i < info.len) {
        size_t start = i;
        bool quoted = false;
        while (i < info.len && (quoted || text[i] != ' ')) {
            if (text[i] == '\\' && quoted) {
                i++;
            } else if (text[i] == '"') {
                quoted = !quoted;
            }
            i++;
        }
        const size_t end = i < info.len ? i : info.len;
        if (end - start > key_len + 1U && memcmp(text + start, key, key_len) == 0 &&
            text[start + key_len] == '=' && text[start + key_len + 1U] != '"' &&
            end - start - key_len - 1U < cap) {
            memcpy(out, text + start + key_len + 1U, end - start - key_len - 1U);
            out[end - start - key_len - 1U] = '\0';
            return true;
        }
        i = end + 1U;
    }
    return false;
}

/* The objective of a join made by `pass` and its basis letter: `r` (read as written: the pass
 * name, or a goal the script states), `i` (derived from the script by the rule of
 * src/passes/abc/objective.c) or `-` (none: an abc script that states no goal). abc records its
 * objective in its run information; any other pass is named. */
static char objective_of(odin3_bytes pass, odin3_bytes info, char *out, size_t cap) {
    out[0] = '\0';
    if (pass.len == 3U && memcmp(pass.ptr, "abc", 3U) == 0) {
        char src[ID_TEXT] = "";
        if (!info_word(info, "objective", out, cap)) {
            return '-';
        }
        return info_word(info, "objective_src", src, sizeof src) && strcmp(src, "stated") == 0
                   ? 'r'
                   : 'i';
    }
    if (pass.len == 0U || pass.len >= cap) {
        return '-';
    }
    memcpy(out, pass.ptr, pass.len);
    out[pass.len] = '\0';
    return 'r';
}

odin3_status odin3_xline_emit_fresh_q(uint32_t rnum, odin3_bytes pass, odin3_bytes info,
                                      odin3_strbuf *out) {
    char id[ID_TEXT];
    (void)snprintf(id, sizeof id, "r%u", (unsigned)rnum);
    char word[ODIN3_XLINE_OBJECTIVE_BYTES];
    const char basis_letter = objective_of(pass, info, word, sizeof word);
    char basis[sizeof "why=-,objective=r,gain=-,pcrit=-,rho=-,slack=-,near=-,npaths=-"];
    (void)snprintf(basis, sizeof basis,
                   "why=-,objective=%c,gain=-,pcrit=-,rho=-,slack=-,near=-,npaths=-", basis_letter);
    odin3_strbuf objective;
    odin3_strbuf_init(&objective);
    odin3_status status = odin3_name_map_escape(&objective, odin3_bytes_cstr(word));
    const odin3_bytes none = {NULL, 0};
    const odin3_nm_field fields[] = {{odin3_bytes_cstr(id), true},
                                     {none, true},
                                     {{objective.data, objective.len}, true},
                                     {none, true},
                                     {none, true},
                                     {none, true},
                                     {none, true},
                                     {none, true},
                                     {none, true},
                                     {none, true},
                                     {odin3_bytes_cstr(basis), true},
                                     {odin3_bytes_cstr("-"), true}};
    status = status == ODIN3_OK
                 ? odin3_name_map_write_line_v2(out, 'Q', fields, sizeof fields / sizeof fields[0])
                 : status;
    odin3_strbuf_free(&objective);
    return status;
}
