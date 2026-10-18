attribute \top 1
module \m
  wire width 8 input 1 \a
  wire width 11 output 2 \y
  cell $neg $c4
    parameter \A_SIGNED 0
    parameter \A_WIDTH 11
    parameter \Y_WIDTH 11
    connect \A { 3'000 \a }
    connect \Y \y
  end
end
