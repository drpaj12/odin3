// Two write ports on two clocks (port 0 on ca, port 1 on cb) and an asynchronous read. When both
// clocks rise in the same instant and write one address, the port with the higher index wins.
module memwr2(input ca, input cb, input wea, input web, input [2:0] waa, input [2:0] wab,
              input [7:0] wda, input [7:0] wdb, input [2:0] ra, output [7:0] q);
  reg [7:0] m [0:7];
  always @(posedge ca) if (wea) m[waa] <= wda;
  always @(posedge cb) if (web) m[wab] <= wdb;
  assign q = m[ra];
endmodule
