attribute \top 1
module \m
  wire width 2 input 1 \a
  wire width 5 output 2 \y
  cell $not $c4
    parameter \A_SIGNED 0
    parameter \A_WIDTH 5
    parameter \Y_WIDTH 5
    connect \A { 3'000 \a }
    connect \Y \y
  end
end
