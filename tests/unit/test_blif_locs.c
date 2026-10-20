/*
 * test_blif_locs.c — read_blif --hdl-locs (spec 5.4 D-4, D-5, D-7): provenance names, `.attr src`,
 * cells that take the location of their output net, and no change without the option. Every case
 * works in its own mkdtemp directory under the build tree.
 */
#include "frontends/blif/reader.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "passes/manager.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef ODIN3_LOCS_ROOT
#error "ODIN3_LOCS_ROOT must name the build tree"
#endif

enum { PATH_BUF = 512, DIR_BUF = 256, CHUNK = 4096, LOG_MAX = 4096 };

static char dir[DIR_BUF];
static odin3_design *design;
static odin3_blif_read_stats stats;
static char log_text[LOG_MAX];
static uint32_t errors;

static void sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    errors += level == ODIN3_LOG_ERROR ? 1U : 0U;
    if (level <= ODIN3_LOG_WARN) {
        size_t used = strlen(log_text);
        (void)snprintf(log_text + used, sizeof log_text - used, "%s\n", msg);
    }
}

static void path_of(const char *name, char *out) {
    (void)snprintf(out, PATH_BUF, "%s/%s", dir, name);
}

void setUp(void) {
    (void)snprintf(dir, sizeof dir, "%s/locs.XXXXXX", ODIN3_LOCS_ROOT);
    TEST_ASSERT_NOT_NULL(mkdtemp(dir));
    log_text[0] = '\0';
    errors = 0;
    stats = (odin3_blif_read_stats){0};
    odin3_log_set_sink(sink, NULL);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
}

void tearDown(void) {
    DIR *handle = opendir(dir);
    for (struct dirent *entry = handle != NULL ? readdir(handle) : NULL; entry != NULL;
         entry = readdir(handle)) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
            char path[PATH_BUF];
            path_of(entry->d_name, path);
            (void)unlink(path);
        }
    }
    if (handle != NULL) {
        (void)closedir(handle);
    }
    (void)rmdir(dir);
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(design);
    design = NULL;
}

static void put_file(const char *name, const char *text) {
    char path[PATH_BUF];
    path_of(name, path);
    FILE *file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(strlen(text), fwrite(text, 1, strlen(text), file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static char *slurp(const char *name) {
    char path[PATH_BUF];
    path_of(name, path);
    FILE *file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(file);
    char *text = odin3_util_calloc(1);
    size_t len = 0;
    char chunk[CHUNK];
    for (size_t got = fread(chunk, 1, sizeof chunk, file); got > 0;
         got = fread(chunk, 1, sizeof chunk, file)) {
        text = odin3_util_realloc(text, len + got + 1);
        memcpy(text + len, chunk, got);
        len += got;
        text[len] = '\0';
    }
    (void)fclose(file);
    return text;
}

/* Reads text as t.blif into the (fresh) design; the option as given. */
static odin3_status read_text(const char *text, bool hdl_locs) {
    char path[PATH_BUF];
    put_file("t.blif", text);
    path_of("t.blif", path);
    odin3_blif_read_opts opts = {.hdl_locs = hdl_locs, .stats = &stats};
    return odin3_blif_read_ex(design, NULL, path, &opts);
}

static void read_ok(const char *text, bool hdl_locs) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, read_text(text, hdl_locs), log_text);
}

/* The dump_locs text of the design (lines `module|kind|name|SRC`). */
static char *dump(void) {
    char path[PATH_BUF];
    path_of("dump.txt", path);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run(design, "dump_locs", odin3_bytes_cstr(path)));
    return slurp("dump.txt");
}

static void run_pass(const char *pass, const char *file, const char *options) {
    char path[PATH_BUF];
    char args[2 * PATH_BUF];
    path_of(file, path);
    (void)snprintf(args, sizeof args, "%s %s", options, path);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_pass_run(design, pass, odin3_bytes_cstr(args)),
                                  log_text);
}

/* The lines of text that hold needle in their kind field (`|node|`, `|net|`), as one text. */
static char *lines_with(const char *text, const char *needle) {
    char *out = odin3_util_calloc(strlen(text) + 1);
    const char *at = text;
    while (*at != '\0') {
        const char *end = strchr(at, '\n');
        size_t len = end != NULL ? (size_t)(end - at) + 1 : strlen(at);
        char line[LOG_MAX];
        TEST_ASSERT_TRUE(len < sizeof line);
        memcpy(line, at, len);
        line[len] = '\0';
        if (strstr(line, needle) != NULL) {
            strcat(out, line);
        }
        at += len;
    }
    return out;
}

static const odin3_module *top_module(void) {
    return odin3_module_get(design, odin3_design_top(design));
}

static uint32_t str_id(const char *text) {
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr(text), &id));
    return id;
}

static odin3_net_id net_named(const char *name) {
    return odin3_module_find_net(top_module(), str_id(name));
}

static const char *const NAMES =
    ".model top\n.inputs a b\n.outputs y z\n"
    ".names a b w@d.v:3\n11 1\n.cname g1\n"
    ".names w@d.v:3 y\n1 1\n.cname g2\n"
    ".latch w@d.v:3 z re a 0\n.cname ff\n"
    ".end\n";

/* T-5: without the option names are byte for byte and no record is restored. */
static void test_no_option_no_change(void) {
    read_ok(NAMES, false);
    TEST_ASSERT_TRUE(odin3_net_valid(net_named("w@d.v:3")));
    TEST_ASSERT_FALSE(odin3_net_valid(net_named("w")));
    char *text = dump();
    TEST_ASSERT_NULL_MESSAGE(strstr(text, "|d.v:"), text);
    TEST_ASSERT_EQUAL_UINT32(0, stats.nets + stats.cells + stats.inherited);
    odin3_util_free(text);
}

/* D-4 a, D-5: the name decodes, and the $sop that drives the located net inherits it. */
static void test_names_decode_and_inherit(void) {
    read_ok(NAMES, true);
    TEST_ASSERT_FALSE(odin3_net_valid(net_named("w@d.v:3")));
    TEST_ASSERT_TRUE(odin3_net_valid(net_named("w")));
    TEST_ASSERT_EQUAL_UINT32(1, stats.nets);
    TEST_ASSERT_EQUAL_UINT32(0, stats.cells);
    TEST_ASSERT_EQUAL_UINT32(1, stats.inherited);
    char *text = dump();
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(text, "top|net|w|d.v:3\n"), text);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(text, "top|node|g1|d.v:3\n"), text);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(text, "top|node|g2|\n"), text);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(text, "top|node|ff|\n"), text);
    odin3_util_free(text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(design, (odin3_check_opts){ODIN3_CHECK_FULL, ODIN3_VIEW_NONE}));
}

/* D-1: the record is DERIVED{SOURCE, IMPORTED}, the HDL first; hier is on both. */
static void test_record_shape_hier_percent_suffix(void) {
    read_ok(".model top\n.inputs a\n.outputs y\n.names a u1/u2/n%3Ax@e.v:9$2\n1 1\n"
            ".names u1/u2/n%3Ax@e.v:9$2 y\n1 1\n.end\n",
            true);
    odin3_net_id net = net_named("n:x");
    TEST_ASSERT_TRUE(odin3_net_valid(net));
    const odin3_prov_record *rec = odin3_prov_get(design, odin3_net_prov(top_module(), net));
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_DERIVED, rec->kind);
    TEST_ASSERT_EQUAL_UINT32(2, rec->parents.count);
    const odin3_prov_record *src = odin3_prov_get(design, rec->parents.ids[0]);
    const odin3_prov_record *imp = odin3_prov_get(design, rec->parents.ids[1]);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_SOURCE, src->kind);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_IMPORTED, imp->kind);
    TEST_ASSERT_EQUAL_UINT32(9, src->locs[0].line);
    TEST_ASSERT_EQUAL_STRING("u1/u2", odin3_strtab_get(odin3_design_strtab(design), rec->hier));
    TEST_ASSERT_EQUAL_STRING("u1/u2", odin3_strtab_get(odin3_design_strtab(design), src->hier));
    TEST_ASSERT_EQUAL_UINT32(4, imp->locs[0].line);
}

/* A name that does not match is untouched. */
static void test_non_match_untouched(void) {
    read_ok(".model top\n.inputs a\n.outputs y\n.names a q@x\n1 1\n.names q@x y\n1 1\n"
            ".names a plain\n1 1\n.names a n@f.v:0\n1 1\n.end\n",
            true);
    TEST_ASSERT_TRUE(odin3_net_valid(net_named("q@x")));
    TEST_ASSERT_TRUE(odin3_net_valid(net_named("plain")));
    TEST_ASSERT_TRUE(odin3_net_valid(net_named("n@f.v:0")));
    TEST_ASSERT_EQUAL_UINT32(0, stats.nets + stats.inherited);
}

/* IR-14: a clash on SHORT appends the first free $N, whichever order the names come in. */
static void test_clash_first_free_suffix(void) {
    read_ok(".model top\n.inputs a\n.outputs y\n"
            ".names a w@d.v:3\n1 1\n.names a w@d.v:4\n1 1\n.names a w$1\n1 1\n"
            ".names a w\n1 1\n.names w@d.v:3 w@d.v:4 y\n11 1\n.end\n",
            true);
    TEST_ASSERT_EQUAL_UINT32(2, stats.nets);
    /* raw names are exact: `w$1` and `w` are the file's nets; the decoded ones moved aside */
    TEST_ASSERT_TRUE(odin3_net_valid(net_named("w")));
    TEST_ASSERT_TRUE(odin3_net_valid(net_named("w$1")));
    char *text = dump();
    char *nets = lines_with(text, "|net|");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(nets, "top|net|w|\n"), nets);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(nets, "top|net|w$1|\n"), nets);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(nets, "|d.v:3\n"), nets);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(nets, "|d.v:4\n"), nets);
    odin3_util_free(nets);
    odin3_util_free(text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(design, (odin3_check_opts){ODIN3_CHECK_FULL, ODIN3_VIEW_NONE}));
}

/* D-4 b: `.attr src` with two locations restores both on the cell and drops the extras key. */
static void test_attr_src_two_locations(void) {
    read_ok(".model top\n.inputs a\n.outputs y\n.names a y\n1 1\n.cname g\n"
            ".attr src \"a.v:3.1-4.2|b.v:5.1-5.4\"\n.attr keep 1\n.end\n",
            true);
    TEST_ASSERT_EQUAL_UINT32(1, stats.cells);
    TEST_ASSERT_EQUAL_UINT32(0, stats.inherited);
    char *text = dump();
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(text, "top|node|g|a.v:3.1-4.2|b.v:5.1-5.4\n"), text);
    odin3_util_free(text);
    odin3_node_id node = odin3_module_find_node(top_module(), str_id("g"));
    TEST_ASSERT_TRUE(odin3_node_valid(node));
    odin3_objref obj = {ODIN3_OBJ_NODE, node.v};
    TEST_ASSERT_NULL(odin3_attr_get(top_module(), obj, str_id("blif.attr:src")));
    const odin3_value *extras = odin3_attr_get(top_module(), obj, str_id("blif_extras"));
    TEST_ASSERT_NOT_NULL(extras);
    TEST_ASSERT_EQUAL_STRING("blif.attr:keep", odin3_strtab_get(odin3_design_strtab(design),
                                                                 extras->str));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(design, (odin3_check_opts){ODIN3_CHECK_FULL, ODIN3_VIEW_NONE}));
}

/* Without the option `.attr src` stays an attribute and no location is made. */
static void test_attr_src_without_option(void) {
    read_ok(".model top\n.inputs a\n.outputs y\n.names a y\n1 1\n.cname g\n"
            ".attr src \"a.v:3.1-4.2\"\n.end\n",
            false);
    odin3_node_id node = odin3_module_find_node(top_module(), str_id("g"));
    odin3_objref obj = {ODIN3_OBJ_NODE, node.v};
    TEST_ASSERT_NOT_NULL(odin3_attr_get(top_module(), obj, str_id("blif.attr:src")));
    char *text = dump();
    TEST_ASSERT_NULL_MESSAGE(strstr(text, "a.v"), text);
    odin3_util_free(text);
}

/* D-7, L-6: a malformed src is a located warning, the attribute stays, the read succeeds. */
static void test_malformed_src_is_located_warning(void) {
    read_ok(".model top\n.inputs a\n.outputs y\n.names a y\n1 1\n.cname g\n"
            ".attr src \"a.v:0\"\n.end\n",
            true);
    TEST_ASSERT_EQUAL_UINT32(1, stats.bad);
    TEST_ASSERT_EQUAL_UINT32(0, stats.cells);
    TEST_ASSERT_EQUAL_UINT32(0, errors);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(log_text, "t.blif:7: `.attr src` value"), log_text);
    odin3_node_id node = odin3_module_find_node(top_module(), str_id("g"));
    odin3_objref obj = {ODIN3_OBJ_NODE, node.v};
    TEST_ASSERT_NOT_NULL(odin3_attr_get(top_module(), obj, str_id("blif.attr:src")));
    char *text = dump();
    TEST_ASSERT_NULL_MESSAGE(strstr(text, "a.v"), text);
    odin3_util_free(text);
}

/* More than 8 locations keep the first 8 and count the object as capped (D-7). */
static void test_src_over_eight_is_capped(void) {
    read_ok(".model top\n.inputs a\n.outputs y\n.names a y\n1 1\n.cname g\n"
            ".attr src \"f.v:1|f.v:2|f.v:3|f.v:4|f.v:5|f.v:6|f.v:7|f.v:8|f.v:9\"\n.end\n",
            true);
    TEST_ASSERT_EQUAL_UINT32(1, stats.capped);
    char *text = dump();
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(text, "f.v:7|f.v:8\n"), text);
    TEST_ASSERT_NULL(strstr(text, "f.v:9"));
    odin3_util_free(text);
}

static const char *const OUTPUTS =
    ".model top\n.inputs a\n.outputs y\n"
    /* own src wins over a located output net; counted as `cells`, not inherited */
    ".names a n1@x.v:1\n1 1\n.cname own\n.attr src \"own.v:7\"\n"
    /* two outputs, the second located: the cell takes it */
    ".subckt bb i=a o1=p o2=n2@x.v:2\n.cname two\n"
    /* two located outputs: the first in pin order wins whatever the file order */
    ".subckt bb i=a o2=n4@x.v:4 o1=n3@x.v:3\n.cname first\n"
    /* no located output: stays without */
    ".subckt bb i=a o1=q1 o2=q2\n.cname none\n"
    ".latch n1@x.v:1 y re a 0\n.cname ff\n"
    ".names n2@x.v:2 n3@x.v:3 n4@x.v:4 p q1 q2 sink\n111111 1\n.end\n"
    ".model bb\n.inputs i\n.outputs o1 o2\n.blackbox\n.end\n";

/* D-5. */
static void test_inherit_rules(void) {
    read_ok(OUTPUTS, true);
    TEST_ASSERT_EQUAL_UINT32(1, stats.cells);
    TEST_ASSERT_EQUAL_UINT32(2, stats.inherited); /* two and first */
    char *text = dump();
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(text, "top|node|own|own.v:7\n"), text);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(text, "top|node|two|x.v:2\n"), text);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(text, "top|node|first|x.v:3\n"), text);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(text, "top|node|none|\n"), text);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(text, "top|node|ff|\n"), text);
    odin3_util_free(text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(design, (odin3_check_opts){ODIN3_CHECK_FULL, ODIN3_VIEW_NONE}));
}

/* The pass takes the option in any position and still wants one path. */
static void test_pass_option(void) {
    put_file("t.blif", NAMES);
    run_pass("read_blif", "t.blif", "--hdl-locs");
    TEST_ASSERT_TRUE(odin3_net_valid(net_named("w")));
    odin3_design_destroy(design);
    design = odin3_design_create();
    char path[PATH_BUF];
    char args[2 * PATH_BUF];
    path_of("t.blif", path);
    (void)snprintf(args, sizeof args, "%s --hdl-locs", path);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run(design, "read_blif", odin3_bytes_cstr(args)));
    TEST_ASSERT_TRUE(odin3_net_valid(net_named("w")));
    odin3_design_destroy(design);
    design = odin3_design_create();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_pass_run(design, "read_blif", odin3_bytes_cstr("--hdl-locs")));
}

/* T-2: write --src-attr, read --hdl-locs, write: the cells keep their locations and the second
 * write is the first one's bytes. */
static void test_src_attr_round_trip(void) {
    read_ok(".model top\n.inputs a b\n.outputs y z\n"
            ".names a b y\n11 1\n.cname g1\n.attr src \"a.v:3.1-4.2|b.v:5\"\n"
            ".latch y z re a 0\n.cname ff\n.attr src \"c.v:9.2\"\n.end\n",
            true);
    char *before = dump();
    run_pass("write_blif", "w1.blif", "--src-attr");
    odin3_design_destroy(design);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    char path[PATH_BUF];
    path_of("w1.blif", path);
    odin3_blif_read_opts opts = {.hdl_locs = true, .stats = &stats};
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_blif_read_ex(design, NULL, path, &opts), log_text);
    TEST_ASSERT_EQUAL_UINT32(2, stats.cells);
    char *after = dump();
    char *want = lines_with(before, "|node|");
    char *got = lines_with(after, "|node|");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(got, "top|node|g1|a.v:3.1-4.2|b.v:5\n"), got);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(got, "top|node|ff|c.v:9.2\n"), got);
    TEST_ASSERT_EQUAL_STRING(want, got);
    run_pass("write_blif", "w2.blif", "--src-attr");
    char *first = slurp("w1.blif");
    char *second = slurp("w2.blif");
    TEST_ASSERT_EQUAL_STRING(first, second);
    odin3_util_free(first);
    odin3_util_free(second);
    odin3_util_free(want);
    odin3_util_free(got);
    odin3_util_free(before);
    odin3_util_free(after);
}

/* VB-10 and D-5 end to end: write --vtr --name-style provenance, read back: the nets decode and
 * the cells inherit (they carry no location of their own in that dialect). */
static void test_vtr_provenance_round_trip(void) {
    read_ok(".model top\n.inputs a b\n.outputs y\n"
            ".names a b w@d.v:3\n11 1\n"
            ".names w@d.v:3 y\n1 1\n.end\n",
            true);
    run_pass("write_blif", "v.blif", "--vtr --name-style provenance");
    odin3_design_destroy(design);
    design = odin3_design_create();
    char path[PATH_BUF];
    path_of("v.blif", path);
    odin3_blif_read_opts opts = {.hdl_locs = true, .stats = &stats};
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_blif_read_ex(design, NULL, path, &opts), log_text);
    TEST_ASSERT_EQUAL_UINT32(1, stats.nets);
    TEST_ASSERT_EQUAL_UINT32(1, stats.inherited);
    char *text = dump();
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(text, "top|net|w|d.v:3\n"), text);
    odin3_util_free(text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(design, (odin3_check_opts){ODIN3_CHECK_FULL, ODIN3_VIEW_NONE}));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_no_option_no_change);
    RUN_TEST(test_names_decode_and_inherit);
    RUN_TEST(test_record_shape_hier_percent_suffix);
    RUN_TEST(test_non_match_untouched);
    RUN_TEST(test_clash_first_free_suffix);
    RUN_TEST(test_attr_src_two_locations);
    RUN_TEST(test_attr_src_without_option);
    RUN_TEST(test_malformed_src_is_located_warning);
    RUN_TEST(test_src_over_eight_is_capped);
    RUN_TEST(test_inherit_rules);
    RUN_TEST(test_pass_option);
    RUN_TEST(test_src_attr_round_trip);
    RUN_TEST(test_vtr_provenance_round_trip);
    return UNITY_END();
}
