/*
 * lower_memref.c — the reference model of `$mem` (see lower_memref.h). Written from the meaning of
 * the cell, not from src/passes/lower: the word of an address is (address - OFFSET) mod 2^ABITS
 * when that is below SIZE; a write port writes the bits its mask enables; the later port wins.
 */
#include "lower_memref.h"

#include <string.h>

static uint64_t bit_mask(uint32_t width) {
    return ((uint64_t)1 << width) - 1U;
}

bool memref_word(const memref_cfg *cfg, uint64_t addr, uint32_t *word) {
    int64_t index = ((int64_t)addr - cfg->offset) & (int64_t)bit_mask(cfg->abits);
    if (index < 0 || index >= (int64_t)cfg->size) {
        return false;
    }
    *word = (uint32_t)index;
    return true;
}

void memref_init(memref *mem, const memref_cfg *cfg, const uint8_t *init) {
    memset(mem, 0, sizeof *mem);
    mem->cfg = *cfg;
    for (uint32_t k = 0; init != NULL && k < cfg->size; k++) {
        for (uint32_t j = 0; j < cfg->width; j++) {
            uint8_t bit = init[k * cfg->width + j];
            mem->value[k] = (uint8_t)(mem->value[k] | ((bit == 1U ? 1U : 0U) << j));
            mem->defined[k] = (uint8_t)(mem->defined[k] | ((bit <= 1U ? 1U : 0U) << j));
        }
    }
}

/* What the array holds at `addr` now (undefined when no word is reached). */
static memref_val peek(const memref *mem, uint64_t addr) {
    uint32_t word = 0;
    memref_val val = {0, 0};
    if (memref_word(&mem->cfg, addr, &word)) {
        val.value = mem->value[word];
        val.defined = mem->defined[word];
    }
    return val;
}

/* The write data of port wport at the read address of the transparent port, over `val`. */
static void bypass(const memref *mem, const memref_in *in, uint32_t rport, memref_val *val) {
    uint64_t addr_mask = bit_mask(mem->cfg.abits);
    for (uint32_t wport = 0; wport < mem->cfg.n_wr; wport++) {
        if ((in->wr_addr[wport] & addr_mask) != (in->rd_addr[rport] & addr_mask)) {
            continue;
        }
        val->value = (val->value & ~in->wr_en[wport]) | (in->wr_data[wport] & in->wr_en[wport]);
        val->defined |= in->wr_en[wport];
    }
}

static void clock_read_ports(memref *mem, const memref_in *in) {
    for (uint32_t rport = 0; rport < mem->cfg.n_rd; rport++) {
        if (!mem->cfg.rd_sync[rport] || !in->rd_en[rport]) {
            continue;
        }
        memref_val val = peek(mem, in->rd_addr[rport]);
        if (mem->cfg.rd_trans[rport]) {
            bypass(mem, in, rport, &val);
        }
        mem->reg[rport] = val;
    }
}

static void clock_write_ports(memref *mem, const memref_in *in) {
    for (uint32_t wport = 0; wport < mem->cfg.n_wr; wport++) {
        uint32_t word = 0;
        if (!memref_word(&mem->cfg, in->wr_addr[wport], &word)) {
            continue;
        }
        uint64_t en = in->wr_en[wport] & bit_mask(mem->cfg.width);
        mem->value[word] = (uint8_t)((mem->value[word] & ~en) | (in->wr_data[wport] & en));
        mem->defined[word] = (uint8_t)(mem->defined[word] | en);
    }
}

void memref_step(memref *mem, const memref_in *in) {
    clock_read_ports(mem, in);
    clock_write_ports(mem, in);
}

memref_val memref_read(const memref *mem, const memref_in *in, uint32_t rport) {
    if (mem->cfg.rd_sync[rport]) {
        return mem->reg[rport];
    }
    return peek(mem, in->rd_addr[rport]);
}
