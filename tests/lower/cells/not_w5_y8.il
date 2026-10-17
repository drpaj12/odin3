attribute \top 1
module \m
  wire width 5 input 1 \a
  wire width 8 output 2 \y
  cell $not $c4
    parameter \A_SIGNED 0
    parameter \A_WIDTH 8
    parameter \Y_WIDTH 8
    connect \A { 3'000 \a }
    connect \Y \y
  end
end
