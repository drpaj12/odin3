/*
 * allow.c — the allow-list of ABC commands (3D spec §7.5; PHASE3 #11, #12): P3-11 in real command
 * names, each verified at the pin (a3001b72) against its Cmd_CommandAdd line, with the class, the
 * effect that drives §7.3 step 11, and the options whose value is a file or a string (§7.3 step
 * 9, I12), read from each command's Extra_UtilGetopt handling and usage text:
 *
 *   if   -S, -J  base/abci/abc.c:23071, 23085 (pLutStruct; usage line 23480: "[-SJ str]")
 *   &if  -S, -J  base/abci/abc.c:45353, 45367 (pLutStruct; usage line 45832: "[-SJ str]")
 *   &ps  -D      base/abci/abc.c:36134 (pDumpFile; usage line 36173: "[-D file]")
 *
 * Every other allowed command reads each option value with atoi/atof, or takes none.
 *
 * Then the refused rows: §7.5's "among others" (files, process, latches), every registered
 * read_* and write_* command with read and write, the & space's &r, &w, &read_* and &write_*
 * ones, and the command each refused abc.rc alias stops at (§7.4). The refused abc.rc aliases'
 * commands that ABC does not register at the pin (restructure, trace_start, trace_check) are not
 * listed: they are "other" anyway.
 *
 * Linear lookup: about 190 rows, at most 1,024 commands per script.
 */
#include "passes/abc/script.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const odin3_abc_command_info k_abc_commands[] = {
    /* optimisation: ntk := aig */
    {"strash", ODIN3_ABC_CLASS_OPT, ODIN3_ABC_FX_NTK_AIG, ""},
    {"balance", ODIN3_ABC_CLASS_OPT, ODIN3_ABC_FX_NTK_AIG, ""},
    {"rewrite", ODIN3_ABC_CLASS_OPT, ODIN3_ABC_FX_NTK_AIG, ""},
    {"refactor", ODIN3_ABC_CLASS_OPT, ODIN3_ABC_FX_NTK_AIG, ""},
    {"resub", ODIN3_ABC_CLASS_OPT, ODIN3_ABC_FX_NTK_AIG, ""},
    {"dc2", ODIN3_ABC_CLASS_OPT, ODIN3_ABC_FX_NTK_AIG, ""},
    {"drw", ODIN3_ABC_CLASS_OPT, ODIN3_ABC_FX_NTK_AIG, ""},
    {"drf", ODIN3_ABC_CLASS_OPT, ODIN3_ABC_FX_NTK_AIG, ""},
    {"dch", ODIN3_ABC_CLASS_OPT, ODIN3_ABC_FX_NTK_AIG, ""},
    {"ifraig", ODIN3_ABC_CLASS_OPT, ODIN3_ABC_FX_NTK_AIG, ""},
    {"fraig", ODIN3_ABC_CLASS_OPT, ODIN3_ABC_FX_NTK_AIG, ""},
    {"fraig_sweep", ODIN3_ABC_CLASS_OPT, ODIN3_ABC_FX_NTK_AIG, ""},
    {"scorr", ODIN3_ABC_CLASS_OPT, ODIN3_ABC_FX_NTK_AIG, ""},
    /* optimisation: keeps */
    {"sweep", ODIN3_ABC_CLASS_OPT, ODIN3_ABC_FX_NTK_KEEP, ""},
    {"cleanup", ODIN3_ABC_CLASS_OPT, ODIN3_ABC_FX_NTK_KEEP, ""},
    {"scleanup", ODIN3_ABC_CLASS_OPT, ODIN3_ABC_FX_NTK_KEEP, ""},
    /* mapping */
    {"if", ODIN3_ABC_CLASS_MAP, ODIN3_ABC_FX_NTK_MAPPED, "SJ"},
    {"mfs", ODIN3_ABC_CLASS_MAP, ODIN3_ABC_FX_NTK_KEEP, ""},
    {"mfs2", ODIN3_ABC_CLASS_MAP, ODIN3_ABC_FX_NTK_KEEP, ""},
    {"lutpack", ODIN3_ABC_CLASS_MAP, ODIN3_ABC_FX_NTK_KEEP, ""},
    /* the & space */
    {"&get", ODIN3_ABC_CLASS_GIA, ODIN3_ABC_FX_GIA_GET, ""},
    {"&put", ODIN3_ABC_CLASS_GIA, ODIN3_ABC_FX_NTK_PUT, ""},
    {"&st", ODIN3_ABC_CLASS_GIA, ODIN3_ABC_FX_GIA_AIG, ""},
    {"&synch2", ODIN3_ABC_CLASS_GIA, ODIN3_ABC_FX_GIA_AIG, ""},
    {"&dch", ODIN3_ABC_CLASS_GIA, ODIN3_ABC_FX_GIA_AIG, ""},
    {"&if", ODIN3_ABC_CLASS_GIA, ODIN3_ABC_FX_GIA_MAPPED, "SJ"},
    {"&mfs", ODIN3_ABC_CLASS_GIA, ODIN3_ABC_FX_GIA_KEEP, ""},
    /* printing: the 32 print_* commands at the pin but print_constr and print_status;
     * print_mint is registered only under ABC_USE_CUDD (on in the pinned build) */
    {"print_auto", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, ""},
    {"print_box", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, ""},
    {"print_buf", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, ""},
    {"print_cell", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, ""},
    {"print_cone", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, ""},
    {"print_delay", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, ""},
    {"print_dsd", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, ""},
    {"print_exdc", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, ""},
    {"print_factor", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, ""},
    {"print_fanio", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, ""},
    {"print_gates", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, ""},
    {"print_genlib", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, ""},
    {"print_gs", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, ""},
    {"print_io", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, ""},
    {"print_kmap", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, ""},
    {"print_latch", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, ""},
    {"print_level", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, ""},
    {"print_lib", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, ""},
    {"print_library", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, ""},
    {"print_lut", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, ""},
    {"print_mffc", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, ""},
    {"print_mint", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, ""},
    {"print_miter", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, ""},
    {"print_npn", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, ""},
    {"print_profile", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, ""},
    {"print_sharing", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, ""},
    {"print_stats", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, ""},
    {"print_supp", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, ""},
    {"print_symm", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, ""},
    {"print_tim", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, ""},
    {"print_unate", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, ""},
    {"print_xcut", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, ""},
    {"&ps", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, "D"},
    {"time", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, ""},
    {"echo", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, ""},
    /* refused, files: print_constr and print_status take files (§7.5) */
    {"source", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"load", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"save", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"print_constr", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"print_status", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    /* refused, files: every registered read_*, write_*, &read_* and &write_*, with read, write,
     * &r, &w, &read and &write */
    {"&r", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"&read", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"&read_blif", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"&read_cblif", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"&read_gig", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"&read_mm", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"&read_stg", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"&read_ver", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"&w", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"&write", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"&write_cnf", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"&write_mm", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"&write_resub", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"&write_truth", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"&write_truths", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"&write_ver", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"read", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"read_aiger", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"read_baf", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"read_bblif", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"read_bench", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"read_blif", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"read_blif_mv", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"read_box", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"read_cell", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"read_cex", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"read_cnf", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"read_constr", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"read_eqn", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"read_fins", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"read_formula", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"read_genlib", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"read_init", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"read_json", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"read_jsonc", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"read_lib", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"read_library", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"read_lut", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"read_mm", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"read_pla", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"read_plamo", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"read_profile", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"read_rom", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"read_scl", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"read_sf", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"read_status", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"read_super", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"read_truth", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"read_verilog", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"write", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"write_aiger", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"write_aiger_cex", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"write_baf", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"write_bblif", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"write_bench", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"write_blif", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"write_blif_mv", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"write_book", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"write_box", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"write_cellnet", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"write_cex", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"write_cnf", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"write_constr", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"write_dot", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"write_edgelist", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"write_eqn", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"write_genlib", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"write_gml", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"write_hie", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"write_hmetis", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"write_json", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"write_jsonc", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"write_lib", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"write_library", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"write_mm", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"write_ntk", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"write_pla", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"write_profile", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"write_scl", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"write_smv", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"write_sorter_cnf", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"write_status", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"write_truth", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"write_verilog", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    /* refused, files: abc.rc names (rh, rty, rvl) that ABC does not register at the pin */
    {"read_hie", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"read_liberty", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    {"read_verlib", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, ""},
    /* refused, process (quit returns -1, cmd.c:386); exit and cd are no ABC commands and ls
     * is registered only on WIN32, but a script naming them gets the process message */
    {"quit", ODIN3_ABC_CLASS_PROCESS, ODIN3_ABC_FX_NONE, ""},
    {"exit", ODIN3_ABC_CLASS_PROCESS, ODIN3_ABC_FX_NONE, ""},
    {"cd", ODIN3_ABC_CLASS_PROCESS, ODIN3_ABC_FX_NONE, ""},
    {"ls", ODIN3_ABC_CLASS_PROCESS, ODIN3_ABC_FX_NONE, ""},
    {"alias", ODIN3_ABC_CLASS_PROCESS, ODIN3_ABC_FX_NONE, ""},
    {"unalias", ODIN3_ABC_CLASS_PROCESS, ODIN3_ABC_FX_NONE, ""},
    {"set", ODIN3_ABC_CLASS_PROCESS, ODIN3_ABC_FX_NONE, ""},
    {"unset", ODIN3_ABC_CLASS_PROCESS, ODIN3_ABC_FX_NONE, ""},
    {"history", ODIN3_ABC_CLASS_PROCESS, ODIN3_ABC_FX_NONE, ""},
    {"load_plugin", ODIN3_ABC_CLASS_PROCESS, ODIN3_ABC_FX_NONE, ""},
    /* refused, latches (PHASE3 #12) */
    {"retime", ODIN3_ABC_CLASS_LATCHES, ODIN3_ABC_FX_NONE, ""},
    {"dretime", ODIN3_ABC_CLASS_LATCHES, ODIN3_ABC_FX_NONE, ""},
    {"fretime", ODIN3_ABC_CLASS_LATCHES, ODIN3_ABC_FX_NONE, ""},
    {"zero", ODIN3_ABC_CLASS_LATCHES, ODIN3_ABC_FX_NONE, ""},
    {"init", ODIN3_ABC_CLASS_LATCHES, ODIN3_ABC_FX_NONE, ""},
    {"undc", ODIN3_ABC_CLASS_LATCHES, ODIN3_ABC_FX_NONE, ""},
    {"cycle", ODIN3_ABC_CLASS_LATCHES, ODIN3_ABC_FX_NONE, ""},
    /* other: the commands that stop a refused abc.rc alias (§7.4), listed so that they are
     * verified at the pin; odin3_abc_command_find returns NULL for them */
    {"fraig_store", ODIN3_ABC_CLASS_OTHER, ODIN3_ABC_FX_NONE, ""},
    {"multi", ODIN3_ABC_CLASS_OTHER, ODIN3_ABC_FX_NONE, ""},
    {"logic", ODIN3_ABC_CLASS_OTHER, ODIN3_ABC_FX_NONE, ""},
    {"&vta_gla", ODIN3_ABC_CLASS_OTHER, ODIN3_ABC_FX_NONE, ""},
    {"&gla_derive", ODIN3_ABC_CLASS_OTHER, ODIN3_ABC_FX_NONE, ""},
    {"sop", ODIN3_ABC_CLASS_OTHER, ODIN3_ABC_FX_NONE, ""},
    {"&b", ODIN3_ABC_CLASS_OTHER, ODIN3_ABC_FX_NONE, ""},
    {"rec_add3", ODIN3_ABC_CLASS_OTHER, ODIN3_ABC_FX_NONE, ""},
    {"clockgate", ODIN3_ABC_CLASS_OTHER, ODIN3_ABC_FX_NONE, ""},
    {"collapse", ODIN3_ABC_CLASS_OTHER, ODIN3_ABC_FX_NONE, ""},
    {"care_set", ODIN3_ABC_CLASS_OTHER, ODIN3_ABC_FX_NONE, ""},
    {"eliminate", ODIN3_ABC_CLASS_OTHER, ODIN3_ABC_FX_NONE, ""},
    {"ext_seq_dcs", ODIN3_ABC_CLASS_OTHER, ODIN3_ABC_FX_NONE, ""},
    {"fraig_restore", ODIN3_ABC_CLASS_OTHER, ODIN3_ABC_FX_NONE, ""},
    {"fraig_trust", ODIN3_ABC_CLASS_OTHER, ODIN3_ABC_FX_NONE, ""},
    {"indcut", ODIN3_ABC_CLASS_OTHER, ODIN3_ABC_FX_NONE, ""},
    {"renode", ODIN3_ABC_CLASS_OTHER, ODIN3_ABC_FX_NONE, ""},
    {"ssweep", ODIN3_ABC_CLASS_OTHER, ODIN3_ABC_FX_NONE, ""},
    {"undo", ODIN3_ABC_CLASS_OTHER, ODIN3_ABC_FX_NONE, ""},
};

enum { ABC_COMMAND_ROWS = sizeof k_abc_commands / sizeof k_abc_commands[0] };

uint32_t odin3_abc_command_count(void) {
    return (uint32_t)ABC_COMMAND_ROWS;
}

const odin3_abc_command_info *odin3_abc_command_at(uint32_t index) {
    return index < (uint32_t)ABC_COMMAND_ROWS ? &k_abc_commands[index] : NULL;
}

const odin3_abc_command_info *odin3_abc_command_find(const char *name) {
    if (name == NULL) {
        return NULL;
    }
    for (uint32_t i = 0; i < (uint32_t)ABC_COMMAND_ROWS; i++) {
        const odin3_abc_command_info *row = &k_abc_commands[i];
        if (strcmp(row->name, name) == 0) {
            return row->cls == ODIN3_ABC_CLASS_OTHER ? NULL : row;
        }
    }
    return NULL;
}
