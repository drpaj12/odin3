/*
 * test_json_roundtrip.c — write_json, read_json, write_json, read_json, write_json (spec 5.3 R-1,
 * T-4): the second and the third file are byte for byte the same for every writer feature.
 */
#include "rj_harness.h"

#include "ir/celltype.h"
#include "ir/prov.h"
#include "passes/manager.h"

static odin3_pass_ctx rt_run;

static uint32_t intern(const char *name) {
    return rj_str(name);
}

static odin3_celltype_id type_id(const char *name) {
    odin3_celltype_id id = {0};
    TEST_ASSERT_TRUE_MESSAGE(odin3_celltype_find(rj_design, intern(name), &id), name);
    return id;
}

static odin3_prov_id prov_at(uint32_t line) {
    odin3_srcloc loc = {intern("t.v"), line, 1, line, 1, 0};
    odin3_prov_origin origin = {&loc, 1, 0, 0};
    odin3_prov_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_source(&rt_run, &origin, &id));
    return id;
}

void setUp(void) {
    rj_setup();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(rj_design, intern("builder"), &rt_run));
}
void tearDown(void) {
    rj_teardown();
}

static odin3_module *new_module(const char *name) {
    odin3_module_id mid = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_create(rj_design, intern(name), prov_at(1), &mid));
    return odin3_module_get(rj_design, mid);
}

typedef struct port_args {
    const char *name;
    odin3_dir dir;
    uint32_t width;
} port_args;

static odin3_wire_id add_port(odin3_module *mod, port_args args) {
    odin3_port_spec spec = {
        intern(args.name), args.dir, args.width, args.width == 1, prov_at(2), false, 0, 0, false};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_add_port(mod, &spec, NULL));
    return odin3_module_port_wire(mod, odin3_module_port_count(mod) - 1);
}

/* The nets of a wire, LSB first, in a caller array. */
static odin3_netvec wire_nets(odin3_module *mod, odin3_wire_id wire, odin3_net_id *out) {
    uint32_t width = odin3_wire_width(mod, wire);
    for (uint32_t i = 0; i < width; i++) {
        out[i] = odin3_wire_net(mod, wire, i);
    }
    return (odin3_netvec){out, width};
}

/* A node of the type with the given parameters and one netvec per port. */
typedef struct node_args {
    const char *type;
    const char *name; /* NULL: unnamed */
    const odin3_value *params;
    uint32_t n_params;
} node_args;

static odin3_node_id add_node(odin3_module *mod, node_args args, const odin3_netvec *ports) {
    odin3_node_spec spec = {type_id(args.type), args.name != NULL ? intern(args.name) : 0,
                            prov_at(3), args.params, args.n_params};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(mod, &spec, ports, &node));
    return node;
}

/* --- the round trip -------------------------------------------------------------------------- */

static void check_design(void) {
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK,
        odin3_check_design(rj_design, (odin3_check_opts){ODIN3_CHECK_FULL, ODIN3_VIEW_NONE}));
}

static void read_file(const char *path, const odin3_json_opts *opts) {
    rj_fresh();
    rj_log_clear();
    odin3_status st = odin3_json_read(rj_design, path, opts);
    if (st != ODIN3_OK) {
        TEST_FAIL_MESSAGE(rj_log);
    }
    check_design();
}

/* gen1 is rj_design: write file 1, read, write file 2, read, write file 3 (R-1: files 2 and 3
 * are the same). Returns file 2 (owned by the caller); rj_design is generation 3. */
static char *round_trip(const odin3_json_opts *opts) {
    const char *f1 = rj_path("f1.json");
    const char *f2 = rj_path("f2.json");
    const char *f3 = rj_path("f3.json");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_json_write(rj_design, f1));
    read_file(f1, opts);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_json_write(rj_design, f2));
    read_file(f2, opts);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_json_write(rj_design, f3));
    char *two = rj_slurp(f2);
    char *three = rj_slurp(f3);
    TEST_ASSERT_EQUAL_STRING(two, three);
    odin3_util_free(three);
    return two;
}

static void assert_has(const char *text, const char *needle) {
    if (strstr(text, needle) == NULL) {
        char msg[512];
        (void)snprintf(msg, sizeof msg, "missing: %s", needle);
        TEST_FAIL_MESSAGE(msg);
    }
}

static void assert_lacks_text(const char *text, const char *needle) {
    if (strstr(text, needle) != NULL) {
        char msg[512];
        (void)snprintf(msg, sizeof msg, "unexpected: %s", needle);
        TEST_FAIL_MESSAGE(msg);
    }
}

/* --- R-1 on three cells (plan Task 3 (a)) ---------------------------------------------------- */

static void build_three_cells(void) {
    odin3_module *mod = new_module("top");
    odin3_wire_id wire_a = add_port(mod, (port_args){"a", ODIN3_DIR_IN, 2});
    odin3_wire_id wire_b = add_port(mod, (port_args){"b", ODIN3_DIR_IN, 2});
    odin3_wire_id clk = add_port(mod, (port_args){"clk", ODIN3_DIR_IN, 1});
    odin3_wire_id wire_y = add_port(mod, (port_args){"y", ODIN3_DIR_OUT, 2});
    odin3_wire_id wire_q = add_port(mod, (port_args){"q", ODIN3_DIR_OUT, 2});
    odin3_wire_id wire_l = add_port(mod, (port_args){"l", ODIN3_DIR_OUT, 1});
    odin3_net_id nets_a[2];
    odin3_net_id nets_b[2];
    odin3_net_id nets_clk[1];
    odin3_net_id nets_y[2];
    odin3_net_id nets_q[2];
    odin3_net_id nets_l[1];
    odin3_netvec va = wire_nets(mod, wire_a, nets_a);
    odin3_netvec vb = wire_nets(mod, wire_b, nets_b);
    odin3_netvec vc = wire_nets(mod, clk, nets_clk);
    odin3_netvec vy = wire_nets(mod, wire_y, nets_y);
    odin3_netvec vq = wire_nets(mod, wire_q, nets_q);
    odin3_netvec vl = wire_nets(mod, wire_l, nets_l);
    odin3_value and_params[5] = {odin3_value_int(0), odin3_value_int(0), odin3_value_int(2),
                                 odin3_value_int(2), odin3_value_int(2)};
    odin3_netvec and_ports[3] = {va, vb, vy};
    (void)add_node(mod, (node_args){"$and", "g_and", and_params, 5}, and_ports);
    odin3_value dff_params[2] = {odin3_value_int(2), odin3_value_int(1)};
    odin3_netvec dff_ports[3] = {vc, vy, vq};
    (void)add_node(mod, (node_args){"$dff", NULL, dff_params, 2}, dff_ports);
    static const uint8_t xor_table[4] = {ODIN3_BIT_0, ODIN3_BIT_1, ODIN3_BIT_1, ODIN3_BIT_0};
    odin3_value lut_params[2] = {odin3_value_int(2), {ODIN3_VAL_BITS, 0, xor_table, 4, 0, 0}};
    odin3_netvec lut_ports[2] = {va, vl};
    (void)add_node(mod, (node_args){"$lut", NULL, lut_params, 2}, lut_ports);
}

static void test_three_cells_fixpoint(void) {
    build_three_cells();
    char *text = round_trip(NULL);
    assert_has(text, "\"type\": \"$and\"");
    assert_has(text, "\"type\": \"$dff\"");
    assert_has(text, "\"type\": \"$lut\"");
    assert_has(text, "\"g_and\": {");
    assert_has(text, "\"LUT\": \"0110\"");
    odin3_util_free(text);
}

/* --- T-4: the cases of test_json_writer.c, with a read between ------------------------------- */

enum { OUT_BUF = 64, SEVEN = 7, MAX_COVER = 64 };

/* Adds a port; returns the net of its bit 0 (as the writer's tests do). */
static odin3_net_id port_net(odin3_module *mod, port_args args) {
    odin3_wire_id wire = add_port(mod, args);
    return odin3_wire_net(mod, wire, 0);
}

static odin3_net_id new_net(odin3_module *mod, const char *name) {
    odin3_net_id net = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_net_create(mod, name != NULL ? intern(name) : 0, prov_at(3), &net));
    return net;
}

static void add_gate(odin3_module *mod, const char *name, const odin3_net_id nets[3]) {
    odin3_netvec ports[3] = {{&nets[0], 1}, {&nets[1], 1}, {&nets[2], 1}};
    (void)add_node(mod, (node_args){"$_AND_", name, NULL, 0}, ports);
}

static void add_const(odin3_module *mod, const char *type, odin3_net_id net) {
    odin3_netvec ports[1] = {{&net, 1}};
    (void)add_node(mod, (node_args){type, NULL, NULL, 0}, ports);
}

static void add_latch(odin3_module *mod, const odin3_net_id nets[3], int64_t init) {
    odin3_value param = odin3_value_int(init);
    odin3_netvec ports[3] = {{&nets[0], 1}, {&nets[1], 1}, {&nets[2], 1}};
    (void)add_node(mod, (node_args){"$_DFF_P_", NULL, &param, 1}, ports);
}

static void test_bits_ports_cells_netnames(void) {
    odin3_module *mod = new_module("top");
    odin3_net_id net_a = port_net(mod, (port_args){"a", ODIN3_DIR_IN, 1});
    odin3_net_id net_b = port_net(mod, (port_args){"b", ODIN3_DIR_IN, 1});
    odin3_net_id net_y = port_net(mod, (port_args){"y", ODIN3_DIR_OUT, 1});
    (void)add_port(mod, (port_args){"v", ODIN3_DIR_INOUT, 3});
    add_gate(mod, NULL, (odin3_net_id[3]){net_a, net_b, net_y});
    char *text = round_trip(NULL);
    assert_has(text, "\"type\": \"$_AND_\"");
    assert_has(text, "\"direction\": \"inout\"");
    assert_has(text, "\"v\": {");
    odin3_util_free(text);
}

static void test_constants(void) {
    odin3_module *mod = new_module("top");
    odin3_net_id net_a = port_net(mod, (port_args){"a", ODIN3_DIR_IN, 1});
    odin3_net_id net_y = port_net(mod, (port_args){"y", ODIN3_DIR_OUT, 1});
    odin3_net_id zero = port_net(mod, (port_args){"z", ODIN3_DIR_OUT, 1});
    odin3_net_id one = new_net(mod, "one");
    add_const(mod, "$_CONST1_", one);
    add_const(mod, "$_CONST0_", zero);
    add_gate(mod, NULL, (odin3_net_id[3]){net_a, one, net_y});
    char *text = round_trip(NULL);
    assert_has(text, "\"B\": [ \"1\" ]");
    assert_has(text, "\"direction\": \"output\",\n          \"bits\": [ \"0\" ]");
    assert_has(text, "\"one\": {\n          \"hide_name\": 0,\n          \"bits\": [ \"1\" ]");
    odin3_util_free(text);
}

/* A $sop over `width` fresh input ports with the given cover text; output port "y". */
static void add_sop(odin3_module *mod, uint32_t width, const char *cover) {
    odin3_net_id ins[MAX_COVER];
    for (uint32_t i = 0; i < width; i++) {
        char name[OUT_BUF];
        (void)snprintf(name, sizeof name, "i%u", (unsigned)i);
        ins[i] = port_net(mod, (port_args){name, ODIN3_DIR_IN, 1});
    }
    odin3_net_id out = port_net(mod, (port_args){"y", ODIN3_DIR_OUT, 1});
    odin3_value params[2] = {
        odin3_value_int(width),
        {ODIN3_VAL_COVER, 0, (const uint8_t *)cover, (uint32_t)strlen(cover), 0, width}};
    odin3_netvec ports[2] = {{ins, width}, {&out, 1}};
    (void)add_node(mod, (node_args){"$sop", "g", params, 2}, ports);
}

static char *sop_round_trip(uint32_t width, const char *cover) {
    add_sop(new_module("top"), width, cover);
    return round_trip(NULL);
}

static void test_sop_lut_cases(void) {
    static const struct {
        uint32_t width;
        const char *cover;
        const char *lut;
    } k_cases[] = {{2, "111", "\"LUT\": \"1000\""},
                   {2, "110", "\"LUT\": \"0111\""},
                   {3, "10-1--11", "\"LUT\": \"11110010\""}};
    for (size_t i = 0; i < sizeof k_cases / sizeof k_cases[0]; i++) {
        char *text = sop_round_trip(k_cases[i].width, k_cases[i].cover);
        assert_has(text, "\"type\": \"$lut\"");
        assert_has(text, k_cases[i].lut);
        odin3_util_free(text);
        rj_fresh();
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_pass_run_begin(rj_design, intern("builder"), &rt_run));
    }
}

static void test_sop_six_inputs(void) {
    char *text = sop_round_trip(6, "1111110");
    assert_has(text, "\"type\": \"$lut\"");
    assert_has(text, "\"WIDTH\": \"00000000000000000000000000000110\"");
    odin3_util_free(text);
}

static void test_sop_wide_on_set(void) {
    char *text = sop_round_trip(SEVEN, "1-0---11"
                                       "0------1");
    assert_has(text, "\"type\": \"$sop\"");
    assert_has(text, "\"DEPTH\": \"00000000000000000000000000000010\"");
    assert_has(text, "\"TABLE\": \"0000000000000110000000010010\"");
    odin3_util_free(text);
}

/* The OFF-set helper reads as $sop + $not and writes the same way (R-2 d). */
static void test_sop_wide_off_set(void) {
    char *text = sop_round_trip(SEVEN, "11111110");
    assert_has(text, "\"type\": \"$sop\"");
    assert_has(text, "\"type\": \"$not\"");
    assert_has(text, "\"TABLE\": \"10101010101010\"");
    odin3_util_free(text);
}

/* An empty cover is constant 0: file 1 has a $lut of no inputs, which reads back as the constant
 * cover; the writer then writes its net as the constant "0" and no cell. */
static void test_empty_cover_is_const0(void) {
    add_sop(new_module("top"), SEVEN, "");
    char *first = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_json_write(rj_design, rj_path("e1.json")));
    first = rj_slurp(rj_path("e1.json"));
    assert_has(first, "\"type\": \"$lut\"");
    assert_has(first, "\"A\": [  ]");
    odin3_util_free(first);
    char *text = round_trip(NULL);
    assert_lacks_text(text, "$lut");
    assert_has(text, "\"direction\": \"output\",\n          \"bits\": [ \"0\" ]");
    odin3_util_free(text);
}

/* INIT 0 and 1 are defined, 2 and 3 are x and read back as 3 (R-2 e). */
static void test_latch_init(void) {
    odin3_module *mod = new_module("top");
    odin3_net_id clk = new_net(mod, "clk");
    odin3_net_id din = new_net(mod, "d");
    for (int64_t init = 0; init <= 3; init++) {
        char name[OUT_BUF];
        (void)snprintf(name, sizeof name, "q%d", (int)init);
        add_latch(mod, (odin3_net_id[3]){clk, din, new_net(mod, name)}, init);
    }
    char *text = round_trip(NULL);
    assert_has(text, "\"init\": \"0\"");
    assert_has(text, "\"init\": \"1\"");
    assert_has(text, "\"init\": \"x\"");
    assert_lacks_text(text, "\"INIT\"");
    odin3_util_free(text);
    odin3_module *back = rj_module("top");
    static const int64_t k_want[4] = {0, 1, 3, 3};
    uint32_t seen = 0;
    for (uint32_t i = 1; i < odin3_module_node_end(back); i++) {
        odin3_node_id node = {i};
        const odin3_value *init = odin3_node_param(back, node, 0);
        if (odin3_node_live(back, node) && init != NULL && init->kind == ODIN3_VAL_INT) {
            TEST_ASSERT_EQUAL_INT64(k_want[seen++], init->i);
        }
    }
    TEST_ASSERT_EQUAL_UINT32(4, seen);
}

static void test_latch_init_unnamed_q(void) {
    odin3_module *mod = new_module("top");
    odin3_net_id clk = new_net(mod, "clk");
    odin3_net_id din = new_net(mod, "d");
    odin3_net_id qnet = new_net(mod, NULL);
    odin3_value param = odin3_value_int(1);
    odin3_netvec ports[3] = {{&clk, 1}, {&din, 1}, {&qnet, 1}};
    (void)add_node(mod, (node_args){"$_DFF_N_", NULL, &param, 1}, ports);
    char *text = round_trip(NULL);
    assert_has(text, "\"init\": \"1\"");
    odin3_util_free(text);
}

static void set_attr(odin3_module *mod, odin3_objref obj, const char *key, odin3_value val) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_attr_set(mod, obj, intern(key), &val));
}

static odin3_value str_value(const char *text) {
    odin3_value val = {ODIN3_VAL_STRING, 0, NULL, 0, 0, 0};
    val.str = intern(text);
    return val;
}

/* Attributes of cells, nets and modules; a user src replaces the provenance one. (The BLIF
 * `.param` of a registered type has no parameter to read back into: it is an unknown parameter,
 * spec M-6; an instance of a module takes it as an attribute, test_instance_and_parameters.) */
static void test_user_attributes(void) {
    odin3_module *mod = new_module("top");
    odin3_net_id net_a = port_net(mod, (port_args){"a", ODIN3_DIR_IN, 1});
    odin3_net_id net_y = port_net(mod, (port_args){"y", ODIN3_DIR_OUT, 1});
    odin3_net_id net_n = new_net(mod, "n");
    add_gate(mod, "g", (odin3_net_id[3]){net_a, net_n, net_y});
    odin3_objref cell = {ODIN3_OBJ_NODE, odin3_module_find_node(mod, intern("g")).v};
    set_attr(mod, cell, "keep", odin3_value_int(1));
    set_attr(mod, cell, "blif.attr:src", str_value("\"top.v:7.3-7.9\""));
    set_attr(mod, cell, "blif.attr:mask", str_value("0110"));
    set_attr(mod, (odin3_objref){ODIN3_OBJ_NET, net_n.v}, "note", str_value("hi"));
    set_attr(mod, (odin3_objref){ODIN3_OBJ_MODULE, odin3_module_id_of(mod).v}, "top",
             odin3_value_int(1));
    char *text = round_trip(NULL);
    assert_has(text, "\"keep\": \"00000000000000000000000000000001\"");
    assert_has(text, "\"src\": \"top.v:7.3-7.9\"");
    assert_has(text, "\"mask\": \"0110\"");
    assert_has(text, "\"note\": \"hi\"");
    assert_has(text, "\"top\": \"00000000000000000000000000000001\"");
    odin3_util_free(text);
}

static const odin3_port_def BB_PORTS[] = {
    {"d", ODIN3_DIR_IN, false, 3, NULL, NULL, NULL, false},
    {"q", ODIN3_DIR_OUT, true, 1, NULL, NULL, NULL, false},
};

/* A declared black-box model is a module with attribute blackbox and reads back as a black box. */
static void test_declared_blackbox_module(void) {
    odin3_celltype_def def = {"bb", ODIN3_GRAN_BLACKBOX, 0, BB_PORTS, 2, NULL, 0, NULL, NULL, NULL,
                              NULL};
    odin3_celltype_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_celltype_declare_blackbox(rj_design, &def, &id));
    odin3_module *mod = new_module("top");
    odin3_wire_id data = add_port(mod, (port_args){"data", ODIN3_DIR_IN, 3});
    odin3_wire_id out = add_port(mod, (port_args){"out", ODIN3_DIR_OUT, 1});
    odin3_net_id nd[3];
    odin3_net_id no[1];
    odin3_netvec ports[2] = {wire_nets(mod, data, nd), wire_nets(mod, out, no)};
    (void)add_node(mod, (node_args){"bb", "u", NULL, 0}, ports);
    char *text = round_trip(NULL);
    assert_has(text, "\"bb\": {");
    assert_has(text, "\"blackbox\": \"00000000000000000000000000000001\"");
    assert_has(text, "\"type\": \"bb\"");
    odin3_util_free(text);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_design_declared_model_count(rj_design));
}

/* R-5: the parameters of an instance of a module are attributes parameter:<NAME> after a read;
 * the writer writes any attribute as an attribute (its classify maps only the BLIF keys to
 * parameters), so the fixpoint holds and the Yosys form (parameters) is the writer fix 5.3 reports.
 */
static void test_instance_and_parameters(void) {
    odin3_module *sub = new_module("sub");
    (void)add_port(sub, (port_args){"i", ODIN3_DIR_IN, 2});
    (void)add_port(sub, (port_args){"o", ODIN3_DIR_OUT, 1});
    odin3_module *mod = new_module("top");
    odin3_net_id net_x = port_net(mod, (port_args){"x", ODIN3_DIR_IN, 1});
    odin3_net_id net_y = port_net(mod, (port_args){"y", ODIN3_DIR_OUT, 1});
    odin3_net_id in2[2] = {net_x, net_x};
    odin3_netvec ports[2] = {{in2, 2}, {&net_y, 1}};
    odin3_node_id inst = add_node(mod, (node_args){"sub", "u1", NULL, 0}, ports);
    set_attr(mod, (odin3_objref){ODIN3_OBJ_NODE, inst.v}, "parameter:W", odin3_value_int(8));
    char *text = round_trip(NULL);
    assert_has(text, "\"type\": \"sub\"");
    assert_has(text, "\"u1\": {");
    assert_has(text, "\"parameter:W\": \"00000000000000000000000000001000\"");
    odin3_util_free(text);
}

static void test_parameters_binary(void) {
    odin3_module *mod = new_module("top");
    odin3_net_id net_x = port_net(mod, (port_args){"x", ODIN3_DIR_IN, 1});
    odin3_net_id net_s = port_net(mod, (port_args){"s", ODIN3_DIR_IN, 1});
    odin3_wire_id ywire = add_port(mod, (port_args){"y", ODIN3_DIR_OUT, 2});
    odin3_net_id lo[2] = {net_x, net_x};
    odin3_net_id out[2];
    odin3_netvec vy = wire_nets(mod, ywire, out);
    odin3_value width = odin3_value_int(2);
    odin3_netvec ports[4] = {{lo, 2}, {lo, 2}, {&net_s, 1}, vy};
    (void)add_node(mod, (node_args){"$mux", "m", &width, 1}, ports);
    char *text = round_trip(NULL);
    assert_has(text, "\"type\": \"$mux\"");
    assert_has(text, "\"WIDTH\": \"00000000000000000000000000000010\"");
    odin3_util_free(text);
}

static void test_2d_word_cells(void) {
    odin3_module *mod = new_module("top");
    odin3_net_id net_x = port_net(mod, (port_args){"x", ODIN3_DIR_IN, 1});
    odin3_net_id net_s = port_net(mod, (port_args){"s", ODIN3_DIR_IN, 1});
    odin3_net_id net_y = port_net(mod, (port_args){"y", ODIN3_DIR_OUT, 1});
    odin3_net_id net_z = port_net(mod, (port_args){"z", ODIN3_DIR_OUT, 1});
    odin3_net_id net_n = port_net(mod, (port_args){"n", ODIN3_DIR_OUT, 1});
    odin3_net_id two[2] = {net_x, net_s};
    odin3_value bin[5] = {odin3_value_int(0), odin3_value_int(1), odin3_value_int(2),
                          odin3_value_int(1), odin3_value_int(1)};
    odin3_netvec sx_ports[3] = {{two, 2}, {&net_s, 1}, {&net_y, 1}};
    (void)add_node(mod, (node_args){"$shiftx", "sx", bin, 5}, sx_ports);
    odin3_netvec pw_ports[3] = {{two, 2}, {&net_s, 1}, {&net_z, 1}};
    (void)add_node(mod, (node_args){"$pow", "pw", bin, 5}, pw_ports);
    odin3_value unary[3] = {odin3_value_int(1), odin3_value_int(2), odin3_value_int(1)};
    odin3_netvec ln_ports[2] = {{two, 2}, {&net_n, 1}};
    (void)add_node(mod, (node_args){"$logic_not", "ln", unary, 3}, ln_ports);
    char *text = round_trip(NULL);
    assert_has(text, "\"type\": \"$shiftx\"");
    assert_has(text, "\"type\": \"$pow\"");
    assert_has(text, "\"type\": \"$logic_not\"");
    odin3_util_free(text);
}

/* A wire [5:2] has offset 2; [0:3] is upto; signedness is written. */
static void test_wire_range_attributes(void) {
    odin3_module *mod = new_module("top");
    odin3_wire_spec down = {intern("w"), 5, 2, true, prov_at(3)};
    odin3_wire_spec up = {intern("u"), 0, 3, false, prov_at(3)};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_create(mod, &down, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_create(mod, &up, NULL, NULL));
    char *text = round_trip(NULL);
    assert_has(text, "\"offset\": 2,\n          \"signed\": 1,");
    assert_has(text, "\"upto\": 1,");
    odin3_util_free(text);
    odin3_module *back = rj_module("top");
    odin3_wire_id wire = rj_wire(back, "w");
    TEST_ASSERT_EQUAL_INT32(5, odin3_wire_msb(back, wire));
    TEST_ASSERT_EQUAL_INT32(2, odin3_wire_lsb(back, wire));
    TEST_ASSERT_TRUE(odin3_wire_signed(back, wire));
    wire = rj_wire(back, "u");
    TEST_ASSERT_EQUAL_INT32(0, odin3_wire_msb(back, wire));
    TEST_ASSERT_EQUAL_INT32(3, odin3_wire_lsb(back, wire));
}

/* Ranged ports: [0:3] signed is upto and signed, [5:2] has offset 2, [-1:2] has offset -1. */
static void test_ranged_ports(void) {
    odin3_module *mod = new_module("top");
    static const struct {
        const char *name;
        odin3_dir dir;
        int32_t msb, lsb;
        bool is_signed;
    } k_ports[] = {{"a", ODIN3_DIR_IN, 0, 3, true},
                   {"d", ODIN3_DIR_IN, 5, 2, false},
                   {"n", ODIN3_DIR_OUT, -1, 2, false}};
    for (size_t i = 0; i < sizeof k_ports / sizeof k_ports[0]; i++) {
        odin3_port_spec spec = {.name = intern(k_ports[i].name),
                                .dir = k_ports[i].dir,
                                .prov = prov_at(2),
                                .has_range = true,
                                .msb = k_ports[i].msb,
                                .lsb = k_ports[i].lsb,
                                .is_signed = k_ports[i].is_signed};
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_add_port(mod, &spec, NULL));
    }
    char *text = round_trip(NULL);
    odin3_util_free(text);
    odin3_module *back = rj_module("top");
    for (size_t i = 0; i < sizeof k_ports / sizeof k_ports[0]; i++) {
        odin3_wire_id wire = rj_wire(back, k_ports[i].name);
        TEST_ASSERT_EQUAL_INT32(k_ports[i].msb, odin3_wire_msb(back, wire));
        TEST_ASSERT_EQUAL_INT32(k_ports[i].lsb, odin3_wire_lsb(back, wire));
        TEST_ASSERT_EQUAL_INT(k_ports[i].is_signed, odin3_wire_signed(back, wire));
    }
}

/* A user name equal to a generated key wins; the generated one gets a $u suffix. */
static void test_cell_key_collision(void) {
    odin3_module *mod = new_module("top");
    odin3_net_id nets[6];
    for (int i = 0; i < 6; i++) {
        nets[i] = new_net(mod, NULL);
    }
    add_gate(mod, "$c2", nets);
    add_gate(mod, NULL, nets + 3);
    char *text = round_trip(NULL);
    assert_has(text, "\"$c2\": {");
    assert_has(text, "\"$c2$u1\": {");
    odin3_util_free(text);
}

static void test_netname_key_collision(void) {
    odin3_module *mod = new_module("top");
    odin3_net_id clk = new_net(mod, "clk");
    odin3_net_id din = new_net(mod, "d");
    odin3_net_id qnet = new_net(mod, NULL);
    add_latch(mod, (odin3_net_id[3]){clk, din, qnet}, 1);
    odin3_wire_spec spec = {intern("$n3"), 0, 0, false, prov_at(3)};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_create(mod, &spec, NULL, NULL));
    char *text = round_trip(NULL);
    assert_has(text, "\"$n3\": {");
    assert_has(text, "\"$n3$u1\": {");
    odin3_util_free(text);
}

/* A cell named like a net is bumped; the netname keeps the user's name. */
static void test_cell_named_like_net(void) {
    odin3_module *mod = new_module("top");
    odin3_net_id net_a = port_net(mod, (port_args){"a", ODIN3_DIR_IN, 1});
    odin3_net_id net_b = port_net(mod, (port_args){"b", ODIN3_DIR_IN, 1});
    odin3_net_id foo = new_net(mod, "foo");
    odin3_net_id net_y = port_net(mod, (port_args){"y", ODIN3_DIR_OUT, 1});
    add_gate(mod, "foo", (odin3_net_id[3]){net_a, net_b, foo});
    add_gate(mod, "a", (odin3_net_id[3]){foo, net_b, net_y});
    char *text = round_trip(NULL);
    assert_has(text, "\"foo$u1\": {\n          \"hide_name\": 0,\n          \"type\": \"$_AND_\"");
    assert_has(text, "\"a$u1\": {\n          \"hide_name\": 0,\n          \"type\": \"$_AND_\"");
    odin3_util_free(text);
}

/* Two generated keys that coincide: both OFF-set wide covers want "$c<N+1>$sop"; the second is
 * bumped, and the bump survives the read. */
static void add_wide_offset_sop(odin3_module *mod, const char *name) {
    odin3_net_id ins[SEVEN];
    for (uint32_t i = 0; i < SEVEN; i++) {
        ins[i] = new_net(mod, NULL);
    }
    odin3_net_id out = new_net(mod, NULL);
    static const char k_cover[] = "1-0---10";
    odin3_value params[2] = {
        odin3_value_int(SEVEN),
        {ODIN3_VAL_COVER, 0, (const uint8_t *)k_cover, (uint32_t)strlen(k_cover), 0, SEVEN}};
    odin3_netvec ports[2] = {{ins, SEVEN}, {&out, 1}};
    (void)add_node(mod, (node_args){"$sop", name, params, 2}, ports);
}

static void test_generated_keys_coincide(void) {
    odin3_module *mod = new_module("top");
    char name[OUT_BUF];
    (void)snprintf(name, sizeof name, "$c%u", (unsigned)odin3_module_node_end(mod) + 1);
    add_wide_offset_sop(mod, name);
    add_wide_offset_sop(mod, NULL);
    char *text = round_trip(NULL);
    char key[OUT_BUF * 2];
    (void)snprintf(key, sizeof key, "\"%s$sop\": {", name);
    assert_has(text, key);
    (void)snprintf(key, sizeof key, "\"%s$sop$u1\": {", name);
    assert_has(text, key);
    odin3_util_free(text);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_three_cells_fixpoint);
    RUN_TEST(test_bits_ports_cells_netnames);
    RUN_TEST(test_constants);
    RUN_TEST(test_sop_lut_cases);
    RUN_TEST(test_sop_six_inputs);
    RUN_TEST(test_sop_wide_on_set);
    RUN_TEST(test_sop_wide_off_set);
    RUN_TEST(test_empty_cover_is_const0);
    RUN_TEST(test_latch_init);
    RUN_TEST(test_latch_init_unnamed_q);
    RUN_TEST(test_user_attributes);
    RUN_TEST(test_declared_blackbox_module);
    RUN_TEST(test_instance_and_parameters);
    RUN_TEST(test_parameters_binary);
    RUN_TEST(test_2d_word_cells);
    RUN_TEST(test_wire_range_attributes);
    RUN_TEST(test_ranged_ports);
    RUN_TEST(test_cell_key_collision);
    RUN_TEST(test_netname_key_collision);
    RUN_TEST(test_cell_named_like_net);
    RUN_TEST(test_generated_keys_coincide);
    return UNITY_END();
}
