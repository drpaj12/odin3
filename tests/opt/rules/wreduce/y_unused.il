attribute \top 1
module \y_unused
  wire width 8 input 1 \a
  wire width 8 input 2 \b
  wire width 4 output 3 \y
  wire width 8 \t
  wire $n29
  wire $n30
  wire $n31
  wire $n32
  cell $and $c6
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \a [3:0]
    connect \B \b [3:0]
    connect \Y { $n32 $n31 $n30 $n29 }
  end
  connect \y { $n32 $n31 $n30 $n29 }
  connect \t [3:0] { $n32 $n31 $n30 $n29 }
end
