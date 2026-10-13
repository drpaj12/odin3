// M4: parameter port list — variants: #(parameter A = 1, B = 2), several parameter keywords, a ranged and a signed parameter, localparam inside the header (SystemVerilog; Odin II and Parmys accept it, so 2C parses it and 2D decides), an empty #()
module m4_params #(parameter A = 1, B = 2, parameter [3:0] C = 4'd5, parameter signed D = 1) (input a, output y);
  assign y = a;
endmodule

module m4_localparam_header #(parameter W = 4, localparam LW = 2) (input a, output y);
  assign y = a;
endmodule

module m4_empty_params #() (input a, output y);
  assign y = a;
endmodule
module m4_top (input a, output y1, y2, y3);
  m4_params u1 (.a(a), .y(y1));
  m4_localparam_header u2 (.a(a), .y(y2));
  m4_empty_params u3 (.a(a), .y(y3));
endmodule
