interface bus_if #(parameter int W = 8) (input logic clk);
    logic [W-1:0] data;
    logic         valid;
    modport slave (input data, valid);
endinterface

module leaf (bus_if.slave s, input logic clk, output q);
    assign q = s.valid & ^s.data;
endmodule

module top (input logic clk, input logic [7:0] d, output [7:0] q, output qq);
    bus_if #(.W(8)) bus (.clk(clk));
    assign bus.data = d;
    assign bus.valid = 1'b1;
    assign q = bus.valid ? bus.data : 8'd0;
    leaf u_leaf (.s(bus), .clk(clk), .q(qq));
endmodule
