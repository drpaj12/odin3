attribute \top 1
module \m
  wire width 8 input 1 signed \a
  wire width 8 output 2 \y
  cell $not $c3
    parameter \A_SIGNED 1
    parameter \A_WIDTH 8
    parameter \Y_WIDTH 8
    connect \A \a
    connect \Y \y
  end
end
