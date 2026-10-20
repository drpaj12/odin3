// FSM micro: 32 states (rule R-1: the auto boundary is 32 states; below one-hot, from 32 up binary).
module states32(input clk, input rst, input go, output reg o);
  reg [4:0] st;
  always @(posedge clk or posedge rst)
    if (rst) st <= 0;
    else case (st)
      0: if (go) st <= 1;
      1: st <= 2;
      2: if (go) st <= 3;
      3: st <= 4;
      4: if (go) st <= 5;
      5: st <= 6;
      6: if (go) st <= 7;
      7: st <= 8;
      8: if (go) st <= 9;
      9: st <= 10;
      10: if (go) st <= 11;
      11: st <= 12;
      12: if (go) st <= 13;
      13: st <= 14;
      14: if (go) st <= 15;
      15: st <= 16;
      16: if (go) st <= 17;
      17: st <= 18;
      18: if (go) st <= 19;
      19: st <= 20;
      20: if (go) st <= 21;
      21: st <= 22;
      22: if (go) st <= 23;
      23: st <= 24;
      24: if (go) st <= 25;
      25: st <= 26;
      26: if (go) st <= 27;
      27: st <= 28;
      28: if (go) st <= 29;
      29: st <= 30;
      30: if (go) st <= 31;
      31: st <= 0;
    endcase
  always @* o = (st == 31);
endmodule
