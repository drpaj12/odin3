attribute \top 1
module \t
  wire width 4 input 1 \a
  wire width 3 input 2 \b
  wire width 4 output 3 \y
  wire $n13
  wire $n14
  wire $n15
  cell $add $c6
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 3
    parameter \B_WIDTH 3
    parameter \Y_WIDTH 3
    connect \A \a [3:1]
    connect \B \b
    connect \Y { $n15 $n14 $n13 }
  end
  connect \y { $n15 $n14 $n13 \a [0] }
end
