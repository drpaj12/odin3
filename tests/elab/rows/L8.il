# generated: elab-check --write
attribute \top 1
module \l8_spaced_numbers
  wire width 8 output 1 \y1
  wire width 8 output 2 \y2
  wire width 8 output 3 \y3
  wire width 8 output 4 \y5
  wire width 4 output 5 \y4
  wire width 4 output 6 \y7
  wire width 32 output 7 \y6
  connect \y1 8'11111111
  connect \y2 8'11111111
  connect \y3 8'11111111
  connect \y5 8'10100101
  connect \y4 4'1010
  connect \y7 4'0011
  connect \y6 32'00000000000000000000000011111111
end
