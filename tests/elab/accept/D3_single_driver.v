// D3 (accepted, 2D-7b): one tri-state driver on an internal net becomes logic (a $pmux over x).
module d3_single (input a, b, en, output y);
  wire t;
  assign t = en ? a : 1'bz;
  assign y = ~t & b;
endmodule
