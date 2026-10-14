attribute \top 1
module \t
  wire width 4 input 1 \a
  wire width 4 input 2 \b
  wire width 4 input 3 \c
  wire width 4 output 4 \y
  wire width 4 output 5 \z
  wire $n21
  wire $n22
  wire $n23
  wire $n24
  cell $add $c10
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \a
    connect \B \b
    connect \Y { $n24 $n23 $n22 $n21 }
  end
  cell $mul $c11
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A { $n24 $n23 $n22 $n21 }
    connect \B \c
    connect \Y \y
  end
  connect \z \y
end
