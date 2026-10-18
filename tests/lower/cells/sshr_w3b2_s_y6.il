attribute \top 1
module \m
  wire width 3 input 1 signed \a
  wire width 2 input 2 \b
  wire width 6 output 3 \y
  cell $sshr $c4
    parameter \A_SIGNED 1
    parameter \B_SIGNED 0
    parameter \A_WIDTH 3
    parameter \B_WIDTH 2
    parameter \Y_WIDTH 6
    connect \A \a
    connect \B \b
    connect \Y \y
  end
end
