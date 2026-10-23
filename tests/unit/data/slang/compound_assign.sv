// #264: a compound assignment to a bit-select lowers to `q[i] = q[i] op rhs`, the operator applied
// once. Each module has an explicit twin in compound_assign_twins.sv; the gate BLIFs of the pair
// must be equivalent (tests/cli/test_cli.sh sv_compound_assign).
module xor_compound (input [4:0] a, input b, output reg [4:0] q);
  always @(a, b) begin
    q = a;
    q[2] ^= b;
  end
endmodule

module plus_compound (input [4:0] a, input b, output reg [4:0] q);
  always @(a, b) begin
    q = a;
    q[2] += b;
  end
endmodule
