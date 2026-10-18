attribute \top 1
module \m
  wire width 5 input 1 \a
  wire width 5 output 2 \y
  cell $not $c3
    parameter \A_SIGNED 0
    parameter \A_WIDTH 5
    parameter \Y_WIDTH 5
    connect \A \a
    connect \Y \y
  end
end
