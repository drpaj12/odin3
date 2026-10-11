// T18: block-local declarations in named blocks — variants: reg, integer and localparam declared in a named block, an attribute before a block-item declaration (Icarus rejects the attribute; Yosys accepts it)
module t18_block_decls (input clk, input [3:0] a, output reg [3:0] q, output reg [3:0] n);
  always @(posedge clk) begin : body
    reg [3:0] t;
    integer i;
    localparam K = 2;
    (* keep *) reg u;
    t = a + K;
    u = t[0];
    for (i = 0; i < 4; i = i + 1)
      n[i] <= t[3 - i] ^ u;
    q <= t;
  end
endmodule
