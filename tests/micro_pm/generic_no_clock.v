// 4.2 B-5 (review M2): the clock of pcie is not connected; partial_map leaves it for the writer's
// E3b refusal instead of tying it to gnd.
module generic_no_clock (
    input rst,
    input [3:0] rx,
    output [3:0] tx,
    output link_up
);
  pcie u_pcie (.rst(rst), .rx(rx), .tx(tx), .link_up(link_up));
endmodule
