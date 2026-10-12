/*
 * alias.c — the ABC alias table (3D spec §7.4): every `alias` line of third_party/abc/abc.rc at
 * the pin (a3001b72), in file order. Linked ABC never reads abc.rc, so the checker expands
 * aliases itself; test_abc_alias_table parses the submodule's abc.rc and asserts equality, so a
 * pin bump that changes abc.rc fails until this table follows.
 *
 * Regenerate the rows of k_abc_aliases from the repository root with
 *
 *   awk '/^alias[ \t]/ { s = $0; sub(/^alias[ \t]+/, "", s); n = s; sub(/[ \t].*$/, "", n);
 *        s = substr(s, length(n) + 1); sub(/^[ \t]+/, "", s);
 *        if (s ~ /^"/) { s = substr(s, 2); sub(/".*$/, "", s) }
 *        else { sub(/[ \t]+$/, "", s); gsub(/[ \t]+/, " ", s) }
 *        printf "    {\"%s\", \"%s\"},\n", n, s }' third_party/abc/abc.rc
 *
 * then run clang-format. A quoted expansion is kept as written between its quotes; an unquoted
 * one is its words joined by one blank, as ABC's `alias` command stores them.
 */
#include "passes/abc/abc_test.h"
#include "passes/abc/script.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const odin3_abc_alias k_abc_aliases[] = {
    {"hi", "history"},
    {"b", "balance"},
    {"cg", "clockgate"},
    {"cl", "cleanup"},
    {"clp", "collapse"},
    {"cs", "care_set"},
    {"el", "eliminate"},
    {"esd", "ext_seq_dcs"},
    {"f", "fraig"},
    {"fs", "fraig_sweep"},
    {"fsto", "fraig_store"},
    {"fres", "fraig_restore"},
    {"fr", "fretime"},
    {"ft", "fraig_trust"},
    {"ic", "indcut"},
    {"lp", "lutpack"},
    {"pcon", "print_cone"},
    {"pd", "print_dsd"},
    {"pex", "print_exdc -d"},
    {"pf", "print_factor"},
    {"pfan", "print_fanio"},
    {"pg", "print_gates"},
    {"pl", "print_level"},
    {"plat", "print_latch"},
    {"pio", "print_io"},
    {"pk", "print_kmap"},
    {"pm", "print_miter"},
    {"ps", "print_stats"},
    {"psb", "print_stats -b"},
    {"psu", "print_supp"},
    {"psy", "print_symm"},
    {"pun", "print_unate"},
    {"q", "quit"},
    {"r", "read"},
    {"ra", "read_aiger"},
    {"r3", "retime -M 3"},
    {"r3f", "retime -M 3 -f"},
    {"r3b", "retime -M 3 -b"},
    {"ren", "renode"},
    {"rh", "read_hie"},
    {"ri", "read_init"},
    {"rl", "read_blif"},
    {"rb", "read_bench"},
    {"ret", "retime"},
    {"dret", "dretime"},
    {"rp", "read_pla"},
    {"rt", "read_truth"},
    {"rv", "read_verilog"},
    {"rvl", "read_verlib"},
    {"rsup", "read_super mcnc5_old.super"},
    {"rlib", "read_library"},
    {"rlibc", "read_library cadence.genlib"},
    {"rty", "read_liberty"},
    {"rlut", "read_lut"},
    {"rw", "rewrite"},
    {"rwz", "rewrite -z"},
    {"rf", "refactor"},
    {"rfz", "refactor -z"},
    {"re", "restructure"},
    {"rez", "restructure -z"},
    {"rs", "resub"},
    {"rsz", "resub -z"},
    {"sa", "set autoexec ps"},
    {"scl", "scleanup"},
    {"sif", "if -s"},
    {"so", "source -x"},
    {"st", "strash"},
    {"sw", "sweep"},
    {"ssw", "ssweep"},
    {"tr0", "trace_start"},
    {"tr1", "trace_check"},
    {"trt", "r c.blif; st; tr0; b; tr1"},
    {"u", "undo"},
    {"w", "write"},
    {"wa", "write_aiger"},
    {"wb", "write_bench"},
    {"wc", "write_cnf"},
    {"wh", "write_hie"},
    {"wl", "write_blif"},
    {"wp", "write_pla"},
    {"wv", "write_verilog"},
    {"resyn", "b; rw; rwz; b; rwz; b"},
    {"resyn2", "b; rw; rf; b; rw; rwz; b; rfz; rwz; b"},
    {"resyn2a", "b; rw; b; rw; rwz; b; rwz; b"},
    {"resyn3", "b; rs; rs -K 6; b; rsz; rsz -K 6; b; rsz -K 5; b"},
    {"compress", "b -l; rw -l; rwz -l; b -l; rwz -l; b -l"},
    {"compress2", "b -l; rw -l; rf -l; b -l; rw -l; rwz -l; b -l; rfz -l; rwz -l; b -l"},
    {"choice", "fraig_store; resyn; fraig_store; resyn2; fraig_store; fraig_restore"},
    {"choice2", "fraig_store; balance; fraig_store; resyn; fraig_store; resyn2; fraig_store; "
                "resyn2; fraig_store; fraig_restore"},
    {"rwsat", "st; rw -l; b -l; rw -l; rf -l"},
    {"drwsat2", "st; drw; b -l; drw; drf; ifraig -C 20; drw; b -l; drw; drf"},
    {"share", "st; multi -m; sop; fx; resyn2"},
    {"addinit", "read_init; undc; strash; zero"},
    {"blif2aig", "undc; strash; zero"},
    {"v2p", "&vta_gla; &ps; &gla_derive; &put; w 1.aig; pdr -v"},
    {"g2p", "&ps; &gla_derive; &put; w 2.aig; pdr -v"},
    {"&sw_", "&put; sweep; st; &get"},
    {"&fx_", "&put; sweep; sop; fx; st; &get"},
    {"&dc3", "&b; &jf -K 6; &b; &jf -K 4; &b"},
    {"&dc4", "&b; &jf -K 7; &fx; &b; &jf -K 5; &fx; &b"},
    {"src_rw", "st; rw -l; rwz -l; rwz -l"},
    {"src_rs", "st; rs -K 6 -N 2 -l; rs -K 9 -N 2 -l; rs -K 12 -N 2 -l"},
    {"src_rws", "st; rw -l; rs -K 6 -N 2 -l; rwz -l; rs -K 9 -N 2 -l; rwz -l; rs -K 12 -N 2 -l"},
    {"resyn2rs", "b; rs -K 6; rw; rs -K 6 -N 2; rf; rs -K 8; b; rs -K 8 -N 2; rw; rs -K 10; rwz; "
                 "rs -K 10 -N 2; b; rs -K 12; rfz; rs -K 12 -N 2; rwz; b"},
    {"r2rs", "b; rs -K 6; rw; rs -K 6 -N 2; rf; rs -K 8; b; rs -K 8 -N 2; rw; rs -K 10; rwz; rs -K "
             "10 -N 2; b; rs -K 12; rfz; rs -K 12 -N 2; rwz; b"},
    {"compress2rs", "b -l; rs -K 6 -l; rw -l; rs -K 6 -N 2 -l; rf -l; rs -K 8 -l; b -l; rs -K 8 -N "
                    "2 -l; rw -l; rs -K 10 -l; rwz -l; rs -K 10 -N 2 -l; b -l; rs -K 12 -l; rfz "
                    "-l; rs -K 12 -N 2 -l; rwz -l; b -l"},
    {"c2rs", "b -l; rs -K 6 -l; rw -l; rs -K 6 -N 2 -l; rf -l; rs -K 8 -l; b -l; rs -K 8 -N 2 -l; "
             "rw -l; rs -K 10 -l; rwz -l; rs -K 10 -N 2 -l; b -l; rs -K 12 -l; rfz -l; rs -K 12 -N "
             "2 -l; rwz -l; b -l"},
    {"&resyn2rs", "&put; resyn2rs; &get"},
    {"&compress2rs", "&put; compress2rs; &get"},
    {"fix_aig", "logic; undc; strash; zero"},
    {"fix_blif", "undc; strash; zero"},
    {"recadd3", "st; rec_add3; b; rec_add3; dc2; rec_add3; if -K 8; bidec; st; rec_add3; dc2; "
                "rec_add3; if -g -K 6; st; rec_add3"},
};

enum { ABC_ALIAS_ROWS = sizeof k_abc_aliases / sizeof k_abc_aliases[0] };

/* The table odin3_abc_test_set_aliases installed, or NULL for k_abc_aliases. */
static const odin3_abc_alias *abc_test_aliases;
static uint32_t abc_test_alias_count;

void odin3_abc_test_set_aliases(const odin3_abc_alias *table, uint32_t n) {
    abc_test_aliases = table;
    abc_test_alias_count = table != NULL ? n : 0;
}

uint32_t odin3_abc_alias_count(void) {
    return abc_test_aliases != NULL ? abc_test_alias_count : (uint32_t)ABC_ALIAS_ROWS;
}

const odin3_abc_alias *odin3_abc_alias_at(uint32_t index) {
    if (index >= odin3_abc_alias_count()) {
        return NULL;
    }
    return abc_test_aliases != NULL ? &abc_test_aliases[index] : &k_abc_aliases[index];
}

const odin3_abc_alias *odin3_abc_alias_find(const char *name) {
    if (name == NULL) {
        return NULL;
    }
    uint32_t count = odin3_abc_alias_count();
    for (uint32_t i = 0; i < count; i++) {
        const odin3_abc_alias *alias = odin3_abc_alias_at(i);
        if (strcmp(alias->name, name) == 0) {
            return alias;
        }
    }
    return NULL;
}
