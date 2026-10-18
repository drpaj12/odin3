attribute \top 1
module \m
  wire input 1 signed \a
  wire width 4 output 2 \y
  cell $neg $c3
    parameter \A_SIGNED 1
    parameter \A_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A { \a \a \a \a }
    connect \Y \y
  end
end
