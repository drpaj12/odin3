# generated: elab-check --write
attribute \top 1
module \l10_strings
  wire width 24 input 1 \s
  wire output 2 \eq
  wire output 3 \ne
  wire width 8 output 4 \c
  wire width 40 output 5 \t
  cell $eq $c8
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 24
    parameter \B_WIDTH 24
    parameter \Y_WIDTH 1
    connect \A \s
    connect \B 24'011000010110001001100011
    connect \Y \eq
  end
  connect \ne 1'1
  connect \c 8'01100011
  connect \t 40'0110100001100101011011000110110001101111
end
