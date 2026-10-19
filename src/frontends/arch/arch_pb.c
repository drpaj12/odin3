/*
 * arch_pb.c — the pb_type hierarchy of a VPR architecture (spec P-1 … P-7).
 */
#include "frontends/arch/arch_read.h"

odin3_status arch_parse_pbs(arch_rd *rd, const odin3_xml_elem *blocks) {
    (void)rd;
    (void)blocks;
    return ODIN3_OK;
}

odin3_status arch_parse_directlist(arch_rd *rd, const odin3_xml_elem *list) {
    (void)rd;
    (void)list;
    return ODIN3_OK;
}

void arch_finish_widths(arch_rd *rd) {
    (void)rd;
}
