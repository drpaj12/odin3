// The RTL of tests/micro/verilog/common/spram.v with the RAM written as an array: the reference
// for the soft `single_port_ram` that --vtr-compat builds on an arch without that model (D18).
module spram_instance (
    input [7:0] address,
    input [15:0] value_in,
    input we,
    input clock,
    output reg [15:0] value_out
);
    reg [15:0] mem [0:255];

    always @(posedge clock) begin
        if (we) mem[address] <= value_in;
        value_out <= mem[address];
    end
endmodule
