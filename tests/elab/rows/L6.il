# generated: elab-check --write
attribute \top 1
module \l6_signed_literals
  wire width 4 output 1 signed \y1
  wire width 4 output 2 signed \y5
  wire width 8 output 3 signed \y2
  wire width 8 output 4 signed \y4
  wire width 6 output 5 signed \y3
  wire width 32 output 6 signed \y6
  wire width 33 output 7 signed \y7
  connect \y1 4'1000
  connect \y5 4'0011
  connect \y2 8'11110000
  connect \y4 8'11110000
  connect \y3 6'111111
  connect \y6 32'00000000000000000000000000000101
  connect \y7 33'010000000000000000000000000000000
end
