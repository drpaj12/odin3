attribute \top 1
module \m
  wire width 3 input 1 signed \a
  wire width 6 output 2 \y
  cell $not $c3
    parameter \A_SIGNED 1
    parameter \A_WIDTH 6
    parameter \Y_WIDTH 6
    connect \A { \a [2] \a [2] \a [2] \a }
    connect \Y \y
  end
end
