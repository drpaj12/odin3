// blocking_temps_nested: blocking temporaries across nested switches
// yosys -V: Yosys 0.55 (git sha1 60f126cd0, g++ 13.3.0-6ubuntu2~24.04.1 -fPIC -O3)
module t (input [1:0] s, input u, input [3:0] a, b, output reg [3:0] y, z);
  reg [3:0] t;
  always @* begin
    t = a;
    case (s)
      2'd0: begin t = t & b; y = t; end
      2'd1: begin
        if (u) t = t | b;
        y = t + 4'd1;
      end
      default: y = ~t;
    endcase
    z = t ^ y;
  end
endmodule
