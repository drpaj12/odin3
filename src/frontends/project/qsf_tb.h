/*
 * qsf_tb.h — Quartus EDA_TEST_BENCH_* assignments to project testbenches (spec
 * docs/specs/2026-10-31-project-testbench-design.md section 2). The .qsf reader notes each
 * assignment while it runs; the mapping needs them all (a test bench is a group of assignments
 * tied by -section_id), so it happens after the file.
 */
#ifndef ODIN3_FRONTENDS_PROJECT_QSF_TB_H
#define ODIN3_FRONTENDS_PROJECT_QSF_TB_H

#include "frontends/project/project.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>

/* One noted assignment; the bytes live in the reader's arena until the read ends. */
typedef struct odin3_qsf_tb_item {
    odin3_bytes name, section, value;
    odin3_loc at;
} odin3_qsf_tb_item;

void odin3_qsf_tb_init(odin3_vec *items);

/* Whether the (upper-cased) assignment name belongs to the test bench group. */
bool odin3_qsf_tb_is_name(odin3_bytes name);

/* Notes an assignment (section {NULL,0} when it has no -section_id). NO_MEMORY. */
odin3_status odin3_qsf_tb_note(odin3_vec *items, const odin3_qsf_tb_item *item);

/*
 * Maps the noted assignments onto proj->testbenches (a test bench without a file is dropped with a
 * WARNING); every assignment that cannot be mapped is a WARNING at its line. Relative files
 * resolve against project_dir; a missing file is `missing_file` like any path.
 */
odin3_status odin3_qsf_tb_finish(odin3_project *proj, const odin3_vec *items,
                                 odin3_bytes project_dir);

#endif
