attribute \top 1
module \m
  wire width 5 input 1 \a
  wire width 3 input 2 \b
  wire width 5 output 3 \y
  cell $xnor $c4
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 5
    parameter \B_WIDTH 3
    parameter \Y_WIDTH 5
    connect \A \a
    connect \B \b
    connect \Y \y
  end
end
