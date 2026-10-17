attribute \top 1
module \m
  wire width 17 input 1 signed \a
  wire width 20 output 2 \y
  cell $neg $c3
    parameter \A_SIGNED 1
    parameter \A_WIDTH 20
    parameter \Y_WIDTH 20
    connect \A { \a [16] \a [16] \a [16] \a }
    connect \Y \y
  end
end
