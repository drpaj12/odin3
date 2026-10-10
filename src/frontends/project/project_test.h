/*
 * project_test.h — hidden test hook for the project record's caps (never part of the ABI).
 */
#ifndef ODIN3_FRONTENDS_PROJECT_PROJECT_TEST_H
#define ODIN3_FRONTENDS_PROJECT_PROJECT_TEST_H

#include "frontends/project/project.h"

/* Lowers the record's caps (defaults ODIN3_PROJ_MAX_*) so tests can hit them cheaply. */
void odin3_project_test_set_limits(odin3_project *proj, odin3_proj_limits limits);

#endif
