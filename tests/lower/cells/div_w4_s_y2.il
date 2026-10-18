attribute \top 1
module \m
  wire width 4 input 1 signed \a
  wire width 4 input 2 signed \b
  wire width 2 output 3 \y
  wire $n11
  wire $n12
  wire $n13
  wire $n14
  cell $div $c4
    parameter \A_SIGNED 1
    parameter \B_SIGNED 1
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \a
    connect \B \b
    connect \Y { $n14 $n13 $n12 $n11 }
  end
  connect \y { $n12 $n11 }
end
