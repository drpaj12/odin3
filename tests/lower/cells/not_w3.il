attribute \top 1
module \m
  wire width 3 input 1 \a
  wire width 3 output 2 \y
  cell $not $c3
    parameter \A_SIGNED 0
    parameter \A_WIDTH 3
    parameter \Y_WIDTH 3
    connect \A \a
    connect \Y \y
  end
end
