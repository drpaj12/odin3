attribute \top 1
module \one_bit_logic
  wire width 8 input 1 \a
  wire width 8 input 2 \b
  wire width 8 output 3 \y
  wire width 8 output 4 \z
  wire $n34
  wire $n35
  cell $logic_and $c8
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 8
    parameter \Y_WIDTH 1
    connect \A \a
    connect \B \b
    connect \Y $n34
  end
  cell $lt $c9
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 8
    parameter \Y_WIDTH 1
    connect \A \a
    connect \B \b
    connect \Y $n35
  end
  connect \y { 7'0000000 $n34 }
  connect \z { 7'0000000 $n35 }
end
