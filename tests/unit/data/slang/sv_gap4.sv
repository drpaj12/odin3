module c4(input logic [7:0] a, input logic [3:0] b, input logic [1:0] sel, output logic y, output logic [3:0] z, output logic [7:0] w, output logic [5:0] v);
  typedef enum logic [1:0] {A,B,C,D} st_t;
  st_t s;
  logic [3:0] p;
  assign p = '{b[3], b[2], 1'b0, 1'b1} ;
  assign y = (4'(a) == b);
  assign w = 8'(b);
  assign v = 6'(a[7:2]);
  always_comb begin
    unique case (sel)
      2'd0: s = A;
      2'd1: s = B;
      2'd2: s = st_t'(sel);
      2'd3: s = D;
    endcase
    priority if (sel[0]) z = p; else z = 4'(s);
  end
endmodule
