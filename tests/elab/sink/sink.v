// sink.v — one design that uses every construct 2D's elaboration builds (Tasks 17 to 24): ports,
// parameters, nets and variables, continuous assignments, gates, instances, memories (reads,
// writes under if/case/loops/tasks, initial contents), register arrays, processes with
// if/case/for/repeat, functions and tasks, initial values and per-bit assignment rules. Stage 2's
// OOM sweep (test_elab_stage2_init.c) and the elab_no_pending CTest read it.
module leaf #(parameter W = 4) (input [W-1:0] a, output [W-1:0] y);
  assign y = ~a;
endmodule

module sink #(parameter W = 8, parameter DEPTH = 4) (
    input clk, input rst, input we, input [1:0] sel,
    input [W-1:0] d, input [2:0] wa, input [2:0] ra, input signed [W-1:0] sd,
    output [W-1:0] q, output reg [W-1:0] acc, output [3:0] flags);
  localparam AW = $clog2(DEPTH);
  wire [W-1:0] inv;
  wire both = d[0] & d[1];
  wire any_hi;
  reg [W-1:0] mem [0:DEPTH-1];
  reg [W-1:0] shadow [1:4];
  reg [W-1:0] regs [0:1];
  reg [3:0] cnt = 4'd3;
  reg k;
  reg [W-1:0] t1;
  integer i;

  initial k = 1'b1;
  initial begin
    mem[0] = 8'h12;
    for (i = 1; i < DEPTH; i = i + 1) mem[i] = i;
  end

  leaf #(.W(W)) u0 (.a(d), .y(inv));
  or g0 (any_hi, d[W-1], d[W-2]);

  function [W-1:0] bump(input [W-1:0] x);
    begin
      bump = x + 1;
    end
  endfunction

  task store(input [AW-1:0] a, input [W-1:0] v);
    begin
      mem[a] <= v;
    end
  endtask

  always @(posedge clk) begin
    if (rst) cnt <= 4'd0; else cnt <= cnt + 4'd1;
    case (sel)
      2'd0: acc <= bump(d);
      2'd1: acc <= acc + d;
      2'd2: acc <= sd >>> 1;
      default: acc <= ~acc;
    endcase
  end

  always @(posedge clk) begin
    if (we) store(wa[AW-1:0], d);
    for (i = 0; i < 2; i = i + 1) if (sel[0]) shadow[wa + i] <= d ^ i;
  end

  always @(posedge clk or posedge rst) begin
    if (rst) regs[0] <= 0; else regs[0] <= d;
  end

  always @* begin
    t1 = 0;
    repeat (2) t1 = t1 + inv;
    if (any_hi) t1[0] = both;
  end

  assign q = mem[ra[AW-1:0]] ^ shadow[ra] ^ t1 ^ regs[0];
  assign flags = {k, both, cnt[1:0]};

  genvar g;
  generate
    for (g = 0; g < 2; g = g + 1) begin : gb
      wire [W-1:0] m = d << g;
    end
  endgenerate
endmodule
