module fsm(input clk, input rst, input x, output reg y, output [1:0] st);
  reg [1:0] s;
  assign st = s;
  always @(posedge clk) begin
    if (rst) begin s <= 0; y <= 0; end
    else begin
      y <= 0;
      case (s)
        2'd0: s <= x ? 2'd1 : 2'd0;
        2'd1: s <= x ? 2'd1 : 2'd2;
        2'd2: begin if (x) begin s <= 2'd3; end else s <= 2'd0; end
        2'd3: begin s <= x ? 2'd1 : 2'd2; y <= 1; end
      endcase
    end
  end
endmodule
