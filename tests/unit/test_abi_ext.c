/*
 * test_abi_ext.c — the public ABI's extension points: plugin cell types and passes (registered
 * directly and by the example plugin), the provenance visitors and the ABC script option (3D);
 * links only libodin3.so.
 */
#include "odin3/odin3.h"
#include "unity.h"

#include <stdbool.h>
#include <stddef.h>
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
#ifndef BAD_INIT_PLUGIN_PATH
#error "BAD_INIT_PLUGIN_PATH must name tests/unit/plugin_bad_init.c's plugin"
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

/* The whole file at path, NUL-terminated (free it). */
static char *read_whole(const char *path) {
    FILE *file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_INT(0, fseek(file, 0, SEEK_END));
    long size = ftell(file);
    TEST_ASSERT_TRUE(size >= 0);
    TEST_ASSERT_EQUAL_INT(0, fseek(file, 0, SEEK_SET));
    char *text = malloc((size_t)size + 1);
    TEST_ASSERT_NOT_NULL(text);
    TEST_ASSERT_EQUAL_size_t((size_t)size, fread(text, 1, (size_t)size, file));
    text[size] = '\0';
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    return text;
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

/* Registers the one-field-broken copy of a valid definition; expects a refusal logged for reason
 * (and so for that field, not an earlier check). */
static void expect_refused(const or_type *type, const char *reason) {
    log_text[0] = '\0';
    log_len = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_celltype_register(&type->def));
    TEST_ASSERT_TRUE_MESSAGE(log_has(reason), reason);
}

/* Fields the ABI layer checks itself. */
static void test_celltype_rejects_bad_abi_fields(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_celltype_register(NULL));
    TEST_ASSERT_TRUE(log_has("odin3_celltype_register: invalid argument"));
    or_type type;
    static const uint32_t k_bad_gran[] = {ODIN3_GRAN_MODULE, ODIN3_GRAN_PORT, 99};
    for (size_t i = 0; i < sizeof k_bad_gran / sizeof k_bad_gran[0]; i++) {
        or_type_init(&type, "abi_bad");
        type.def.gran = k_bad_gran[i];
        expect_refused(&type, "granularity must be");
    }
    or_type_init(&type, "abi_bad");
    type.def.flags = 1U << 5; /* not an ODIN3_CT_* flag */
    expect_refused(&type, "unknown flag bits");
    or_type_init(&type, "abi_bad");
    type.ports[1].dir = 3;
    expect_refused(&type, "port direction out of range");
    or_type_init(&type, "abi_bad");
    type.params[1].kind = 4;
    expect_refused(&type, "parameter kind out of range");
    or_type_init(&type, "abi_bad");
    type.params[1].dflt = 1; /* a STRING default cannot be given */
    expect_refused(&type, "only an INT parameter has a default");
    or_type_init(&type, "abi_bad");
    type.def.ports = NULL;
    expect_refused(&type, "NULL port or parameter array");
    or_type_init(&type, "abi bad");
    expect_refused(&type, "the name must be printable ASCII without blanks");
    or_type_init(&type, "abi_bad");
    type.ports[0].name = "A=1";
    expect_refused(&type, "a port name must be printable ASCII");
    or_type_init(&type, "abi_bad");
    type.params[1].name = "T\nAG";
    expect_refused(&type, "a parameter name must be printable ASCII");
    or_type_init(&type, "abi_bad");
    type.ports[0].width = 4; /* besides its width_param */
    expect_refused(&type, "sized by width_param must have width 0");
    or_type_init(&type, "abi_bad");
    type.ports[1].width = 2;
    expect_refused(&type, "a scalar port must have the constant width 1");
}

/* Fields the IR's own validation refuses; none of the refusals registers anything. */
static void test_celltype_rejects_bad_ir_fields(void) {
    or_type type;
    or_type_init(&type, "abi_bad");
    type.def.flags = ODIN3_CT_SEQ_EDGE | ODIN3_CT_SEQ_LEVEL;
    expect_refused(&type, "SEQ_EDGE and SEQ_LEVEL are exclusive");
    or_type_init(&type, "abi_bad");
    type.ports[0].width_param = "TAG"; /* not an INT parameter */
    expect_refused(&type, "width parameter is not an int parameter");
    or_type_init(&type, "abi_bad");
    type.ports[1].name = "A";
    expect_refused(&type, "duplicate port name");
    or_type_init(&type, "abi_bad");
    type.ports[1].name = NULL;
    expect_refused(&type, "port without a name");
    or_type_init(&type, "$sop"); /* a built-in's name */
    expect_refused(&type, "already registered");
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
    odin3_plugin_pass pass = {"abi_echo", help, echo_run, &rec, NULL, {NULL}};
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
    odin3_plugin_pass pass = {"abi_fail", "abi_fail: always fails", fail_run, NULL, NULL, {NULL}};
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
    const odin3_plugin_pass bad[] = {{"abi_rej", "h", NULL, NULL, NULL, {NULL}},
                                     {NULL, "h", echo_run, NULL, NULL, {NULL}},
                                     {"abi_rej", NULL, echo_run, NULL, NULL, {NULL}},
                                     {"abi rej", "h", echo_run, NULL, NULL, {NULL}},
                                     {"", "h", echo_run, NULL, NULL, {NULL}},
                                     {"check", "h", echo_run, NULL, NULL, {NULL}},
                                     {"abi\"rej", "h", echo_run, NULL, NULL, {NULL}},
                                     {"abi_rej", "h", echo_run, NULL, NULL, {&marker}}};
    uint32_t before = odin3_pass_get_count();
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pass_register(&bad[i]));
    }
    TEST_ASSERT_TRUE(log_has("odin3_pass_register: pass 'abi_rej': reserved slot is set"));
    TEST_ASSERT_EQUAL_UINT32(before, odin3_pass_get_count());
    odin3_plugin_pass good = {"abi_rej", "h", echo_run, NULL, NULL, {NULL}};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_register(&good));
}

/* 2D EL-33: the processes slot (the first of the old reserved slots). NULL means REFUSE; the
 * address of ODIN3_PASS_PROCESSES_OK registers an OK pass; a value above OK is refused and
 * nothing is registered. The value is read at registration, so the plugin's byte may change
 * (test_pass_manager's test_plugin_processes_read_once runs such a pass on a design with
 * processes: this file links only libodin3, which cannot make one). */
static void test_pass_register_processes_slot(void) {
    uint8_t one = ODIN3_PASS_PROCESSES_OK;
    uint8_t two = 2;
    odin3_plugin_pass refuse = {"abi_proc_null", "h", fail_run, NULL, NULL, {NULL}};
    odin3_plugin_pass ok = {"abi_proc_ok", "h", fail_run, NULL, &one, {NULL}};
    odin3_plugin_pass bad = {"abi_proc_bad", "h", fail_run, NULL, &two, {NULL}};
    /* the layout of four reserved pointer slots is kept: processes took the first */
    TEST_ASSERT_EQUAL_size_t(8 * sizeof(void *), sizeof(odin3_plugin_pass));
    TEST_ASSERT_EQUAL_size_t(4 * sizeof(void *), offsetof(odin3_plugin_pass, processes));
    uint32_t before = odin3_pass_get_count();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pass_register(&bad));
    TEST_ASSERT_TRUE(
        log_has("odin3_pass_register: pass 'abi_proc_bad': processes must be 0 (REFUSE) or 1 "
                "(OK), not 2"));
    TEST_ASSERT_EQUAL_UINT32(before, odin3_pass_get_count());
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, pass_index("abi_proc_bad"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_register(&refuse));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_register(&ok));
    one = 2; /* read once (test_pass_manager runs such a pass on processes) */
    TEST_ASSERT_EQUAL_UINT32(before + 2, odin3_pass_get_count());
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

/* A plugin whose init registers a pass and a cell type, then fails: it stays loaded, so its pass
 * (code and user data in the object) still runs safely (once crashed: dlclose after init). */
static void test_failed_init_keeps_plugin_loaded(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PLUGIN, odin3_plugin_load(BAD_INIT_PLUGIN_PATH));
    TEST_ASSERT_NOT_EQUAL_UINT32(UINT32_MAX, pass_index("bad_init_pass"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_run_pass(design, "bad_init_pass", NULL));
    TEST_ASSERT_TRUE(log_has("bad_init_pass: ran"));
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

/* A callback of the forward visitor that walks each object backward (nested visitor calls). */
typedef struct nested_walk {
    uint32_t objects;
    uint32_t on_line; /* objects whose first source is the queried line */
} nested_walk;

static void walk_back(const odin3_prov_object *found, void *user) {
    nested_walk *walk = user;
    source_list src = {0};
    walk->objects++;
    if (odin3_prov_visit_sources(design, found->obj, collect_source, &src) == ODIN3_OK &&
        src.first.line == HAND_SUB_LINE) {
        walk->on_line++;
    }
}

static void test_prov_nested_visits(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_path(hand_path()));
    nested_walk walk = {0};
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_prov_visit_objects(design, hand_path(), HAND_SUB_LINE, walk_back, &walk));
    TEST_ASSERT_TRUE(walk.objects >= 1);
    TEST_ASSERT_EQUAL_UINT32(walk.objects, walk.on_line);
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

/* --- the ABC script option (3D AB-27) ----------------------------------------------------- */

/* An snprintf result len that fits a buffer of cap bytes. */
static void fits(int len, size_t cap) {
    TEST_ASSERT_TRUE(len > 0 && (size_t)len < cap);
}

/* read_project on the corpus case o3proj_abc_script, then dump_project to ODIN3_TEST_TMP: the
 * dump's text (free it). */
static char *read_and_dump(void) {
    char entry[PATH_BUF];
    fits(snprintf(entry, sizeof entry, "\"%s/../projects/o3proj_abc_script/project.o3proj\"",
                  ODIN3_BLIF_FIXTURES),
         sizeof entry);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_run_pass(design, "read_project", entry));
    char out[PATH_BUF];
    fits(snprintf(out, sizeof out, "\"%s\"", ODIN3_TEST_TMP), sizeof out);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_run_pass(design, "dump_project", out));
    return read_whole(ODIN3_TEST_TMP);
}

/* The path set reaches read_project, which replaces the project's abc_script with it and logs
 * the info line. */
static void test_pass_abc_script_option(void) {
    char cli[PATH_BUF];
    fits(snprintf(cli, sizeof cli, "%s/../projects/neg_abc_script_twice/abc/map.abc",
                  ODIN3_BLIF_FIXTURES),
         sizeof cli);
    char line[LINE_BUF];
    fits(snprintf(line, sizeof line, "--abc-script %s replaces the project abc_script abc/map.abc",
                  cli),
         sizeof line);
    odin3_log_level level = odin3_log_get_level();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_log_set_level(ODIN3_LOG_INFO));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_set_abc_script(cli));
    char *dump = read_and_dump();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_set_abc_script(NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_log_set_level(level));
    TEST_ASSERT_NOT_NULL(strstr(dump, "/neg_abc_script_twice/abc/map.abc\","));
    free(dump);
    TEST_ASSERT_TRUE(log_has(line));
}

/* Cleared (NULL), the project's own abc_script is kept. */
static void test_pass_abc_script_cleared(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_set_abc_script("ignored.abc"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_set_abc_script(NULL));
    char *dump = read_and_dump();
    TEST_ASSERT_NOT_NULL(strstr(dump, "/o3proj_abc_script/abc/map.abc\","));
    TEST_ASSERT_NULL(strstr(dump, "ignored.abc"));
    free(dump);
}

/* The setter is declared inside the cffi cdef block of odin3.h (the Python binding sees it). */
static void test_pass_abc_script_in_cdef(void) {
    char path[PATH_BUF];
    fits(snprintf(path, sizeof path, "%s/../../include/odin3/odin3.h", ODIN3_CLI_FIXTURES),
         sizeof path);
    char *text = read_whole(path);
    /* the cdef's begin marker, then the declaration, then the end marker, in that order */
    const char *begin = strstr(text, "/* ODIN3_CDEF_BEGIN */");
    const char *decl =
        begin != NULL
            ? strstr(begin, "\nodin3_status odin3_pass_set_abc_script(const char *path);\n")
            : NULL;
    const char *end = decl != NULL ? strstr(decl, "/* ODIN3_CDEF_END */") : NULL;
    TEST_ASSERT_NOT_NULL(end);
    free(text);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_celltype_register_copies_and_creates_nodes);
    RUN_TEST(test_celltype_reserved_slot_rejected);
    RUN_TEST(test_celltype_rejects_bad_abi_fields);
    RUN_TEST(test_celltype_rejects_bad_ir_fields);
    RUN_TEST(test_pass_register_runs_from_script);
    RUN_TEST(test_pass_failure_is_returned);
    RUN_TEST(test_pass_register_rejects);
    RUN_TEST(test_pass_register_processes_slot);
    RUN_TEST(test_example_plugin_pass_and_celltype);
    RUN_TEST(test_failed_init_keeps_plugin_loaded);
    RUN_TEST(test_prov_sources_of_objects);
    RUN_TEST(test_prov_objects_of_line);
    RUN_TEST(test_prov_invalid_arguments);
    RUN_TEST(test_prov_nested_visits);
    RUN_TEST(test_pass_abc_script_option);
    RUN_TEST(test_pass_abc_script_cleared);
    RUN_TEST(test_pass_abc_script_in_cdef);
    return UNITY_END();
}
