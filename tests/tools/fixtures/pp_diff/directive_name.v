// pp-diff declared divergence (2B spec §7.3, §10.4): a compiler directive name defined
// as a macro is a syntax error in Odin III; Icarus and Yosys both accept it.
`define timescale 1
module m; wire [3:0] w = 4'd1; endmodule
