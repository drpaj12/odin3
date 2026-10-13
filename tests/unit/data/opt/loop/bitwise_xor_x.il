module \m
  wire width 2 input 1 \a2
  wire width 2 output 2 \y
  cell $xor \c
    parameter \A_WIDTH 2
    parameter \B_WIDTH 2
    parameter \Y_WIDTH 2
    connect \A \a2
    connect \B 2'x1
    connect \Y \y
  end
end
