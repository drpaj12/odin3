`define MAX(a,b) ((a)>(b)?(a):(b))
module m(input [3:0] x, y, z, output [3:0] o);
assign o = `MAX(`MAX(x,y),z);
endmodule
