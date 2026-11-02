module top (  // tests/flatten/hierref.v:18
  (* src = "tests/flatten/hierref.v:18.13-18.22" *) input clk,  // tests/flatten/hierref.v:18
  (* src = "tests/flatten/hierref.v:18.24-18.31" *) input d,  // tests/flatten/hierref.v:18
  (* src = "tests/flatten/hierref.v:18.33-18.40" *) input a,  // tests/flatten/hierref.v:18
  (* src = "tests/flatten/hierref.v:18.42-18.54" *) output r,  // tests/flatten/hierref.v:18
  (* src = "tests/flatten/hierref.v:18.56-18.64" *) output q,  // tests/flatten/hierref.v:18
  (* src = "tests/flatten/hierref.v:18.66-18.74" *) output y,  // tests/flatten/hierref.v:18
  (* src = "tests/flatten/hierref.v:18.76-18.84" *) output z  // tests/flatten/hierref.v:18
);
  (* src = "tests/flatten/hierref.v:21.30-21.38" *) wire \u_r.regs ;  // tests/flatten/hierref.v:21
  (* src = "tests/flatten/hierref.v:22.14-22.17" *) wire \u.w ;  // tests/flatten/hierref.v:22
  (* src = "tests/flatten/hierref.v:6.17-6.26|tests/flatten/hierref.v:19.11-19.40" *) wire \u_r.clk ;  // tests/flatten/hierref.v:6
  (* src = "tests/flatten/hierref.v:6.28-6.35|tests/flatten/hierref.v:19.11-19.40" *) wire \u_r.d ;  // tests/flatten/hierref.v:6
  (* src = "tests/flatten/hierref.v:6.37-6.45|tests/flatten/hierref.v:19.11-19.40" *) wire \u_r.q ;  // tests/flatten/hierref.v:6
  (* src = "tests/flatten/hierref.v:7.14-7.18|tests/flatten/hierref.v:19.11-19.40" *) wire [31:0] \u_r.regs_1 ;  // tests/flatten/hierref.v:7
  (* src = "tests/flatten/hierref.v:12.17-12.24|tests/flatten/hierref.v:20.11-20.27" *) wire \u.a ;  // tests/flatten/hierref.v:12
  (* src = "tests/flatten/hierref.v:12.26-12.34|tests/flatten/hierref.v:20.11-20.27" *) wire \u.y ;  // tests/flatten/hierref.v:12
  (* src = "tests/flatten/hierref.v:13.8-13.9|tests/flatten/hierref.v:20.11-20.27" *) wire \u.w_1 ;  // tests/flatten/hierref.v:13
  reg [0:0] \$c12 ;
  reg [31:0] \$c13 ;
  assign z = \u.w ;
  assign \u_r.clk = clk;
  assign \u_r.d = d;
  assign \u_r.q = q;
  assign \u_r.regs_1 [0] = q;
  assign \u.a = a;
  assign \u.y = y;
  assign \u.w_1 = y;
  assign y = ~a;  // tests/flatten/hierref.v:14
  (* src = "tests/flatten/hierref.v:21.3-21.43" *)
  always @(posedge clk) \$c12 <= 1'bx;  // tests/flatten/hierref.v:21
  assign r = \$c12 ;
  (* src = "tests/flatten/hierref.v:8.3-8.49|tests/flatten/hierref.v:19.11-19.40" *)
  always @(posedge clk) \$c13 <= {\u_r.regs_1 [30:1], q, d};  // tests/flatten/hierref.v:8
  assign {\u_r.regs_1 [31:1], q} = \$c13 ;
endmodule
