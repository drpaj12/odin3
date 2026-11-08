module xcase2(input [2:0] cmd, output reg [3:0] y, output reg [3:0] k);
  // casez: z and ? are wildcards, an x bit is not (it matches nothing with --case-xz-never)
  always @* begin
    casez (cmd)
      3'b1?0: y = 4'h1;
      3'b0x1: y = 4'h2;
      3'bz11: y = 4'h3;
      default: y = 4'h4;
    endcase
  end
  // a constant selector with an x item bit: not folded, left to the switch
  always @(cmd) begin
    case (3'd2)
      3'b0x0: k = 4'h7;
      3'd2: k = 4'h8;
      default: k = 4'h9;
    endcase
  end
endmodule
