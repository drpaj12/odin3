/*
 * test_project_flows.c — unit tests for the schema 2 project statements (`schema`, `tool`,
 * `flowdef`, `step`, `provlink`, `flow-var`; spec docs/specs/2026-11-03-v1.4-silicon-provenance.md
 * section 4): the dump text, a schema 1 file dumping as before, and every located error.
 */
#include "ast/srcman.h"
#include "frontends/project/project.h"
#include "ir/design.h"
#include "unity.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { PATH_BUF = 1024, MSG_MAX = 4096, OUT_MAX = 16384 };

static odin3_design *design;
static odin3_project *proj;
static char captured[MSG_MAX];
static int errors;
static char tmp[PATH_BUF / 2];

static void sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (level == ODIN3_LOG_ERROR) {
        errors++;
        (void)snprintf(captured, sizeof captured, "%s", msg);
    }
}

void setUp(void) {
    odin3_log_set_sink(sink, NULL);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_project(design, &proj));
    errors = 0;
    captured[0] = '\0';
    const char *base = getenv("TMPDIR");
    (void)snprintf(tmp, sizeof tmp, "%s/odin3_flowproj_XXXXXX",
                   base != NULL && base[0] != '\0' ? base : "/tmp");
    TEST_ASSERT_NOT_NULL(mkdtemp(tmp));
}

void tearDown(void) {
    odin3_log_set_sink(NULL, NULL);
    char path[PATH_BUF];
    (void)snprintf(path, sizeof path, "%s/p.o3proj", tmp);
    (void)unlink(path);
    TEST_ASSERT_EQUAL_INT(0, rmdir(tmp));
    odin3_design_destroy(design);
}

static uint32_t id(const char *str) {
    uint32_t got = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_project_intern(proj, odin3_bytes_cstr(str), &got));
    return got;
}

static odin3_status read_o3(const char *text) {
    char path[PATH_BUF];
    (void)snprintf(path, sizeof path, "%s/p.o3proj", tmp);
    FILE *fp = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(fp);
    TEST_ASSERT_EQUAL_size_t(strlen(text), fwrite(text, 1, strlen(text), fp));
    TEST_ASSERT_EQUAL_INT(0, fclose(fp));
    odin3_proj_path file = {id("p.o3proj"), id(path), {0}};
    errors = 0;
    captured[0] = '\0';
    return odin3_read_o3proj(proj, &file);
}

static char *dump(void) {
    static char text[OUT_MAX];
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_project_finish(proj));
    odin3_strbuf buf;
    odin3_strbuf_init(&buf);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_project_dump(proj, &buf));
    (void)snprintf(text, sizeof text, "%s", (const char *)buf.data);
    odin3_strbuf_free(&buf);
    return text;
}

static const char *const k_ok =
    "schema 2\n"
    "tool openroad required openroad version-arg -version\n"
    "tool klayout optional python module klayout.db\n"
    "flowdef si description \"Odin III to GDS\"\n"
    "step si elab stage ir tool odin3 out map=elab.map\n"
    "step si map stage gates tool odin3 needs elab in lib=a.o3lib out netlist=g.v args \"x y\"\n"
    "step si place stage placed tool openroad needs map\n"
    "provlink si map from elab kind cone confidence medium\n"
    "provlink si place from map kind name key instance\n"
    "provlink si place from place kind physical_origin scope inserted\n"
    "flow-var si clock_port Clock\n";

static void test_dump(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_o3(k_ok));
    const char *text = dump();
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(text, "\"schema\": 2,"), text);
    TEST_ASSERT_NOT_NULL(strstr(text, "{\"id\": \"openroad\", \"required\": true, \"exe\": "
                                      "\"openroad\", \"version_arg\": \"-version\", "
                                      "\"module\": null, \"env\": null}"));
    TEST_ASSERT_NOT_NULL(strstr(text, "\"module\": \"klayout.db\""));
    TEST_ASSERT_NOT_NULL(strstr(text, "\"name\": \"si\", \"description\": \"Odin III to GDS\", "
                                      "\"vars\": {\"clock_port\": \"Clock\"}"));
    TEST_ASSERT_NOT_NULL(strstr(text, "\"needs\": [\"elab\"], \"in\": [{\"role\": \"lib\", "
                                      "\"path\": \"a.o3lib\"}], \"out\": [{\"role\": \"netlist\", "
                                      "\"path\": \"g.v\"}], \"args\": \"x y\""));
    TEST_ASSERT_NOT_NULL(strstr(text, "{\"from\": \"map\", \"kind\": \"name\", \"key\": "
                                      "\"instance\", \"scope\": null, \"confidence\": null}"));
    TEST_ASSERT_NOT_NULL(strstr(text, "\"kind\": \"physical_origin\", \"key\": null, "
                                      "\"scope\": \"inserted\""));
}

static void test_schema1_dump_unchanged(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_o3("top m\n"));
    const char *text = dump();
    TEST_ASSERT_NULL(strstr(text, "\"schema\""));
    TEST_ASSERT_NULL(strstr(text, "\"flows\""));
    TEST_ASSERT_NULL(strstr(text, "\"tools\""));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_project_finish(proj));
}

static void test_schema1_explicit(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_o3("schema 1\ntop m\n"));
    TEST_ASSERT_NULL(strstr(dump(), "\"flows\""));
}

static void reset_project(void) {
    odin3_design_destroy(design);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_project(design, &proj));
}

static void expect_error(const char *text, const char *kind, const char *detail) {
    reset_project();
    TEST_ASSERT_NOT_EQUAL(ODIN3_OK, read_o3(text));
    TEST_ASSERT_EQUAL_INT(1, errors);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(captured, kind), captured);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(captured, detail), captured);
}

static void test_errors(void) {
    expect_error("schema 3\n", "unsupported_schema", "'3'");
    expect_error("top m\nschema 2\n", "syntax", "first statement");
    expect_error("schema 0\n", "syntax", "1 or 2");
    expect_error("schema x\n", "syntax", "number");
    expect_error("flowdef a\n", "syntax", "schema 2");
    expect_error("step a b stage ir tool odin3\n", "syntax", "schema 2");
}

static void test_flow_errors(void) {
    expect_error("schema 2\nflowdef 1x\n", "syntax", "flow name");
    expect_error("schema 2\nflowdef a\nflowdef a\n", "syntax", "given twice");
    expect_error("schema 2\nflowdef a nonsense x\n", "syntax", "usage");
    expect_error("schema 2\nstep a s stage ir tool odin3\n", "syntax", "not defined");
}

static void test_step_errors(void) {
    const char *pre = "schema 2\nflowdef a\n";
    char text[512];
    (void)snprintf(text, sizeof text, "%sstep a s stage nope tool odin3\n", pre);
    expect_error(text, "unknown_option", "unknown stage");
    (void)snprintf(text, sizeof text, "%sstep a s stage ir tool ghost\n", pre);
    expect_error(text, "syntax", "not declared");
    (void)snprintf(text, sizeof text, "%sstep a s stage ir tool odin3 needs t\n", pre);
    expect_error(text, "syntax", "not defined earlier");
    (void)snprintf(text, sizeof text, "%sstep a s stage ir tool odin3 needs s\n", pre);
    expect_error(text, "syntax", "not defined earlier");
    (void)snprintf(text, sizeof text, "%sstep a s stage ir tool odin3 stage gds\n", pre);
    expect_error(text, "syntax", "repeated step keyword");
    (void)snprintf(text, sizeof text, "%sstep a s stage ir tool odin3 colour red\n", pre);
    expect_error(text, "unknown_option", "unknown step keyword");
    (void)snprintf(text, sizeof text, "%sstep a s stage ir tool odin3 in nopath\n", pre);
    expect_error(text, "syntax", "role=path");
    (void)snprintf(text, sizeof text,
                   "%sstep a s stage ir tool odin3\nstep a s stage ir tool odin3\n", pre);
    expect_error(text, "syntax", "given twice");
    (void)snprintf(text, sizeof text, "%sstep a s stage ir tool odin3 out x=y z\n", pre);
    expect_error(text, "syntax", "usage");
    (void)snprintf(text, sizeof text, "%sstep a s stage ir\n", pre);
    expect_error(text, "syntax", "usage");
}

static void test_provlink_errors(void) {
    const char *pre = "schema 2\nflowdef a\nstep a s stage ir tool odin3\nstep a t stage gates "
                      "tool odin3 needs s\n";
    char text[512];
    (void)snprintf(text, sizeof text, "%sprovlink a q from s kind name\n", pre);
    expect_error(text, "syntax", "not defined in the flow");
    (void)snprintf(text, sizeof text, "%sprovlink a t from q kind name\n", pre);
    expect_error(text, "syntax", "not defined in the flow");
    (void)snprintf(text, sizeof text, "%sprovlink a t from s kind weird\n", pre);
    expect_error(text, "unknown_option", "unknown link kind");
    (void)snprintf(text, sizeof text, "%sprovlink a t from s kind name confidence sure\n", pre);
    expect_error(text, "unknown_option", "unknown confidence");
    (void)snprintf(text, sizeof text, "%sprovlink a t from t kind name\n", pre);
    expect_error(text, "syntax", "itself only");
    (void)snprintf(text, sizeof text, "%sprovlink a t from s kind name kind id\n", pre);
    expect_error(text, "syntax", "repeated provlink keyword");
    (void)snprintf(text, sizeof text, "%sprovlink a t from s scope x\n", pre);
    expect_error(text, "syntax", "usage");
    (void)snprintf(text, sizeof text, "%sflow-var nope x y\n", pre);
    expect_error(text, "syntax", "not defined");
    (void)snprintf(text, sizeof text, "%sflow-var a 1x y\n", pre);
    expect_error(text, "syntax", "variable name");
}

static void test_tool_errors(void) {
    expect_error("schema 2\ntool a maybe x\n", "syntax", "required or optional");
    expect_error("schema 2\ntool a required x\ntool a required y\n", "syntax", "given twice");
    expect_error("schema 2\ntool a required x env\n", "syntax", "usage");
    expect_error("schema 2\ntool a required x colour red\n", "unknown_option", "tool keyword");
    expect_error("schema 2\ntool a required x env A env B\n", "syntax", "repeated");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_dump);
    RUN_TEST(test_schema1_dump_unchanged);
    RUN_TEST(test_schema1_explicit);
    RUN_TEST(test_errors);
    RUN_TEST(test_flow_errors);
    RUN_TEST(test_step_errors);
    RUN_TEST(test_provlink_errors);
    RUN_TEST(test_tool_errors);
    return UNITY_END();
}
