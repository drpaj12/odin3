/*
 * abc_script_cases.c — the scripts test_abc_script accepts (abc_script_cases.h): one per accepted
 * example of the plan's Task 11 list, plus the forms the checker's output must keep for
 * CmdSplitLine (option clusters, attached values, signed and exponent numbers, echo words).
 */
#include "abc_script_cases.h"

#include "passes/abc/abc.h"

#include <stddef.h>

const abc_script_case k_abc_script_accepted[] = {
    {"if -K {K}", 6, ODIN3_ABC_EXCHANGE_AIG},
    {"if -K {K}", 16, ODIN3_ABC_EXCHANGE_BLIF},
    {"b -l\nif -K 6\n", 0, ODIN3_ABC_EXCHANGE_AIG},
    {"resyn -v\nif -K 6\n", 0, ODIN3_ABC_EXCHANGE_AIG},
    {"strash\r\nif -K 6\r\n", 0, ODIN3_ABC_EXCHANGE_BLIF},
    {"strash #\x01 a comment holds any byte but NUL: \"'!;{N}\nif -K 6", 0,
     ODIN3_ABC_EXCHANGE_BLIF},
    {"echo hello-world\nif -K 6", 0, ODIN3_ABC_EXCHANGE_AIG},
    {"&if -K 6", 0, ODIN3_ABC_EXCHANGE_AIG},
    {"strash; if -K 6; mfs2", 0, ODIN3_ABC_EXCHANGE_BLIF},
    {"&get -n; &if -K 6", 0, ODIN3_ABC_EXCHANGE_BLIF},
    {"&resyn2rs -v\n&if -K 6\n", 0, ODIN3_ABC_EXCHANGE_AIG},
    {"strash; if -K 6; &get -m", 0, ODIN3_ABC_EXCHANGE_AIG},
    {"strash; if -K 6; &get -m; &st; &if -K 6", 0, ODIN3_ABC_EXCHANGE_AIG},
    {"strash; if -K 6; &get -m -m; &if -K 6", 0, ODIN3_ABC_EXCHANGE_AIG},
    {"strash; if -K 6; &get -mc; &if -K 6", 0, ODIN3_ABC_EXCHANGE_AIG},
    {"strash; &get -m; &if -K 6", 0, ODIN3_ABC_EXCHANGE_BLIF},
    {"strash; if -g; if -K 6", 0, ODIN3_ABC_EXCHANGE_AIG},
    {"&if -K 6 -u", 0, ODIN3_ABC_EXCHANGE_AIG},
    {"&if -K 6; &put", 0, ODIN3_ABC_EXCHANGE_AIG},
    {"if -K6 -vm", 0, ODIN3_ABC_EXCHANGE_AIG},
    {"if -K {K} -v; print_stats; time; echo {K}", 6, ODIN3_ABC_EXCHANGE_AIG},
    {"compress2rs; dch -f; if -K {K} -v; mfs2 -v; lutpack", 4, ODIN3_ABC_EXCHANGE_BLIF},
    {"print_stats -v\t;\t&ps ;; if -K 6 ;", 0, ODIN3_ABC_EXCHANGE_AIG},
    {"scorr -v -F 2; fraig -C 100; if -K 6 -D 1.5e3 -E +2 -W -0.5", 0, ODIN3_ABC_EXCHANGE_AIG},
    {"&st; &synch2; &dch; &if -K 6; &mfs; &ps", 0, ODIN3_ABC_EXCHANGE_AIG},
    {"# lib/abc/vtr.abc's commands, one line\nstrash; ifraig -v; scorr -v; dc2 -v; dch -f; "
     "if -K {K} -v; mfs2 -v; print_stats",
     6, ODIN3_ABC_EXCHANGE_BLIF},
};

const size_t k_abc_script_accepted_count =
    sizeof k_abc_script_accepted / sizeof k_abc_script_accepted[0];
