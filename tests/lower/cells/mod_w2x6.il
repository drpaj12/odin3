attribute \top 1
module \m
  wire width 2 input 1 \a
  wire width 6 input 2 \b
  wire width 6 output 3 \y
  cell $mod $c4
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 2
    parameter \B_WIDTH 6
    parameter \Y_WIDTH 6
    connect \A \a
    connect \B \b
    connect \Y \y
  end
end
