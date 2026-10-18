attribute \top 1
module \m
  wire width 8 input 1 signed \a
  wire width 11 output 2 \y
  cell $neg $c3
    parameter \A_SIGNED 1
    parameter \A_WIDTH 11
    parameter \Y_WIDTH 11
    connect \A { \a [7] \a [7] \a [7] \a }
    connect \Y \y
  end
end
