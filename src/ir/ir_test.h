/*
 * ir_test.h — test-only corruption hooks for the check tests (hidden; never exported, never
 * called outside tests/unit). Each fault breaks one IR §9 invariant on purpose.
 */
#ifndef ODIN3_IR_TEST_H
#define ODIN3_IR_TEST_H

#include "ir/module.h"
#include "odin3/odin3.h"

#include <stdint.h>

/* One fault per rule; `id` (see odin3_ir_test_corrupt) names the object it is applied to. */
typedef enum odin3_ir_test_fault {
    ODIN3_IR_TEST_PIN_BAD_PORT,   /* rule 1: pin id's port index = its type's port count */
    ODIN3_IR_TEST_PIN_NOT_IN_NET, /* rule 2: connected pin id leaves its net's array, keeps net */
    ODIN3_IR_TEST_NET_EXTRA_PIN,  /* rule 2: connected pin id forgets its net, stays listed */
    ODIN3_IR_TEST_PARTITION,      /* rule 3: net id's first driver and last sink swap places */
    ODIN3_IR_TEST_DRIVER_COUNT,   /* rule 3: net id's driver count = its pin count + 1 */
    ODIN3_IR_TEST_MULTI_DRIVER,   /* rule 4: a new $_CONST0_ also drives net id */
    ODIN3_IR_TEST_PIN_COUNT,      /* rule 5: node id's pin count drops by one */
    ODIN3_IR_TEST_DUP_NAME,       /* rule 6: node id takes another live node's name */
    ODIN3_IR_TEST_BAD_PROV,       /* rule 7: node id's prov = one past the last record */
    ODIN3_IR_TEST_WIRE_BACKREF,   /* rule 8: the net at wire id bit 0 drops that primary */
    ODIN3_IR_TEST_PORT_LIST,      /* rule 9: port entry id points at the next port's node */
    ODIN3_IR_TEST_VIEW,           /* rule 10: 1-bit $not node id becomes a $_NOT_ */
    ODIN3_IR_TEST_DEAD_NODE_LIVE_PIN, /* rule 11: node id dies with its pins still connected */
    ODIN3_IR_TEST_PORT_WIRE_MISMATCH, /* rule 9: port id's pin 0 moves to a new net */
    ODIN3_IR_TEST_FAULT_COUNT
} odin3_ir_test_fault;

/* A fault and the object (pin, net, node, wire or port index, per fault) it is applied to. */
typedef struct odin3_ir_test_target {
    odin3_ir_test_fault fault;
    uint32_t id;
} odin3_ir_test_target;

/*
 * Applies the fault, writing IR fields directly (bypassing the API's guarantees). Preconditions:
 * PIN_NOT_IN_NET and NET_EXTRA_PIN need a live connected pin; PARTITION a live net with a driver
 * and a sink; DRIVER_COUNT and MULTI_DRIVER a live net; PIN_COUNT a live node with a pin;
 * DUP_NAME a live node and another live named node; BAD_PROV a live node; WIRE_BACKREF a live
 * wire whose bit 0 is its net's primary; PORT_LIST a port index of a module with two or more
 * ports; VIEW a live $not node of width 1; DEAD_NODE_LIVE_PIN a live non-port node;
 * PORT_WIRE_MISMATCH a port index whose pin 0 is connected. ODIN3_ERR_INVALID_ARG (logged) when
 * they do not hold (nothing changes); ODIN3_ERR_NO_MEMORY from MULTI_DRIVER's node creation or
 * PORT_WIRE_MISMATCH's net creation and connect (which goes through the API).
 */
odin3_status odin3_ir_test_corrupt(odin3_module *module, odin3_ir_test_target target);

#endif
