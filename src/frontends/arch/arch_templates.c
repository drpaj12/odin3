/*
 * arch_templates.c — the four VTR primitives as `.o3lib` cells (spec L-2). The text of each cell is
 * the text of the same cell in lib/vtr.o3lib with its comments removed; test_arch_lib compares
 * them, so they cannot drift. The signature lists the model ports (i: input, o: output, c: clock
 * input) a VPR `<model>` must have, in any order, for the template to apply.
 */
#include "frontends/arch/arch_templates.h"

const odin3_arch_template odin3_arch_templates[ODIN3_ARCH_N_TEMPLATES] = {
    {"adder",
     "i:a i:b i:cin o:cout o:sumout",
     "cell adder hard\n"
     "  in  a 1\n"
     "  in  b 1\n"
     "  in  cin 1\n"
     "  out cout 1\n"
     "  out sumout 1\n"
     "  fn  sumout = a ^ b ^ cin\n"
     "  fn  cout   = (a & b) | (a & cin) | (b & cin)\n"
     "end\n",
    },
    {"multiply",
     "i:a i:b o:out",
     "cell multiply hard\n"
     "  param WIDTH int 1\n"
     "  in  a WIDTH\n"
     "  in  b WIDTH\n"
     "  out out 2 * WIDTH\n"
     "  fn  out = a * b\n"
     "end\n",
    },
    {"single_port_ram",
     "c:clk i:data i:addr i:we o:out",
     "cell single_port_ram hard\n"
     "  param ADDR_WIDTH int 15\n"
     "  param DATA_WIDTH int 1\n"
     "  in  clk 1 clock\n"
     "  in  data DATA_WIDTH\n"
     "  in  addr ADDR_WIDTH\n"
     "  in  we 1\n"
     "  out out DATA_WIDTH\n"
     "  memory words 2 ** ADDR_WIDTH ; width DATA_WIDTH\n"
     "  write sync clk we addr data\n"
     "  read sync clk addr\n"
     "end\n",
    },
    {"dual_port_ram",
     "c:clk i:data2 i:data1 i:addr2 i:addr1 i:we2 i:we1 o:out2 o:out1",
     "cell dual_port_ram hard\n"
     "  param ADDR_WIDTH int 15\n"
     "  param DATA_WIDTH int 1\n"
     "  in  clk 1 clock\n"
     "  in  data2 DATA_WIDTH\n"
     "  in  data1 DATA_WIDTH\n"
     "  in  addr2 ADDR_WIDTH\n"
     "  in  addr1 ADDR_WIDTH\n"
     "  in  we2 1\n"
     "  in  we1 1\n"
     "  out out2 DATA_WIDTH\n"
     "  out out1 DATA_WIDTH\n"
     "  memory words 2 ** ADDR_WIDTH ; width DATA_WIDTH\n"
     "  write sync clk we1 addr1 data1 ; write sync clk we2 addr2 data2\n"
     "  read sync clk addr2 ; read sync clk addr1\n"
     "end\n",
    },
};
