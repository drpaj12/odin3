attribute \top 1
module \m
  wire input 1 \a
  wire width 4 output 2 \y
  cell $neg $c4
    parameter \A_SIGNED 0
    parameter \A_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A { 3'000 \a }
    connect \Y \y
  end
end
