// The RTL of tests/micro/verilog/common/dpram.v (both write enables are the clock, as there) with
// the RAM written as an array: the reference for the soft `dual_port_ram` (D18).
module dpram_instance (
    input [7:0] address1,
    input [7:0] address2,
    input [15:0] value_in1,
    input [15:0] value_in2,
    input we1,
    input we2,
    input clock,
    output reg [15:0] value_out1,
    output reg [15:0] value_out2
);
    reg [15:0] mem [0:255];

    always @(posedge clock) begin
        if (clock) mem[address1] <= value_in1;
        if (clock) mem[address2] <= value_in2;
        value_out1 <= mem[address1];
        value_out2 <= mem[address2];
    end
endmodule
