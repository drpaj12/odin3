attribute \top 1
module \m
  wire input 1 \a
  wire output 2 \y
  cell $not $c3
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a
    connect \Y \y
  end
end
