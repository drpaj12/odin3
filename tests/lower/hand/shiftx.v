// lower-sim's design with a $shiftx: no simulate hook, so the tool reports it "skipped", never
// "pass" (3B spec 14.3). The indexed part-select with a variable base is what makes a $shiftx.
module m(input [7:0] a, input [2:0] i, output [3:0] y);
  assign y = a[i +: 4];
endmodule
