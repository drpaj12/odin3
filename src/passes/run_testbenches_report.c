/*
 * run_testbenches_report.c — line comparison and the text and JSON reports of `run_testbenches`.
 */
#include "passes/run_testbenches.h"

#include "util/str.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { DECIMAL = 10, JSON_FIRST_PLAIN = 0x20, WHEN_BUF = 32 };

static const char k_end[] = "<end of output>";

/* The line starting at text[pos] (without its '\n'); *next is the start of the following one;
 * false when pos is at or past the end. */
static bool next_line(rt_text text, size_t pos, rt_text *line, size_t *next) {
    if (pos >= text.len) {
        return false;
    }
    size_t end = pos;
    while (end < text.len && text.data[end] != '\n') {
        end++;
    }
    *line = (rt_text){text.data + pos, end - pos};
    *next = end < text.len ? end + 1 : end;
    return true;
}

static char *dup_line(rt_text line) {
    char *copy = malloc(line.len + 1);
    if (copy != NULL) {
        memcpy(copy, line.data, line.len);
        copy[line.len] = '\0';
    }
    return copy;
}

/* The 0-based index of the first space-separated field in which a and b differ. */
static uint32_t first_field(rt_text left, rt_text right) {
    uint32_t field = 0;
    size_t i = 0;
    for (; i < left.len && i < right.len && left.data[i] == right.data[i]; i++) {
        field += left.data[i] == ' ' ? 1U : 0U;
    }
    return field;
}

/* The want line's first field as a decimal integer. */
static bool leading_number(rt_text line, uint64_t *out) {
    size_t i = 0;
    uint64_t value = 0;
    while (i < line.len && line.data[i] >= '0' && line.data[i] <= '9') {
        value = value * DECIMAL + (uint64_t)(line.data[i] - '0');
        i++;
    }
    *out = value;
    return i > 0 && (i == line.len || line.data[i] == ' ');
}

bool rt_compare(rt_text want, rt_text got, rt_mismatch *mm) {
    size_t wpos = 0;
    size_t gpos = 0;
    uint32_t number = 0;
    for (;;) {
        rt_text wline = {k_end, sizeof k_end - 1};
        rt_text gline = {k_end, sizeof k_end - 1};
        size_t wnext = wpos;
        size_t gnext = gpos;
        bool have_w = next_line(want, wpos, &wline, &wnext);
        bool have_g = next_line(got, gpos, &gline, &gnext);
        if (!have_w && !have_g) {
            return false;
        }
        number++;
        if (!have_w || !have_g || wline.len != gline.len ||
            memcmp(wline.data, gline.data, wline.len) != 0) {
            mm->present = true;
            mm->line = number;
            mm->has_time = have_w && leading_number(wline, &mm->time);
            mm->field = first_field(wline, gline);
            mm->want = dup_line(wline);
            mm->got = dup_line(gline);
            return true;
        }
        wpos = wnext;
        gpos = gnext;
    }
}

void rt_result_free(rt_result *res) {
    free(res->mm.want);
    free(res->mm.got);
    res->mm.want = NULL;
    res->mm.got = NULL;
}

static const char *state_name(rt_state state) {
    static const char *const names[] = {"pass", "fail", "error"};
    return names[state];
}

static const char *or_empty(const char *text) {
    return text != NULL ? text : "";
}

odin3_status rt_report_text(const rt_result *res, size_t count, odin3_strbuf *out) {
    odin3_status st = ODIN3_OK;
    size_t counts[3] = {0, 0, 0};
    for (size_t i = 0; st == ODIN3_OK && i < count; i++) {
        const rt_result *one = &res[i];
        counts[one->state]++;
        st = odin3_strbuf_appendf(out, "%s %s %s %" PRIu64 " ms\n", one->name,
                                  state_name(one->state), one->on, one->wall_ms);
        if (st == ODIN3_OK && one->state == RT_FAIL && one->mm.present) {
            char when[WHEN_BUF] = "";
            if (one->mm.has_time) {
                (void)snprintf(when, sizeof when, " time %" PRIu64, one->mm.time);
            }
            st = odin3_strbuf_appendf(
                out, "  mismatch: %s line %u%s field %u: want \"%s\" got \"%s\"\n",
                one->mm.compared, (unsigned)one->mm.line, when, (unsigned)one->mm.field,
                or_empty(one->mm.want), or_empty(one->mm.got));
        } else if (st == ODIN3_OK && one->state == RT_ERROR) {
            st = odin3_strbuf_appendf(out, "  error: %s\n", one->error);
        } else if (st == ODIN3_OK && one->timeout) {
            st = odin3_strbuf_appendf(out, "  timeout: %s\n", one->error);
        }
    }
    return st != ODIN3_OK
               ? st
               : odin3_strbuf_appendf(out,
                                      "run_testbenches: %zu passed, %zu failed, %zu "
                                      "errors\n",
                                      counts[RT_PASS], counts[RT_FAIL], counts[RT_ERROR]);
}

static odin3_status json_string(odin3_strbuf *out, const char *text) {
    odin3_status st = odin3_strbuf_append(out, odin3_bytes_cstr("\""));
    for (const char *at = text; st == ODIN3_OK && *at != '\0'; at++) {
        unsigned char byte = (unsigned char)*at;
        if (byte == '"' || byte == '\\') {
            st = odin3_strbuf_appendf(out, "\\%c", *at);
        } else if (byte < JSON_FIRST_PLAIN) {
            st = odin3_strbuf_appendf(out, "\\u%04x", (unsigned)byte);
        } else {
            st = odin3_strbuf_append(out, (odin3_bytes){at, 1});
        }
    }
    return st != ODIN3_OK ? st : odin3_strbuf_append(out, odin3_bytes_cstr("\""));
}

static odin3_status json_mismatch(odin3_strbuf *out, const rt_mismatch *mm) {
    odin3_status st = odin3_strbuf_appendf(out, "{\"compared\": ");
    st = st != ODIN3_OK ? st : json_string(out, mm->compared);
    st = st != ODIN3_OK
             ? st
             : odin3_strbuf_appendf(out, ", \"line\": %u, \"time\": ", (unsigned)mm->line);
    if (st == ODIN3_OK && mm->has_time) {
        st = odin3_strbuf_appendf(out, "%" PRIu64, mm->time);
    } else if (st == ODIN3_OK) {
        st = odin3_strbuf_append(out, odin3_bytes_cstr("null"));
    }
    st = st != ODIN3_OK
             ? st
             : odin3_strbuf_appendf(out, ", \"field\": %u, \"want\": ", (unsigned)mm->field);
    st = st != ODIN3_OK ? st : json_string(out, or_empty(mm->want));
    st = st != ODIN3_OK ? st : odin3_strbuf_append(out, odin3_bytes_cstr(", \"got\": "));
    st = st != ODIN3_OK ? st : json_string(out, or_empty(mm->got));
    return st != ODIN3_OK ? st : odin3_strbuf_append(out, odin3_bytes_cstr("}"));
}

static odin3_status json_result(odin3_strbuf *out, const rt_result *one) {
    odin3_status st = odin3_strbuf_append(out, odin3_bytes_cstr("{\"name\": "));
    st = st != ODIN3_OK ? st : json_string(out, one->name);
    st = st != ODIN3_OK ? st
                        : odin3_strbuf_appendf(out, ", \"status\": \"%s\", \"on\": \"%s\"",
                                               state_name(one->state), one->on);
    st = st != ODIN3_OK ? st
                        : odin3_strbuf_appendf(out,
                                               ", \"wall_ms\": %" PRIu64 ", \"end_time\": %" PRIu64
                                               ", \"lines\": %u, \"mismatch\": ",
                                               one->wall_ms, one->end_time, (unsigned)one->lines);
    if (st == ODIN3_OK && one->mm.present && one->state == RT_FAIL) {
        st = json_mismatch(out, &one->mm);
    } else if (st == ODIN3_OK) {
        st = odin3_strbuf_append(out, odin3_bytes_cstr("null"));
    }
    st = st != ODIN3_OK
             ? st
             : odin3_strbuf_appendf(
                   out, ", \"timeout\": %s, \"error\": ", one->timeout ? "true" : "false");
    if (st == ODIN3_OK && (one->state == RT_ERROR || one->timeout)) {
        st = json_string(out, one->error);
    } else if (st == ODIN3_OK) {
        st = odin3_strbuf_append(out, odin3_bytes_cstr("null"));
    }
    return st != ODIN3_OK ? st : odin3_strbuf_append(out, odin3_bytes_cstr("}"));
}

odin3_status rt_report_json(const rt_result *res, size_t count, odin3_strbuf *out) {
    size_t counts[3] = {0, 0, 0};
    odin3_status st = odin3_strbuf_append(out, odin3_bytes_cstr("{\"testbenches\": ["));
    for (size_t i = 0; st == ODIN3_OK && i < count; i++) {
        counts[res[i].state]++;
        st = i > 0 ? odin3_strbuf_append(out, odin3_bytes_cstr(",\n  ")) : ODIN3_OK;
        st = st != ODIN3_OK ? st : json_result(out, &res[i]);
    }
    return st != ODIN3_OK
               ? st
               : odin3_strbuf_appendf(out,
                                      "],\n \"passed\": %zu, \"failed\": %zu, "
                                      "\"errors\": %zu}\n",
                                      counts[RT_PASS], counts[RT_FAIL], counts[RT_ERROR]);
}
