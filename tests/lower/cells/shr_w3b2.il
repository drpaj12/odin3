attribute \top 1
module \m
  wire width 3 input 1 \a
  wire width 2 input 2 \b
  wire width 3 output 3 \y
  cell $shr $c4
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 3
    parameter \B_WIDTH 2
    parameter \Y_WIDTH 3
    connect \A \a
    connect \B \b
    connect \Y \y
  end
end
