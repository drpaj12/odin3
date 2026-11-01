// Level-sensitive latch whose data input reads its own output: an always @* with an incomplete
// assignment, as boundtop's inferred latches give. The loop runs through a transparent latch, so
// the simulator refuses the netlist (combinational loop) while scoap does not need it.
module latch_loop(input en, input sel, input a, input b, output y);
  reg q;
  always @* begin
    if (en)
      q = sel ? a : (q ^ b);
  end
  assign y = q & a;
endmodule
