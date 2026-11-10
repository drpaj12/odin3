// A megafunction instance without a model (read with --auto-blackbox): it must pass through to the
// Quartus netlist as an instance with its parameters, its unconnected ports open, and no module
// definition of it in the netlist.
module blackbox_scfifo(
    input clk,
    input [7:0] d,
    input wr,
    input rd,
    output [7:0] q,
    output empty
);
    scfifo #(
        .add_ram_output_register("ON"),
        .intended_device_family("Cyclone V"),
        .lpm_numwords(16),
        .lpm_showahead("OFF"),
        .lpm_type("scfifo"),
        .lpm_width(8),
        .lpm_widthu($clog2(16)),
        .use_eab("ON")
    ) fifo (
        .clock(clk),
        .data(d),
        .rdreq(rd),
        .wrreq(wr),
        .empty(empty),
        .q(q),
        .aclr(),
        .almost_empty(),
        .usedw()
    );
endmodule
