attribute \top 1
module \t
  wire width 4 input 1 \a
  wire width 4 output 2 \y
  wire width 4 \k
  wire $n15
  wire $n16
  wire $n17
  cell $mul $c6
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 3
    parameter \Y_WIDTH 3
    connect \A \a
    connect \B 3'011
    connect \Y { $n17 $n16 $n15 }
  end
  connect \y { $n17 $n16 $n15 1'0 }
  connect \k 4'0110
end
