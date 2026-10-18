// Many mixed cells over three modules, with flip-flops, a latch-free enable register and a memory:
// the determinism fixture of the 3B-4 review (M4). Map or pointer order has something to reorder.
module lanes(input clk, input rst, input en, input [5:0] a, input [5:0] b, input [2:0] sh,
             output [5:0] sum, output [5:0] mix, output lt, output eq, output reg [5:0] q);
  wire [5:0] shifted = a << sh;
  wire [5:0] shiftr = $signed(b) >>> sh;
  assign sum = a + b;
  assign mix = (a & shifted) | (b ^ shiftr);
  assign lt = $signed(a) < $signed(b);
  assign eq = a == b;
  always @(posedge clk)
    if (rst) q <= 6'd0;
    else if (en) q <= sum ^ mix;
endmodule

module store(input clk, input we, input [2:0] wa, input [3:0] wd, input [2:0] ra,
             output [3:0] rd, output reg [3:0] last);
  reg [3:0] mem [0:7];
  always @(posedge clk) begin
    if (we) mem[wa] <= wd;
    last <= wd;
  end
  assign rd = mem[ra];
endmodule

module top(input clk, input rst, input en, input we, input [5:0] a, input [5:0] b,
           input [2:0] sh, input [2:0] ra, output [5:0] sum, output [5:0] prod, output [5:0] pick,
           output lt, output eq, output [3:0] rd, output [3:0] last, output [5:0] q);
  wire [5:0] mix;
  lanes l0(.clk(clk), .rst(rst), .en(en), .a(a), .b(b), .sh(sh), .sum(sum), .mix(mix), .lt(lt),
           .eq(eq), .q(q));
  store s0(.clk(clk), .we(we), .wa(sh), .wd(mix[3:0]), .ra(ra), .rd(rd), .last(last));
  assign prod = a[2:0] * b[2:0];
  reg [5:0] p;
  always @* begin
    p = 6'd0;
    (* parallel_case *) case (1'b1)
      sh[0]: p = a;
      sh[1]: p = b;
      sh[2]: p = mix;
    endcase
  end
  assign pick = p;
endmodule
