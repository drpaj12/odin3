attribute \top 1
module \m
  wire width 4 input 1 \a
  wire width 3 input 2 signed \b
  wire width 2 output 3 \y
  cell $shiftx $c4
    parameter \A_SIGNED 0
    parameter \B_SIGNED 1
    parameter \A_WIDTH 4
    parameter \B_WIDTH 3
    parameter \Y_WIDTH 2
    connect \A \a
    connect \B \b
    connect \Y \y
  end
end
