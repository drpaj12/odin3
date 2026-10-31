/*
 * qsf_tb.c — Quartus EDA_TEST_BENCH_* assignments to project testbenches (see qsf_tb.h).
 */
#include "frontends/project/qsf_tb.h"

#include "ast/srcman.h"
#include "ir/design.h"
#include "util/log.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    TB_NAME_KIND,
    TB_FILE_KIND,
    TB_MODULE_KIND,
    TB_RUN_KIND,
    TB_DUT_KIND,
    TB_SELECT_KIND,
    TB_OTHER_KIND
};

enum {
    DECIMAL = 10,
    TIMESCALE_READ_MAX = 1 << 20,
    TB_VALUE_MAX = 64,
    UNIT_COUNT = 6,
    FS_PER_PS = 1000,
    PS_PER_NS = 1000,
    NS_PER_US = 1000,
    US_PER_MS = 1000,
    MS_PER_S = 1000
};

static const char k_prefix[] = "EDA_TEST_BENCH_";
static const char k_select[] = "EDA_NATIVELINK_SIMULATION_TEST_BENCH";
static const char k_instance[] = "EDA_DESIGN_INSTANCE_NAME";

typedef struct tb_unit {
    const char *name;
    uint64_t fs;
} tb_unit;

/* Time units in femtoseconds. */
static const tb_unit k_units[UNIT_COUNT] = {
    {"fs", 1},
    {"ps", FS_PER_PS},
    {"ns", (uint64_t)FS_PER_PS *PS_PER_NS},
    {"us", (uint64_t)FS_PER_PS *PS_PER_NS *NS_PER_US},
    {"ms", (uint64_t)FS_PER_PS *PS_PER_NS *NS_PER_US *US_PER_MS},
    {"s", (uint64_t)FS_PER_PS *PS_PER_NS *NS_PER_US *US_PER_MS *MS_PER_S},
};

/* What the (upper-cased) assignment name is, one of the TB_*_KIND values. */
static int kind_of(odin3_bytes name) {
    static const char *const suffixes[] = {"NAME", "FILE", "MODULE_NAME", "RUN_SIM_FOR"};
    size_t prefix = sizeof k_prefix - 1;
    if (name.len > prefix && memcmp(name.ptr, k_prefix, prefix) == 0) {
        odin3_bytes rest = {(const char *)name.ptr + prefix, name.len - prefix};
        for (int i = 0; i < TB_DUT_KIND; i++) {
            if (odin3_proj_bytes_is(rest, suffixes[i])) {
                return i;
            }
        }
        return odin3_proj_bytes_is(rest, "DESIGN_INSTANCE_NAME") ? TB_DUT_KIND : TB_OTHER_KIND;
    }
    if (odin3_proj_bytes_is(name, k_instance)) {
        return TB_DUT_KIND;
    }
    return odin3_proj_bytes_is(name, k_select) ? TB_SELECT_KIND : -1;
}

bool odin3_qsf_tb_is_name(odin3_bytes name) {
    return kind_of(name) >= 0;
}

void odin3_qsf_tb_init(odin3_vec *items) {
    odin3_vec_init(items, sizeof(odin3_qsf_tb_item));
}

odin3_status odin3_qsf_tb_note(odin3_vec *items, const odin3_qsf_tb_item *item) {
    odin3_qsf_tb_item *slot = odin3_vec_push(items);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = *item;
    return ODIN3_OK;
}

/* One test bench being assembled. */
typedef struct tb_build {
    odin3_bytes name, file, top, dut, run_for;
    odin3_loc at, file_at, run_at;
    bool has_file;
} tb_build;

static void warn(odin3_project *proj, odin3_loc at, const char *what, odin3_bytes name) {
    odin3_diag(proj->design, ODIN3_LOG_WARN, at, "%s: '%.*s'", what, (int)name.len,
               (const char *)name.ptr);
}

static tb_build *find_build(odin3_vec *builds, odin3_bytes name) {
    for (size_t i = 0; i < builds->len; i++) {
        tb_build *tb = odin3_vec_at(builds, i);
        if (odin3_proj_bytes_eq(tb->name, name)) {
            return tb;
        }
    }
    return NULL;
}

static odin3_status declare(odin3_vec *builds, const odin3_qsf_tb_item *item) {
    if (find_build(builds, item->value) != NULL) {
        return ODIN3_OK;
    }
    tb_build *tb = odin3_vec_push(builds);
    if (tb == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *tb = (tb_build){.name = item->value, .at = item->at};
    return ODIN3_OK;
}

/* Applies one non-declaring assignment to its test bench. */
static void apply(odin3_project *proj, tb_build *tb, int kind, const odin3_qsf_tb_item *item) {
    switch (kind) {
    case TB_FILE_KIND:
        if (tb->has_file) {
            warn(proj, item->at, "extra EDA_TEST_BENCH_FILE not run (the first file is used)",
                 item->value);
            break;
        }
        tb->has_file = true;
        tb->file = item->value;
        tb->file_at = item->at;
        break;
    case TB_MODULE_KIND:
        tb->top = item->value;
        break;
    case TB_RUN_KIND:
        tb->run_for = item->value;
        tb->run_at = item->at;
        break;
    case TB_DUT_KIND:
        tb->dut = odin3_proj_bytes_is(item->value, "NA") ? (odin3_bytes){NULL, 0} : item->value;
        break;
    default:
        break;
    }
}

/* The unit in fs for the text at p ("ns", "us", ...), 0 when it names none. */
static uint64_t unit_at(const char *text) {
    for (int i = 0; i < UNIT_COUNT; i++) {
        if (strncmp(text, k_units[i].name, strlen(k_units[i].name)) == 0) {
            return k_units[i].fs;
        }
    }
    return 0;
}

static const char *skip_blanks(const char *text) {
    while (*text == ' ' || *text == '\t') {
        text++;
    }
    return text;
}

/* The testbench time unit in fs from the first `timescale of the file (multiplier times unit);
 * false when the file has none or cannot be read. */
static bool read_timescale(const char *path, uint64_t *unit_fs) {
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return false;
    }
    char *text = malloc((size_t)TIMESCALE_READ_MAX + 1U);
    size_t got = text != NULL ? fread(text, 1, TIMESCALE_READ_MAX, fp) : 0;
    (void)fclose(fp);
    const char *mark = NULL;
    if (text != NULL) {
        text[got] = '\0';
        mark = strstr(text, "`timescale");
    }
    bool found = false;
    if (mark != NULL) {
        char *end = NULL;
        uint64_t mult = strtoull(mark + strlen("`timescale"), &end, DECIMAL);
        uint64_t unit = unit_at(skip_blanks(end));
        found = mult > 0 && unit > 0;
        *unit_fs = mult * unit;
    }
    free(text);
    return found;
}

/* "<n> <unit>" (blanks optional) as a count of fs; false when it is none. */
static bool run_for_fs(odin3_bytes value, uint64_t *out) {
    char text[TB_VALUE_MAX];
    if (value.len == 0 || value.len >= sizeof text) {
        return false;
    }
    memcpy(text, value.ptr, value.len);
    text[value.len] = '\0';
    char *end = NULL;
    uint64_t count = strtoull(text, &end, DECIMAL);
    uint64_t unit = end != text ? unit_at(skip_blanks(end)) : 0;
    if (unit == 0 || count > UINT64_MAX / unit) {
        return false;
    }
    *out = count * unit;
    return true;
}

/* max-time (testbench units) from the RUN_SIM_FOR value; 0 = no limit. WARNINGs for a bad value,
 * a missing `timescale (1 ns assumed) and a value that is no whole number of units. */
static uint64_t max_time_of(odin3_project *proj, const tb_build *tb, const char *file) {
    uint64_t total = 0;
    if (tb->run_for.ptr == NULL) {
        return 0;
    }
    if (!run_for_fs(tb->run_for, &total)) {
        warn(proj, tb->run_at, "EDA_TEST_BENCH_RUN_SIM_FOR is not '<n> <unit>', not mapped",
             tb->run_for);
        return 0;
    }
    uint64_t unit = 0;
    if (!read_timescale(file, &unit)) {
        unit = k_units[2].fs; /* ns */
        warn(proj, tb->run_at, "testbench file has no `timescale, assuming 1 ns", tb->run_for);
    }
    uint64_t units = total / unit;
    if (total % unit != 0 || (total > 0 && units == 0)) {
        warn(proj, tb->run_at, "EDA_TEST_BENCH_RUN_SIM_FOR is not a whole number of time units",
             tb->run_for);
        units = units == 0 && total > 0 ? 1 : units;
    }
    return units;
}

static odin3_status intern_opt(odin3_project *proj, odin3_bytes text, uint32_t *id) {
    if (text.ptr == NULL || text.len == 0) {
        *id = 0;
        return ODIN3_OK;
    }
    return odin3_project_intern(proj, text, id);
}

/* The record for one assembled test bench. */
static odin3_status add_build(odin3_project *proj, const tb_build *tb, odin3_bytes project_dir) {
    if (!tb->has_file) {
        warn(proj, tb->at, "test bench has no EDA_TEST_BENCH_FILE, dropped", tb->name);
        return ODIN3_OK;
    }
    odin3_proj_testbench out = {.on = ODIN3_PROJ_TB_RTL, .at = tb->at};
    odin3_proj_path_req req = {project_dir, tb->file, tb->file_at, ODIN3_PROJ_WANT_FILE};
    odin3_status res = odin3_project_path(proj, &req, &out.file);
    res = res != ODIN3_OK ? res : odin3_project_intern(proj, tb->name, &out.name);
    res = res != ODIN3_OK ? res : intern_opt(proj, tb->top, &out.top);
    res = res != ODIN3_OK ? res : intern_opt(proj, tb->dut, &out.dut);
    if (res == ODIN3_OK) {
        const char *file = odin3_strtab_get(odin3_design_strtab(proj->design), out.file.resolved);
        out.max_time = max_time_of(proj, tb, file);
        res = odin3_project_add_testbench(proj, &out);
    }
    return res;
}

/* Pass 1: every declaring assignment (NAME, then NATIVELINK for names not yet declared). */
static odin3_status declare_all(const odin3_vec *items, odin3_vec *builds) {
    const odin3_qsf_tb_item *list = items->data;
    odin3_status res = ODIN3_OK;
    for (int pass = TB_NAME_KIND; res == ODIN3_OK && pass <= TB_SELECT_KIND;
         pass += TB_SELECT_KIND - TB_NAME_KIND) {
        for (size_t i = 0; res == ODIN3_OK && i < items->len; i++) {
            res = kind_of(list[i].name) == pass ? declare(builds, &list[i]) : ODIN3_OK;
        }
    }
    return res;
}

odin3_status odin3_qsf_tb_finish(odin3_project *proj, const odin3_vec *items,
                                 odin3_bytes project_dir) {
    odin3_vec builds;
    odin3_vec_init(&builds, sizeof(tb_build));
    odin3_status res = declare_all(items, &builds);
    const odin3_qsf_tb_item *list = items->data;
    for (size_t i = 0; res == ODIN3_OK && i < items->len; i++) {
        int kind = kind_of(list[i].name);
        if (kind == TB_NAME_KIND || kind == TB_SELECT_KIND) {
            continue;
        }
        tb_build *tb = kind == TB_OTHER_KIND ? NULL : find_build(&builds, list[i].section);
        if (kind == TB_OTHER_KIND) {
            warn(proj, list[i].at, "test bench assignment not mapped", list[i].name);
        } else if (tb == NULL) {
            warn(proj, list[i].at,
                 "test bench assignment in a section that is no declared test bench", list[i].name);
        } else {
            apply(proj, tb, kind, &list[i]);
        }
    }
    for (size_t i = 0; res == ODIN3_OK && i < builds.len; i++) {
        res = add_build(proj, odin3_vec_at(&builds, i), project_dir);
    }
    odin3_vec_free(&builds);
    return res;
}
