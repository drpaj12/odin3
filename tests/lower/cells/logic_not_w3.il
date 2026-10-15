attribute \top 1
module \m
  wire width 3 input 1 \a
  wire output 2 \y
  cell $logic_not $c3
    parameter \A_SIGNED 0
    parameter \A_WIDTH 3
    parameter \Y_WIDTH 1
    connect \A \a
    connect \Y \y
  end
end
