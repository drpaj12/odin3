attribute \top 1
module \m
  wire width 17 input 1 \a
  wire width 17 output 2 \y
  cell $neg $c3
    parameter \A_SIGNED 0
    parameter \A_WIDTH 17
    parameter \Y_WIDTH 17
    connect \A \a
    connect \Y \y
  end
end
