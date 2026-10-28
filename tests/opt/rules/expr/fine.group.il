attribute \top 1
module \t
  wire width 4 input 1 \a
  wire width 2 input 2 \b
  wire width 4 output 3 \y
  wire $n12
  wire $n13
  cell $and $c6
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 2
    parameter \B_WIDTH 2
    parameter \Y_WIDTH 2
    connect \A \a [3:2]
    connect \B \b
    connect \Y { $n13 $n12 }
  end
  connect \y { $n13 $n12 \a [1:0] }
end
