// S2: a configuration (REJECT).
module m (input a, output y);
  assign y = a;
endmodule

config c;
  design work.m;
  default liblist work;
endconfig
