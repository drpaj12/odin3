/*
 * allow.c — the allow-list of ABC commands (3D spec §7.5; PHASE3 #11, #12): P3-11 in real command
 * names, each verified at the pin (a3001b72) against its Cmd_CommandAdd line, with the class, the
 * effect that drives §7.3 step 11, and file_opts: the options that read or write a file, or that
 * take a string (§7.3 step 9 and its Task 10 review erratum; I12). Each is cited at the line that
 * assigns its value or flag in base/abci/abc.c, then at the code it enables:
 *
 *   print_stats -b  abc.c:1670  fSaveBest: Abc_NtkCompareAndSaveBest writes <spec>_best.blif
 *                               (abcPrint.c:309-310, 137-141)
 *   print_stats -d  abc.c:1673  fDumpResult: its writes are commented out at the pin
 *                               (abcPrint.c:311-319, 417-421); listed anyway
 *   rewrite -x      abc.c:8317  fPrecompute: Rwr_Precompute writes npn4_aig_array.txt
 *                               (abc.c:8338-8340, rwrUtil.c:435)
 *   resub -P, -D    abc.c:8738, 8749  Log2Probs/Log2Divs: both nonzero write p..d..v.._<name>.bin
 *                               (abcResub.c:164-172)
 *   scorr -P        abc.c:25839 nPartSize: the partitioned run writes part%03d.aig
 *                               (sswPart.c:245, 286-288)
 *   scorr -e, -f    abc.c:26004, 26007  fEquivDump/fEquivDump2: equiv%03d.aig (sswSweep.c:354-363)
 *   scorr -I        abc.c:25905 nItersStop: stopping at that iteration writes srm.blif
 *                               (sswCore.c:300-303; Task 11's audit, with -I 0)
 *   if -S, -J       abc.c:23077, 23091  pLutStruct = the string (usage abc.c:23480 "[-SJ str]")
 *   if -f           abc.c:23167 fDumpFile: If_ManStop writes <name>__n.._k...bin (ifMan.c:351-365)
 *   &if -S, -J      abc.c:45359, 45373  pLutStruct = the string (usage abc.c:45832 "[-SJ str]")
 *   &ps -D          abc.c:36140 pDumpFile = the file (usage abc.c:36173 "[-D file]";
 *                               Gia_ManLogAigStats, giaMan.c:460-462)
 *   mfs2 -i         abc.c:5916  fIndDCs: with the frames set (-j) the unrolled network is written
 *                               to unroll_dump.blif (abc.c:5946-5950, abcMfs.c:555; Task 11)
 *
 * crash_opts: the options with which the command crashes ABC at the pin (SIGSEGV, which kills
 * Odin III; Task 10 review, fix round 2), each reproduced on a sequential and a combinational
 * input, cited at the line that assigns the flag, then at the crash:
 *
 *   if -s           abc.c:23125 fDelayOptLut: If_CutLutBalanceEval (ifDelay.c:387)
 *   &if -s          abc.c:45416 fDelayOptLut: If_CutLutBalanceEval (ifDelay.c:387)
 *   &dch -m         abc.c:50024 fMinLevel: Gia_ManChoiceMinLevel_rec (giaEquiv.c:571)
 *   &put -e         abc.c:35243 fFindEnables: on a mapped & network (after &if or &get -m),
 *                               Abc_NtkFromMappedGia_rec (abcDar.c:896; fix round 3)
 *   print_auto -O   abc.c:2652  Output: an index past the last output reads a stray BDD
 *                               (Extra_SupportArray, extraBddMisc.c:550; fix round 3)
 *
 * Task 11's first audit (every pair of each allowed command's other options, valued ones at 2,
 * and each valued option alone at 0, 1, 64, -1, 3 and 1000, on the same two designs, with the
 * Release abc) found these, each backtraced in the Debug abc; a value-dependent crash refuses the
 * whole option, as -O does:
 *
 *   dc2 -f          abc.c:18480 fFanout: off with -l asserts p->pFanData
 *                               (Aig_ManStartReverseLevels, aigTiming.c:146)
 *   drw -f          abc.c:18259 fFanout: off with -l, the same assertion
 *   drw -C, -N      abc.c:18231, 18242  nCutsMax 0 (Aig_MmFixedEntryFetch, aigMem.c:197) and
 *                               nSubgMax 0 (Dar_LibCutMatch, darLib.c:741)
 *   drf -C          abc.c:18377 nCutsMax 0 asserts in Vec_VecEntry (vecVec.h:275)
 *   scorr -N        abc.c:25938 nConstrs: more constraints than outputs (Aig_ManDfsNodes from
 *                               Ssw_ManUpdateEquivs, sswCore.c:197)
 *   if -c           abc.c:23164 fUseTtPerm: with -g asserts in Vec_WecEntry (vecWec.h:156)
 *   mfs -t          abc.c:5715  fOneHotness: with -C 2, sat_solver_solve (satSolver.c:2072)
 *   mfs2 -Z         abc.c:5879  nFirstFixed: 64 asserts in Vec_StrWriteEntry (vecStr.h:390)
 *   dch -W          abc.c:18664 nWords 0 reads past the simulation words
 *                               (Dch_PerformRandomSimulation, dchSim.c:219, valgrind); glibc aborts
 *   &synch2 -W      abc.c:40960 nWords 0 corrupts the heap (glibc aborts)
 *   &dch -W         abc.c:49978 nWords 0 corrupts the heap (glibc aborts)
 *   &if -w          abc.c:45472 fVerboseTrace: with -g or -x it traces the LUT levels of an AIG
 *                               that has no mapping (Gia_ManLutLevel, giaIf.c:170)
 *
 * Task 11 review fix round 1 (rulings C2 and I1; step 9 also caps every number at 1000000). Each
 * was reproduced in the Debug abc; the first four crash at or below the cap, the next three only
 * far past it (2^31-1), and the ruling refuses them all:
 *
 *   resub -F        abc.c:8726  nLevelsOdc 10 or more asserts nLevels < 10
 *                               (Abc_NtkDontCareAlloc, abcOdc.c:171)
 *   scorr -F        abc.c:25861 nFramesK 1000000 (Ssw_SmlStart, sswSim.c:1152)
 *   mfs -M          abc.c:5672  nWinMax 1000000 (Vec_PtrPush, mfsDiv.c:233)
 *   if -Y, &if -Y   abc.c:23009, 45300  nAndArea 100000 asserts (If_CutAreaDerefed, ifCut.c:1117)
 *   scorr -S        abc.c:25894 nFramesAddSim
 *   mfs2 -M, &mfs -M  abc.c:5846, 56018  nWinSizeMax
 *   &st -M          abc.c:37064 Multi
 *   dch -s          abc.c:18692 fSynthesis: asserts CoNum % 3 == 0 (Gia_ManToAigSkip,
 *                               giaAig.c:372), on any network whose outputs are not a multiple of 3
 *
 * print_box crashes with any option, so it is a refused row of class CRASH.
 *
 * How the others were checked: every option of every allowed command was mapped to the variable
 * its case assigns, and the usage texts were searched for dump, file, write, save, record and log.
 * Then each allowed command ran once in the abc tool at the pin, once per option (numeric options
 * with 2), on a strashed, a mapped, a & and a &-mapped network, each run in an empty directory: the
 * only files created came from the options above. Task 11's first audit then ran option pairs and
 * small values with the Release abc, where NDEBUG compiles ABC's assert out, so it saw signals
 * only (review I2). Its fix round 1 re-ran with the Debug abc, on scripts the checker accepts:
 * every allowed command at default options in 14 states, every option pair, every valued option
 * at 0, 1, 3, 64, 1000 and 1000000, zero to two positional numbers, and the print and & pairs
 * after a mapping step, on Task 10's sequential design (8 inputs, 4 outputs, 4 latches) and
 * combinational one (4 inputs, 2 outputs): 11,764 runs, clean. Triples, larger networks and
 * longer command sequences were not run, so the lists may still be incomplete.
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
    {"strash", ODIN3_ABC_CLASS_OPT, ODIN3_ABC_FX_NTK_AIG, "", ""},
    {"balance", ODIN3_ABC_CLASS_OPT, ODIN3_ABC_FX_NTK_AIG, "", ""},
    {"rewrite", ODIN3_ABC_CLASS_OPT, ODIN3_ABC_FX_NTK_AIG, "x", ""},
    {"refactor", ODIN3_ABC_CLASS_OPT, ODIN3_ABC_FX_NTK_AIG, "", ""},
    {"resub", ODIN3_ABC_CLASS_OPT, ODIN3_ABC_FX_NTK_AIG, "PD", "F"},
    {"dc2", ODIN3_ABC_CLASS_OPT, ODIN3_ABC_FX_NTK_AIG, "", "f"},
    {"drw", ODIN3_ABC_CLASS_OPT, ODIN3_ABC_FX_NTK_AIG, "", "fCN"},
    {"drf", ODIN3_ABC_CLASS_OPT, ODIN3_ABC_FX_NTK_AIG, "", "C"},
    {"dch", ODIN3_ABC_CLASS_OPT, ODIN3_ABC_FX_NTK_AIG, "", "Ws"},
    {"ifraig", ODIN3_ABC_CLASS_OPT, ODIN3_ABC_FX_NTK_AIG, "", ""},
    {"fraig", ODIN3_ABC_CLASS_OPT, ODIN3_ABC_FX_NTK_AIG, "", ""},
    {"fraig_sweep", ODIN3_ABC_CLASS_OPT, ODIN3_ABC_FX_NTK_AIG, "", ""},
    {"scorr", ODIN3_ABC_CLASS_OPT, ODIN3_ABC_FX_NTK_AIG, "PefI", "NFS"},
    /* optimisation: keeps */
    {"sweep", ODIN3_ABC_CLASS_OPT, ODIN3_ABC_FX_NTK_KEEP, "", ""},
    {"cleanup", ODIN3_ABC_CLASS_OPT, ODIN3_ABC_FX_NTK_KEEP, "", ""},
    {"scleanup", ODIN3_ABC_CLASS_OPT, ODIN3_ABC_FX_NTK_KEEP, "", ""},
    /* mapping */
    {"if", ODIN3_ABC_CLASS_MAP, ODIN3_ABC_FX_NTK_MAPPED, "SJf", "scY"},
    {"mfs", ODIN3_ABC_CLASS_MAP, ODIN3_ABC_FX_NTK_KEEP, "", "tM"},
    {"mfs2", ODIN3_ABC_CLASS_MAP, ODIN3_ABC_FX_NTK_KEEP, "i", "ZM"},
    {"lutpack", ODIN3_ABC_CLASS_MAP, ODIN3_ABC_FX_NTK_KEEP, "", ""},
    /* the & space */
    {"&get", ODIN3_ABC_CLASS_GIA, ODIN3_ABC_FX_GIA_GET, "", ""},
    {"&put", ODIN3_ABC_CLASS_GIA, ODIN3_ABC_FX_NTK_PUT, "", "e"},
    {"&st", ODIN3_ABC_CLASS_GIA, ODIN3_ABC_FX_GIA_AIG, "", "M"},
    {"&synch2", ODIN3_ABC_CLASS_GIA, ODIN3_ABC_FX_GIA_AIG, "", "W"},
    {"&dch", ODIN3_ABC_CLASS_GIA, ODIN3_ABC_FX_GIA_AIG, "", "mW"},
    {"&if", ODIN3_ABC_CLASS_GIA, ODIN3_ABC_FX_GIA_MAPPED, "SJ", "swY"},
    {"&mfs", ODIN3_ABC_CLASS_GIA, ODIN3_ABC_FX_GIA_KEEP, "", "M"},
    /* printing: the 34 print_* commands at the pin but print_constr, print_status, print_box,
     * print_cone and print_npn;
     * print_mint is registered only under ABC_USE_CUDD (on in the pinned build) */
    {"print_auto", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, "", "O"},
    {"print_buf", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, "", ""},
    {"print_cell", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, "", ""},
    {"print_delay", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, "", ""},
    {"print_dsd", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, "", ""},
    {"print_exdc", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, "", ""},
    {"print_factor", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, "", ""},
    {"print_fanio", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, "", ""},
    {"print_gates", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, "", ""},
    {"print_genlib", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, "", ""},
    {"print_gs", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, "", ""},
    {"print_io", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, "", ""},
    {"print_kmap", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, "", ""},
    {"print_latch", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, "", ""},
    {"print_level", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, "", ""},
    {"print_lib", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, "", ""},
    {"print_library", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, "", ""},
    {"print_lut", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, "", ""},
    {"print_mffc", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, "", ""},
    {"print_mint", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, "", ""},
    {"print_miter", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, "", ""},
    {"print_profile", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, "", ""},
    {"print_sharing", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, "", ""},
    {"print_stats", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, "bd", ""},
    {"print_supp", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, "", ""},
    {"print_symm", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, "", ""},
    {"print_tim", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, "", ""},
    {"print_unate", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, "", ""},
    {"print_xcut", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, "", ""},
    {"&ps", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, "D", ""},
    {"time", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, "", ""},
    {"echo", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE, "", ""},
    /* refused, files: print_constr and print_status take files (§7.5) */
    {"source", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"load", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"save", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"print_constr", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"print_status", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    /* refused, files: every registered read_*, write_*, &read_* and &write_*, with read, write,
     * &r, &w, &read and &write */
    {"&r", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"&read", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"&read_blif", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"&read_cblif", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"&read_gig", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"&read_mm", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"&read_stg", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"&read_ver", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"&w", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"&write", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"&write_cnf", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"&write_mm", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"&write_resub", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"&write_truth", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"&write_truths", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"&write_ver", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"read", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"read_aiger", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"read_baf", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"read_bblif", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"read_bench", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"read_blif", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"read_blif_mv", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"read_box", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"read_cell", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"read_cex", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"read_cnf", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"read_constr", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"read_eqn", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"read_fins", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"read_formula", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"read_genlib", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"read_init", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"read_json", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"read_jsonc", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"read_lib", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"read_library", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"read_lut", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"read_mm", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"read_pla", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"read_plamo", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"read_profile", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"read_rom", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"read_scl", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"read_sf", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"read_status", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"read_super", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"read_truth", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"read_verilog", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"write", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"write_aiger", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"write_aiger_cex", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"write_baf", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"write_bblif", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"write_bench", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"write_blif", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"write_blif_mv", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"write_book", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"write_box", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"write_cellnet", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"write_cex", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"write_cnf", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"write_constr", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"write_dot", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"write_edgelist", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"write_eqn", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"write_genlib", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"write_gml", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"write_hie", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"write_hmetis", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"write_json", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"write_jsonc", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"write_lib", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"write_library", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"write_mm", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"write_ntk", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"write_pla", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"write_profile", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"write_scl", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"write_smv", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"write_sorter_cnf", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"write_status", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"write_truth", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"write_verilog", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    /* refused, files: abc.rc names (rh, rty, rvl) that ABC does not register at the pin */
    {"read_hie", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"read_liberty", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    {"read_verlib", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE, "", ""},
    /* refused, process (quit returns -1, cmd.c:386); exit and cd are no ABC commands and ls
     * is registered only on WIN32, but a script naming them gets the process message */
    {"quit", ODIN3_ABC_CLASS_PROCESS, ODIN3_ABC_FX_NONE, "", ""},
    {"exit", ODIN3_ABC_CLASS_PROCESS, ODIN3_ABC_FX_NONE, "", ""},
    {"cd", ODIN3_ABC_CLASS_PROCESS, ODIN3_ABC_FX_NONE, "", ""},
    {"ls", ODIN3_ABC_CLASS_PROCESS, ODIN3_ABC_FX_NONE, "", ""},
    {"alias", ODIN3_ABC_CLASS_PROCESS, ODIN3_ABC_FX_NONE, "", ""},
    {"unalias", ODIN3_ABC_CLASS_PROCESS, ODIN3_ABC_FX_NONE, "", ""},
    {"set", ODIN3_ABC_CLASS_PROCESS, ODIN3_ABC_FX_NONE, "", ""},
    {"unset", ODIN3_ABC_CLASS_PROCESS, ODIN3_ABC_FX_NONE, "", ""},
    {"history", ODIN3_ABC_CLASS_PROCESS, ODIN3_ABC_FX_NONE, "", ""},
    {"load_plugin", ODIN3_ABC_CLASS_PROCESS, ODIN3_ABC_FX_NONE, "", ""},
    /* refused, crashes ABC at the pin: If_LibBoxPrint dereferences the NULL box library
     * (ifCom.c:521, ifLibBox.c:350), and a script cannot load one */
    {"print_box", ODIN3_ABC_CLASS_CRASH, ODIN3_ABC_FX_NONE, "", ""},
    /* print_cone asserts Abc_NtkIsStrash on any sequential logic network (Abc_AigConst1,
     * abcAig.c:685, from abc.c:3124); print_npn's truth table divides by zero for 0
     * (Extra_PermSchedule, extraUtilMisc.c:2209), asserts for 5 (dauNpn.c:976) and overruns
     * word pTruth[16] for 257 or more digits (abc.c:2365, 2387; Task 11 review C1, I1) */
    {"print_cone", ODIN3_ABC_CLASS_CRASH, ODIN3_ABC_FX_NONE, "", ""},
    {"print_npn", ODIN3_ABC_CLASS_CRASH, ODIN3_ABC_FX_NONE, "", ""},
    /* refused, latches (PHASE3 #12) */
    {"retime", ODIN3_ABC_CLASS_LATCHES, ODIN3_ABC_FX_NONE, "", ""},
    {"dretime", ODIN3_ABC_CLASS_LATCHES, ODIN3_ABC_FX_NONE, "", ""},
    {"fretime", ODIN3_ABC_CLASS_LATCHES, ODIN3_ABC_FX_NONE, "", ""},
    {"zero", ODIN3_ABC_CLASS_LATCHES, ODIN3_ABC_FX_NONE, "", ""},
    {"init", ODIN3_ABC_CLASS_LATCHES, ODIN3_ABC_FX_NONE, "", ""},
    {"undc", ODIN3_ABC_CLASS_LATCHES, ODIN3_ABC_FX_NONE, "", ""},
    {"cycle", ODIN3_ABC_CLASS_LATCHES, ODIN3_ABC_FX_NONE, "", ""},
    /* other: the commands that stop a refused abc.rc alias (§7.4), listed so that they are
     * verified at the pin; odin3_abc_command_find returns NULL for them */
    {"fraig_store", ODIN3_ABC_CLASS_OTHER, ODIN3_ABC_FX_NONE, "", ""},
    {"multi", ODIN3_ABC_CLASS_OTHER, ODIN3_ABC_FX_NONE, "", ""},
    {"logic", ODIN3_ABC_CLASS_OTHER, ODIN3_ABC_FX_NONE, "", ""},
    {"&vta_gla", ODIN3_ABC_CLASS_OTHER, ODIN3_ABC_FX_NONE, "", ""},
    {"&gla_derive", ODIN3_ABC_CLASS_OTHER, ODIN3_ABC_FX_NONE, "", ""},
    {"sop", ODIN3_ABC_CLASS_OTHER, ODIN3_ABC_FX_NONE, "", ""},
    {"&b", ODIN3_ABC_CLASS_OTHER, ODIN3_ABC_FX_NONE, "", ""},
    {"rec_add3", ODIN3_ABC_CLASS_OTHER, ODIN3_ABC_FX_NONE, "", ""},
    {"clockgate", ODIN3_ABC_CLASS_OTHER, ODIN3_ABC_FX_NONE, "", ""},
    {"collapse", ODIN3_ABC_CLASS_OTHER, ODIN3_ABC_FX_NONE, "", ""},
    {"care_set", ODIN3_ABC_CLASS_OTHER, ODIN3_ABC_FX_NONE, "", ""},
    {"eliminate", ODIN3_ABC_CLASS_OTHER, ODIN3_ABC_FX_NONE, "", ""},
    {"ext_seq_dcs", ODIN3_ABC_CLASS_OTHER, ODIN3_ABC_FX_NONE, "", ""},
    {"fraig_restore", ODIN3_ABC_CLASS_OTHER, ODIN3_ABC_FX_NONE, "", ""},
    {"fraig_trust", ODIN3_ABC_CLASS_OTHER, ODIN3_ABC_FX_NONE, "", ""},
    {"indcut", ODIN3_ABC_CLASS_OTHER, ODIN3_ABC_FX_NONE, "", ""},
    {"renode", ODIN3_ABC_CLASS_OTHER, ODIN3_ABC_FX_NONE, "", ""},
    {"ssweep", ODIN3_ABC_CLASS_OTHER, ODIN3_ABC_FX_NONE, "", ""},
    {"undo", ODIN3_ABC_CLASS_OTHER, ODIN3_ABC_FX_NONE, "", ""},
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
