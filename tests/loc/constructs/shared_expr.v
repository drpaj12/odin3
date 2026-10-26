// The walk enters every node once. A plain revisiting walk is exponential on this design: each arm of
// a long else-if ladder holds the next IF, and the arm of an IF hands its child to the walk twice
// (the pattern that hung bgm.v). The declarators of one declaration share their range, as well.
module shared_expr (input [7:0] a, b, input [3:0] sel, input op, output reg [8:0] s, output co,
                    output [7:0] t);
  wire [7:0] t1, t2, t3;
  assign {co, t} = op ? ({1'b0, a} + {1'b0, b}) : ({1'b0, a} - {1'b0, b});
  assign t1 = a ^ b;
  assign t2 = {2{t1[3:0]}};
  assign t3 = t1 & t2;
  always @* begin
    if (sel == 4'd0) s = {1'b0, a};
    else if (sel == 4'd1) s = {1'b0, b};
    else if (sel == 4'd2) s = {1'b0, t1};
    else if (sel == 4'd3) s = {1'b0, t2};
    else if (sel == 4'd4) s = {1'b0, t3};
    else if (sel == 4'd5) s = {1'b0, a} + {1'b0, b};
    else if (sel == 4'd6) s = {1'b0, a} - {1'b0, b};
    else if (sel == 4'd7) s = {1'b0, a} & {1'b0, b};
    else if (sel == 4'd8) s = {1'b0, a} | {1'b0, b};
    else if (sel == 4'd9) s = {1'b0, a} ^ {1'b0, b};
    else if (sel == 4'd10) s = {&t3, t1};
    else if (sel == 4'd11) s = {|t3, t2};
    else s = 9'd0;
  end
endmodule
