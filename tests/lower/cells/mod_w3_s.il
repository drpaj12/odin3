attribute \top 1
module \m
  wire width 3 input 1 signed \a
  wire width 3 input 2 signed \b
  wire width 3 output 3 \y
  cell $mod $c4
    parameter \A_SIGNED 1
    parameter \B_SIGNED 1
    parameter \A_WIDTH 3
    parameter \B_WIDTH 3
    parameter \Y_WIDTH 3
    connect \A \a
    connect \B \b
    connect \Y \y
  end
end
