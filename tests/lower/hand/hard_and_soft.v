// Plan Task 9 step 3 (O2): hard blocks (multiply, adder, single_port_ram) survive `lower`, the soft
// $mul, $add and $mem next to them are lowered to bit cells.
module top(input clk, input [3:0] a, input [3:0] b, input [7:0] c, input [7:0] d, input we,
           input [2:0] addr, input [7:0] din, output [7:0] hp, output [7:0] sum,
           output [7:0] sm, output [7:0] ram, output [7:0] soft_ram, output cout, output sumout);
  multiply #(.WIDTH(4)) hm (.a(a), .b(b), .out(hp));
  adder ha (.a(a[0]), .b(b[0]), .cin(we), .cout(cout), .sumout(sumout));
  defparam hr.ADDR_WIDTH = 3;
  defparam hr.DATA_WIDTH = 8;
  single_port_ram hr (.clk(clk), .addr(addr), .we(we), .data(din), .out(ram));
  assign sum = c + d;
  assign sm = a * b;
  reg [7:0] mem [0:7];
  always @(posedge clk) if (we) mem[addr] <= din;
  assign soft_ram = mem[addr];
endmodule
