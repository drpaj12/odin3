module \m
  wire width 2 input 1 \a2
  wire input 2 \b
  wire width 2 output 3 \y
  cell $and \c
    parameter \A_WIDTH 2
    parameter \B_WIDTH 2
    parameter \Y_WIDTH 2
    connect \A \a2
    connect \B { 1'1 \b }
    connect \Y \y
  end
end
