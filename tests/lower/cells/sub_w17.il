attribute \top 1
module \m
  wire width 17 input 1 \a
  wire width 17 input 2 \b
  wire width 17 output 3 \y
  cell $sub $c4
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 17
    parameter \B_WIDTH 17
    parameter \Y_WIDTH 17
    connect \A \a
    connect \B \b
    connect \Y \y
  end
end
