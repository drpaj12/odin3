// P4: typed parameters — variants: parameter integer, localparam integer (MUST); parameter real, realtime and time, localparam real (REJECT)
module p4_typed_parameters (output [31:0] y, z);
  parameter integer I = 5;
  localparam integer J = 6;
  assign y = I;
  assign z = J;
endmodule
