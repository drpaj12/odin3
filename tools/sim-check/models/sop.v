// sop.v — Yosys's `$sop` cell (a sum of products; `read_blif -sop` makes one per .names) for the
// sim-check reference with a Yosys older than 0.45, whose `simplemap` leaves $sop cells alone.
// TABLE encoding as in Yosys's simlib.v: for term i and input j, bit 2*WIDTH*i + 2*j set means the
// term needs A[j] = 0, the bit above it set means it needs A[j] = 1, neither means don't care; Y
// is the OR of the terms. (simlib.v's always @* loop gives the same values but runs far slower in
// Icarus on large netlists.)
module \$sop (A, Y);
  parameter WIDTH = 0;
  parameter DEPTH = 0;
  parameter TABLE = 0;
  input [WIDTH-1:0] A;
  output Y;
  // Bit b (0: needs 0, 1: needs 1) of every input of term i, as a mask over A.
  function [WIDTH-1:0] mask;
    input integer i;
    input integer b;
    integer j;
    begin
      for (j = 0; j < WIDTH; j = j + 1) mask[j] = TABLE[2*WIDTH*i + 2*j + b];
    end
  endfunction
  genvar i;
  generate
    if (DEPTH == 0) begin : empty
      assign Y = 1'b0;
    end else begin : cover
      wire [DEPTH-1:0] term;
      for (i = 0; i < DEPTH; i = i + 1) begin : t
        localparam [WIDTH-1:0] NEED0 = mask(i, 0);
        localparam [WIDTH-1:0] NEED1 = mask(i, 1);
        assign term[i] = ~|(A & NEED0) & ~|(~A & NEED1);
      end
      assign Y = |term;
    end
  endgenerate
endmodule
