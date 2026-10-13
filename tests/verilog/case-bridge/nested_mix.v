// Task 22: nested if and case, blocking temporaries and non-blocking registers in one block.
// (Every path assigns q: an enable or a reset would be a $_DFFE_/$_SDFF_ gate, which the
// equiv-check cannot read.)
module t (input clk, en, input [1:0] m, input [3:0] d, output reg [3:0] q);
  reg [3:0] tmp;
  always @(posedge clk) begin
    tmp = d + 4'd1;
    if (en) begin
      case (m)
        2'd0: q <= tmp;
        2'd1: begin tmp = tmp ^ d; q <= tmp; end
        default: q <= d;
      endcase
    end else q <= ~d;
  end
endmodule
