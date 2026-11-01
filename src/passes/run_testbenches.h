/*
 * run_testbenches.h — records, comparison and report of the `run_testbenches` pass (spec
 * docs/specs/2026-10-31-project-testbench-design.md sections 3 and 4). Private to src/passes.
 */
#ifndef ODIN3_PASSES_RUN_TESTBENCHES_H
#define ODIN3_PASSES_RUN_TESTBENCHES_H

#include "odin3/odin3.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { RT_NAME = 256, RT_STEP = 512 };

typedef enum rt_state { RT_PASS, RT_FAIL, RT_ERROR } rt_state;

/* The first differing line of two output texts. */
typedef struct rt_mismatch {
    bool present;
    const char *compared; /* "expect", "expect (netlist)" or "rtl vs netlist" */
    uint32_t line;        /* 1-based */
    bool has_time;
    uint64_t time; /* the first field of the want line when it is a decimal integer */
    uint32_t field;
    char *want, *got; /* owned; "<end of output>" when a text ended first */
} rt_mismatch;

typedef struct rt_result {
    char name[RT_NAME];
    rt_state state;
    const char *on; /* "rtl", "netlist" or "both" */
    uint64_t wall_ms, end_time;
    uint32_t lines;
    char error[RT_STEP]; /* "<step>: <status>: <cause>" for an error, the timeout text for a hang */
    bool timeout;        /* a fail: the run hit its time limit without `$finish` */
    rt_mismatch mm;
} rt_result;

/* A text and its length (not NUL-terminated). */
typedef struct rt_text {
    const char *data;
    size_t len;
} rt_text;

/*
 * Compares want and got line by line. Equal: false, *mm untouched. Otherwise true with *mm filled
 * (present, line, time, field, want, got owned by *mm; `compared` set by the caller). NO_MEMORY
 * is reported as a mismatch with NULL want and got.
 */
bool rt_compare(rt_text want, rt_text got, rt_mismatch *mm);

void rt_result_free(rt_result *res);

/* The text report: one line per result, details, then the totals line. */
odin3_status rt_report_text(const rt_result *res, size_t count, odin3_strbuf *out);

/* The JSON report. */
odin3_status rt_report_json(const rt_result *res, size_t count, odin3_strbuf *out);

#endif
