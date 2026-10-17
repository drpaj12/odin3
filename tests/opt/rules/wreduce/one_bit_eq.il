attribute \top 1
module \one_bit_eq
  wire width 8 input 1 \a
  wire width 8 input 2 \b
  wire width 8 output 3 \y
  wire $n26
  cell $eq $c6
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 8
    parameter \Y_WIDTH 1
    connect \A \a
    connect \B \b
    connect \Y $n26
  end
  connect \y { 7'0000000 $n26 }
end
