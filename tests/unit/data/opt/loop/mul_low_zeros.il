module \m
  wire width 2 input 1 \b2
  wire width 4 input 2 \a4
  wire width 8 output 3 \y
  cell $mul \c
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 8
    connect \A \a4
    connect \B { \b2 2'00 }
    connect \Y \y
  end
end
