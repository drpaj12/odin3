attribute \top 1
module \m
  wire width 8 input 1 signed \a
  wire width 3 input 2 \b
  wire width 5 output 3 \y
  wire $n17
  wire $n18
  wire $n19
  wire $n20
  wire $n21
  wire $n22
  wire $n23
  wire $n24
  cell $shl $c4
    parameter \A_SIGNED 1
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 3
    parameter \Y_WIDTH 8
    connect \A \a
    connect \B \b
    connect \Y { $n24 $n23 $n22 $n21 $n20 $n19 $n18 $n17 }
  end
  connect \y { $n21 $n20 $n19 $n18 $n17 }
end
