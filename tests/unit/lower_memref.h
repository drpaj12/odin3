/*
 * lower_memref.h — the independent reference model of `$mem` (3B plan Task 8 step 2, spec §7,
 * §14.1): the simlib / Verilog meaning of a memory written as a C array, sharing no code with
 * src/passes/lower. One memref_step is one clock cycle of the 1E simulator: the inputs hold for
 * the whole cycle, the clock edge comes once, and the outputs are read at the end of the cycle.
 * Values carry a defined flag per bit: a bit never written and not in INIT is undefined (x), as is
 * a read of an address that reaches no word, and the register of a sync read port before its first
 * enabled edge. The 2-valued simulator shows 0 for those; the test compares defined bits only.
 * Test code.
 */
#ifndef ODIN3_TESTS_UNIT_LOWER_MEMREF_H
#define ODIN3_TESTS_UNIT_LOWER_MEMREF_H

#include <stdbool.h>
#include <stdint.h>

enum { MEMREF_MAX_WORDS = 64, MEMREF_MAX_PORTS = 3, MEMREF_MAX_WIDTH = 8 };

/* The memory: SIZE words of WIDTH bits, OFFSET, ABITS, the ports. All write ports share a clock;
 * read port rport is synchronous (registered) when rd_sync[rport], and then transparent when
 * rd_trans[rport] and it shares the write clock. */
typedef struct memref_cfg {
    uint32_t size;
    uint32_t width;
    uint32_t abits;
    uint32_t n_rd;
    uint32_t n_wr;
    int32_t offset;
    bool rd_sync[MEMREF_MAX_PORTS];
    bool rd_trans[MEMREF_MAX_PORTS];
} memref_cfg;

/* The inputs of one cycle. wr_en is a bit mask over the WIDTH bits of the port. */
typedef struct memref_in {
    uint64_t rd_addr[MEMREF_MAX_PORTS];
    bool rd_en[MEMREF_MAX_PORTS];
    uint64_t wr_addr[MEMREF_MAX_PORTS];
    uint64_t wr_en[MEMREF_MAX_PORTS];
    uint64_t wr_data[MEMREF_MAX_PORTS];
} memref_in;

/* A read result: the bits and which of them are defined. */
typedef struct memref_val {
    uint64_t value;
    uint64_t defined;
} memref_val;

typedef struct memref {
    memref_cfg cfg;
    uint8_t value[MEMREF_MAX_WORDS];   /* per word, a mask of WIDTH bits */
    uint8_t defined[MEMREF_MAX_WORDS]; /* per word, which bits are defined */
    memref_val reg[MEMREF_MAX_PORTS];  /* the register of each sync read port */
} memref;

/* A new model; init[k * width + j] (0, 1, or 2: x) is the INIT bit of word k, or NULL: all x. */
void memref_init(memref *mem, const memref_cfg *cfg, const uint8_t *init);

/* The word an address reaches: true and *word, or false (no such word: x on reads, writes lost). */
bool memref_word(const memref_cfg *cfg, uint64_t addr, uint32_t *word);

/* One cycle: the sync read registers take their values, then the writes happen. */
void memref_step(memref *mem, const memref_in *in);

/* Read port rport as the simulator shows it at the end of the cycle whose inputs are `in`. */
memref_val memref_read(const memref *mem, const memref_in *in, uint32_t rport);

#endif
