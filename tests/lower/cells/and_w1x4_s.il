attribute \top 1
module \m
  wire input 1 signed \a
  wire width 4 input 2 signed \b
  wire width 4 output 3 \y
  cell $and $c4
    parameter \A_SIGNED 1
    parameter \B_SIGNED 1
    parameter \A_WIDTH 1
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \a
    connect \B \b
    connect \Y \y
  end
end
