module \m
  wire width 4 input 1 \a4
  wire width 4 output 2 \y
  cell $and \c
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \a4
    connect \B 4'10x1
    connect \Y \y
  end
end
