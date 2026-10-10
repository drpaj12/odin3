/*
 * test_abi_ext.c — the public ABI's extension points: plugin cell types and passes (registered
 * directly and by the example plugin) and the provenance visitors; links only libodin3.so.
 */
#include "odin3/odin3.h"
#include "unity.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ODIN3_BLIF_FIXTURES
#error "ODIN3_BLIF_FIXTURES must name tests/golden/blif"
#endif
#ifndef ODIN3_CLI_FIXTURES
#error "ODIN3_CLI_FIXTURES must name tests/cli"
#endif
#ifndef ODIN3_TEST_TMP
#error "ODIN3_TEST_TMP must name a scratch file path"
#endif
#ifndef EXAMPLE_PLUGIN_PATH
#error "EXAMPLE_PLUGIN_PATH must name the example plugin"
#endif

enum { LOG_CAP = 1 << 14, PATH_BUF = 512, LINE_BUF = 2 * PATH_BUF, ARGS_BUF = 128 };
enum { MAX_CALLS = 4, MAX_HITS = 64 };

/* hand_body.blif: the `.model top` line and the `.subckt sub` line of the cell u_sub. */
enum { HAND_TOP_LINE = 2, HAND_SUB_LINE = 18 };

static odin3_design *design;
static char log_text[LOG_CAP];
static size_t log_len;

static void capture_sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    int wrote = snprintf(log_text + log_len, LOG_CAP - log_len, "%d %s\n", (int)level, msg);
    if (wrote > 0 && log_len + (size_t)wrote < LOG_CAP) {
        log_len += (size_t)wrote;
    }
}

static bool log_has(const char *needle) {
    return strstr(log_text, needle) != NULL;
}

void setUp(void) {
    log_text[0] = '\0';
    log_len = 0;
    odin3_log_set_sink(capture_sink, NULL);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
}

void tearDown(void) {
    odin3_design_destroy(design);
    design = NULL;
    odin3_log_set_sink(NULL, NULL);
}

/* --- helpers over the ABI ------------------------------------------------------------------ */

static const char *hand_path(void) {
    static char path[PATH_BUF];
    (void)snprintf(path, sizeof path, "%s/hand_body.blif", ODIN3_BLIF_FIXTURES);
    return path;
}

/* Runs read_blif on path in the design. */
static odin3_status read_path(const char *path) {
    char args[LINE_BUF];
    (void)snprintf(args, sizeof args, "\"%s\"", path);
    return odin3_design_run_pass(design, "read_blif", args);
}

static uint32_t top_module(void) {
    uint32_t top = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_top_module(design, &top));
    TEST_ASSERT_NOT_EQUAL_UINT32(0, top);
    return top;
}

static odin3_ref node_named(const char *name) {
    uint32_t module = top_module();
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_lookup_node(design, module, name, &id));
    TEST_ASSERT_NOT_EQUAL_UINT32(0, id);
    return (odin3_ref){module, id};
}

static const char *type_of(odin3_ref node) {
    const char *name = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_get_type_name(design, node, &name));
    return name;
}

static uint32_t port_width(odin3_ref node, uint32_t port) {
    uint32_t width = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_get_port_width(design, node, port, &width));
    return width;
}

static const char *port_name(odin3_ref node, uint32_t port) {
    const char *name = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_get_port_name(design, node, port, &name));
    return name;
}

static int64_t param_int(odin3_ref node, uint32_t index) {
    int64_t value = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_get_param_int(design, node, index, &value));
    return value;
}

/* Writes text to the scratch file ODIN3_TEST_TMP. */
static void write_tmp(const char *text) {
    FILE *file = fopen(ODIN3_TEST_TMP, "w");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_TRUE(fputs(text, file) >= 0);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static char *dup_str(const char *text) {
    size_t len = strlen(text) + 1;
    char *copy = malloc(len);
    TEST_ASSERT_NOT_NULL(copy);
    memcpy(copy, text, len);
    return copy;
}

/* --- cell types ---------------------------------------------------------------------------- */

/* A valid two-port hard cell named name: A sized by parameter W, a scalar 1-bit Y, and a STRING
 * parameter TAG; every reserved slot NULL. */
typedef struct or_type {
    odin3_plugin_port ports[2];
    odin3_plugin_param params[2];
    odin3_plugin_celltype def;
} or_type;

static void or_type_init(or_type *type, const char *name) {
    memset(type, 0, sizeof *type);
    type->ports[0] = (odin3_plugin_port){"A", ODIN3_DIR_IN, 0, "W", false};
    type->ports[1] = (odin3_plugin_port){"Y", ODIN3_DIR_OUT, 1, NULL, true};
    type->params[0] = (odin3_plugin_param){"W", ODIN3_VAL_INT, 1};
    type->params[1] = (odin3_plugin_param){"TAG", ODIN3_VAL_STRING, 0};
    type->def.name = name;
    type->def.gran = ODIN3_GRAN_HARD;
    type->def.ports = type->ports;
    type->def.n_ports = 2;
    type->def.params = type->params;
    type->def.n_params = 2;
}

/* Review Focus 3: a plain-data definition with its reserved slots zeroed registers, is copied
 * (its strings are freed right after), and a reader instantiates it. */
static void test_celltype_register_copies_and_creates_nodes(void) {
    or_type type;
    char *name = dup_str("abi_or");
    char *port_a = dup_str("A");
    char *width = dup_str("W");
    or_type_init(&type, name);
    type.ports[0].name = port_a;
    type.ports[0].width_param = width;
    type.params[0].name = width;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_celltype_register(&type.def));
    free(name);
    free(port_a);
    free(width);
    memset(&type, 0xA5, sizeof type);

    odin3_design_destroy(design); /* a design created after the registration holds the type */
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    write_tmp(".model t\n.inputs a[0] a[1] a[2]\n.outputs y\n"
              ".subckt abi_or A[0]=a[0] A[1]=a[1] A[2]=a[2] Y=y\n.cname u0\n.end\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_path(ODIN3_TEST_TMP));
    odin3_ref node = node_named("u0");
    TEST_ASSERT_EQUAL_STRING("abi_or", type_of(node));
    odin3_granularity gran = ODIN3_GRAN_WORD;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_get_granularity(design, node, &gran));
    TEST_ASSERT_EQUAL_INT(ODIN3_GRAN_HARD, gran);
    TEST_ASSERT_EQUAL_STRING("A", port_name(node, 0));
    TEST_ASSERT_EQUAL_STRING("Y", port_name(node, 1));
    TEST_ASSERT_EQUAL_UINT32(3, port_width(node, 0)); /* W inferred from the connected bits */
    TEST_ASSERT_EQUAL_UINT32(1, port_width(node, 1));
    TEST_ASSERT_EQUAL_INT64(3, param_int(node, 0));
    const char *tag = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_get_param_text(design, node, 1, &tag));
    TEST_ASSERT_EQUAL_STRING("", tag);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_run_pass(design, "check", NULL));
}

static void test_celltype_reserved_slot_rejected(void) {
    static int marker;
    or_type type;
    or_type_init(&type, "abi_reserved");
    type.def.reserved[3] = &marker;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_celltype_register(&type.def));
    TEST_ASSERT_TRUE(
        log_has("odin3_celltype_register: cell type 'abi_reserved': reserved slot is set"));
    type.def.reserved[3] = NULL; /* nothing was registered: the name is still free */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_celltype_register(&type.def));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_celltype_register(&type.def));
}

/* Registers the one-field-broken copy of a valid definition; expects a logged refusal. */
static void expect_refused(const or_type *type) {
    log_text[0] = '\0';
    log_len = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_celltype_register(&type->def));
    TEST_ASSERT_TRUE(log_len > 0);
}

static void test_celltype_rejects_bad_fields(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_celltype_register(NULL));
    TEST_ASSERT_TRUE(log_has("odin3_celltype_register: invalid argument"));
    or_type type;
    static const uint32_t k_bad_gran[] = {ODIN3_GRAN_MODULE, ODIN3_GRAN_PORT, 99};
    for (size_t i = 0; i < sizeof k_bad_gran / sizeof k_bad_gran[0]; i++) {
        or_type_init(&type, "abi_bad");
        type.def.gran = k_bad_gran[i];
        expect_refused(&type);
    }
    or_type_init(&type, "abi_bad");
    type.def.flags = 1U << 5; /* not an ODIN3_CT_* flag */
    expect_refused(&type);
    or_type_init(&type, "abi_bad");
    type.def.flags = ODIN3_CT_SEQ_EDGE | ODIN3_CT_SEQ_LEVEL;
    expect_refused(&type);
    or_type_init(&type, "abi_bad");
    type.ports[1].dir = 3;
    expect_refused(&type);
    or_type_init(&type, "abi_bad");
    type.params[1].kind = 4;
    expect_refused(&type);
    or_type_init(&type, "abi_bad");
    type.params[1].dflt = 1; /* a STRING default cannot be given */
    expect_refused(&type);
    or_type_init(&type, "abi_bad");
    type.ports[0].width_param = "TAG"; /* not an INT parameter */
    expect_refused(&type);
    or_type_init(&type, "abi_bad");
    type.ports[1].name = "A";
    expect_refused(&type);
    or_type_init(&type, "abi_bad");
    type.ports[1].name = NULL;
    expect_refused(&type);
    or_type_init(&type, "abi_bad");
    type.def.ports = NULL;
    expect_refused(&type);
    or_type_init(&type, "$sop"); /* a built-in's name */
    expect_refused(&type);
    or_type_init(&type, "abi_bad"); /* none of the refusals registered it */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_celltype_register(&type.def));
}

/* --- plugin passes ------------------------------------------------------------------------- */

typedef struct echo_record {
    uint32_t calls;
    char args[MAX_CALLS][ARGS_BUF];
    uint32_t modules; /* the module count the last call read through the ABI */
    odin3_design *seen;
} echo_record;

static odin3_status echo_run(odin3_design *target, const char *args, void *user) {
    echo_record *rec = user;
    if (rec->calls < MAX_CALLS) {
        (void)snprintf(rec->args[rec->calls], ARGS_BUF, "%s", args);
    }
    rec->calls++;
    rec->seen = target;
    return odin3_design_get_module_count(target, &rec->modules);
}

static odin3_status fail_run(odin3_design *target, const char *args, void *user) {
    (void)target;
    (void)args;
    (void)user;
    (void)odin3_log_write(ODIN3_LOG_ERROR, "abi_fail: refusing");
    return ODIN3_ERR_IO;
}

static uint32_t pass_index(const char *name) {
    for (uint32_t i = 0; i < odin3_pass_get_count(); i++) {
        const char *have = NULL;
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_get_name(i, &have));
        if (strcmp(have, name) == 0) {
            return i;
        }
    }
    return UINT32_MAX;
}

static void test_pass_register_runs_from_script(void) {
    static echo_record rec;
    char help[] = "abi_echo [args]: records its arguments";
    odin3_plugin_pass pass = {"abi_echo", help, echo_run, &rec, {NULL}};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_register(&pass));
    memset(help, 'x', sizeof help - 1); /* help was copied */
    uint32_t index = pass_index("abi_echo");
    TEST_ASSERT_EQUAL_UINT32(odin3_pass_get_count() - 1, index);
    const char *have = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_get_help(index, &have));
    TEST_ASSERT_EQUAL_STRING("abi_echo [args]: records its arguments", have);

    char script[LINE_BUF];
    (void)snprintf(script, sizeof script, "read_blif \"%s\"; abi_echo  a \"b c\" ;abi_echo",
                   hand_path());
    odin3_script_src src = {"-p", ODIN3_SCRIPT_BY_COMMAND};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_run_script(design, script, src));
    TEST_ASSERT_EQUAL_UINT32(2, rec.calls);
    TEST_ASSERT_EQUAL_STRING("a \"b c\"", rec.args[0]);
    TEST_ASSERT_EQUAL_STRING("", rec.args[1]);
    TEST_ASSERT_EQUAL_PTR(design, rec.seen);
    TEST_ASSERT_EQUAL_UINT32(2, rec.modules); /* top and sub (bb is a black box) */
    TEST_ASSERT_TRUE(log_has("pass abi_echo: "));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_run_pass(design, "abi_echo", "x"));
    TEST_ASSERT_EQUAL_UINT32(3, rec.calls);
    TEST_ASSERT_EQUAL_STRING("x", rec.args[2]);
}

static void test_pass_failure_is_returned(void) {
    odin3_plugin_pass pass = {"abi_fail", "abi_fail: always fails", fail_run, NULL, {NULL}};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_register(&pass));
    odin3_script_src src = {"-p", ODIN3_SCRIPT_BY_COMMAND};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO, odin3_design_run_script(design, "check; abi_fail", src));
    TEST_ASSERT_TRUE(log_has("abi_fail: refusing"));
    TEST_ASSERT_TRUE(log_has("-p: command 2: pass 'abi_fail' failed"));
}

static void test_pass_register_rejects(void) {
    static int marker;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pass_register(NULL));
    TEST_ASSERT_TRUE(log_has("odin3_pass_register: invalid argument"));
    const odin3_plugin_pass bad[] = {
        {"abi_rej", "h", NULL, NULL, {NULL}},       {NULL, "h", echo_run, NULL, {NULL}},
        {"abi_rej", NULL, echo_run, NULL, {NULL}},  {"abi rej", "h", echo_run, NULL, {NULL}},
        {"", "h", echo_run, NULL, {NULL}},          {"check", "h", echo_run, NULL, {NULL}},
        {"abi_rej", "h", echo_run, NULL, {&marker}}};
    uint32_t before = odin3_pass_get_count();
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pass_register(&bad[i]));
    }
    TEST_ASSERT_TRUE(log_has("odin3_pass_register: pass 'abi_rej': reserved slot is set"));
    TEST_ASSERT_EQUAL_UINT32(before, odin3_pass_get_count());
    odin3_plugin_pass good = {"abi_rej", "h", echo_run, NULL, {NULL}};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_register(&good));
}

/* --- the example plugin -------------------------------------------------------------------- */

/* Review Focus 3 through a plugin: example_plugin.so registers example_and (plain data, reserved
 * slots zeroed) and example_count; a script reads cells of the type and runs the pass. */
static void test_example_plugin_pass_and_celltype(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_plugin_load(EXAMPLE_PLUGIN_PATH));
    TEST_ASSERT_NOT_EQUAL_UINT32(UINT32_MAX, pass_index("example_count"));
    odin3_design_destroy(design); /* a design created after the plugin loaded holds its type */
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    char script[LINE_BUF];
    (void)snprintf(script, sizeof script, "read_blif \"%s/example_plugin.blif\"; example_count",
                   ODIN3_CLI_FIXTURES);
    odin3_script_src src = {"-p", ODIN3_SCRIPT_BY_COMMAND};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_run_script(design, script, src));
    TEST_ASSERT_TRUE(log_has("example_count: 2 example_and cells"));
    odin3_ref wide = node_named("u_wide");
    odin3_ref narrow = node_named("u_narrow");
    TEST_ASSERT_EQUAL_STRING("example_and", type_of(wide));
    TEST_ASSERT_EQUAL_UINT32(2, port_width(wide, 2));
    TEST_ASSERT_EQUAL_UINT32(1, port_width(narrow, 2));
    TEST_ASSERT_EQUAL_INT64(2, param_int(wide, 0));
    const char *recorded = NULL;
    odin3_obj top = {top_module(), ODIN3_OBJ_MODULE, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_attr_get_string(design, top, "example_count", &recorded));
    TEST_ASSERT_EQUAL_STRING("2", recorded);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_design_run_pass(design, "example_count", "extra"));
}

/* --- provenance ---------------------------------------------------------------------------- */

typedef struct source_list {
    uint32_t count;
    odin3_source first;
    char file[PATH_BUF];
} source_list;

static void collect_source(const odin3_source *src, void *user) {
    source_list *list = user;
    if (list->count == 0) {
        list->first = *src;
        (void)snprintf(list->file, sizeof list->file, "%s", src->file);
    }
    list->count++;
}

typedef struct object_list {
    uint32_t count;
    odin3_prov_object hits[MAX_HITS];
} object_list;

static void collect_object(const odin3_prov_object *found, void *user) {
    object_list *list = user;
    if (list->count < MAX_HITS) {
        list->hits[list->count] = *found;
    }
    list->count++;
}

static bool has_object(const object_list *list, odin3_obj obj) {
    for (uint32_t i = 0; i < list->count && i < MAX_HITS; i++) {
        const odin3_obj *have = &list->hits[i].obj;
        if (have->module == obj.module && have->kind == obj.kind && have->id == obj.id) {
            return list->hits[i].live;
        }
    }
    return false;
}

static void test_prov_sources_of_objects(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_path(hand_path()));
    odin3_ref sub = node_named("u_sub");
    source_list list = {0};
    odin3_obj obj = {sub.module, ODIN3_OBJ_NODE, sub.id};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_visit_sources(design, obj, collect_source, &list));
    TEST_ASSERT_EQUAL_UINT32(1, list.count);
    TEST_ASSERT_EQUAL_STRING(hand_path(), list.file);
    TEST_ASSERT_EQUAL_UINT32(HAND_SUB_LINE, list.first.line);
    TEST_ASSERT_EQUAL_UINT32(1, list.first.col);
    TEST_ASSERT_EQUAL_UINT32(HAND_SUB_LINE, list.first.end_line);

    source_list mod = {0};
    odin3_obj top = {sub.module, ODIN3_OBJ_MODULE, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_visit_sources(design, top, collect_source, &mod));
    TEST_ASSERT_EQUAL_UINT32(1, mod.count);
    TEST_ASSERT_EQUAL_UINT32(HAND_TOP_LINE, mod.first.line);
}

static void test_prov_objects_of_line(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_path(hand_path()));
    odin3_ref sub = node_named("u_sub");
    object_list list = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_visit_objects(design, hand_path(), HAND_SUB_LINE,
                                                             collect_object, &list));
    TEST_ASSERT_TRUE(list.count >= 1);
    TEST_ASSERT_TRUE(has_object(&list, (odin3_obj){sub.module, ODIN3_OBJ_NODE, sub.id}));
    /* every object found names that line among its sources */
    for (uint32_t i = 0; i < list.count && i < MAX_HITS; i++) {
        source_list src = {0};
        TEST_ASSERT_EQUAL_INT(
            ODIN3_OK, odin3_prov_visit_sources(design, list.hits[i].obj, collect_source, &src));
        TEST_ASSERT_EQUAL_UINT32(HAND_SUB_LINE, src.first.line);
    }
    object_list mod = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_visit_objects(design, hand_path(), HAND_TOP_LINE,
                                                             collect_object, &mod));
    TEST_ASSERT_TRUE(has_object(&mod, (odin3_obj){sub.module, ODIN3_OBJ_MODULE, sub.module}));

    object_list none = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_visit_objects(design, "nope.blif", HAND_SUB_LINE,
                                                             collect_object, &none));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_prov_visit_objects(design, hand_path(), 1, collect_object, &none));
    TEST_ASSERT_EQUAL_UINT32(0, none.count);
}

static void test_prov_invalid_arguments(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_path(hand_path()));
    uint32_t top = top_module();
    source_list list = {0};
    object_list objs = {0};
    odin3_obj node1 = {top, ODIN3_OBJ_NODE, 1};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_prov_visit_sources(NULL, node1, collect_source, &list));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_prov_visit_sources(design, node1, NULL, &list));
    const odin3_obj bad[] = {{top, 9, 1},
                             {top, ODIN3_OBJ_NODE, 0},
                             {top, ODIN3_OBJ_NET, 100000},
                             {0, ODIN3_OBJ_MODULE, 0},
                             {77, ODIN3_OBJ_WIRE, 1}};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                              odin3_prov_visit_sources(design, bad[i], collect_source, &list));
    }
    TEST_ASSERT_EQUAL_UINT32(0, list.count);
    TEST_ASSERT_TRUE(log_has("odin3_prov_visit_sources: invalid argument"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_prov_visit_objects(NULL, "f", 1, collect_object, &objs));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_prov_visit_objects(design, NULL, 1, collect_object, &objs));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_prov_visit_objects(design, hand_path(), 1, NULL, &objs));
    TEST_ASSERT_TRUE(log_has("odin3_prov_visit_objects: invalid argument"));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_celltype_register_copies_and_creates_nodes);
    RUN_TEST(test_celltype_reserved_slot_rejected);
    RUN_TEST(test_celltype_rejects_bad_fields);
    RUN_TEST(test_pass_register_runs_from_script);
    RUN_TEST(test_pass_failure_is_returned);
    RUN_TEST(test_pass_register_rejects);
    RUN_TEST(test_example_plugin_pass_and_celltype);
    RUN_TEST(test_prov_sources_of_objects);
    RUN_TEST(test_prov_objects_of_line);
    RUN_TEST(test_prov_invalid_arguments);
    return UNITY_END();
}
