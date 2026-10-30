/* netlist_gates.c — read_verilog -netlist: the bit-level gates write_verilog spells as expressions.
 * See netlist_locs.h (odin3_netlist_xnor). */
#include "frontends/elab/elab_internal.h"
#include "frontends/elab/netlist_locs.h"
#include "ir/module.h"
#include "odin3/odin3.h"

#include <stdbool.h>
#include <stdint.h>

enum { XOR_PINS = 3, XOR_PIN_A = 0, XOR_PIN_B = 1, XOR_PIN_Y = 2 };

/* The $xor cell the child of `~(...)` just made: one-bit A, B and Y, the newest node, and Y drives
 * nothing yet (the `~` is about to). A node with no id 0 means the pattern does not hold. */
static odin3_node_id fresh_xor(const odin3_elab2 *e2, const odin3_sig *arg) {
    odin3_node_id none = {0};
    const odin3_module *module = e2->mod;
    if (arg->width != 1 || odin3_sb_is_const(arg->bits[0])) {
        return none;
    }
    odin3_net_id net = {arg->bits[0]};
    odin3_pin_id driver = odin3_net_driver(module, net);
    if (driver.v == 0 || odin3_net_sinks(module, net).count != 0) {
        return none;
    }
    odin3_node_id node = odin3_pin_node(module, driver);
    bool newest = node.v + 1U == odin3_module_node_end(module);
    odin3_pinslice pins = odin3_node_pins(module, node);
    bool shape = newest && odin3_node_type(module, node).v == e2->types[ODIN3_E2T_XOR].v &&
                 pins.count == XOR_PINS;
    return shape && driver.v == pins.first.v + XOR_PIN_Y ? node : none;
}

odin3_status odin3_netlist_xnor(odin3_elab2 *e2, const odin3_netlist_xnor_req *req, odin3_sig *out,
                                bool *made) {
    *made = false;
    const odin3_sig *arg = req->arg;
    odin3_node_id xor_node = fresh_xor(e2, arg);
    if (!e2->opts.netlist || xor_node.v == 0) {
        return ODIN3_OK;
    }
    odin3_module *module = e2->mod;
    odin3_pinslice pins = odin3_node_pins(module, xor_node);
    odin3_net_id in_a = odin3_pin_net(module, (odin3_pin_id){pins.first.v + XOR_PIN_A});
    odin3_net_id in_b = odin3_pin_net(module, (odin3_pin_id){pins.first.v + XOR_PIN_B});
    odin3_sig sig_a;
    odin3_sig sig_b;
    odin3_status st = odin3_elab2_sig_new(e2, 1, &sig_a);
    st = st != ODIN3_OK ? st : odin3_elab2_sig_new(e2, 1, &sig_b);
    if (st != ODIN3_OK || in_a.v == 0 || in_b.v == 0) {
        return st;
    }
    sig_a.bits[0] = in_a.v;
    sig_b.bits[0] = in_b.v;
    /* the xor and its Y net were made for this `~` only: replaced by the one XNOR gate */
    st = odin3_node_delete(module, xor_node);
    st = st != ODIN3_OK ? st : odin3_net_delete(module, (odin3_net_id){arg->bits[0]});
    odin3_elab2_cell_req cell = {ODIN3_E2T_G_XNOR, req->node, &sig_a, &sig_b, 1, req->dest, NULL};
    st = st != ODIN3_OK ? st : odin3_elab2_cell(e2, &cell, out);
    *made = st == ODIN3_OK;
    return st;
}
