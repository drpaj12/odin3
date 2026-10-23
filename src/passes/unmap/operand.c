/*
 * operand.c — operand vector helpers (C-5, C-6).
 */
#include "passes/unmap/operand.h"

#include "passes/unmap/kit.h"

static bool is_zero_tail(um_cls cls, bool open_too) {
    return cls == UM_GND || cls == UM_PAD || (open_too && cls == UM_NONE);
}

uint32_t um_trim_tail(const odin3_module *module, um_vec *vec, bool open_too) {
    uint32_t removed = 0;
    while (vec->len > 1 && is_zero_tail(um_net_class(module, vec->nets[vec->len - 1]), open_too)) {
        vec->len--;
        removed++;
    }
    return removed;
}

uint32_t um_top_run(const odin3_module *module, const um_vec *vec) {
    if (vec->len < 2 || um_net_class(module, vec->nets[vec->len - 1]) != UM_SIG) {
        return vec->len;
    }
    odin3_net_id top = vec->nets[vec->len - 1];
    uint32_t width = vec->len;
    while (width >= 2 && vec->nets[width - 2].v == top.v) {
        width--;
    }
    return width;
}
