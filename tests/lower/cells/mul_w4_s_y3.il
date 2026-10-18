attribute \top 1
module \m
  wire width 4 input 1 signed \a
  wire width 4 input 2 signed \b
  wire width 3 output 3 \y
  wire $n12
  wire $n13
  wire $n14
  wire $n15
  cell $mul $c4
    parameter \A_SIGNED 1
    parameter \B_SIGNED 1
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \a
    connect \B \b
    connect \Y { $n15 $n14 $n13 $n12 }
  end
  connect \y { $n14 $n13 $n12 }
end
