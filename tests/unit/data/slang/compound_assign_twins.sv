// The explicit forms of compound_assign.sv, and the operator applied twice (the wrong lowering:
// `q[2] ^ b ^ b` is `q[2]`): the explicit twin must be equivalent to the compound form, and the
// twice-applied control must NOT be (tests/cli/test_cli.sh sv_compound_assign).
module xor_explicit (input [4:0] a, input b, output reg [4:0] q);
  always @(a, b) begin
    q = a;
    q[2] = q[2] ^ b;
  end
endmodule

module plus_explicit (input [4:0] a, input b, output reg [4:0] q);
  always @(a, b) begin
    q = a;
    q[2] = q[2] + b;
  end
endmodule

module xor_twice (input [4:0] a, input b, output reg [4:0] q);
  always @(a, b) begin
    q = a;
    q[2] = q[2] ^ b ^ b;
  end
endmodule

module plus_twice (input [4:0] a, input b, output reg [4:0] q);
  always @(a, b) begin
    q = a;
    q[2] = q[2] + b + b;
  end
endmodule
